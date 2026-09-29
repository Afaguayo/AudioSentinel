#include "exposure.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace as
{

double SafeHours(double db)
{
    return REF_HOURS * std::pow(2.0, (REF_DB - db) / EXCHANGE_DB);
}

std::string FormatDuration(double hours)
{
    if (hours >= 24.0)
        return "more than 24 h";
    int minutes = (int)(hours * 60.0);
    if (minutes < 1)
        return "under 1 min";
    if (minutes < 60)
        return std::to_string(minutes) + " min";
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
}

double EstimateLevel(double rms, double volumeDb, bool muted, int loudness)
{
    if (muted || !(rms > 1e-5))
        return 0.0;
    loudness = std::clamp(loudness, 0, LOUDNESS_COUNT - 1);
    double dbfs = 20.0 * std::log10(rms);
    return std::clamp(dbfs + volumeDb + LOUDNESS_OFFSETS[loudness], 0.0, 140.0);
}

std::string Today()
{
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
    return buf;
}

Zone ZoneFor(double db, bool paused, bool deviceOk)
{
    if (paused)
        return Zone::Paused;
    if (!deviceOk || db < 1.0)
        return Zone::Silent;
    if (db < 80.0)
        return Zone::Safe;
    if (db < 90.0)
        return Zone::Loud;
    return Zone::Harmful;
}

const char* ZoneLabel(Zone zone)
{
    switch (zone)
    {
    case Zone::Paused: return "PAUSED";
    case Zone::Silent: return "SILENT";
    case Zone::Safe: return "SAFE";
    case Zone::Loud: return "LOUD";
    default: return "HARMFUL";
    }
}

double Smoother::Step(double level)
{
    value_ += (level - value_) * (level > value_ ? 0.6 : 0.2);
    if (value_ < 0.5)
        value_ = 0.0;
    return value_;
}

double SpotifyShare(const DayStats& stats)
{
    if (stats.exposure <= 0.0)
        return 0.0;
    return std::clamp(stats.spotifyExposure / stats.exposure, 0.0, 1.0);
}

void Tracker::Tick(double level, double displayDb, double dt, bool spotifyPlaying)
{
    if (level <= 0.0 || dt <= 0.0)
        return;
    double added = dt / (SafeHours(level) * 3600.0);
    stats.exposure += added;
    if (spotifyPlaying)
        stats.spotifyExposure += added;
    if (level >= 40.0)
        stats.listenSeconds += dt;
    stats.peakDb = std::max(stats.peakDb, displayDb);
}

bool Tracker::Rollover(const std::string& today)
{
    if (stats.date == today)
        return false;
    Reset();
    stats.date = today;
    return true;
}

void Tracker::Reset()
{
    std::string date = stats.date;
    stats = DayStats{};
    stats.date = date;
}

std::vector<Alert> AlertPolicy::Update(DayStats& stats, double db, bool paused,
                                       bool spotifyPlaying, double now)
{
    static const double marks[] = { 0.5, 0.8, 1.0 };
    std::vector<Alert> alerts;

    // Allowance milestones; only the highest one crossed is announced.
    int reached = std::clamp(stats.alertLevel, 0, 3);
    while (reached < 3 && stats.exposure >= marks[reached])
        reached++;
    if (reached > stats.alertLevel)
    {
        stats.alertLevel = reached;
        if (reached == 3)
        {
            alerts.push_back({ "Daily safe-listening limit reached",
                               "More loud listening today risks hearing damage. Take a break or turn the volume down a lot." });
            lastOver_ = now;
        }
        else
        {
            std::string pct = std::to_string((int)(marks[reached - 1] * 100));
            alerts.push_back({ pct + "% of today's allowance used",
                               "You have used " + pct + "% of today's safe-listening allowance. Turning the volume down makes the rest last much longer." });
        }
    }
    else if (stats.exposure >= 1.0 && db >= 70.0 && now - lastOver_ > 30.0 * 60.0)
    {
        alerts.push_back({ "Still over today's limit",
                           "You are " + std::to_string((int)(stats.exposure * 100)) +
                               "% through today's allowance. Consider a break." });
        lastOver_ = now;
    }

    // Sustained very loud audio.
    loudSeconds_ = (!paused && db >= LOUD_ALERT_DB) ? loudSeconds_ + 1 : 0;
    if (loudSeconds_ >= LOUD_ALERT_SECONDS && now - lastLoud_ > 10.0 * 60.0)
    {
        std::string source = spotifyPlaying ? "Spotify is very loud: " : "Very loud: ";
        alerts.push_back({ source + std::to_string((int)std::lround(db)) + " dB",
                           "At this volume your whole daily allowance lasts only about " +
                               FormatDuration(SafeHours(db)) + ". Turn it down." });
        lastLoud_ = now;
    }
    return alerts;
}

} // namespace as
