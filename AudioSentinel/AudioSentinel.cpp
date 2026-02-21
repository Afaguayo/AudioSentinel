// AudioSentinel.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <atomic>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <chrono>
#include <vector>
#include <fstream>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mmdevapi.lib")

#define WM_TRAYICON (WM_USER + 1)
#define IDT_UPDATE 1

std::atomic<bool> g_running(true);
std::atomic<double> g_db(0.0);
std::atomic<double> g_exposure(0.0);

std::vector<double> g_history;
const int MAX_HISTORY = 150;

double g_opacity = 0.85;
double DAILY_LIMIT = 1.0;

HWND g_dashboard = nullptr;
NOTIFYICONDATA g_nid = {};

double calculateSafeHours(double db)
{
    return 8.0 * pow(2.0, (85.0 - db) / 3.0);
}

void SaveExposure()
{
    std::ofstream file("exposure.dat");
    if (file.is_open())
    {
        file << g_exposure.load();
        file.close();
    }
}

void LoadExposure()
{
    std::ifstream file("exposure.dat");
    if (file.is_open())
    {
        double val;
        file >> val;
        g_exposure.store(val);
        file.close();
    }
}

void UpdateTrayTooltip()
{
    double db = g_db.load();
    std::wstring tip = L"dB: " + std::to_wstring((int)db);
    wcscpy_s(g_nid.szTip, tip.c_str());
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

void AudioThread()
{
    CoInitialize(nullptr);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;

    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

    pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
        nullptr, (void**)&pAudioClient);

    WAVEFORMATEX* pwfx = nullptr;
    pAudioClient->GetMixFormat(&pwfx);

    pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000,
        0,
        pwfx,
        nullptr
    );

    pAudioClient->GetService(__uuidof(IAudioCaptureClient),
        (void**)&pCaptureClient);

    pAudioClient->Start();

    double smoothedDb = 0.0;
    const double alpha = 0.2;

    auto lastTime = std::chrono::steady_clock::now();

    while (g_running)
    {
        UINT32 packetLength = 0;
        pCaptureClient->GetNextPacketSize(&packetLength);

        while (packetLength > 0)
        {
            BYTE* pData;
            UINT32 numFrames;
            DWORD flags;

            pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);

            float* samples = (float*)pData;
            int channels = pwfx->nChannels;

            double sum = 0.0;
            for (UINT32 i = 0; i < numFrames * channels; i++)
                sum += samples[i] * samples[i];

            double rms = sqrt(sum / (numFrames * channels));
            double db = 20.0 * log10(rms + 1e-12);
            double db_spl = db + 100.0;

            smoothedDb = alpha * db_spl + (1 - alpha) * smoothedDb;
            g_db.store(smoothedDb);

            g_history.push_back(smoothedDb);
            if (g_history.size() > MAX_HISTORY)
                g_history.erase(g_history.begin());

            double safeHours = calculateSafeHours(smoothedDb);

            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = now - lastTime;
            lastTime = now;

            if (safeHours > 0)
            {
                double increment = elapsed.count() / (safeHours * 3600.0);
                g_exposure.store(g_exposure.load() + increment);
            }

            pCaptureClient->ReleaseBuffer(numFrames);
            pCaptureClient->GetNextPacketSize(&packetLength);
        }

        Sleep(200);
    }

    pAudioClient->Stop();
    CoUninitialize();
}

LRESULT CALLBACK DashboardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, TRUE);
        break;

    case WM_KEYDOWN:
        if (wParam == VK_UP)
            g_opacity = min(1.0, g_opacity + 0.05);
        else if (wParam == VK_DOWN)
            g_opacity = max(0.3, g_opacity - 0.05);

        SetLayeredWindowAttributes(hwnd, 0, (BYTE)(255 * g_opacity), LWA_ALPHA);
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);

        HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &rect, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        double db = g_db.load();
        double exposure = (g_exposure.load() / DAILY_LIMIT) * 100.0;

        std::wstring t1 = L"dB: " + std::to_wstring((int)db);
        std::wstring t2 = L"Allowance Used: " + std::to_wstring((int)exposure) + L"%";

        TextOut(hdc, 20, 10, t1.c_str(), t1.length());
        TextOut(hdc, 20, 30, t2.c_str(), t2.length());

        int graphTop = 60;
        int graphHeight = 60;

        for (size_t i = 1; i < g_history.size(); i++)
        {
            double val = g_history[i];
            COLORREF color;

            if (val < 80)
                color = RGB(0, 220, 120);
            else if (val < 90)
                color = RGB(255, 200, 0);
            else
                color = RGB(255, 60, 60);

            HPEN pen = CreatePen(PS_SOLID, 2, color);
            SelectObject(hdc, pen);

            int x1 = 20 + (int)i - 1;
            int y1 = graphTop + graphHeight - (int)(g_history[i - 1] / 120.0 * graphHeight);

            int x2 = 20 + (int)i;
            int y2 = graphTop + graphHeight - (int)(val / 120.0 * graphHeight);

            MoveToEx(hdc, x1, y1, NULL);
            LineTo(hdc, x2, y2);

            DeleteObject(pen);
        }

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TRAYICON)
    {
        if (lParam == WM_LBUTTONDOWN)
        {
            if (!g_dashboard)
            {
                WNDCLASS wc = {};
                wc.lpfnWndProc = DashboardProc;
                wc.hInstance = GetModuleHandle(NULL);
                wc.lpszClassName = L"DashboardClass";
                RegisterClass(&wc);

                g_dashboard = CreateWindowEx(
                    WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                    wc.lpszClassName,
                    L"Audio Dashboard",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    200, 200, 320, 180,
                    NULL, NULL, wc.hInstance, NULL);

                SetLayeredWindowAttributes(g_dashboard, 0, (BYTE)(255 * g_opacity), LWA_ALPHA);
                SetTimer(g_dashboard, IDT_UPDATE, 500, NULL);
            }

            ShowWindow(g_dashboard, SW_SHOW);
        }
        else if (lParam == WM_RBUTTONDOWN)
        {
            PostQuitMessage(0);
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    LoadExposure();

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TrayClass";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, wc.lpszClassName, L"", 0,
        0, 0, 0, 0, NULL, NULL, hInst, NULL);

    g_nid = {};
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_INFORMATION);
    wcscpy_s(g_nid.szTip, L"Audio Monitor");

    Shell_NotifyIcon(NIM_ADD, &g_nid);

    std::thread audioThread(AudioThread);

    SetTimer(hwnd, IDT_UPDATE, 1000, [](HWND, UINT, UINT_PTR, DWORD) {
        UpdateTrayTooltip();
        });

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    audioThread.join();

    SaveExposure();
    Shell_NotifyIcon(NIM_DELETE, &g_nid);

    return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
