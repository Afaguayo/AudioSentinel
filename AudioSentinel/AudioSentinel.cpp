// AudioSentinel: a tray app that estimates how loud your PC's audio output is
// and how much of today's safe-listening allowance you have used.
//
// This file is the Windows front end (Win32 + WASAPI + GDI+). The exposure
// model, alerts, settings and Spotify title parsing live in ../core and are
// shared with the Linux app.
//
//   AudioSentinel.exe                            start (shows the dashboard)
//   AudioSentinel.exe --startup                  start in the tray (used at sign-in)
//   AudioSentinel.exe --render-dashboard x.png   draw the dashboard with sample data

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
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "resource.h"
#include "../core/dashboard.h"
#include "../core/exposure.h"
#include "../core/settings.h"
#include "../core/spotify.h"

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

constexpr int DASHBOARD_WIDTH = 440;
constexpr int DASHBOARD_HEIGHT = 450;

constexpr const wchar_t* APP_NAME = L"AudioSentinel";
constexpr const wchar_t* TRAY_CLASS = L"AudioSentinelTray";
constexpr const wchar_t* DASH_CLASS = L"AudioSentinelDashboard";
constexpr const wchar_t* MUTEX_NAME = L"Local\\AudioSentinel.Instance";
constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum MenuId : UINT
{
    ID_OPEN = 100,
    ID_PAUSE,
    ID_NOTIFY,
    ID_SPOTIFY,
    ID_TOPMOST,
    ID_STARTUP,
    ID_RESET,
    ID_ABOUT,
    ID_EXIT,
    ID_LOUD_0,  // ID_LOUD_0 + index into as::LOUDNESS_OFFSETS
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
    as::Tracker tracker;
} g;

std::atomic<bool> g_running{ true };
std::atomic<bool> g_paused{ false };
std::atomic<bool> g_deviceOk{ false };
std::atomic<bool> g_spotifyPlaying{ false };
std::atomic<double> g_db{ 0.0 };  // smoothed level for display
std::atomic<int> g_loudness{ 1 };

// UI thread only.
as::Settings g_set;
std::unique_ptr<as::IniFile> g_ini;
as::AlertPolicy g_alerts;
as::NowPlaying g_nowPlaying;

HINSTANCE g_inst = nullptr;
HWND g_tray = nullptr;
HWND g_dash = nullptr;
NOTIFYICONDATAW g_nid = {};
HICON g_trayIcon = nullptr;
int g_trayIconKey = -1;
UINT g_taskbarCreated = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::wstring Wide(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
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

void LoadState()
{
    g_ini->Load();
    std::lock_guard<std::mutex> lk(g.lock);
    as::LoadState(*g_ini, as::Today(), g_set, g.tracker.stats);
    g_loudness = g_set.loudness;
}

void SaveState()
{
    {
        std::lock_guard<std::mutex> lk(g.lock);
        as::StoreState(*g_ini, g_set, g.tracker.stats);
    }
    g_ini->Save();
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
// Spotify: the desktop app titles its main window "Artist - Title" while
// playing (see as::FromSpotifyWindowTitle).
// ---------------------------------------------------------------------------

bool IsSpotifyProcess(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (!ok)
        return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"Spotify.exe") == 0;
}

BOOL CALLBACK FindSpotifyWindow(HWND hwnd, LPARAM result)
{
    if (GetWindowTextLengthW(hwnd) == 0)
        return TRUE;
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"Chrome_WidgetWin_", 17) != 0)  // Spotify is a Chromium app
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!IsSpotifyProcess(pid))
        return TRUE;
    *reinterpret_cast<HWND*>(result) = hwnd;
    return FALSE;
}

