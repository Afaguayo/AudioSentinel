// AudioSentinel for Linux: GTK 3 dashboard, tray icon (AppIndicator),
// desktop notifications, PulseAudio/PipeWire capture and Spotify via MPRIS.
//
//   audiosentinel                      start (shows the dashboard)
//   audiosentinel --startup            start in the tray (used at login)
//   audiosentinel --probe SECONDS      measure without a GUI and print the results
//   audiosentinel --render-dashboard FILE.png   draw the dashboard with sample data

#include "../core/dashboard.h"
#include "../core/exposure.h"
#include "../core/settings.h"
#include "draw.h"
#include "mpris.h"
#include "pulse_audio.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libnotify/notify.h>
#ifdef HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifndef AS_VERSION
#define AS_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace
{

constexpr const char* APP_ID = "io.github.afaguayo.AudioSentinel";

fs::path XdgDir(const char* env, const char* fallback)
{
    const char* v = std::getenv(env);
    if (v && *v)
        return v;
    return fs::path(g_get_home_dir()) / fallback;
}

fs::path StateFile() { return XdgDir("XDG_STATE_HOME", ".local/state") / "audiosentinel" / "AudioSentinel.ini"; }
fs::path AutostartFile() { return XdgDir("XDG_CONFIG_HOME", ".config") / "autostart" / "audiosentinel.desktop"; }

fs::path RuntimeDir()
{
    const char* v = std::getenv("XDG_RUNTIME_DIR");
    fs::path dir = (v && *v) ? fs::path(v) / "audiosentinel"
                             : fs::path(g_get_tmp_dir()) / ("audiosentinel-" + std::to_string(getuid()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

double MonotonicSeconds() { return g_get_monotonic_time() / 1e6; }

// ---------------------------------------------------------------------------
// Monitor: the measuring engine, shared by the GUI and --probe
// ---------------------------------------------------------------------------

class Monitor
{
public:
    ~Monitor() { Stop(); }

    void Start()
    {
        volume.Start();
        startedAt_ = MonotonicSeconds();
        running_ = true;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop()
    {
        running_ = false;
        if (thread_.joinable())
            thread_.join();
        volume.Stop();
    }

    std::mutex lock;  // guards tracker and history
    as::Tracker tracker;
    std::deque<float> history;

    std::atomic<double> db{ 0.0 };
    std::atomic<double> rms{ 0.0 };  // last raw reading, for --probe
    std::atomic<double> firstReadAt{ -1.0 };  // seconds from Start() to the first reading
    std::atomic<bool> paused{ false };
    std::atomic<bool> deviceOk{ false };
    std::atomic<bool> spotifyPlaying{ false };
    std::atomic<int> loudness{ 1 };
    SinkVolume volume;

    std::string LastError()
    {
        std::lock_guard<std::mutex> lk(errorLock_);
        return error_;
    }

private:
    void Run()
    {
        MonitorCapture capture;
        as::Smoother smoother;
        auto last = std::chrono::steady_clock::now();
        while (running_)
        {
            if (!capture.IsOpen() && !capture.Open())
            {
                SetError(capture.LastError());
                deviceOk = false;
                db = 0.0;
                // Retry quickly at first, then every 2 s.
                int waits = failures_++ < 5 ? 2 : 20;
                for (int i = 0; i < waits && running_; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                last = std::chrono::steady_clock::now();
                continue;
            }
            failures_ = 0;

            double reading = 0.0;
            bool ok = capture.ReadRms(1.0 / as::TICKS_PER_SECOND, reading);
            auto now = std::chrono::steady_clock::now();
            // Cap dt so a resume from suspend doesn't count the whole gap.
            double dt = std::min(std::chrono::duration<double>(now - last).count(), 1.0);
            last = now;
            if (!ok)
            {
                SetError(capture.LastError());
                capture.Close();
                deviceOk = false;
                continue;
            }
            deviceOk = true;
            rms = reading;
            if (firstReadAt < 0)
                firstReadAt = MonotonicSeconds() - startedAt_;

            double level = paused ? 0.0
                                  : as::EstimateLevel(reading, volume.EffectiveVolumeDb(),
                                                      volume.Muted(), loudness);
            double shown = smoother.Step(level);
            db = shown;

            std::lock_guard<std::mutex> lk(lock);
            history.push_back((float)shown);
            while (history.size() > as::HISTORY_LEN)
                history.pop_front();
            tracker.Tick(level, shown, dt, spotifyPlaying);
        }
    }

    void SetError(const std::string& e)
    {
        std::lock_guard<std::mutex> lk(errorLock_);
        error_ = e;
    }

    std::thread thread_;
    std::atomic<bool> running_{ false };
    int failures_ = 0;
    double startedAt_ = 0.0;
    std::mutex errorLock_;
    std::string error_;
};

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------

struct Gui
{
    explicit Gui(fs::path iniPath) : ini(std::move(iniPath)) {}

    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* area = nullptr;
    GtkWidget* menu = nullptr;
    GtkWidget* itemOpen = nullptr;
    GtkWidget* itemPause = nullptr;
    GtkWidget* itemNotify = nullptr;
    GtkWidget* itemSpotify = nullptr;
    GtkWidget* itemTop = nullptr;
    GtkWidget* itemAutostart = nullptr;
    GtkWidget* itemLoud[as::LOUDNESS_COUNT] = {};
    bool syncing = false;  // true while menu checks are set from code
#ifdef HAVE_APPINDICATOR
    AppIndicator* indicator = nullptr;
#endif
    bool hasTray = false;
    bool startHidden = false;
    bool activated = false;
    guint redrawTimer = 0;
    int iconKey = -1;
    fs::path iconDir;

    as::IniFile ini;
    as::Settings settings;
    as::AlertPolicy alerts;
    as::NowPlaying nowPlaying;
    SpotifyWatcher spotify;
    Monitor monitor;
    int ticks = 0;
};

Gui* g = nullptr;

void SaveState()
{
    {
        std::lock_guard<std::mutex> lk(g->monitor.lock);
        as::StoreState(g->ini, g->settings, g->monitor.tracker.stats);
    }
    g->ini.Save();
}

as::DashboardModel BuildModel()
{
    as::DashboardModel m;
    {
        std::lock_guard<std::mutex> lk(g->monitor.lock);
        m.stats = g->monitor.tracker.stats;
        m.history.assign(g->monitor.history.begin(), g->monitor.history.end());
    }
    m.db = g->monitor.db;
    m.paused = g->monitor.paused;
    m.deviceOk = g->monitor.deviceOk;
    m.spotifyEnabled = g->settings.spotify;
    m.spotify = g->nowPlaying;
    return m;
}

bool DashboardVisible() { return g->window && gtk_widget_get_visible(g->window); }

void Notify(const as::Alert& alert)
{
    if (!g->settings.notifications)
        return;
    NotifyNotification* n = notify_notification_new(alert.title.c_str(), alert.text.c_str(), "dialog-warning");
    notify_notification_set_app_name(n, "AudioSentinel");
    notify_notification_show(n, nullptr);
    g_object_unref(n);
}

void UpdateTrayIcon(as::Zone zone, double fraction)
{
#ifdef HAVE_APPINDICATOR
    int key = (int)zone * 1000 + (int)(std::min(fraction, 1.0) * 24.0);
    if (!g->indicator || key == g->iconKey)
        return;
    // A new file name per state: indicator hosts cache icons by path.
    fs::path icon = g->iconDir / ("tray-" + std::to_string(key) + ".png");
    if (!fs::exists(icon))
        RenderTrayIconPng(icon.string(), 64, zone, fraction);
    app_indicator_set_icon_full(g->indicator, icon.c_str(), "AudioSentinel");
    g->iconKey = key;
#else
    (void)zone;
    (void)fraction;
#endif
}

void SyncMenu()
{
    g->syncing = true;
    gtk_menu_item_set_label(GTK_MENU_ITEM(g->itemOpen), DashboardVisible() ? "Hide dashboard" : "Open dashboard");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g->itemPause), g->monitor.paused);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g->itemNotify), g->settings.notifications);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g->itemSpotify), g->settings.spotify);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g->itemTop), g->settings.topmost);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g->itemAutostart), fs::exists(AutostartFile()));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g->itemLoud[g->settings.loudness]), TRUE);
    g->syncing = false;
}

