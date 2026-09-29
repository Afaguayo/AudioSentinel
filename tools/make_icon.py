"""Draws AudioSentinel.ico (a level gauge) with the standard library only.

Run: python3 tools/make_icon.py  ->  writes AudioSentinel/AudioSentinel.ico
                                      and linux/audiosentinel.png
"""
import math
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "AudioSentinel" / "AudioSentinel.ico"
PNG_OUT = ROOT / "linux" / "audiosentinel.png"
SIZES = [16, 24, 32, 48, 64, 256]
SUPERSAMPLE = 4

BG_TOP = (34, 40, 54)
BG_BOTTOM = (16, 19, 26)
STOPS = [(0.0, (61, 220, 132)), (0.55, (255, 176, 32)), (1.0, (255, 90, 95))]
GAUGE_START, GAUGE_SWEEP = 135.0, 270.0  # degrees, clockwise from +x, gap at bottom
NEEDLE_T = 0.62


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def gradient(t):
    for (t0, c0), (t1, c1) in zip(STOPS, STOPS[1:]):
        if t <= t1:
            return lerp(c0, c1, (t - t0) / (t1 - t0))
    return STOPS[-1][1]


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def sample(x, y):
    """Colour (r, g, b, a) at unit coordinates x, y in [0, 1)."""
    # Rounded-square background.
    r, half = 0.22, 0.47
    qx, qy = abs(x - 0.5) - (half - r), abs(y - 0.5) - (half - r)
    outside = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
    if outside > 0:
        return (0, 0, 0, 0)
    color = lerp(BG_TOP, BG_BOTTOM, y)

    cx, cy = 0.5, 0.54
    dx, dy = x - cx, y - cy
    dist = math.hypot(dx, dy)
    angle = (math.degrees(math.atan2(dy, dx)) - GAUGE_START) % 360.0

    # Gauge arc.
    if abs(dist - 0.29) < 0.065 and angle <= GAUGE_SWEEP:
        color = gradient(angle / GAUGE_SWEEP)

    # Needle and hub.
    na = math.radians(GAUGE_START + GAUGE_SWEEP * NEEDLE_T)
    nx, ny = cx + 0.27 * math.cos(na), cy + 0.27 * math.sin(na)
    if seg_dist(x, y, cx, cy, nx, ny) < 0.035 or dist < 0.075:
        color = (240, 243, 248)
    return (*color, 255)


def render(size):
    n = SUPERSAMPLE
    pixels = []
    for py in range(size):
        row = []
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(n):
                for sx in range(n):
                    c = sample((px + (sx + 0.5) / n) / size, (py + (sy + 0.5) / n) / size)
                    a = c[3] / 255.0
                    acc[0] += c[0] * a
                    acc[1] += c[1] * a
                    acc[2] += c[2] * a
                    acc[3] += a
            a = acc[3] / (n * n)
            if a > 0:
                rgb = [int(round(acc[i] / acc[3])) for i in range(3)]
            else:
                rgb = [0, 0, 0]
            row.append((*rgb, int(round(a * 255))))
        pixels.append(row)
    return pixels


def png_bytes(pixels):
    size = len(pixels)
    raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in pixels)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def bmp_bytes(pixels):
    """32-bit BGRA DIB (bottom-up) plus an all-zero AND mask, as ICO expects."""
    size = len(pixels)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b"".join(bytes((p[2], p[1], p[0], p[3])) for row in reversed(pixels) for p in row)
    mask_row = ((size + 31) // 32) * 4
    return header + xor + b"\x00" * (mask_row * size)


def main():
    images = []
    for s in SIZES:
        px = render(s)
        images.append((s, png_bytes(px) if s >= 256 else bmp_bytes(px)))
        if s == 256:
            PNG_OUT.write_bytes(png_bytes(px))
            print(f"wrote {PNG_OUT}")

    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, data in images:
        dim = 0 if s >= 256 else s
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(data for _, data in images)
    OUT.write_bytes(out)
    print(f"wrote {OUT} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