as::NowPlaying ReadSpotify()
{
    static HWND cached = nullptr;
    if (cached && (!IsWindow(cached) || GetWindowTextLengthW(cached) == 0))
        cached = nullptr;
    if (!cached)
        EnumWindows(FindSpotifyWindow, reinterpret_cast<LPARAM>(&cached));
    if (!cached)
        return {};
    wchar_t title[512];
    GetWindowTextW(cached, title, 512);
    return as::FromSpotifyWindowTitle(Utf8(title));
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
        as::Smoother smoother;
        bool open = false;
        int ticksSinceDeviceCheck = 0;
        int ticksUntilRetry = 0;
        auto last = std::chrono::steady_clock::now();

        while (g_running)
        {
            Sleep(1000 / as::TICKS_PER_SECOND);

            auto now = std::chrono::steady_clock::now();
            // Cap dt so a resume from sleep doesn't count the whole gap.
            double dt = std::min(std::chrono::duration<double>(now - last).count(), 1.0);
            last = now;

            if (!open && enumerator && --ticksUntilRetry <= 0)
            {
                open = capture.Open(enumerator.Get());
                ticksSinceDeviceCheck = 0;
                ticksUntilRetry = 2 * as::TICKS_PER_SECOND;
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
            if (open && ++ticksSinceDeviceCheck >= 2 * as::TICKS_PER_SECOND)
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
                level = as::EstimateLevel(std::sqrt(sumSquares / (double)count), volumeDb, muted, g_loudness);
            }

            double shown = smoother.Step(level);
            g_db = shown;

            std::lock_guard<std::mutex> lk(g.lock);
            g.history.push_back((float)shown);
            while (g.history.size() > as::HISTORY_LEN)
                g.history.pop_front();
            g.tracker.Tick(level, shown, dt, g_spotifyPlaying);
        }
        capture.Close();
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Look and feel
// ---------------------------------------------------------------------------

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
const G::Color COLOR_SPOTIFY(255, 30, 215, 96);

G::Color ZoneColor(as::Zone z)
{
    switch (z)
    {
    case as::Zone::Safe: return COLOR_GREEN;
    case as::Zone::Loud: return COLOR_AMBER;
    case as::Zone::Harmful: return COLOR_RED;
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

// Draws text on one line, ending in "..." if it doesn't fit in maxWidth.
void DrawLabelClipped(G::Graphics& gr, const std::wstring& text, const G::Font& font,
                      const G::Color& color, float x, float y, float maxWidth)
{
    G::StringFormat format;
    format.SetTrimming(G::StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(G::StringFormatFlagsNoWrap);
    G::SolidBrush brush(color);
    G::RectF box(x, y, maxWidth, font.GetHeight(&gr) + 2.0f);
    gr.DrawString(text.c_str(), (INT)text.size(), &font, box, &format, &brush);
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

// A ring that fills up with today's allowance around a dot showing the current level.
HICON MakeTrayIcon(as::Zone zone, double fraction)
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
    as::Zone zone = as::ZoneFor(db, g_paused, g_deviceOk);
    int key = (int)zone * 1000 + (int)(std::min(fraction, 1.0) * 24.0);
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
    if (zone == as::Zone::Paused)
        tip += L"Paused";
    else if (zone == as::Zone::Silent)
        tip += L"Nothing playing";
    else
        tip += std::to_wstring((int)std::lround(db)) + L" dB (" + Wide(as::ZoneLabel(zone)) + L")";
    tip += L"\n" + std::to_wstring((int)(fraction * 100.0)) + L"% of today's allowance used";
    if (g_set.spotify && g_nowPlaying.playing)
        tip += L"\nSpotify: " + Wide(as::DescribeTrack(g_nowPlaying));

    g_nid.uFlags = NIF_ICON | NIF_TIP;
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void Notify(const as::Alert& alert)
{
    if (!g_set.notifications)
        return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(n.szInfoTitle, Wide(alert.title).c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, Wide(alert.text).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

as::DashboardModel BuildModel()
{
    as::DashboardModel m;
    {
        std::lock_guard<std::mutex> lk(g.lock);
        m.stats = g.tracker.stats;
        m.history.assign(g.history.begin(), g.history.end());
    }
    m.db = g_db;
    m.paused = g_paused;
    m.deviceOk = g_deviceOk;
    m.spotifyEnabled = g_set.spotify;
    m.spotify = g_nowPlaying;
    return m;
}

// Draws the dashboard at scale k (1.0 = 96 DPI) into a W x H pixel area.
void PaintDashboard(G::Graphics& gr, float W, float H, float k, const as::DashboardModel& m)
{
    as::Zone zone = as::ModelZone(m);
    G::Color zoneColor = ZoneColor(zone);
    bool noNumber = zone == as::Zone::Silent || zone == as::Zone::Paused;

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
    std::wstring number = noNumber ? L"--" : std::to_wstring((int)std::lround(m.db));
    DrawLabel(gr, number, fontBig, COLOR_TEXT, margin - 4 * k, 28 * k);
    DrawLabel(gr, L"dB", fontUnit, COLOR_MUTED, margin + TextWidth(gr, number, fontBig) - 10 * k, 58 * k);

    std::wstring pill = Wide(as::ZoneLabel(zone));
    float pillW = TextWidth(gr, pill, fontLabelBold) + 22 * k;
    G::RectF pillRect(W - margin - pillW, 44 * k, pillW, 26 * k);
    FillRoundRect(gr, WithAlpha(zoneColor, 45), pillRect, 13 * k);
    DrawLabel(gr, pill, fontLabelBold, zoneColor, pillRect.X + 11 * k, pillRect.Y + 5 * k);

    DrawLabel(gr, Wide(as::StatusLine(m)), fontBody, COLOR_TEXT, margin, 96 * k);

    // Spotify row.
    std::wstring spotify = Wide(as::SpotifyLine(m));
    if (!spotify.empty())
    {
        G::SolidBrush dot(m.spotify.playing ? COLOR_SPOTIFY : COLOR_GREY);
        gr.FillEllipse(&dot, margin, 122 * k, 12 * k, 12 * k);
        DrawLabelClipped(gr, spotify, fontBody, m.spotify.playing ? COLOR_TEXT : COLOR_MUTED,
                         margin + 18 * k, 119 * k, W - 2 * margin - 18 * k);
    }

    // Allowance card.
    G::RectF card(margin, 150 * k, W - 2 * margin, 82 * k);
    FillRoundRect(gr, COLOR_CARD, card, 10 * k);
    float inner = card.X + 14 * k, innerRight = card.GetRight() - 14 * k;
    G::Color allowanceColor = AllowanceColor(m.stats.exposure);
    DrawLabel(gr, L"Today's allowance used", fontLabel, COLOR_MUTED, inner, card.Y + 12 * k);
    DrawLabelRight(gr, std::to_wstring((int)(m.stats.exposure * 100.0)) + L"%", fontValue, allowanceColor,
                   innerRight, card.Y + 9 * k);
    G::RectF bar(inner, card.Y + 36 * k, innerRight - inner, 10 * k);
    FillRoundRect(gr, COLOR_TRACK, bar, 5 * k);
    float filled = (float)std::min(m.stats.exposure, 1.0) * bar.Width;
    if (filled > 0.5f)
        FillRoundRect(gr, allowanceColor, G::RectF(bar.X, bar.Y, std::max(filled, bar.Height), bar.Height), 5 * k);
    DrawLabel(gr, Wide(as::StatsLine(m)), fontLabel, COLOR_MUTED, inner, card.Y + 56 * k);

    // History graph card.
    G::RectF graphCard(margin, 244 * k, W - 2 * margin, H - 244 * k - 40 * k);
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
    float refY = yFor((float)as::REF_DB);
    gr.DrawLine(&refLine, plot.X, refY, plot.GetRight(), refY);
    DrawLabelRight(gr, L"85 dB", fontTiny, WithAlpha(COLOR_RED, 200), plot.GetRight(), refY - 16 * k);

    if (m.history.size() >= 2)
    {
        std::vector<G::PointF> points;
        points.reserve(m.history.size() + 2);
        float step = plot.Width / (float)(as::HISTORY_LEN - 1);
        float x0 = plot.GetRight() - step * (float)(m.history.size() - 1);
        for (size_t i = 0; i < m.history.size(); i++)
            points.emplace_back(x0 + step * (float)i, yFor(m.history[i]));

        G::Color lineColor = noNumber ? COLOR_GREEN : zoneColor;
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

void PaintDashboardWindow(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right, height = rc.bottom;
    if (width <= 0 || height <= 0)
        return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
    {
        G::Graphics gr(mem);
        PaintDashboard(gr, (float)width, (float)height, GetDpiForWindow(hwnd) / 96.0f, BuildModel());
    }
    BitBlt(hdc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

// Renders the dashboard with sample data to a PNG (used by CI for screenshots).
bool RenderDashboardPng(const wchar_t* path)
{
    const float scale = 2.0f;
    G::Bitmap bmp((INT)(DASHBOARD_WIDTH * scale), (INT)(DASHBOARD_HEIGHT * scale), PixelFormat32bppARGB);
    {
        G::Graphics gr(&bmp);
        PaintDashboard(gr, DASHBOARD_WIDTH * scale, DASHBOARD_HEIGHT * scale, scale, as::DemoModel());
    }

    UINT count = 0, size = 0;
    G::GetImageEncodersSize(&count, &size);
    if (size == 0)
        return false;
    std::vector<BYTE> buffer(size);
    auto* encoders = reinterpret_cast<G::ImageCodecInfo*>(buffer.data());
    G::GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; i++)
    {
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0)
            return bmp.Save(path, &encoders[i].Clsid, nullptr) == G::Ok;
    }
    return false;
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
    RECT r = { 0, 0, MulDiv(DASHBOARD_WIDTH, dpi, 96), MulDiv(DASHBOARD_HEIGHT, dpi, 96) };
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
    SetTimer(g_dash, TIMER_REDRAW, 1000 / as::TICKS_PER_SECOND, nullptr);
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
    CheckMenuRadioItem(loud, ID_LOUD_0, ID_LOUD_2, ID_LOUD_0 + g_set.loudness, MF_BYCOMMAND);

    bool visible = g_dash && IsWindowVisible(g_dash) && !IsIconic(g_dash);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_OPEN, visible ? L"Hide dashboard" : L"Open dashboard");
    SetMenuDefaultItem(menu, ID_OPEN, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_PAUSE, L"Pause monitoring");
    AppendMenuW(menu, MF_STRING | (g_set.notifications ? MF_CHECKED : 0), ID_NOTIFY, L"Warning notifications");
    AppendMenuW(menu, MF_STRING | (g_set.spotify ? MF_CHECKED : 0), ID_SPOTIFY, L"Show Spotify");
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
    case ID_SPOTIFY:
        g_set.spotify = !g_set.spotify;
        SaveState();
        break;
    case ID_LOUD_0:
    case ID_LOUD_1:
    case ID_LOUD_2:
        g_set.loudness = (int)(id - ID_LOUD_0);
        g_loudness = g_set.loudness;
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
                g.tracker.Reset();
            }
            SaveState();
        }
        break;
    case ID_ABOUT:
        MessageBoxW(g_dash && IsWindowVisible(g_dash) ? g_dash : nullptr,
                    L"AudioSentinel 1.2\n\n"
                    L"Listens to what your PC plays and estimates how loud it is. Your daily "
                    L"allowance follows the NIOSH guideline: 85 dB for 8 hours, halved for every "
                    L"3 dB louder (88 dB = 4 h, 91 dB = 2 h, ...).\n\n"
                    L"Levels are estimates: pick how loud your headphones or speakers are under "
                    L"\"My headphones/speakers are\" to calibrate. You get a warning at 50%, 80% "
                    L"and 100% of the allowance, and when audio is very loud.\n\n"
                    L"While Spotify plays, the dashboard shows the track and how much of today's "
                    L"exposure came from Spotify.\n\n"
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
// Once-a-second housekeeping: Spotify, day rollover, warnings, tray, autosave
// ---------------------------------------------------------------------------

void OnTick()
{
    static int ticks = 0;

    g_nowPlaying = g_set.spotify ? ReadSpotify() : as::NowPlaying{};
    g_spotifyPlaying = g_nowPlaying.playing;

    double db = g_db;
    std::vector<as::Alert> alerts;
    double fraction;
    {
        std::lock_guard<std::mutex> lk(g.lock);
        g.tracker.Rollover(as::Today());
        alerts = g_alerts.Update(g.tracker.stats, db, g_paused, g_nowPlaying.playing,
                                 GetTickCount64() / 1000.0);
        fraction = g.tracker.stats.exposure;
    }
    for (const auto& alert : alerts)
        Notify(alert);

    UpdateTray(db, fraction);

    if (!alerts.empty() || ++ticks % 30 == 0)
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
        PaintDashboardWindow(hwnd, hdc);
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

    // Headless mode for CI: draw the dashboard with sample data and exit.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++)
    {
        if (wcscmp(argv[i], L"--render-dashboard") == 0)
        {
            G::GdiplusStartupInput input;
            ULONG_PTR token = 0;
            G::GdiplusStartup(&token, &input, nullptr);
            bool ok = RenderDashboardPng(argv[i + 1]);
            G::GdiplusShutdown(token);
            LocalFree(argv);
            return ok ? 0 : 1;
        }
    }
    if (argv)
        LocalFree(argv);

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

    g_ini = std::make_unique<as::IniFile>(std::filesystem::path(StateDir()) / L"AudioSentinel.ini");
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