gboolean OnTick(gpointer)
{
    if (g->settings.spotify)
        g->nowPlaying = g->spotify.Read();
    else
        g->nowPlaying = as::NowPlaying{};
    g->monitor.spotifyPlaying = g->nowPlaying.playing;

    double db = g->monitor.db;
    std::vector<as::Alert> alerts;
    double fraction;
    {
        std::lock_guard<std::mutex> lk(g->monitor.lock);
        g->monitor.tracker.Rollover(as::Today());
        alerts = g->alerts.Update(g->monitor.tracker.stats, db, g->monitor.paused,
                                  g->nowPlaying.playing, MonotonicSeconds());
        fraction = g->monitor.tracker.stats.exposure;
    }
    for (const auto& a : alerts)
        Notify(a);

    UpdateTrayIcon(as::ZoneFor(db, g->monitor.paused, g->monitor.deviceOk), fraction);
    SyncMenu();

    if (!alerts.empty() || ++g->ticks % 30 == 0)
        SaveState();
    return G_SOURCE_CONTINUE;
}

gboolean OnRedraw(gpointer)
{
    if (g->area)
        gtk_widget_queue_draw(g->area);
    return G_SOURCE_CONTINUE;
}

void ApplyWindowSettings()
{
    if (!g->window)
        return;
    gtk_widget_set_opacity(g->window, g->settings.opacity / 100.0);
    gtk_window_set_keep_above(GTK_WINDOW(g->window), g->settings.topmost);
}

