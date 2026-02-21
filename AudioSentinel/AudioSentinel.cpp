// AudioSentinel.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <iostream>
#include <cmath>
#include <chrono>
#include <iomanip>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mmdevapi.lib")

double calculateSafeHours(double db) {
    return 8.0 * pow(2.0, (85.0 - db) / 3.0);
}

int main() {
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
    double exposureDose = 0.0;

    auto lastTime = std::chrono::steady_clock::now();

    while (true) {
        UINT32 packetLength = 0;
        pCaptureClient->GetNextPacketSize(&packetLength);

        while (packetLength > 0) {
            BYTE* pData;
            UINT32 numFrames;
            DWORD flags;

            pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);

            float* samples = (float*)pData;
            int channels = pwfx->nChannels;

            double sum = 0.0;
            for (UINT32 i = 0; i < numFrames * channels; i++) {
                sum += samples[i] * samples[i];
            }

            double rms = sqrt(sum / (numFrames * channels));
            double db = 20.0 * log10(rms + 1e-12);

            // Temporary calibration offset
            double db_spl = db + 100.0;

            // Smooth it
            smoothedDb = alpha * db_spl + (1.0 - alpha) * smoothedDb;

            double safeHours = calculateSafeHours(smoothedDb);

            // Time tracking
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = now - lastTime;
            lastTime = now;

            if (smoothedDb > 0 && safeHours > 0) {
                exposureDose += elapsed.count() / (safeHours * 3600.0);
            }

            system("cls");

            // Level Bar
            int barWidth = 50;
            int level = (int)((smoothedDb / 120.0) * barWidth);
            if (level > barWidth) level = barWidth;
            if (level < 0) level = 0;

            std::cout << "Audio Sentinel\n";
            std::cout << "-----------------------------\n";

            std::cout << "Level: "
                << std::fixed << std::setprecision(2)
                << smoothedDb << " dB\n\n";

            std::cout << "[";
            for (int i = 0; i < barWidth; i++) {
                if (i < level) std::cout << "#";
                else std::cout << " ";
            }
            std::cout << "]\n\n";

            if (smoothedDb >= 90.0)
                std::cout << "!!! DANGER ZONE !!!\n";

            std::cout << "Safe Listening Time: "
                << safeHours << " hours\n";

            std::cout << "Daily Exposure Used: "
                << exposureDose * 100.0 << "%\n";

            if (exposureDose >= 1.0)
                std::cout << ">>> DAILY LIMIT REACHED <<<\n";

            pCaptureClient->ReleaseBuffer(numFrames);
            pCaptureClient->GetNextPacketSize(&packetLength);
        }

        Sleep(100);
    }

    pAudioClient->Stop();
    CoUninitialize();
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
