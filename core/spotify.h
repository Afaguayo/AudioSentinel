// What Spotify is doing, as reported by each platform.
#pragma once

#include <string>

namespace as
{

struct NowPlaying
{
    bool running = false;
    bool playing = false;
    std::string artist;  // UTF-8
    std::string title;   // UTF-8
};

// Windows: Spotify's main window is titled "Artist - Title" while playing and
// "Spotify", "Spotify Free" or "Spotify Premium" while paused or idle.
NowPlaying FromSpotifyWindowTitle(const std::string& title);

// "Artist — Title", "Title", or "" when nothing is known.
std::string DescribeTrack(const NowPlaying& np);

} // namespace as