void HideDashboard()
{
    if (!g->window)
        return;
    gtk_widget_hide(g->window);
    if (g->redrawTimer)
    {
        g_source_remove(g->redrawTimer);
        g->redrawTimer = 0;
    }
    SyncMenu();
}

gboolean OnDraw(GtkWidget* widget, cairo_t* cr, gpointer)
{
    DrawDashboard(cr, gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget), BuildModel());
    return TRUE;
}

gboolean OnKey(GtkWidget*, GdkEventKey* event, gpointer)
{
    if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_Down)
    {
        g->settings.opacity = std::clamp(g->settings.opacity + (event->keyval == GDK_KEY_Up ? 5 : -5), 30, 100);
        ApplyWindowSettings();
        SaveState();
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Escape)
    {
        if (g->hasTray)
            HideDashboard();
        else
            gtk_window_iconify(GTK_WINDOW(g->window));
        return TRUE;
    }
    return FALSE;
}

gboolean OnButton(GtkWidget*, GdkEventButton* event, gpointer)
{
    if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_SECONDARY)
    {
        SyncMenu();
        gtk_menu_popup_at_pointer(GTK_MENU(g->menu), (GdkEvent*)event);
        return TRUE;
    }
    return FALSE;
}

gboolean OnDelete(GtkWidget*, GdkEvent*, gpointer)
{
    if (g->hasTray)
    {
        HideDashboard();  // keep running in the tray
        return TRUE;
    }
    SaveState();
    g_application_quit(G_APPLICATION(g->app));  // no tray: closing the window quits
    return TRUE;
}

void CreateDashboard()
{
    g->window = gtk_application_window_new(g->app);
    gtk_window_set_title(GTK_WINDOW(g->window), "AudioSentinel");
    gtk_window_set_icon_name(GTK_WINDOW(g->window), "audiosentinel");
    gtk_window_set_default_size(GTK_WINDOW(g->window), DASHBOARD_WIDTH, DASHBOARD_HEIGHT);
    gtk_window_set_resizable(GTK_WINDOW(g->window), FALSE);

    g->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(g->area, DASHBOARD_WIDTH, DASHBOARD_HEIGHT);
    gtk_widget_add_events(g->area, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(g->window), g->area);

    g_signal_connect(g->area, "draw", G_CALLBACK(OnDraw), nullptr);
    g_signal_connect(g->area, "button-press-event", G_CALLBACK(OnButton), nullptr);
    g_signal_connect(g->window, "key-press-event", G_CALLBACK(OnKey), nullptr);
    g_signal_connect(g->window, "delete-event", G_CALLBACK(OnDelete), nullptr);
    ApplyWindowSettings();
}

