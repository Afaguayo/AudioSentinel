#include "dashboard.h"

#include <cmath>

namespace as
{

Zone ModelZone(const DashboardModel& m)
{
    return ZoneFor(m.db, m.paused, m.deviceOk);
}

std::string StatusLine(const DashboardModel& m)
{
    Zone zone = ModelZone(m);
    if (zone == Zone::Paused)
        return "Monitoring is paused; exposure isn't being counted.";
    if (!m.deviceOk)
        return "No audio output device found.";
    if (zone == Zone::Silent)
        return "Nothing is playing right now.";
    if (m.stats.exposure >= 1.0)
        return "Daily allowance used up. Take a break or turn it down.";
    if (m.db < 70.0)
        return "Comfortable level; it barely touches your allowance.";
    return "At this level: " + FormatDuration((1.0 - m.stats.exposure) * SafeHours(m.db)) +
           " of safe listening left today.";
}

std::string SpotifyLine(const DashboardModel& m)
{
    if (!m.spotifyEnabled || !m.spotify.running)
        return "";
    const std::string prefix = "Spotify \xC2\xB7 ";  // middle dot
    if (!m.spotify.playing)
        return prefix + "paused";
    std::string track = DescribeTrack(m.spotify);
    return prefix + (track.empty() ? "playing" : track);
}

std::string StatsLine(const DashboardModel& m)
{
    std::string line = "Listening today: " + FormatDuration(m.stats.listenSeconds / 3600.0);
    if (m.stats.peakDb > 0.0)
        line += "    Peak: " + std::to_string((int)std::lround(m.stats.peakDb)) + " dB";
    if (m.spotifyEnabled && m.stats.spotifyExposure > 0.0)
        line += "    Spotify: " + std::to_string((int)std::lround(SpotifyShare(m.stats) * 100.0)) + "%";
    return line;
}

DashboardModel DemoModel()
{
    DashboardModel m;
    m.db = 84.0;
    m.stats.date = "2026-09-28";
    m.stats.exposure = 0.62;
    m.stats.spotifyExposure = 0.41;
    m.stats.listenSeconds = 2 * 3600 + 25 * 60;
    m.stats.peakDb = 93.0;
    for (size_t i = 0; i < HISTORY_LEN; i++)
    {
        double t = (double)i / TICKS_PER_SECOND;
        double v = 74.0 + 8.0 * std::sin(t / 9.0) + 3.0 * std::sin(t * 1.7) + (i > 400 ? 6.0 : 0.0);
        m.history.push_back((float)v);
    }
    m.spotify.running = true;
    m.spotify.playing = true;
    m.spotify.artist = "Daft Punk";
    m.spotify.title = "One More Time";
    return m;
}

} // namespace as
