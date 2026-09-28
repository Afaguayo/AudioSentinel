// AudioSentinel: a tray app that estimates how loud your PC's audio output is
// and how much of today's safe-listening allowance you have used.
//
// Exposure model: NIOSH recommendation of 85 dB for 8 hours with a 3 dB
// exchange rate (every +3 dB halves the safe time). The loudness in dB SPL is
// an estimate: the loopback signal level (dBFS) plus the Windows master volume,
// plus an offset for how loud the user's headphones/speakers are.

#define NOMINMAX
#include <windows.h>
#include <algorithm>
using std::max;
using std::min;
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "resource.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")

using Microsoft::WRL::ComPtr;
namespace G = Gdiplus;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 2;
constexpr UINT_PTR TIMER_TICK = 1;    // tray window, once per second
constexpr UINT_PTR TIMER_REDRAW = 2;  // dashboard, while visible

constexpr int TICKS_PER_SECOND = 4;
constexpr int HISTORY_SECONDS = 120;
constexpr size_t HISTORY_LEN = TICKS_PER_SECOND * HISTORY_SECONDS;

constexpr double REF_DB = 85.0;      // level allowed for REF_HOURS per day
constexpr double REF_HOURS = 8.0;
constexpr double EXCHANGE_DB = 3.0;  // +3 dB halves the allowed time
constexpr double LOUD_ALERT_DB = 95.0;

constexpr const wchar_t* APP_NAME = L"AudioSentinel";
constexpr const wchar_t* TRAY_CLASS = L"AudioSentinelTray";
constexpr const wchar_t* DASH_CLASS = L"AudioSentinelDashboard";
constexpr const wchar_t* MUTEX_NAME = L"Local\\AudioSentinel.Instance";
constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// dBFS -> estimated dB SPL offset for "Quiet", "Typical" and "Loud" outputs.
constexpr double LOUDNESS_OFFSETS[] = { 90.0, 100.0, 110.0 };

