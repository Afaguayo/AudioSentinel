#include "spotify.h"

namespace as
{

NowPlaying FromSpotifyWindowTitle(const std::string& title)
{
    NowPlaying np;
    np.running = true;
    size_t sep = title.find(" - ");
    bool idle = title.empty() || (sep == std::string::npos && title.rfind("Spotify", 0) == 0);
    if (idle)
        return np;

    np.playing = true;
    if (sep == std::string::npos)
    {
        np.title = title;  // ads and some podcasts have no artist part
    }
    else
    {
        np.artist = title.substr(0, sep);
        np.title = title.substr(sep + 3);
    }
    return np;
}

std::string DescribeTrack(const NowPlaying& np)
{
    if (np.artist.empty())
        return np.title;
    if (np.title.empty())
        return np.artist;
    return np.artist + " \xE2\x80\x94 " + np.title;  // em dash
}

} // namespace as