void ShowDashboard()
{
    if (!g->window)
        CreateDashboard();
    gtk_widget_show_all(g->window);
    gtk_window_present(GTK_WINDOW(g->window));
    if (!g->redrawTimer)
        g->redrawTimer = g_timeout_add(1000 / as::TICKS_PER_SECOND, OnRedraw, nullptr);
    SyncMenu();
}

void SetAutostart(bool enable)
{
    fs::path file = AutostartFile();
    std::error_code ec;
    if (!enable)
    {
        fs::remove(file, ec);
        return;
    }
    fs::create_directories(file.parent_path(), ec);
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    std::ofstream out(file);
    out << "[Desktop Entry]\nType=Application\nName=AudioSentinel\n"
        << "Comment=Safe-listening monitor\nIcon=audiosentinel\n"
        << "Exec=\"" << (ec ? std::string("audiosentinel") : exe.string()) << "\" --startup\n"
        << "X-GNOME-Autostart-enabled=true\n";
}

// --- menu callbacks ---

void OnOpen(GtkMenuItem*, gpointer)
{
    if (DashboardVisible())
        HideDashboard();
    else
        ShowDashboard();
}

void OnToggle(GtkCheckMenuItem* item, gpointer)
{
    if (g->syncing)
        return;
    bool on = gtk_check_menu_item_get_active(item);
    GtkWidget* w = GTK_WIDGET(item);
    if (w == g->itemPause)
        g->monitor.paused = on;
    else if (w == g->itemNotify)
        g->settings.notifications = on;
    else if (w == g->itemSpotify)
        g->settings.spotify = on;
    else if (w == g->itemTop)
    {
        g->settings.topmost = on;
        ApplyWindowSettings();
    }
    else if (w == g->itemAutostart)
        SetAutostart(on);
    SaveState();
}

void OnLoudness(GtkCheckMenuItem* item, gpointer data)
{
    if (g->syncing || !gtk_check_menu_item_get_active(item))
        return;
    g->settings.loudness = GPOINTER_TO_INT(data);
    g->monitor.loudness = g->settings.loudness;
    SaveState();
}

void OnReset(GtkMenuItem*, gpointer)
{
    GtkWidget* dialog = gtk_message_dialog_new(
        DashboardVisible() ? GTK_WINDOW(g->window) : nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO, "Reset today's exposure, listening time and peak level to zero?");
    gtk_window_set_title(GTK_WINDOW(dialog), "AudioSentinel");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
    {
        {
            std::lock_guard<std::mutex> lk(g->monitor.lock);
            g->monitor.tracker.Reset();
        }
        SaveState();
    }
    gtk_widget_destroy(dialog);
}

void OnAbout(GtkMenuItem*, gpointer)
{
    gtk_show_about_dialog(
        DashboardVisible() ? GTK_WINDOW(g->window) : nullptr, "program-name", "AudioSentinel",
        "version", AS_VERSION, "logo-icon-name", "audiosentinel", "website",
        "https://github.com/Afaguayo/AudioSentinel", "comments",
        "Estimates how loud your audio is and warns before you use up the daily safe-listening "
        "allowance (NIOSH: 85 dB for 8 hours, halved for every 3 dB louder). While Spotify plays, "
        "it shows the track and how much of today's exposure came from it.",
        nullptr);
}

void OnQuit(GtkMenuItem*, gpointer)
{
    SaveState();
    g_application_quit(G_APPLICATION(g->app));
}

GtkWidget* AddItem(const char* label, GCallback cb)
{
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", cb, nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->menu), item);
    return item;
}

