// Platform-independent logic shared by the Windows and Linux apps: the
// loudness estimate, the daily exposure counters and when to warn.
#pragma once

#include <string>
#include <vector>

namespace as
{

// NIOSH: 85 dB is safe for 8 hours; every +3 dB halves the safe time.
constexpr double REF_DB = 85.0;
constexpr double REF_HOURS = 8.0;
constexpr double EXCHANGE_DB = 3.0;
constexpr double LOUD_ALERT_DB = 95.0;
constexpr int LOUD_ALERT_SECONDS = 10;

// dBFS -> estimated dB SPL for "Quiet", "Typical" and "Loud" outputs.
constexpr double LOUDNESS_OFFSETS[] = { 90.0, 100.0, 110.0 };
constexpr int LOUDNESS_COUNT = 3;

double SafeHours(double db);

// "45 min", "3 h 12 min", "under 1 min", "more than 24 h".
std::string FormatDuration(double hours);

// Estimated dB SPL from the RMS of full-scale samples (1.0 = 0 dBFS), the
// output volume in dB (0 = full) and the calibration preset. 0 means silence.
double EstimateLevel(double rms, double volumeDb, bool muted, int loudness);

// Local date as YYYY-MM-DD.
std::string Today();

enum class Zone { Paused, Silent, Safe, Loud, Harmful };
Zone ZoneFor(double db, bool paused, bool deviceOk);
const char* ZoneLabel(Zone zone);

// Display smoothing: rises fast, falls slowly, so the number is readable.
class Smoother
{
public:
    double Step(double level);
    double Value() const { return value_; }

private:
    double value_ = 0.0;
};

struct DayStats
{
    std::string date;
    double exposure = 0.0;         // fraction of the allowance (1.0 = 100%)
    double spotifyExposure = 0.0;  // part of exposure counted while Spotify played
    double listenSeconds = 0.0;    // time with audible output
    double peakDb = 0.0;
    int alertLevel = 0;            // allowance milestones already announced
};

// Share of today's exposure that happened while Spotify was playing (0..1).
double SpotifyShare(const DayStats& stats);

// Accumulates exposure. Not thread-safe; callers hold their own lock.
class Tracker
{
public:
    // level: this tick's estimate (0 = silence); displayDb: smoothed value.
    void Tick(double level, double displayDb, double dt, bool spotifyPlaying);
    // Starts a new day when the date changed. Returns true if it did.
    bool Rollover(const std::string& today);
    void Reset();

    DayStats stats;
};

struct Alert
{
    std::string title;
    std::string text;
};

// Decides which warnings to show. Call once per second.
class AlertPolicy
{
public:
    std::vector<Alert> Update(DayStats& stats, double db, bool paused, bool spotifyPlaying,
                              double nowSeconds);

private:
    int loudSeconds_ = 0;
    double lastLoud_ = -1e12;
    double lastOver_ = -1e12;
};

} // namespace as
