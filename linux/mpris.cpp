#include "mpris.h"

#include <string>

namespace
{

const char* const BUS_NAMES[] = {
    "org.mpris.MediaPlayer2.spotify",
    "org.mpris.MediaPlayer2.spotifyd",
    "org.mpris.MediaPlayer2.ncspot",
};

std::string StringProperty(GVariant* dict, const char* key)
{
    std::string result;
    if (GVariant* v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING))
    {
        result = g_variant_get_string(v, nullptr);
        g_variant_unref(v);
    }
    return result;
}

// xesam:artist is a list of names; join them with ", ".
std::string Artists(GVariant* dict)
{
    std::string result;
    if (GVariant* v = g_variant_lookup_value(dict, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY))
    {
        gsize n = 0;
        const gchar** names = g_variant_get_strv(v, &n);
        for (gsize i = 0; i < n; i++)
        {
            if (!result.empty())
                result += ", ";
            result += names[i];
        }
        g_free(names);
        g_variant_unref(v);
    }
    return result;
}

} // namespace

SpotifyWatcher::~SpotifyWatcher()
{
    for (GDBusProxy* p : proxies_)
        g_object_unref(p);
}

void SpotifyWatcher::Start()
{
    if (!proxies_.empty())
        return;
    for (const char* name : BUS_NAMES)
    {
        // The proxy follows the name: it loads properties when the player
        // starts and clears them when it quits, so Spotify can come and go.
        GError* error = nullptr;
        GDBusProxy* proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, nullptr, name,
            "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", nullptr, &error);
        if (proxy)
            proxies_.push_back(proxy);
        if (error)
            g_error_free(error);
    }
}

as::NowPlaying SpotifyWatcher::Read() const
{
    as::NowPlaying np;
    for (GDBusProxy* proxy : proxies_)
    {
        gchar* owner = g_dbus_proxy_get_name_owner(proxy);
        if (!owner)
            continue;
        g_free(owner);

        np.running = true;
        if (GVariant* status = g_dbus_proxy_get_cached_property(proxy, "PlaybackStatus"))
        {
            if (g_variant_is_of_type(status, G_VARIANT_TYPE_STRING))
                np.playing = std::string(g_variant_get_string(status, nullptr)) == "Playing";
            g_variant_unref(status);
        }
        if (GVariant* meta = g_dbus_proxy_get_cached_property(proxy, "Metadata"))
        {
            if (g_variant_is_of_type(meta, G_VARIANT_TYPE_VARDICT))
            {
                np.title = StringProperty(meta, "xesam:title");
                np.artist = Artists(meta);
            }
            g_variant_unref(meta);
        }
        return np;
    }
    return np;
}
