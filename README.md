# AudioSentinel

A lightweight Windows tray app that keeps an eye on how loud the audio your PC is playing is, and how much of a day's safe-listening allowance you've used. It's written in C++ against the raw Win32 and Windows audio (WASAPI) APIs, with no frameworks.

## Use it

Download `AudioSentinel.exe` from [Releases](../../releases/latest), or build it yourself (below), and run it. An icon appears in the system tray:

- **Hover** to see the current level, e.g. `dB: 72`.
- **Left-click** to open the dashboard: the live level, the % of today's allowance used, and a scrolling graph that's green below 80 dB, yellow from 80 to 90 dB, and red above 90 dB. **Up/Down arrow** keys change the dashboard's transparency.
- **Right-click** to quit. Your running exposure is saved to `exposure.dat` next to the app and picked up again on the next launch.

## How it works

1. **Capture:** a background thread opens the default playback device in WASAPI **loopback** mode, so it hears exactly what your speakers or headphones are being sent. It doesn't use a microphone.
2. **Level:** for each buffer it computes the RMS of the samples, converts that to decibels (`20·log10(rms)`), adds a +100 offset to map it onto a familiar SPL-like scale, and smooths the result (exponential moving average, α = 0.2).
3. **Exposure:** it uses the NIOSH rule that 85 dB is safe for 8 hours, and every 3 dB louder halves the safe time:

   ```text
   safe hours = 8 × 2^((85 − dB) / 3)
   ```

   Every buffer adds `elapsed time ÷ safe time` to your exposure, so 100% means a full day's allowance.

## Limitations

- **The dB value is an estimate.** It's based on the digital signal, so it doesn't know your volume knob, your headphones' sensitivity or your amplifier. Treat it as a relative gauge, not a calibrated meter.
- **No pop-up alert yet.** It shows your level and allowance, but doesn't notify you when you pass 100%.
- **The allowance doesn't reset daily.** `exposure.dat` keeps adding up until you delete it.

## Build

Open `AudioSentinel.sln` in **Visual Studio 2022** (Desktop development with C++) and build **Release | x64**, or from a Developer Command Prompt:

```bat
msbuild AudioSentinel.sln /p:Configuration=Release /p:Platform=x64
```

The app is `x64\Release\AudioSentinel.exe`. GitHub Actions builds it on every push and attaches it to a release when a `v*` tag is pushed.