enum MenuId : UINT
{
    ID_OPEN = 100,
    ID_PAUSE,
    ID_NOTIFY,
    ID_TOPMOST,
    ID_STARTUP,
    ID_RESET,
    ID_ABOUT,
    ID_EXIT,
    ID_LOUD_0,  // ID_LOUD_0 + index into LOUDNESS_OFFSETS
    ID_LOUD_1,
    ID_LOUD_2,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Written by the audio thread, read by the UI thread; guarded by lock.
struct Shared
{
    std::mutex lock;
    std::deque<float> history;  // smoothed dB, one sample per tick
    double exposure = 0.0;      // fraction of today's allowance (1.0 = 100%)
    double listenSeconds = 0.0; // time with audible output today
    double peakDb = 0.0;
    std::wstring date;          // local date the counters belong to
} g;

std::atomic<bool> g_running{ true };
std::atomic<bool> g_paused{ false };
std::atomic<bool> g_deviceOk{ false };
std::atomic<double> g_db{ 0.0 };  // smoothed level for display
std::atomic<int> g_loudness{ 1 };

struct Settings
{
    bool notifications = true;
    bool topmost = false;
    int opacity = 96;  // percent
} g_set;

HINSTANCE g_inst = nullptr;
HWND g_tray = nullptr;
HWND g_dash = nullptr;
NOTIFYICONDATAW g_nid = {};
HICON g_trayIcon = nullptr;
int g_trayIconKey = -1;
UINT g_taskbarCreated = 0;
std::wstring g_ini;

int g_alertLevel = 0;  // how many of ALERT_MARKS were already announced today
int g_loudSeconds = 0;
ULONGLONG g_lastLoudAlert = 0;
ULONGLONG g_lastOverAlert = 0;

const double ALERT_MARKS[] = { 0.5, 0.8, 1.0 };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

double SafeHours(double db)
{
    return REF_HOURS * std::pow(2.0, (REF_DB - db) / EXCHANGE_DB);
}

std::wstring Today()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::wstring FormatDuration(double hours)
{
    if (hours >= 24.0)
        return L"more than 24 h";
    int minutes = (int)(hours * 60.0);
    if (minutes < 1)
        return L"under 1 min";
    if (minutes < 60)
        return std::to_wstring(minutes) + L" min";
    int h = minutes / 60, m = minutes % 60;
    return std::to_wstring(h) + L" h " + std::to_wstring(m) + L" min";
}

std::wstring StateDir()
{
    std::wstring dir;
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
    {
        dir = std::wstring(path) + L"\\AudioSentinel";
        CoTaskMemFree(path);
    }
    else
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        dir = std::wstring(tmp) + L"AudioSentinel";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

double IniGetDouble(const wchar_t* section, const wchar_t* key, double def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, g_ini.c_str());
    return buf[0] ? _wtof(buf) : def;
}

int IniGetInt(const wchar_t* section, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(section, key, def, g_ini.c_str());
}

void IniSet(const wchar_t* section, const wchar_t* key, const std::wstring& value)
{
    WritePrivateProfileStringW(section, key, value.c_str(), g_ini.c_str());
}

void LoadState()
{
    g_set.notifications = IniGetInt(L"Settings", L"Notifications", 1) != 0;
    g_set.topmost = IniGetInt(L"Settings", L"Topmost", 0) != 0;
    g_set.opacity = std::clamp(IniGetInt(L"Settings", L"Opacity", 96), 30, 100);
    g_loudness = std::clamp(IniGetInt(L"Settings", L"Loudness", 1), 0, 2);

    wchar_t date[32];
    GetPrivateProfileStringW(L"Today", L"Date", L"", date, 32, g_ini.c_str());

    std::lock_guard<std::mutex> lk(g.lock);
    g.date = Today();
    if (g.date == date)
    {
        g.exposure = IniGetDouble(L"Today", L"Exposure", 0.0);
        g.listenSeconds = IniGetDouble(L"Today", L"ListenSeconds", 0.0);
        g.peakDb = IniGetDouble(L"Today", L"PeakDb", 0.0);
        g_alertLevel = IniGetInt(L"Today", L"Alerts", 0);
    }
}

void SaveState()
{
    std::wstring date;
    double exposure, listen, peak;
    {
        std::lock_guard<std::mutex> lk(g.lock);
        date = g.date;
        exposure = g.exposure;
        listen = g.listenSeconds;
        peak = g.peakDb;
    }
    wchar_t buf[64];
    IniSet(L"Today", L"Date", date);
    swprintf_s(buf, L"%.8f", exposure);
    IniSet(L"Today", L"Exposure", buf);
    swprintf_s(buf, L"%.1f", listen);
    IniSet(L"Today", L"ListenSeconds", buf);
    swprintf_s(buf, L"%.1f", peak);
    IniSet(L"Today", L"PeakDb", buf);
    IniSet(L"Today", L"Alerts", std::to_wstring(g_alertLevel));

    IniSet(L"Settings", L"Notifications", g_set.notifications ? L"1" : L"0");
    IniSet(L"Settings", L"Topmost", g_set.topmost ? L"1" : L"0");
    IniSet(L"Settings", L"Opacity", std::to_wstring(g_set.opacity));
    IniSet(L"Settings", L"Loudness", std::to_wstring(g_loudness.load()));
}

bool IsStartupEnabled()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    LSTATUS status = RegQueryValueExW(key, APP_NAME, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void SetStartup(bool enable)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (enable)
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\" --startup";
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Audio capture (WASAPI loopback of the default output device)
// ---------------------------------------------------------------------------

struct SampleFormat
{
    bool isFloat = true;
    int bytes = 4;       // bytes per sample
    int channels = 2;
    int frameBytes = 8;
};

SampleFormat DescribeFormat(const WAVEFORMATEX* wf)
{
    SampleFormat f;
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
    {
        // The KSDATAFORMAT_SUBTYPE_* GUIDs carry the classic format tag in Data1.
        tag = (WORD)reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf)->SubFormat.Data1;
    }
    f.isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT);
    f.bytes = wf->wBitsPerSample / 8;
    f.channels = wf->nChannels;
    f.frameBytes = wf->nBlockAlign;
    return f;
}

double ReadSample(const BYTE* p, const SampleFormat& f)
{
    if (f.isFloat)
        return f.bytes == 8 ? *reinterpret_cast<const double*>(p) : *reinterpret_cast<const float*>(p);
    switch (f.bytes)
    {
    case 2:
        return *reinterpret_cast<const int16_t*>(p) / 32768.0;
    case 3:
    {
        uint32_t v = ((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24);
        return (int32_t)v / 2147483648.0;
    }
    case 4:
        return *reinterpret_cast<const int32_t*>(p) / 2147483648.0;
    default:
        return 0.0;
    }
}

std::wstring DefaultDeviceId(IMMDeviceEnumerator* enumerator)
{
    std::wstring result;
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)))
        {
            result = id;
            CoTaskMemFree(id);
        }
    }
    return result;
}

class LoopbackCapture
{
public:
    ~LoopbackCapture() { Close(); }

