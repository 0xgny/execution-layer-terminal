#!/usr/bin/env python3
"""Generate assets/icon.icns for the macOS app bundle.

Pure stdlib: writes a 2048px RGBA PNG by hand (zlib + struct), then hands it to
sips/iconutil for the downscaled iconset. No Pillow, no design tooling needed --
`scripts/make_dmg.sh` calls this automatically when the .icns is missing.

The mark is a dark rounded tile with three candlesticks, matching the terminal's
own palette (see apply_theme() in cpp/src/terminal_gui.cpp).
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import zlib

S = 2048  # supersampled canvas; sips does the antialiasing on the way down

BG_TOP = (0x16, 0x1B, 0x24)
BG_BOT = (0x0A, 0x0D, 0x12)
EDGE = (0x22, 0xD3, 0xEE)
GREEN = (0x26, 0xD0, 0x7C)
RED = (0xFF, 0x5C, 0x5C)
CYAN = (0x22, 0xD3, 0xEE)


def blend(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


class Canvas:
    def __init__(self, size):
        self.size = size
        self.px = bytearray(size * size * 4)  # transparent

    def put(self, x, y, rgb, alpha=255):
        if not (0 <= x < self.size and 0 <= y < self.size):
            return
        i = (y * self.size + x) * 4
        self.px[i : i + 4] = bytes((*rgb, alpha))

    def rounded_rect(self, x0, y0, x1, y1, radius, color_at):
        """color_at(x, y) -> rgb, so callers can paint gradients."""
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                cx = min(max(x, x0 + radius), x1 - radius)
                cy = min(max(y, y0 + radius), y1 - radius)
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy <= radius * radius:
                    self.put(x, y, color_at(x, y))

    def rect(self, x0, y0, x1, y1, rgb):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                self.put(x, y, rgb)

    def write_png(self, path):
        raw = b"".join(
            b"\x00" + bytes(self.px[y * self.size * 4 : (y + 1) * self.size * 4])
            for y in range(self.size)
        )

        def chunk(tag, data):
            body = tag + data
            return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

        with open(path, "wb") as fh:
            fh.write(b"\x89PNG\r\n\x1a\n")
            fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", self.size, self.size, 8, 6, 0, 0, 0)))
            fh.write(chunk(b"IDAT", zlib.compress(raw, 9)))
            fh.write(chunk(b"IEND", b""))


def draw(c: Canvas):
    m = round(S * 0.085)          # macOS icons sit inset inside their canvas
    x0, y0, x1, y1 = m, m, S - m, S - m
    span = y1 - y0

    def bg(_x, y):
        return blend(BG_TOP, BG_BOT, (y - y0) / span)

    c.rounded_rect(x0, y0, x1, y1, span * 0.225, bg)

    # Candlesticks: (center_x_frac, wick_top, wick_bot, body_top, body_bot, color)
    candles = [
        (0.28, 0.60, 0.86, 0.66, 0.82, RED),
        (0.50, 0.34, 0.74, 0.42, 0.68, GREEN),
        (0.72, 0.16, 0.56, 0.22, 0.48, GREEN),
    ]
    body_w = span * 0.115
    wick_w = span * 0.028
    for fx, wt, wb, bt, bb, col in candles:
        cx = x0 + span * fx
        c.rect(cx - wick_w / 2, y0 + span * wt, cx + wick_w / 2, y0 + span * wb, col)
        c.rect(cx - body_w / 2, y0 + span * bt, cx + body_w / 2, y0 + span * bb, col)

    # Baseline rule + a tick of accent in the corner, so it reads at 32px too.
    c.rect(x0 + span * 0.14, y0 + span * 0.895, x1 - span * 0.14, y0 + span * 0.912, CYAN)
    c.rect(x0 + span * 0.14, y0 + span * 0.10, x0 + span * 0.30, y0 + span * 0.117, EDGE)


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/icon.icns"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    canvas = Canvas(S)
    draw(canvas)

    with tempfile.TemporaryDirectory() as tmp:
        master = os.path.join(tmp, "master.png")
        canvas.write_png(master)

        iconset = os.path.join(tmp, "icon.iconset")
        os.makedirs(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale, suffix in ((1, ""), (2, "@2x")):
                px = size * scale
                dest = os.path.join(iconset, f"icon_{size}x{size}{suffix}.png")
                subprocess.run(
                    ["sips", "-z", str(px), str(px), master, "--out", dest],
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)

    print(f"[make_icon] wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
