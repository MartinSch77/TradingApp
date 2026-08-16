# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for packaging/make_icon.py.

Pure stdlib (struct + zlib), no image library involved, so no dependency to
install here. Tests avoid asserting on exact pixel content — geometry is
resolution-dependent — and instead check dimensions/format/invariants plus
the branch conditions of the small geometry helpers directly."""

import struct
import zlib

import pytest

import make_icon as mi


# --------------------------------------------------------------------------
# _rounded_square
# --------------------------------------------------------------------------

def test_rounded_square_center_is_inside():
    assert mi._rounded_square(128, 128, 256, 52) is True


def test_rounded_square_corner_outside_radius_is_excluded():
    # The extreme corner (0, 0) is outside a rounded square of radius 52.
    assert mi._rounded_square(0, 0, 256, 52) is False


def test_rounded_square_edge_midpoint_is_inside():
    # Midpoint of a straight edge (not near a corner) is inside — exercises
    # the branch where cx/cy are clamped to x/y themselves (not the radius).
    assert mi._rounded_square(128, 0.5, 256, 52) is True


# --------------------------------------------------------------------------
# _near_segment
# --------------------------------------------------------------------------

def test_near_segment_point_on_the_line():
    assert mi._near_segment(5, 5, (0, 0), (10, 10), 2.0) is True


def test_near_segment_point_far_away():
    assert mi._near_segment(0, 100, (0, 0), (10, 10), 2.0) is False


def test_near_segment_zero_length_segment_uses_point_distance():
    # a == b collapses the segment to a point (length2 == 0 branch).
    assert mi._near_segment(0.5, 0.5, (0, 0), (0, 0), 2.0) is True
    assert mi._near_segment(10, 10, (0, 0), (0, 0), 2.0) is False


def test_near_segment_clamped_beyond_endpoint():
    # Closest point is beyond b, so t clamps to 1.0.
    assert mi._near_segment(20, 0, (0, 0), (10, 0), 2.0) is False
    assert mi._near_segment(11, 0, (0, 0), (10, 0), 2.0) is True


# --------------------------------------------------------------------------
# _in_triangle
# --------------------------------------------------------------------------

def test_in_triangle_point_inside_ccw_triangle():
    assert mi._in_triangle(1, 1, (0, 0), (4, 0), (0, 4)) is True


def test_in_triangle_point_outside():
    assert mi._in_triangle(10, 10, (0, 0), (4, 0), (0, 4)) is False


def test_in_triangle_point_inside_cw_triangle():
    # Same triangle, opposite winding — exercises the "all <= 0" branch.
    assert mi._in_triangle(1, 1, (0, 0), (0, 4), (4, 0)) is True


# --------------------------------------------------------------------------
# render / write_png
# --------------------------------------------------------------------------

def test_render_returns_expected_byte_length():
    size = 8
    raw = mi.render(size)
    # Each row is a filter-type byte (0x00) followed by 4 bytes/pixel.
    assert len(raw) == size * (1 + 4 * size)


def test_render_corner_pixel_is_fully_transparent():
    size = 16
    raw = mi.render(size)
    stride = 1 + 4 * size
    row0 = raw[0:stride]
    # First byte of the row is the PNG filter type; first pixel starts next.
    corner_alpha = row0[1 + 3]
    assert corner_alpha == 0


def test_render_center_pixel_is_opaque():
    size = 16
    raw = mi.render(size)
    stride = 1 + 4 * size
    mid_row = raw[stride * (size // 2):stride * (size // 2 + 1)]
    mid_pixel_offset = 1 + 4 * (size // 2)
    center_alpha = mid_row[mid_pixel_offset + 3]
    assert center_alpha == 255


def test_write_png_produces_valid_png_with_correct_ihdr(tmp_path):
    size = 12
    path = tmp_path / "icon.png"
    mi.write_png(path, size)

    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"

    # IHDR chunk immediately follows the signature: length(4) + "IHDR"(4) + 13 bytes.
    ihdr_len = struct.unpack(">I", data[8:12])[0]
    assert ihdr_len == 13
    assert data[12:16] == b"IHDR"
    width, height, bit_depth, color_type, *_rest = struct.unpack(">2I5B", data[16:16 + 13])
    assert width == size
    assert height == size
    assert bit_depth == 8
    assert color_type == 6  # RGBA


def test_write_png_idat_decompresses_to_render_output(tmp_path):
    size = 6
    path = tmp_path / "icon.png"
    mi.write_png(path, size)
    data = path.read_bytes()

    # Locate the IDAT chunk by scanning (small file, single IDAT expected).
    idx = data.index(b"IDAT")
    idat_len = struct.unpack(">I", data[idx - 4:idx])[0]
    idat_data = data[idx + 4:idx + 4 + idat_len]
    decompressed = zlib.decompress(idat_data)

    expected = mi.render(size)
    assert decompressed == expected


def test_write_png_prints_summary(tmp_path, capsys):
    size = 4
    path = tmp_path / "icon.png"
    mi.write_png(path, size)
    out = capsys.readouterr().out
    assert f"{size}x{size}" in out
    assert str(path) in out