    bool Open(IMMDeviceEnumerator* enumerator)
    {
        Close();
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
            return false;

        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)))
        {
            id_ = id;
            CoTaskMemFree(id);
        }

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client_.GetAddressOf()))))
            return false;
        if (FAILED(client_->GetMixFormat(&mix_)))
            return false;
        format_ = DescribeFormat(mix_);
        if (format_.bytes == 0 || format_.channels == 0)
            return false;

        const REFERENCE_TIME bufferDuration = 2000000;  // 200 ms
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                       bufferDuration, 0, mix_, nullptr)))
            return false;
        if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient),
                                       reinterpret_cast<void**>(capture_.GetAddressOf()))))
            return false;

        // Loopback audio is captured before the master volume is applied, so the
        // volume is read separately. Missing volume control is not fatal.
        device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                         reinterpret_cast<void**>(volume_.GetAddressOf()));

        return SUCCEEDED(client_->Start());
    }

    void Close()
    {
        if (client_)
            client_->Stop();
        capture_.Reset();
        volume_.Reset();
        client_.Reset();
        if (mix_)
        {
            CoTaskMemFree(mix_);
            mix_ = nullptr;
        }
        id_.clear();
    }

    // Adds all pending samples to sumSquares/count. Returns false when the
    // device is gone and the capture must be reopened.
    bool Read(double& sumSquares, uint64_t& count)
    {
        UINT32 packet = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr))
                break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                for (UINT32 i = 0; i < frames; i++)
                {
                    const BYTE* frame = data + (size_t)i * format_.frameBytes;
                    for (int c = 0; c < format_.channels; c++)
                    {
                        double s = ReadSample(frame + (size_t)c * format_.bytes, format_);
                        sumSquares += s * s;
                    }
                }
            }
            count += (uint64_t)frames * format_.channels;

            capture_->ReleaseBuffer(frames);
            hr = capture_->GetNextPacketSize(&packet);
        }
        return SUCCEEDED(hr);
    }

    // Master volume in dB (0 = full volume, negative = attenuated).
    double VolumeDb(bool& muted)
    {
        muted = false;
        if (!volume_)
            return 0.0;
        BOOL mute = FALSE;
        float level = 0.0f;
        volume_->GetMute(&mute);
        volume_->GetMasterVolumeLevel(&level);
        muted = mute != FALSE;
        return level;
    }

    const std::wstring& DeviceId() const { return id_; }

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    ComPtr<IAudioEndpointVolume> volume_;
    WAVEFORMATEX* mix_ = nullptr;
    SampleFormat format_;
    std::wstring id_;
};

void AudioThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));

        LoopbackCapture capture;
        bool open = false;
        int ticksSinceDeviceCheck = 0;
        int ticksUntilRetry = 0;
        double smoothed = 0.0;
        auto last = std::chrono::steady_clock::now();

        while (g_running)
        {
            Sleep(1000 / TICKS_PER_SECOND);

            auto now = std::chrono::steady_clock::now();
            // Cap dt so a resume from sleep doesn't count the whole gap.
            double dt = std::min(std::chrono::duration<double>(now - last).count(), 1.0);
            last = now;

            if (!open && enumerator && --ticksUntilRetry <= 0)
            {
                open = capture.Open(enumerator.Get());
                ticksSinceDeviceCheck = 0;
                ticksUntilRetry = 2 * TICKS_PER_SECOND;
            }

            double sumSquares = 0.0;
            uint64_t count = 0;
            if (open && !capture.Read(sumSquares, count))
            {
                capture.Close();
                open = false;
                ticksUntilRetry = 0;
            }

            // Follow the user when they switch output device (e.g. speakers -> headphones).
            if (open && ++ticksSinceDeviceCheck >= 2 * TICKS_PER_SECOND)
            {
                ticksSinceDeviceCheck = 0;
                if (DefaultDeviceId(enumerator.Get()) != capture.DeviceId())
                {
                    capture.Close();
                    open = false;
                    ticksUntilRetry = 0;
                }
            }
            g_deviceOk = open;

            // Loopback delivers no packets while nothing plays, which counts as silence.
            double level = 0.0;
            if (count > 0 && !g_paused)
            {
                bool muted = false;
                double volumeDb = capture.VolumeDb(muted);
                double rms = std::sqrt(sumSquares / (double)count);
                if (!muted && rms > 1e-5)
                {
                    double dbfs = 20.0 * std::log10(rms);
                    level = std::clamp(dbfs + volumeDb + LOUDNESS_OFFSETS[g_loudness.load()], 0.0, 140.0);
                }
            }

            smoothed += (level - smoothed) * (level > smoothed ? 0.6 : 0.2);
            if (smoothed < 0.5)
                smoothed = 0.0;
            g_db = smoothed;

            std::lock_guard<std::mutex> lk(g.lock);
            g.history.push_back((float)smoothed);
            while (g.history.size() > HISTORY_LEN)
                g.history.pop_front();
            if (level > 0.0)
            {
                g.exposure += dt / (SafeHours(level) * 3600.0);
                if (level >= 40.0)
                    g.listenSeconds += dt;
                g.peakDb = std::max(g.peakDb, smoothed);
            }
        }
        capture.Close();
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Look and feel
// ---------------------------------------------------------------------------

enum Zone { ZONE_PAUSED, ZONE_SILENT, ZONE_SAFE, ZONE_LOUD, ZONE_HARMFUL };

