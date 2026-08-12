#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate packaging/tradingapp.png — the app icon the AppImage needs.

Pure standard library (zlib + struct), so it needs no image tooling on any
platform: the icon is drawn here rather than committed as an opaque blob, which
keeps its provenance reviewable. The PNG next to this file is its output and is
committed, because linuxdeploy and the .desktop entry want a real file.

Drawing: a dark rounded square with a rising three-segment line and an arrow
head, in the app's own signal green (#25b563, the same value as
src/ui/Palette.h's kGreen). Rendered at 4x and box-filtered down, which is all
the anti-aliasing a flat-shape icon needs.

Usage: packaging/make_icon.py [size]     (default 256)
"""

import struct
import sys
import zlib
from pathlib import Path

SS = 4  # supersampling factor
BG = (0x14, 0x18, 0x1F)      # near-black slate, matches the app's dark chrome
GREEN = (0x25, 0xB5, 0x63)   # trading::ui::kGreen
GREY = (0x9A, 0x9A, 0x9A)    # trading::ui::kGrey — the baseline


def _rounded_square(x: float, y: float, size: float, radius: float) -> bool:
    """Inside test for a rounded square covering [0,size)^2."""
    cx = min(max(x, radius), size - radius)
    cy = min(max(y, radius), size - radius)
    return ((x - cx) ** 2 + (y - cy) ** 2) <= (radius ** 2) + 1e-9


def _near_segment(x: float, y: float, a: tuple, b: tuple, width: float) -> bool:
    """Distance from (x, y) to segment a-b within width/2 (a capsule)."""
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    length2 = (dx * dx) + (dy * dy)
    t = 0.0 if length2 == 0 else ((x - ax) * dx + (y - ay) * dy) / length2
    t = min(max(t, 0.0), 1.0)
    px, py = ax + (t * dx), ay + (t * dy)
    return (((x - px) ** 2) + ((y - py) ** 2)) <= ((width / 2.0) ** 2)


def _in_triangle(x: float, y: float, p1: tuple, p2: tuple, p3: tuple) -> bool:
    def side(ax, ay, bx, by):
        return ((bx - ax) * (y - ay)) - ((by - ay) * (x - ax))
    d1 = side(*p1, *p2)
    d2 = side(*p2, *p3)
    d3 = side(*p3, *p1)
    return (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0)


def render(size: int) -> bytes:
    """RGBA rows for the icon at `size` pixels, box-filtered from SSx."""
    big = size * SS
    unit = big / 256.0  # geometry below is expressed in 256-px units

    # A rising line: down-left to up-right, with one dip so it reads as a chart.
    line = [(46 * unit, 176 * unit), (98 * unit, 128 * unit),
            (140 * unit, 158 * unit), (190 * unit, 84 * unit)]
    stroke = 17 * unit
    # Arrow head on the last segment's own direction (0.56, -0.83 normalized),
    # so the tip continues the line instead of sitting beside it.
    head = [(212 * unit, 52 * unit), (208 * unit, 88 * unit), (176 * unit, 72 * unit)]
    baseline = [(40 * unit, 208 * unit), (216 * unit, 208 * unit)]

    radius = 52 * unit
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = (px * SS) + sx + 0.5
                    y = (py * SS) + sy + 0.5
                    if not _rounded_square(x, y, big, radius):
                        continue  # transparent outside the rounded square
                    colour = BG
                    if _near_segment(x, y, baseline[0], baseline[1], 6 * unit):
                        colour = GREY
                    on_line = any(_near_segment(x, y, line[i], line[i + 1], stroke)
                                  for i in range(len(line) - 1))
                    if on_line or _in_triangle(x, y, *head):
                        colour = GREEN
                    r += colour[0]
                    g += colour[1]
                    b += colour[2]
                    a += 255
            samples = SS * SS
            row += bytes((r // samples, g // samples, b // samples, a // samples))
        rows.append(bytes(row))
    return b"".join(b"\x00" + r for r in rows)


def write_png(path: Path, size: int) -> None:
    raw = render(size)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    header = struct.pack(">2I5B", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", header)
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)
    print(f"wrote {path} ({size}x{size}, {len(png)} bytes)")


if __name__ == "__main__":
    write_png(Path(__file__).with_name("tradingapp.png"),
              int(sys.argv[1]) if len(sys.argv) > 1 else 256)
