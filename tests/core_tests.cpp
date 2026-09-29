// Unit tests for the shared core. Build and run with CTest (see CMakeLists.txt).
#include "../core/dashboard.h"
#include "../core/exposure.h"
#include "../core/settings.h"
#include "../core/spotify.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* expr, const char* file, int line)
{
    g_checks++;
    if (!ok)
    {
        g_failures++;
        std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    }
}

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

struct TestCase
{
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Register
{
    Register(const char* name, std::function<void()> fn) { Registry().push_back({ name, std::move(fn) }); }
};

#define TEST(name)                                   \
    void name();                                     \
    Register register_##name(#name, name);           \
    void name()

using namespace as;

// --- exposure model --------------------------------------------------------

TEST(safe_hours_follows_niosh)
{
    CHECK(Near(SafeHours(85), 8.0, 1e-9));
    CHECK(Near(SafeHours(88), 4.0, 1e-9));
    CHECK(Near(SafeHours(94), 1.0, 1e-9));
    CHECK(Near(SafeHours(82), 16.0, 1e-9));
}

TEST(format_duration)
{
    CHECK(FormatDuration(0.001) == "under 1 min");
    CHECK(FormatDuration(0.75) == "45 min");
    CHECK(FormatDuration(3.2) == "3 h 12 min");
    CHECK(FormatDuration(30) == "more than 24 h");
}

TEST(estimate_level)
{
    // Full-scale sine: RMS 1/sqrt(2) = -3.01 dBFS; "Typical" adds 100.
    CHECK(Near(EstimateLevel(1.0 / std::sqrt(2.0), 0.0, false, 1), 96.99, 0.01));
    // Master volume at -20 dB and the "Quiet" preset.
    CHECK(Near(EstimateLevel(0.1, -20.0, false, 0), 50.0, 1e-9));
    CHECK(EstimateLevel(0.5, 0.0, true, 1) == 0.0);    // muted
    CHECK(EstimateLevel(0.0, 0.0, false, 1) == 0.0);   // digital silence
    CHECK(EstimateLevel(1e-7, 0.0, false, 1) == 0.0);  // below the noise floor
    CHECK(EstimateLevel(1.0, 0.0, false, 99) == 110.0); // bad preset is clamped
}

TEST(zones)
{
    CHECK(ZoneFor(90, true, true) == Zone::Paused);
    CHECK(ZoneFor(90, false, false) == Zone::Silent);
    CHECK(ZoneFor(0, false, true) == Zone::Silent);
    CHECK(ZoneFor(79.9, false, true) == Zone::Safe);
    CHECK(ZoneFor(80, false, true) == Zone::Loud);
    CHECK(ZoneFor(90, false, true) == Zone::Harmful);
    CHECK(std::string(ZoneLabel(Zone::Harmful)) == "HARMFUL");
}

TEST(smoother_rises_fast_and_decays_to_zero)
{
    Smoother s;
    CHECK(Near(s.Step(100), 60, 1e-9));
    for (int i = 0; i < 200; i++)
        s.Step(0);
    CHECK(s.Value() == 0.0);
}

TEST(tracker_counts_one_allowance_in_eight_hours_at_85)
{
    Tracker t;
    t.stats.date = "2026-01-01";
    for (int i = 0; i < 8 * 3600 * 4; i++)
        t.Tick(85.0, 85.0, 0.25, false);
    CHECK(Near(t.stats.exposure, 1.0, 1e-6));
    CHECK(Near(t.stats.listenSeconds, 8 * 3600.0, 1e-3));
    CHECK(t.stats.spotifyExposure == 0.0);
}

TEST(tracker_ignores_silence_and_attributes_spotify)
{
    Tracker t;
    t.Tick(0.0, 0.0, 3600.0, true);  // silence costs nothing
    CHECK(t.stats.exposure == 0.0);
    t.Tick(94.0, 94.0, 1800.0, true);  // 30 min at 94 dB = half the allowance
    t.Tick(94.0, 94.0, 1800.0, false);
    CHECK(Near(t.stats.exposure, 1.0, 1e-9));
    CHECK(Near(SpotifyShare(t.stats), 0.5, 1e-9));
    CHECK(Near(t.stats.peakDb, 94.0, 1e-9));
}

TEST(tracker_rolls_over_at_new_day)
{
    Tracker t;
    t.stats.date = "2026-01-01";
    t.Tick(100, 100, 60, true);
    CHECK(!t.Rollover("2026-01-01"));
    CHECK(t.stats.exposure > 0);
    CHECK(t.Rollover("2026-01-02"));
    CHECK(t.stats.exposure == 0.0 && t.stats.spotifyExposure == 0.0 && t.stats.peakDb == 0.0);
    CHECK(t.stats.date == "2026-01-02");
}

TEST(today_is_iso_date)
{
    std::string d = Today();
    CHECK(d.size() == 10 && d[4] == '-' && d[7] == '-');
}

// --- alerts ----------------------------------------------------------------

TEST(alerts_announce_each_milestone_once)
{
    AlertPolicy p;
    DayStats s;
    s.exposure = 0.49;
    CHECK(p.Update(s, 70, false, false, 0).empty());
    s.exposure = 0.55;
    auto a = p.Update(s, 70, false, false, 1);
    CHECK(a.size() == 1 && a[0].title.find("50%") == 0);
    CHECK(p.Update(s, 70, false, false, 2).empty());
    s.exposure = 1.2;  // jumping past 80% straight to 100% announces only the limit
    a = p.Update(s, 70, false, false, 3);
    CHECK(a.size() == 1 && a[0].title == "Daily safe-listening limit reached");
    CHECK(s.alertLevel == 3);
}

TEST(alerts_remind_every_30_minutes_when_over)
{
    AlertPolicy p;
    DayStats s;
    s.exposure = 1.5;
    s.alertLevel = 3;
    CHECK(p.Update(s, 75, false, false, 0).size() == 1);   // first reminder
    CHECK(p.Update(s, 75, false, false, 60).empty());
    CHECK(p.Update(s, 60, false, false, 1900).empty());    // quiet: no nagging
    CHECK(p.Update(s, 75, false, false, 1900).size() == 1);
}

TEST(alerts_warn_after_ten_loud_seconds_naming_spotify)
{
    AlertPolicy p;
    DayStats s;
    std::vector<Alert> a;
    for (int i = 0; i < 9; i++)
        CHECK(p.Update(s, 100, false, true, i).empty());
    a = p.Update(s, 100, false, true, 9);
    CHECK(a.size() == 1 && a[0].title == "Spotify is very loud: 100 dB");
    for (int i = 10; i < 100; i++)
        CHECK(p.Update(s, 100, false, true, i).empty());  // rate limited
    CHECK(p.Update(s, 100, true, false, 700).empty());    // paused resets the streak
}

// --- spotify ---------------------------------------------------------------

TEST(spotify_window_titles)
{
    NowPlaying np = FromSpotifyWindowTitle("Daft Punk - One More Time");
    CHECK(np.running && np.playing && np.artist == "Daft Punk" && np.title == "One More Time");
    CHECK(DescribeTrack(np) == "Daft Punk \xE2\x80\x94 One More Time");

    np = FromSpotifyWindowTitle("Artist - Song - Remastered 2011");
    CHECK(np.artist == "Artist" && np.title == "Song - Remastered 2011");

    for (const char* idle : { "Spotify", "Spotify Free", "Spotify Premium", "" })
    {
        np = FromSpotifyWindowTitle(idle);
        CHECK(np.running && !np.playing);
    }

    np = FromSpotifyWindowTitle("Advertisement");
    CHECK(np.playing && np.artist.empty() && DescribeTrack(np) == "Advertisement");
}

// --- dashboard text ----------------------------------------------------------

TEST(dashboard_lines)
{
    DashboardModel m;
    m.db = 0;
    CHECK(StatusLine(m) == "Nothing is playing right now.");
    m.paused = true;
    CHECK(StatusLine(m).find("paused") != std::string::npos);
    m.paused = false;
    m.deviceOk = false;
    CHECK(StatusLine(m) == "No audio output device found.");
    m.deviceOk = true;
    m.db = 60;
    CHECK(StatusLine(m).find("Comfortable") == 0);
    m.db = 88;  // 4 h allowed, half used -> 2 h left
    m.stats.exposure = 0.5;
    CHECK(StatusLine(m) == "At this level: 2 h 0 min of safe listening left today.");
    m.stats.exposure = 1.0;
    CHECK(StatusLine(m).find("used up") != std::string::npos);

    CHECK(SpotifyLine(m).empty());  // Spotify not running
    m.spotify = FromSpotifyWindowTitle("Spotify Premium");
    CHECK(SpotifyLine(m) == "Spotify \xC2\xB7 paused");
    m.spotify = FromSpotifyWindowTitle("A - B");
    CHECK(SpotifyLine(m) == "Spotify \xC2\xB7 A \xE2\x80\x94 B");
    m.spotifyEnabled = false;
    CHECK(SpotifyLine(m).empty());

    m.spotifyEnabled = true;
    m.stats.listenSeconds = 3900;
    m.stats.peakDb = 91.6;
    m.stats.spotifyExposure = 0.25;
    CHECK(StatsLine(m) == "Listening today: 1 h 5 min    Peak: 92 dB    Spotify: 25%");
}

TEST(demo_model_is_complete)
{
    DashboardModel m = DemoModel();
    CHECK(m.history.size() == HISTORY_LEN);
    CHECK(ModelZone(m) == Zone::Loud);
    CHECK(!SpotifyLine(m).empty());
}

// --- settings --------------------------------------------------------------

TEST(ini_round_trip_and_day_rules)
{
    auto dir = std::filesystem::temp_directory_path() / "audiosentinel-test";
    std::filesystem::remove_all(dir);
    IniFile ini(dir / "nested" / "AudioSentinel.ini");
    CHECK(!ini.Load());  // missing file is fine

    Settings set;
    set.notifications = false;
    set.spotify = false;
    set.loudness = 2;
    set.opacity = 70;
    DayStats st;
    st.date = "2026-03-04";
    st.exposure = 0.4321;
    st.spotifyExposure = 0.2;
    st.peakDb = 91.5;
    st.alertLevel = 1;
    StoreState(ini, set, st);
    CHECK(ini.Save());

    IniFile again(ini.Path());
    CHECK(again.Load());
    Settings set2;
    DayStats st2;
    LoadState(again, "2026-03-04", set2, st2);
    CHECK(!set2.notifications && !set2.spotify && set2.loudness == 2 && set2.opacity == 70);
    CHECK(Near(st2.exposure, 0.4321, 1e-8) && Near(st2.spotifyExposure, 0.2, 1e-8));
    CHECK(Near(st2.peakDb, 91.5, 1e-9) && st2.alertLevel == 1);

    LoadState(again, "2026-03-05", set2, st2);  // another day: counters start at zero
    CHECK(st2.exposure == 0.0 && st2.alertLevel == 0 && st2.date == "2026-03-05");
    std::filesystem::remove_all(dir);
}

TEST(ini_reads_files_written_by_version_1_1)
{
    // v1.1 wrote this with WritePrivateProfileString (CRLF, no Spotify keys).
    auto path = std::filesystem::temp_directory_path() / "audiosentinel-v11.ini";
    {
        std::ofstream out(path, std::ios::binary);
        out << "[Today]\r\nDate=2026-09-27\r\nExposure=0.12500000\r\nAlerts=0\r\n"
               "[Settings]\r\nNotifications=1\r\nOpacity=250\r\nLoudness=-3\r\n";
    }
    IniFile ini(path);
    CHECK(ini.Load());
    Settings set;
    DayStats st;
    LoadState(ini, "2026-09-27", set, st);
    CHECK(Near(st.exposure, 0.125, 1e-9) && st.spotifyExposure == 0.0);
    CHECK(set.spotify);                          // new setting defaults on
    CHECK(set.opacity == 100 && set.loudness == 0);  // out-of-range values clamped
    std::filesystem::remove(path);
}

} // namespace

int main()
{
    for (auto& t : Registry())
    {
        int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