Zone ZoneFor(double db)
{
    if (g_paused)
        return ZONE_PAUSED;
    if (!g_deviceOk || db < 1.0)
        return ZONE_SILENT;
    if (db < 80.0)
        return ZONE_SAFE;
    if (db < 90.0)
        return ZONE_LOUD;
    return ZONE_HARMFUL;
}

const wchar_t* ZoneLabel(Zone z)
{
    switch (z)
    {
    case ZONE_PAUSED: return L"PAUSED";
    case ZONE_SILENT: return L"SILENT";
    case ZONE_SAFE: return L"SAFE";
    case ZONE_LOUD: return L"LOUD";
    default: return L"HARMFUL";
    }
}

const G::Color COLOR_BG(255, 18, 20, 26);
const G::Color COLOR_CARD(255, 29, 33, 42);
const G::Color COLOR_TRACK(255, 45, 50, 62);
const G::Color COLOR_TEXT(255, 236, 239, 244);
const G::Color COLOR_MUTED(255, 140, 147, 160);
const G::Color COLOR_DIM(255, 96, 102, 115);
const G::Color COLOR_GREEN(255, 61, 220, 132);
const G::Color COLOR_AMBER(255, 255, 176, 32);
const G::Color COLOR_RED(255, 255, 90, 95);
const G::Color COLOR_GREY(255, 138, 143, 152);

G::Color ZoneColor(Zone z)
{
    switch (z)
    {
    case ZONE_SAFE: return COLOR_GREEN;
    case ZONE_LOUD: return COLOR_AMBER;
    case ZONE_HARMFUL: return COLOR_RED;
    default: return COLOR_GREY;
    }
}

G::Color AllowanceColor(double fraction)
{
    if (fraction < 0.5)
        return COLOR_GREEN;
    if (fraction < 0.8)
        return COLOR_AMBER;
    return COLOR_RED;
}

G::Color WithAlpha(const G::Color& c, BYTE alpha)
{
    return G::Color(alpha, c.GetR(), c.GetG(), c.GetB());
}

