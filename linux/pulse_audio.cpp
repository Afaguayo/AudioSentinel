#include "pulse_audio.h"

#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace
{
constexpr uint32_t RATE = 48000;
constexpr uint8_t CHANNELS = 2;
}

bool MonitorCapture::Open()
{
    Close();
    pa_sample_spec spec{};
    spec.format = PA_SAMPLE_FLOAT32NE;
    spec.rate = RATE;
    spec.channels = CHANNELS;

    // Small fragments so reads return promptly.
    pa_buffer_attr attr{};
    attr.maxlength = (uint32_t)-1;
    attr.fragsize = (uint32_t)pa_usec_to_bytes(50 * 1000, &spec);

    int error = 0;
    stream_ = pa_simple_new(nullptr, "AudioSentinel", PA_STREAM_RECORD, "@DEFAULT_MONITOR@",
                            "Loudness monitor", &spec, nullptr, &attr, &error);
    error_ = stream_ ? "" : pa_strerror(error);
    return stream_ != nullptr;
}

void MonitorCapture::Close()
{
    if (stream_)
    {
        pa_simple_free(stream_);
        stream_ = nullptr;
    }
}

bool MonitorCapture::ReadRms(double seconds, double& rms)
{
    if (!stream_)
        return false;
    size_t samples = (size_t)(RATE * seconds) * CHANNELS;
    std::vector<float> buffer(samples);
    int error = 0;
    if (pa_simple_read(stream_, buffer.data(), buffer.size() * sizeof(float), &error) < 0)
    {
        error_ = pa_strerror(error);
        return false;
    }
    double sum = 0.0;
    for (float s : buffer)
        sum += (double)s * s;
    rms = std::sqrt(sum / (double)samples);
    return true;
}

void SinkVolume::Start()
{
    if (running_)
        return;
    running_ = true;
    poller_ = std::thread([this] {
        while (running_)
        {
            Poll();
            for (int i = 0; i < 10 && running_; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Disconnect();
    });
}

void SinkVolume::Stop()
{
    running_ = false;
    if (poller_.joinable())
        poller_.join();
}

std::string SinkVolume::ServerName() const
{
    std::lock_guard<std::mutex> lk(namesLock_);
    return serverName_;
}

std::string SinkVolume::SinkName() const
{
    std::lock_guard<std::mutex> lk(namesLock_);
    return sinkName_;
}

void SinkVolume::Disconnect()
{
    if (loop_)
        pa_threaded_mainloop_stop(loop_);
    if (context_)
    {
        pa_context_disconnect(context_);
        pa_context_unref(context_);
        context_ = nullptr;
    }
    if (loop_)
    {
        pa_threaded_mainloop_free(loop_);
        loop_ = nullptr;
    }
}

void SinkVolume::Poll()
{
    if (context_)
    {
        pa_threaded_mainloop_lock(loop_);
        pa_context_state_t state = pa_context_get_state(context_);
        pa_threaded_mainloop_unlock(loop_);
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            Disconnect();  // server restarted; reconnect below
    }

    if (!context_)
    {
        loop_ = pa_threaded_mainloop_new();
        if (!loop_)
            return;
        context_ = pa_context_new(pa_threaded_mainloop_get_api(loop_), "AudioSentinel volume");
        if (!context_ || pa_context_connect(context_, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0 ||
            pa_threaded_mainloop_start(loop_) < 0)
        {
            Disconnect();
            return;
        }
    }

    pa_threaded_mainloop_lock(loop_);
    if (pa_context_get_state(context_) == PA_CONTEXT_READY)
    {
        if (pa_operation* op = pa_context_get_server_info(context_, &SinkVolume::OnServerInfo, this))
            pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(loop_);
}

void SinkVolume::OnServerInfo(pa_context* c, const pa_server_info* info, void* self)
{
    auto* me = static_cast<SinkVolume*>(self);
    if (!info)
        return;
    std::string server = info->server_name ? info->server_name : "";
    me->pipewire_ = server.find("PipeWire") != std::string::npos;
    {
        std::lock_guard<std::mutex> lk(me->namesLock_);
        me->serverName_ = server;
    }
    if (info->default_sink_name)
    {
        if (pa_operation* op = pa_context_get_sink_info_by_name(c, info->default_sink_name,
                                                                &SinkVolume::OnSinkInfo, self))
            pa_operation_unref(op);
    }
}

void SinkVolume::OnSinkInfo(pa_context*, const pa_sink_info* info, int eol, void* self)
{
    auto* me = static_cast<SinkVolume*>(self);
    if (eol || !info)
        return;
    double db = pa_sw_volume_to_dB(pa_cvolume_max(&info->volume));
    if (!std::isfinite(db))
        db = -100.0;
    db = std::clamp(db, -100.0, 20.0);

    // PipeWire's monitor ports ignore the sink volume by default, and hardware
    // volume is applied after the monitor tap: in both cases add it. PulseAudio
    // applies software volume before the monitor, so it is already included.
    bool monitorHasVolume = !me->pipewire_ && !(info->flags & PA_SINK_HW_VOLUME_CTRL);
    me->effectiveDb_ = monitorHasVolume ? 0.0 : db;
    me->muted_ = info->mute != 0;
    std::lock_guard<std::mutex> lk(me->namesLock_);
    me->sinkName_ = info->name ? info->name : "";
}
