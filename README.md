# AudioSentinel

A small tray app for **Windows and Linux** that watches how loud your PC's audio is and warns you before you use up your daily safe-listening allowance.

![icon](AudioSentinel/AudioSentinel.ico)

## Install (Windows)

1. Download **`AudioSentinel-Setup-x.y.z.exe`** from the [latest release](https://github.com/Afaguayo/AudioSentinel/releases/latest).
2. Run it. No admin rights are needed; it installs for your user only.
3. Leave "Start AudioSentinel automatically when I sign in" ticked if you want it always on.

Prefer no installer? Download `AudioSentinel-portable.exe` and run it from anywhere.

> Windows SmartScreen may say "Windows protected your PC" because the exe isn't code-signed. Click **More info → Run anyway**.

## Install (Linux)

There's no package yet; build it from source (see [Build on Linux](#build-on-linux)), then run `audiosentinel`. It works with both PulseAudio and PipeWire, since PipeWire provides the same PulseAudio API through `pipewire-pulse`.

**Tray icon:** on KDE, Xfce, Cinnamon and most other desktops it just works. On GNOME, install the *AppIndicator and KStatusNotifierItem Support* extension. Without a tray, the dashboard stays as a normal window, and closing it quits the app.

## Using it

- The **tray icon** shows a dot for the current level (grey: silent, green: safe, amber: loud, red: harmful). A ring around it fills up as you use today's allowance.
- **Left-click** the icon to open or hide the dashboard. It shows the current dB, how long you can keep listening at this level, today's allowance, and a graph of the last 2 minutes.
- **Right-click** the icon, or the dashboard, for options:
  - pause monitoring
  - turn notifications on or off
  - show or hide Spotify
  - calibrate for your headphones/speakers
  - keep on top
  - start with Windows (Linux: start at login)
  - reset today's count
  - exit
- In the dashboard, **↑/↓** changes opacity and **Esc** hides it. Closing the window keeps the app running in the tray.

You get a notification at **50%, 80% and 100%** of the daily allowance, plus a reminder every 30 minutes after that while you keep listening. You're also warned when audio stays above **95 dB** for 10 seconds.

## Spotify

While Spotify is open, the dashboard shows what's playing, for example `Spotify · Daft Punk — One More Time`, or `Spotify · paused`. On Windows, the tray tooltip also names the track.

AudioSentinel also tracks how much of today's exposure happened while Spotify was playing, shown as "Spotify: 66%" in the dashboard. A very-loud warning says "Spotify is very loud" when Spotify was the one playing.

How it finds Spotify:

- **Windows:** reads the Spotify desktop app's window title. The window is titled "Artist - Title" while playing, and "Spotify Premium" or similar while paused. No login or API key is needed.
- **Linux:** reads MPRIS, the standard D-Bus interface Spotify publishes. The `spotifyd` and `ncspot` clients are recognized too.

The loudness measurement is the same for all audio. Spotify's share counts everything playing while Spotify plays, so a video playing at the same moment is counted as Spotify too. You can turn the Spotify row off with **Show Spotify** in the menu.

## How it works

- Captures what the default output device plays, four times a second. Windows uses WASAPI loopback. Linux reads the PulseAudio/PipeWire monitor of the default output. It follows you when you switch devices.
- Estimated level = signal level (dBFS) + Windows master volume + a calibration offset for how loud your output is. The offset is 90, 100 or 110 dB for the *Quiet*, *Typical* and *Loud* settings.
- Daily allowance follows the NIOSH guideline: **85 dB for 8 hours**, halved for every 3 dB more (88 dB → 4 h, 91 dB → 2 h, 94 dB → 1 h…).
- Counters reset at local midnight. They're saved to `%LOCALAPPDATA%\AudioSentinel\AudioSentinel.ini` on Windows and `~/.local/state/audiosentinel/AudioSentinel.ini` on Linux, so a restart doesn't lose them.
- The exposure maths, warnings, settings file and Spotify parsing live in `core/`, shared by both apps and covered by unit tests. The platform code is in `AudioSentinel/` (Windows) and `linux/`.

The dB figure is an estimate. Your PC can't know how efficient your headphones are, so set the calibration to match them.

## Limitations

- **The dB value is an estimate**, not a calibrated meter (see above). It measures what Windows sends to the device, not sound from other sources around you.
- **Apps with exclusive-mode audio** (some games and pro audio tools) bypass the shared mixer and aren't counted while they hold the device.
- **Platforms:** Windows 10/11 x64 and Linux with GTK 3. macOS isn't supported.

## Build on Windows

Requirements: Visual Studio 2022 with the *Desktop development with C++* workload.

```
msbuild AudioSentinel.sln /p:Configuration=Release /p:Platform=x64
```

Or open `AudioSentinel.sln` in Visual Studio and build **Release | x64**. The exe lands in `x64\Release\`. It links the C runtime statically, so it runs on any Windows 10/11 PC without extra installs.

To build the installer, install [Inno Setup 6](https://jrsoftware.org/isinfo.php) and run:

```
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=1.0.0 installer\AudioSentinel.iss
```

The installer is written to `dist\`.

## Build on Linux

Install the dependencies. On Debian/Ubuntu:

```
sudo apt install cmake g++ pkg-config libgtk-3-dev libpulse-dev libnotify-dev libayatana-appindicator3-dev
```

On Fedora: `gtk3-devel pulseaudio-libs-devel libnotify-devel libayatana-appindicator-gtk3-devel`.

Then build, test and install:

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

`cmake --install` adds a menu entry and an icon. The app still builds without `libayatana-appindicator3`, just with no tray icon.

## Tests

- `ctest` runs the unit tests of the shared core, on any OS.
- `tests/linux_integration.sh` checks the Linux app end to end:
  - it plays a 1 kHz tone of known loudness through a virtual PulseAudio output, and checks the measured dB at 100% and at 50% volume, when muted, and in silence
  - it starts a fake Spotify on D-Bus, and checks that the track and the play/pause state are read
  - it runs the GUI under Xvfb
- GitHub Actions runs all of this on every push. It also renders dashboard screenshots on both platforms.

**Releases:** pushing a tag like `v1.0.0` makes GitHub Actions build the exe and installer on Windows and attach both to a GitHub release.

The icon is generated by `python3 tools/make_icon.py`, which needs only the standard library.