void AddRoundRect(G::GraphicsPath& path, const G::RectF& r, float radius)
{
    float d = std::min(radius * 2.0f, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(G::Graphics& gr, const G::Color& color, const G::RectF& r, float radius)
{
    G::GraphicsPath path;
    AddRoundRect(path, r, radius);
    G::SolidBrush brush(color);
    gr.FillPath(&brush, &path);
}

float TextWidth(G::Graphics& gr, const std::wstring& text, const G::Font& font)
{
    G::RectF box;
    gr.MeasureString(text.c_str(), (INT)text.size(), &font, G::PointF(0, 0), &box);
    return box.Width;
}

void DrawLabel(G::Graphics& gr, const std::wstring& text, const G::Font& font,
               const G::Color& color, float x, float y)
{
    G::SolidBrush brush(color);
    gr.DrawString(text.c_str(), (INT)text.size(), &font, G::PointF(x, y), &brush);
}

void DrawLabelRight(G::Graphics& gr, const std::wstring& text, const G::Font& font,
                    const G::Color& color, float right, float y)
{
    DrawLabel(gr, text, font, color, right - TextWidth(gr, text, font), y);
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

// A ring that fills up with today's allowance around a dot showing the current level.
HICON MakeTrayIcon(Zone zone, double fraction)
{
    int size = GetSystemMetrics(SM_CXSMICON);
    G::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        G::Graphics gr(&bmp);
        gr.SetSmoothingMode(G::SmoothingModeAntiAlias);
        gr.Clear(G::Color(0, 0, 0, 0));

        float s = (float)size;
        float stroke = s * 0.17f;
        float inset = stroke / 2.0f + s * 0.03f;
        G::RectF ring(inset, inset, s - 2 * inset, s - 2 * inset);

        G::Pen track(G::Color(170, 120, 126, 138), stroke);
        gr.DrawEllipse(&track, ring);
        float sweep = (float)std::min(fraction, 1.0) * 360.0f;
        if (sweep > 1.0f)
        {
            G::Pen arc(AllowanceColor(fraction), stroke);
            gr.DrawArc(&arc, ring, -90.0f, sweep);
        }

        float dot = s - 2 * (inset + stroke) - s * 0.02f;
        G::SolidBrush fill(ZoneColor(zone));
        gr.FillEllipse(&fill, (s - dot) / 2.0f, (s - dot) / 2.0f, dot, dot);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

void AddTrayIcon()
{
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
    wcscpy_s(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void UpdateTray(double db, double fraction)
{
    Zone zone = ZoneFor(db);
    int key = zone * 1000 + (int)(std::min(fraction, 1.0) * 24.0);
    if (key != g_trayIconKey)
    {
        HICON icon = MakeTrayIcon(zone, fraction);
        if (icon)
        {
            HICON old = g_trayIcon;
            g_trayIcon = icon;
            g_nid.hIcon = icon;
            if (old)
                DestroyIcon(old);
            g_trayIconKey = key;
        }
    }

    std::wstring tip = std::wstring(APP_NAME) + L"\n";
    if (zone == ZONE_PAUSED)
        tip += L"Paused";
    else if (zone == ZONE_SILENT)
        tip += L"Nothing playing";
    else
        tip += std::to_wstring((int)std::lround(db)) + L" dB (" + ZoneLabel(zone) + L")";
    tip += L"\n" + std::to_wstring((int)(fraction * 100.0)) + L"% of today's allowance used";

    g_nid.uFlags = NIF_ICON | NIF_TIP;
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void Notify(const std::wstring& title, const std::wstring& text)
{
    if (!g_set.notifications)
        return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

void PaintDashboard(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right, height = rc.bottom;
    if (width <= 0 || height <= 0)
        return;
    float k = GetDpiForWindow(hwnd) / 96.0f;
    float W = (float)width, H = (float)height;

    std::vector<float> history;
    double exposure, listen, peak;
    {
        std::lock_guard<std::mutex> lk(g.lock);
        history.assign(g.history.begin(), g.history.end());
        exposure = g.exposure;
        listen = g.listenSeconds;
        peak = g.peakDb;
    }
    double db = g_db;
    Zone zone = ZoneFor(db);
    G::Color zoneColor = ZoneColor(zone);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
    {
        G::Graphics gr(mem);
        gr.SetSmoothingMode(G::SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(G::TextRenderingHintClearTypeGridFit);
        gr.Clear(COLOR_BG);

        G::FontFamily family(L"Segoe UI");
        G::Font fontBig(&family, 52 * k, G::FontStyleBold, G::UnitPixel);
        G::Font fontUnit(&family, 20 * k, G::FontStyleRegular, G::UnitPixel);
        G::Font fontLabel(&family, 12 * k, G::FontStyleRegular, G::UnitPixel);
        G::Font fontLabelBold(&family, 12 * k, G::FontStyleBold, G::UnitPixel);
        G::Font fontBody(&family, 13 * k, G::FontStyleRegular, G::UnitPixel);
        G::Font fontValue(&family, 15 * k, G::FontStyleBold, G::UnitPixel);
        G::Font fontTiny(&family, 11 * k, G::FontStyleRegular, G::UnitPixel);

        float margin = 20 * k;

        // Header: current level and zone pill.
        DrawLabel(gr, L"CURRENT LEVEL", fontLabelBold, COLOR_MUTED, margin, 16 * k);
        std::wstring number = (zone == ZONE_SILENT || zone == ZONE_PAUSED)
                                  ? L"--"
                                  : std::to_wstring((int)std::lround(db));
        DrawLabel(gr, number, fontBig, COLOR_TEXT, margin - 4 * k, 28 * k);
        DrawLabel(gr, L"dB", fontUnit, COLOR_MUTED, margin + TextWidth(gr, number, fontBig) - 10 * k, 58 * k);

        std::wstring pill = ZoneLabel(zone);
        float pillW = TextWidth(gr, pill, fontLabelBold) + 22 * k;
        G::RectF pillRect(W - margin - pillW, 44 * k, pillW, 26 * k);
        FillRoundRect(gr, WithAlpha(zoneColor, 45), pillRect, 13 * k);
        DrawLabel(gr, pill, fontLabelBold, zoneColor, pillRect.X + 11 * k, pillRect.Y + 5 * k);

        // Status sentence.
        std::wstring status;
        if (zone == ZONE_PAUSED)
            status = L"Monitoring is paused; exposure isn't being counted.";
        else if (!g_deviceOk)
            status = L"No audio output device found.";
        else if (zone == ZONE_SILENT)
            status = L"Nothing is playing right now.";
        else if (exposure >= 1.0)
            status = L"Daily allowance used up. Take a break or turn it down.";
        else if (db < 70.0)
            status = L"Comfortable level; it barely touches your allowance.";
        else
            status = L"At this level: " + FormatDuration((1.0 - exposure) * SafeHours(db)) +
                     L" of safe listening left today.";
        DrawLabel(gr, status, fontBody, COLOR_TEXT, margin, 96 * k);

        // Allowance card.
        G::RectF card(margin, 126 * k, W - 2 * margin, 82 * k);
        FillRoundRect(gr, COLOR_CARD, card, 10 * k);
        float inner = card.X + 14 * k, innerRight = card.GetRight() - 14 * k;
        G::Color allowanceColor = AllowanceColor(exposure);
        DrawLabel(gr, L"Today's allowance used", fontLabel, COLOR_MUTED, inner, card.Y + 12 * k);
        DrawLabelRight(gr, std::to_wstring((int)(exposure * 100.0)) + L"%", fontValue, allowanceColor,
                       innerRight, card.Y + 9 * k);
        G::RectF bar(inner, card.Y + 36 * k, innerRight - inner, 10 * k);
        FillRoundRect(gr, COLOR_TRACK, bar, 5 * k);
        float filled = (float)std::min(exposure, 1.0) * bar.Width;
        if (filled > 0.5f)
            FillRoundRect(gr, allowanceColor, G::RectF(bar.X, bar.Y, std::max(filled, bar.Height), bar.Height), 5 * k);
        std::wstring stats = L"Listening today: " + FormatDuration(listen / 3600.0);
        if (peak > 0.0)
            stats += L"    Peak: " + std::to_wstring((int)std::lround(peak)) + L" dB";
        DrawLabel(gr, stats, fontLabel, COLOR_MUTED, inner, card.Y + 56 * k);

        // History graph card.
        G::RectF graphCard(margin, 220 * k, W - 2 * margin, H - 220 * k - 40 * k);
        FillRoundRect(gr, COLOR_CARD, graphCard, 10 * k);
        DrawLabel(gr, L"Last 2 minutes", fontLabel, COLOR_MUTED, inner, graphCard.Y + 10 * k);

        G::RectF plot(inner, graphCard.Y + 34 * k, innerRight - inner, graphCard.Height - 46 * k);
        const float minDb = 30.0f, maxDb = 110.0f;
        auto yFor = [&](float v) {
            float t = (std::clamp(v, minDb, maxDb) - minDb) / (maxDb - minDb);
            return plot.GetBottom() - t * plot.Height;
        };

        G::Pen baseline(COLOR_TRACK, 1.0f * k);
        gr.DrawLine(&baseline, plot.X, plot.GetBottom(), plot.GetRight(), plot.GetBottom());
        G::Pen refLine(WithAlpha(COLOR_RED, 150), 1.0f * k);
        refLine.SetDashStyle(G::DashStyleDash);
        float refY = yFor((float)REF_DB);
        gr.DrawLine(&refLine, plot.X, refY, plot.GetRight(), refY);
        DrawLabelRight(gr, L"85 dB", fontTiny, WithAlpha(COLOR_RED, 200), plot.GetRight(), refY - 16 * k);

        if (history.size() >= 2)
        {
            std::vector<G::PointF> points;
            points.reserve(history.size() + 2);
            float step = plot.Width / (float)(HISTORY_LEN - 1);
            float x0 = plot.GetRight() - step * (float)(history.size() - 1);
            for (size_t i = 0; i < history.size(); i++)
                points.emplace_back(x0 + step * (float)i, yFor(history[i]));

            G::Color lineColor = zone == ZONE_SILENT || zone == ZONE_PAUSED ? COLOR_GREEN : zoneColor;
            std::vector<G::PointF> area(points);
            area.emplace_back(points.back().X, plot.GetBottom());
            area.emplace_back(points.front().X, plot.GetBottom());
            G::SolidBrush areaBrush(WithAlpha(lineColor, 40));
            gr.FillPolygon(&areaBrush, area.data(), (INT)area.size());
            G::Pen line(lineColor, 2.0f * k);
            line.SetLineJoin(G::LineJoinRound);
            gr.DrawLines(&line, points.data(), (INT)points.size());
        }

        DrawLabel(gr, L"Right-click for options  \x00B7  \x2191/\x2193 change opacity  \x00B7  Esc hides",
                  fontTiny, COLOR_DIM, margin, H - 28 * k);
    }
    BitBlt(hdc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

void ApplyOpacity()
{
    if (g_dash)
        SetLayeredWindowAttributes(g_dash, 0, (BYTE)(255 * g_set.opacity / 100), LWA_ALPHA);
}

void CreateDashboard()
{
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD exStyle = WS_EX_LAYERED | (g_set.topmost ? WS_EX_TOPMOST : 0);
    g_dash = CreateWindowExW(exStyle, DASH_CLASS, APP_NAME, style, 0, 0, 100, 100,
                             nullptr, nullptr, g_inst, nullptr);
    if (!g_dash)
        return;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_dash, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    ApplyOpacity();

    // Size for the monitor's DPI and park it above the tray, like other tray apps.
    UINT dpi = GetDpiForWindow(g_dash);
    RECT r = { 0, 0, MulDiv(440, dpi, 96), MulDiv(420, dpi, 96) };
    AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, dpi);
    int w = r.right - r.left, h = r.bottom - r.top, gap = MulDiv(16, dpi, 96);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(g_dash, nullptr, work.right - w - gap, work.bottom - h - gap, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShowDashboard()
{
    if (!g_dash)
        CreateDashboard();
    if (!g_dash)
        return;
    ShowWindow(g_dash, SW_SHOWNORMAL);
    SetForegroundWindow(g_dash);
    SetTimer(g_dash, TIMER_REDRAW, 250, nullptr);
    InvalidateRect(g_dash, nullptr, FALSE);
}

void HideDashboard()
{
    if (!g_dash)
        return;
    KillTimer(g_dash, TIMER_REDRAW);
    ShowWindow(g_dash, SW_HIDE);
}

void ToggleDashboard()
{
    if (g_dash && IsWindowVisible(g_dash) && !IsIconic(g_dash))
        HideDashboard();
    else
        ShowDashboard();
}

// ---------------------------------------------------------------------------
// Menu and commands
// ---------------------------------------------------------------------------

void ShowMenu(HWND owner)
{
    HMENU loud = CreatePopupMenu();
    AppendMenuW(loud, MF_STRING, ID_LOUD_0, L"Quiet (earbuds, low-sensitivity speakers)");
    AppendMenuW(loud, MF_STRING, ID_LOUD_1, L"Typical");
    AppendMenuW(loud, MF_STRING, ID_LOUD_2, L"Loud (sensitive headphones, big speakers)");
    CheckMenuRadioItem(loud, ID_LOUD_0, ID_LOUD_2, ID_LOUD_0 + g_loudness, MF_BYCOMMAND);

    bool visible = g_dash && IsWindowVisible(g_dash) && !IsIconic(g_dash);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_OPEN, visible ? L"Hide dashboard" : L"Open dashboard");
    SetMenuDefaultItem(menu, ID_OPEN, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_PAUSE, L"Pause monitoring");
    AppendMenuW(menu, MF_STRING | (g_set.notifications ? MF_CHECKED : 0), ID_NOTIFY, L"Warning notifications");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)loud, L"My headphones/speakers are");
    AppendMenuW(menu, MF_STRING | (g_set.topmost ? MF_CHECKED : 0), ID_TOPMOST, L"Keep dashboard on top");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_RESET, L"Reset today's exposure...");
    AppendMenuW(menu, MF_STRING, ID_ABOUT, L"About AudioSentinel");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);  // so the menu closes when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu
}

void HandleCommand(UINT id)
{
    switch (id)
    {
    case ID_OPEN:
        ToggleDashboard();
        break;
    case ID_PAUSE:
        g_paused = !g_paused;
        break;
    case ID_NOTIFY:
        g_set.notifications = !g_set.notifications;
        SaveState();
        break;
    case ID_LOUD_0:
    case ID_LOUD_1:
    case ID_LOUD_2:
        g_loudness = (int)(id - ID_LOUD_0);
        SaveState();
        break;
    case ID_TOPMOST:
        g_set.topmost = !g_set.topmost;
        if (g_dash)
            SetWindowPos(g_dash, g_set.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SaveState();
        break;
    case ID_STARTUP:
        SetStartup(!IsStartupEnabled());
        break;
    case ID_RESET:
        if (MessageBoxW(g_dash && IsWindowVisible(g_dash) ? g_dash : nullptr,
                        L"Reset today's exposure, listening time and peak level to zero?",
                        APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            {
                std::lock_guard<std::mutex> lk(g.lock);
                g.exposure = 0.0;
                g.listenSeconds = 0.0;
                g.peakDb = 0.0;
            }
            g_alertLevel = 0;
            SaveState();
        }
        break;
    case ID_ABOUT:
        MessageBoxW(g_dash && IsWindowVisible(g_dash) ? g_dash : nullptr,
                    L"AudioSentinel 1.0\n\n"
                    L"Listens to what your PC plays and estimates how loud it is. Your daily "
                    L"allowance follows the NIOSH guideline: 85 dB for 8 hours, halved for every "
                    L"3 dB louder (88 dB = 4 h, 91 dB = 2 h, ...).\n\n"
                    L"Levels are estimates: pick how loud your headphones or speakers are under "
                    L"\"My headphones/speakers are\" to calibrate. You get a warning at 50%, 80% "
                    L"and 100% of the allowance, and when audio is very loud.\n\n"
                    L"The counter resets every day at midnight.\n"
                    L"https://github.com/Afaguayo/AudioSentinel",
                    L"About AudioSentinel", MB_OK | MB_ICONINFORMATION);
        break;
    case ID_EXIT:
        DestroyWindow(g_tray);
        break;
    }
}

// ---------------------------------------------------------------------------
// Once-a-second housekeeping: day rollover, warnings, tray, autosave
// ---------------------------------------------------------------------------

void OnTick()
{
    static int ticks = 0;
    std::wstring today = Today();
    double fraction;
    {
        std::lock_guard<std::mutex> lk(g.lock);
        if (g.date != today)
        {
            g.date = today;
            g.exposure = 0.0;
            g.listenSeconds = 0.0;
            g.peakDb = 0.0;
            g_alertLevel = 0;
        }
        fraction = g.exposure;
    }
    double db = g_db;
    ULONGLONG now = GetTickCount64();

    // Allowance milestones; only the highest one crossed is announced.
    int reached = g_alertLevel;
    while (reached < 3 && fraction >= ALERT_MARKS[reached])
        reached++;
    if (reached > g_alertLevel)
    {
        g_alertLevel = reached;
        if (reached == 3)
        {
            Notify(L"Daily safe-listening limit reached",
                   L"More loud listening today risks hearing damage. Take a break or turn the volume down a lot.");
            g_lastOverAlert = now;
        }
        else
        {
            std::wstring pct = std::to_wstring((int)(ALERT_MARKS[reached - 1] * 100));
            Notify(pct + L"% of today's allowance used",
                   L"You have used " + pct + L"% of today's safe-listening allowance. Turning the volume down makes the rest last much longer.");
        }
        SaveState();
    }
    else if (fraction >= 1.0 && db >= 70.0 && now - g_lastOverAlert > 30ull * 60 * 1000)
    {
        Notify(L"Still over today's limit",
               L"You are " + std::to_wstring((int)(fraction * 100)) + L"% through today's allowance. Consider a break.");
        g_lastOverAlert = now;
    }

    // Sustained very loud audio.
    g_loudSeconds = (!g_paused && db >= LOUD_ALERT_DB) ? g_loudSeconds + 1 : 0;
    if (g_loudSeconds >= 10 && (g_lastLoudAlert == 0 || now - g_lastLoudAlert > 10ull * 60 * 1000))
    {
        Notify(L"Very loud: " + std::to_wstring((int)std::lround(db)) + L" dB",
               L"At this volume your whole daily allowance lasts only about " +
                   FormatDuration(SafeHours(db)) + L". Turn it down.");
        g_lastLoudAlert = now;
    }

    UpdateTray(db, fraction);

    if (++ticks % 30 == 0)
        SaveState();
}

// ---------------------------------------------------------------------------
// Window procedures
// ---------------------------------------------------------------------------

LRESULT CALLBACK DashboardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;  // everything is painted in WM_PAINT; avoids flicker

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintDashboard(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_UP || wParam == VK_DOWN)
        {
            g_set.opacity = std::clamp(g_set.opacity + (wParam == VK_UP ? 5 : -5), 30, 100);
            ApplyOpacity();
            SaveState();
        }
        else if (wParam == VK_ESCAPE)
        {
            HideDashboard();
        }
        return 0;

    case WM_CONTEXTMENU:
        ShowMenu(hwnd);
        return 0;

    case WM_COMMAND:
        HandleCommand(LOWORD(wParam));
        return 0;

    case WM_DPICHANGED:
    {
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        HideDashboard();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_taskbarCreated && g_taskbarCreated != 0)
    {
        // Explorer restarted; the icon has to be added again.
        AddTrayIcon();
        g_trayIconKey = -1;
        return 0;
    }

    switch (msg)
    {
    case WM_TRAYICON:
        switch (LOWORD(lParam))
        {
        case WM_LBUTTONUP:
            ToggleDashboard();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu(hwnd);
            break;
        case NIN_BALLOONUSERCLICK:
            ShowDashboard();
            break;
        }
        return 0;

    case WM_APP_SHOW:
        ShowDashboard();
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_TICK)
            OnTick();
        return 0;

    case WM_COMMAND:
        HandleCommand(LOWORD(wParam));
        return 0;

    case WM_ENDSESSION:
        if (wParam)
            SaveState();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int)
{
    g_inst = inst;

    // One instance only; a second launch just opens the running one's dashboard.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other = FindWindowW(TRAY_CLASS, nullptr);
        if (other)
            PostMessageW(other, WM_APP_SHOW, 0, 0);
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    G::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    G::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    g_ini = StateDir() + L"\\AudioSentinel.ini";
    LoadState();

    HICON appIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = inst;
    wc.hIcon = appIcon;
    wc.lpszClassName = TRAY_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = DashboardProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIconSm = appIcon;
    wc.lpszClassName = DASH_CLASS;
    RegisterClassExW(&wc);

    // A hidden top-level window (not message-only) so it receives TaskbarCreated.
    g_tray = CreateWindowExW(0, TRAY_CLASS, APP_NAME, WS_OVERLAPPED, 0, 0, 0, 0,
                             nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    std::thread audio(AudioThread);
    SetTimer(g_tray, TIMER_TICK, 1000, nullptr);
    OnTick();

    // Launched by the user (not at sign-in): show the dashboard so they see it's running.
    if (!wcsstr(cmdLine, L"--startup"))
        ShowDashboard();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    audio.join();
    SaveState();

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayIcon)
        DestroyIcon(g_trayIcon);
    if (g_dash)
        DestroyWindow(g_dash);

    G::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
