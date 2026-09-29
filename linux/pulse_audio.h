// Linux audio input: what the default output device plays, via the PulseAudio
// API (served by PulseAudio itself or by PipeWire's pipewire-pulse).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct pa_simple;
struct pa_threaded_mainloop;
struct pa_context;

// Records from the default sink's monitor ("@DEFAULT_MONITOR@").
class MonitorCapture
{
public:
    ~MonitorCapture() { Close(); }

    bool Open();
    void Close();
    bool IsOpen() const { return stream_ != nullptr; }

    // Blocks for `seconds` of audio and returns its RMS (1.0 = full scale).
    // Returns false when the stream broke and must be reopened.
    bool ReadRms(double seconds, double& rms);

private:
    pa_simple* stream_ = nullptr;
};

// Polls the default sink's volume once a second on a PulseAudio mainloop thread.
class SinkVolume
{
public:
    ~SinkVolume() { Stop(); }

    void Start();
    void Stop();

    // Volume to add to the monitor's level to estimate what reaches the ears.
    // It is 0 when the monitor already carries the volume (PulseAudio with a
    // software-volume sink), because adding it again would count it twice.
    double EffectiveVolumeDb() const { return effectiveDb_.load(); }
    bool Muted() const { return muted_.load(); }
    std::string ServerName() const;
    std::string SinkName() const;

private:
    void Poll();
    void Disconnect();
    static void OnServerInfo(pa_context* c, const struct pa_server_info* info, void* self);
    static void OnSinkInfo(pa_context* c, const struct pa_sink_info* info, int eol, void* self);

    pa_threaded_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    std::thread poller_;
    std::atomic<bool> running_{ false };
    std::atomic<double> effectiveDb_{ 0.0 };
    std::atomic<bool> muted_{ false };
    std::atomic<bool> pipewire_{ false };
    mutable std::mutex namesLock_;
    std::string serverName_, sinkName_;
};