GtkWidget* AddCheck(const char* label)
{
    GtkWidget* item = gtk_check_menu_item_new_with_label(label);
    g_signal_connect(item, "toggled", G_CALLBACK(OnToggle), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->menu), item);
    return item;
}

void AddSeparator() { gtk_menu_shell_append(GTK_MENU_SHELL(g->menu), gtk_separator_menu_item_new()); }

void BuildMenu()
{
    g->menu = gtk_menu_new();
    g->itemOpen = AddItem("Open dashboard", G_CALLBACK(OnOpen));
    AddSeparator();
    g->itemPause = AddCheck("Pause monitoring");
    g->itemNotify = AddCheck("Warning notifications");
    g->itemSpotify = AddCheck("Show Spotify");

    GtkWidget* loudMenu = gtk_menu_new();
    const char* loudLabels[] = { "Quiet (earbuds, low-sensitivity speakers)", "Typical",
                                 "Loud (sensitive headphones, big speakers)" };
    GSList* group = nullptr;
    for (int i = 0; i < as::LOUDNESS_COUNT; i++)
    {
        g->itemLoud[i] = gtk_radio_menu_item_new_with_label(group, loudLabels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(g->itemLoud[i]));
        g_signal_connect(g->itemLoud[i], "toggled", G_CALLBACK(OnLoudness), GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(loudMenu), g->itemLoud[i]);
    }
    GtkWidget* loud = gtk_menu_item_new_with_label("My headphones/speakers are");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(loud), loudMenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->menu), loud);

    g->itemTop = AddCheck("Keep dashboard on top");
    g->itemAutostart = AddCheck("Start at login");
    AddSeparator();
    AddItem("Reset today's exposure...", G_CALLBACK(OnReset));
    AddItem("About AudioSentinel", G_CALLBACK(OnAbout));
    AddSeparator();
    AddItem("Quit", G_CALLBACK(OnQuit));
    gtk_widget_show_all(g->menu);
    SyncMenu();
}

