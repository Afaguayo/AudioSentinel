#!/usr/bin/env bash
# End-to-end checks for the Linux app. Run inside a D-Bus session:
#   dbus-run-session -- bash tests/linux_integration.sh build/audiosentinel
# Needs: pulseaudio, pulseaudio-utils, python3-gi, xvfb.
set -euo pipefail

BIN=$(realpath "${1:-build/audiosentinel}")
WORK=$(mktemp -d)
FAILED=0
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; pulseaudio --kill 2>/dev/null || true; }
trap cleanup EXIT

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILED=1; }
field() { grep "^$1=" "$2" | cut -d= -f2-; }
# probe SECONDS OUTFILE: run --probe, show its output, never abort the script
probe() { "$BIN" --probe "$1" > "$2" || echo "(probe exit code $?)"; sed 's/^/    /' "$2"; }
# in_range NAME VALUE LOW HIGH
in_range() {
  if python3 -c "import sys; sys.exit(0 if $3 <= $2 <= $4 else 1)"; then pass "$1 = $2 (expected $3..$4)"
  else fail "$1 = $2 (expected $3..$4)"; fi
}

echo "== PulseAudio with a virtual output as the default device"
pulseaudio --start --exit-idle-time=-1 --daemonize=yes
for _ in $(seq 20); do pactl info >/dev/null 2>&1 && break; sleep 0.5; done
pactl load-module module-null-sink sink_name=test_out >/dev/null
pactl set-default-sink test_out
pactl info | grep -E "Server Name|Default Sink"

# 1 kHz sine at half of full scale: RMS 0.3536 = -9.03 dBFS, so with the
# "Typical" +100 dB offset and the volume at 100% the estimate is ~91 dB.
python3 - "$WORK/tone.wav" <<'PY'
import math, struct, sys, wave
rate, secs, amp = 48000, 90, 0.5
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    frame = lambda i: struct.pack("<hh", *(2 * [int(amp * 32767 * math.sin(2 * math.pi * 1000 * i / rate))]))
    w.writeframes(b"".join(frame(i) for i in range(rate * secs)))
PY

echo "== Silence"
probe 3 "$WORK/silence.txt"
[ "$(field device_ok "$WORK/silence.txt")" = 1 ] && pass "capture opens the monitor" || fail "capture did not open"
in_range "silent level_db" "$(field level_db "$WORK/silence.txt")" 0 0

echo "== Tone at 100% volume"
paplay "$WORK/tone.wav" & PIDS+=($!)
sleep 1
probe 4 "$WORK/full.txt"
in_range "rms_dbfs" "$(field rms_dbfs "$WORK/full.txt")" -10.0 -8.0
in_range "level_db" "$(field level_db "$WORK/full.txt")" 90.0 92.0

echo "== Tone at 50% volume (PulseAudio: -18 dB)"
pactl set-sink-volume test_out 50%
sleep 0.5
probe 4 "$WORK/half.txt"
# The volume must count exactly once: ~73 dB. 91 would mean it was ignored,
# 55 that it was counted twice.
in_range "level_db at 50%" "$(field level_db "$WORK/half.txt")" 71.0 75.0

echo "== Muted"
pactl set-sink-mute test_out 1
sleep 0.5
probe 3 "$WORK/muted.txt"
in_range "muted level_db" "$(field level_db "$WORK/muted.txt")" 0 0
pactl set-sink-mute test_out 0
pactl set-sink-volume test_out 100%

echo "== Spotify (fake MPRIS player)"
probe 3 "$WORK/nospotify.txt"
[ "$(field spotify_running "$WORK/nospotify.txt")" = 0 ] && pass "no Spotify detected when absent" || fail "phantom Spotify"

python3 "$(dirname "$0")/fake_spotify.py" Playing "Daft Punk" "One More Time" & SPOT=$!; PIDS+=($SPOT)
sleep 1.5
probe 3 "$WORK/spotify.txt"
[ "$(field spotify_playing "$WORK/spotify.txt")" = 1 ] && pass "Spotify playing detected" || fail "Spotify playing not detected"
[ "$(field spotify_track "$WORK/spotify.txt")" = "Daft Punk — One More Time" ] && pass "track read from MPRIS" || fail "wrong track: $(field spotify_track "$WORK/spotify.txt")"
kill $SPOT

python3 "$(dirname "$0")/fake_spotify.py" Paused "Daft Punk" "One More Time" & SPOT=$!; PIDS+=($SPOT)
sleep 1.5
probe 3 "$WORK/paused.txt"
[ "$(field spotify_running "$WORK/paused.txt")" = 1 ] && [ "$(field spotify_playing "$WORK/paused.txt")" = 0 ] \
  && pass "Spotify paused detected" || fail "Spotify paused state wrong"
kill $SPOT

echo "== GUI smoke test (Xvfb): starts, runs, saves state, quits on SIGTERM"
# Keep XDG_RUNTIME_DIR: PulseAudio's socket lives there.
export XDG_STATE_HOME="$WORK/state" XDG_CONFIG_HOME="$WORK/config"
xvfb-run -a -s "-screen 0 1280x800x24" "$BIN" > "$WORK/gui.log" 2>&1 & GUI=$!; PIDS+=($GUI)
sleep 8
if kill -0 $GUI 2>/dev/null; then pass "GUI still running after 8 s"; else fail "GUI exited early"; cat "$WORK/gui.log"; fi
pkill -TERM -f "^$BIN$" || true
for _ in $(seq 20); do kill -0 $GUI 2>/dev/null || break; sleep 0.25; done
kill -0 $GUI 2>/dev/null && fail "GUI ignored SIGTERM" || pass "GUI quit on SIGTERM"
if grep -q "^Exposure=" "$XDG_STATE_HOME/audiosentinel/AudioSentinel.ini" 2>/dev/null; then
  pass "state saved: $(grep -E '^(Date|Exposure)=' "$XDG_STATE_HOME/audiosentinel/AudioSentinel.ini" | tr '\n' ' ')"
  EXP=$(grep '^Exposure=' "$XDG_STATE_HOME/audiosentinel/AudioSentinel.ini" | cut -d= -f2)
  in_range "exposure counted while the tone played" "$EXP" 0.0000001 0.01
else
  fail "state file not written"
fi
echo "--- GUI log"; cat "$WORK/gui.log" || true

echo "== Dashboard screenshot"
"$BIN" --render-dashboard dashboard-linux.png && pass "rendered dashboard-linux.png" || fail "render failed"

exit $FAILED
