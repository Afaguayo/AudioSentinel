#!/usr/bin/env python3
"""Pretends to be Spotify on the session bus (MPRIS), for the Linux tests.

usage: fake_spotify.py Playing|Paused ARTIST TITLE
"""
import sys

from gi.repository import Gio, GLib

XML = """
<node>
  <interface name="org.mpris.MediaPlayer2.Player">
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
  </interface>
</node>
"""

status, artist, title = sys.argv[1], sys.argv[2], sys.argv[3]


def get_property(conn, sender, path, iface, name):
    if name == "PlaybackStatus":
        return GLib.Variant("s", status)
    if name == "Metadata":
        return GLib.Variant("a{sv}", {
            "xesam:title": GLib.Variant("s", title),
            "xesam:artist": GLib.Variant("as", [artist]),
        })
    return None


def on_bus(conn, name):
    info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
    conn.register_object("/org/mpris/MediaPlayer2", info, None, get_property, None)


Gio.bus_own_name(Gio.BusType.SESSION, "org.mpris.MediaPlayer2.spotify",
                 Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