void CreateTray()
{
#ifdef HAVE_APPINDICATOR
    fs::path icon = g->iconDir / "tray-start.png";
    RenderTrayIconPng(icon.string(), 64, as::Zone::Silent, 0.0);
    g->indicator = app_indicator_new(APP_ID, icon.c_str(), APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_title(g->indicator, "AudioSentinel");
    app_indicator_set_status(g->indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(g->indicator, GTK_MENU(g->menu));
    app_indicator_set_secondary_activate_target(g->indicator, g->itemOpen);  // middle click
    g->hasTray = true;
#endif
}

gboolean OnSignal(gpointer)
{
    SaveState();
    g_application_quit(G_APPLICATION(g->app));
    return G_SOURCE_REMOVE;
}

void OnStartup(GApplication* app, gpointer)
{
    g_application_hold(app);  // keep running while only the tray icon is shown
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
    notify_init("AudioSentinel");

    g->ini.Load();
    {
        std::lock_guard<std::mutex> lk(g->monitor.lock);
        as::LoadState(g->ini, as::Today(), g->settings, g->monitor.tracker.stats);
    }
    g->monitor.loudness = g->settings.loudness;
    g->iconDir = RuntimeDir();

    BuildMenu();
    CreateTray();
    g->spotify.Start();
    g->monitor.Start();

    g_timeout_add_seconds(1, OnTick, nullptr);
    g_unix_signal_add(SIGTERM, OnSignal, nullptr);
    g_unix_signal_add(SIGINT, OnSignal, nullptr);
    g_unix_signal_add(SIGHUP, OnSignal, nullptr);
}

void OnActivate(GApplication*, gpointer)
{
    // The first activation honours --startup; any later launch opens the dashboard.
    bool first = !g->activated;
    g->activated = true;
    if (first && g->startHidden && g->hasTray)
        return;
    ShowDashboard();
}

void OnShutdown(GApplication*, gpointer)
{
    g->monitor.Stop();
    SaveState();
    notify_uninit();
}

// ---------------------------------------------------------------------------
// Headless modes
// ---------------------------------------------------------------------------

int RunProbe(double seconds, int loudness)
{
    Monitor monitor;
    monitor.loudness = loudness;
    SpotifyWatcher spotify;
    spotify.Start();
    monitor.Start();

    // Average the power over the window, skipping the first second while
    // the stream and the volume query settle.
    double start = MonotonicSeconds(), nextSample = start + 1.0;
    double powerSum = 0.0;
    int samples = 0;
    as::NowPlaying np;
    while (MonotonicSeconds() - start < seconds)
    {
        while (g_main_context_iteration(nullptr, FALSE))
        {
        }
        np = spotify.Read();
        monitor.spotifyPlaying = np.playing;
        if (MonotonicSeconds() >= nextSample)
        {
            double r = monitor.rms;
            powerSum += r * r;
            samples++;
            nextSample += 1.0 / as::TICKS_PER_SECOND;
        }
        g_usleep(20 * 1000);
    }

    double meanRms = samples ? std::sqrt(powerSum / samples) : 0.0;
    double exposure;
    {
        std::lock_guard<std::mutex> lk(monitor.lock);
        exposure = monitor.tracker.stats.exposure;
    }
    bool ok = monitor.deviceOk;
    std::printf("device_ok=%d\n", ok ? 1 : 0);
    std::printf("capture_error=%s\n", monitor.LastError().c_str());
    std::printf("first_read_s=%.2f\n", monitor.firstReadAt.load());
    std::printf("server=%s\n", monitor.volume.ServerName().c_str());
    std::printf("sink=%s\n", monitor.volume.SinkName().c_str());
    std::printf("volume_db=%.2f\n", monitor.volume.EffectiveVolumeDb());
    std::printf("muted=%d\n", monitor.volume.Muted() ? 1 : 0);
    std::printf("rms_dbfs=%.2f\n", meanRms > 0 ? 20.0 * std::log10(meanRms) : -200.0);
    std::printf("level_db=%.2f\n", as::EstimateLevel(meanRms, monitor.volume.EffectiveVolumeDb(),
                                                     monitor.volume.Muted(), loudness));
    std::printf("exposure=%.8f\n", exposure);
    std::printf("spotify_running=%d\n", np.running ? 1 : 0);
    std::printf("spotify_playing=%d\n", np.playing ? 1 : 0);
    std::printf("spotify_track=%s\n", as::DescribeTrack(np).c_str());
    monitor.Stop();
    return ok ? 0 : 2;
}

void Usage()
{
    std::printf("usage: audiosentinel [--startup] [--probe SECONDS] [--render-dashboard FILE.png] [--version]\n");
}

} // namespace

int main(int argc, char** argv)
{
    bool startup = false;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--startup")
            startup = true;
        else if (arg == "--version")
        {
            std::printf("AudioSentinel %s\n", AS_VERSION);
            return 0;
        }
        else if (arg == "--probe" && i + 1 < argc)
            return RunProbe(std::atof(argv[i + 1]), 1);
        else if (arg == "--render-dashboard" && i + 1 < argc)
            return RenderDashboardPng(argv[i + 1], as::DemoModel(), 2.0) ? 0 : 1;
        else
        {
            Usage();
            return arg == "--help" || arg == "-h" ? 0 : 64;
        }
    }

    auto gui = std::make_unique<Gui>(StateFile());
    g = gui.get();
    g->startHidden = startup;
    g->app = gtk_application_new(APP_ID, G_APPLICATION_FLAGS_NONE);
    g_signal_connect(g->app, "startup", G_CALLBACK(OnStartup), nullptr);
    g_signal_connect(g->app, "activate", G_CALLBACK(OnActivate), nullptr);
    g_signal_connect(g->app, "shutdown", G_CALLBACK(OnShutdown), nullptr);

    // Options are handled above; GApplication only needs the program name.
    int status = g_application_run(G_APPLICATION(g->app), 1, argv);
    g_object_unref(g->app);
    g = nullptr;
    return status;
}
