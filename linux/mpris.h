// Spotify on Linux: reads the MPRIS D-Bus interface that Spotify (and the
// spotifyd / ncspot clients) publish on the session bus.
#pragma once

#include "../core/spotify.h"

#include <gio/gio.h>

#include <vector>

class SpotifyWatcher
{
public:
    SpotifyWatcher() = default;
    SpotifyWatcher(const SpotifyWatcher&) = delete;
    SpotifyWatcher& operator=(const SpotifyWatcher&) = delete;
    ~SpotifyWatcher();

    // Needs a running GLib main loop to pick up changes. Safe to call when
    // there is no session bus; Spotify then just never shows up.
    void Start();

    // Current state from the cached D-Bus properties. Main thread only.
    as::NowPlaying Read() const;

private:
    std::vector<GDBusProxy*> proxies_;
};
