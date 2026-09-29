// What the dashboard shows, independent of how each platform draws it.
#pragma once

#include "exposure.h"
#include "spotify.h"

#include <cstddef>
#include <string>
#include <vector>

namespace as
{

constexpr int TICKS_PER_SECOND = 4;
constexpr int HISTORY_SECONDS = 120;
constexpr size_t HISTORY_LEN = TICKS_PER_SECOND * HISTORY_SECONDS;

struct DashboardModel
{
    double db = 0.0;  // smoothed level
    bool paused = false;
    bool deviceOk = true;
    DayStats stats;
    std::vector<float> history;  // oldest first, at most HISTORY_LEN
    bool spotifyEnabled = true;
    NowPlaying spotify;
};

Zone ModelZone(const DashboardModel& m);

// "At this level: 2 h 10 min of safe listening left today." and friends.
std::string StatusLine(const DashboardModel& m);

// "Spotify · Artist — Title", "Spotify · paused", or "" when not shown.
std::string SpotifyLine(const DashboardModel& m);

// "Listening today: 1 h 5 min    Peak: 92 dB    Spotify: 60%".
std::string StatsLine(const DashboardModel& m);

// Fixed sample state, used to render screenshots in CI.
DashboardModel DemoModel();

} // namespace as
