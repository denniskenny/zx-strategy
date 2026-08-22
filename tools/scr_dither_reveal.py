#!/usr/bin/env python3
"""Generate dithered reveal frames from a ZX Spectrum .scr file.

Reads a .scr (6144 pixel + 768 attr bytes), extracts pixel states,
and generates progressive reveal frames using Bayer 8x8 ordered
dithering — same algorithm as gen_anglerfish.py.

Usage:
    python3 tools/scr_dither_reveal.py assets/angler_5.scr assets/goo
      -> produces assets/goo_1.scr .. assets/goo_6.scr
"""

import os
import sys

BAYER_8x8 = [
    [ 0, 32,  8, 40,  2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44,  4, 36, 14, 46,  6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [ 3, 35, 11, 43,  1, 33,  9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47,  7, 39, 13, 45,  5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]


def scr_offset(x, y):
    col = x >> 3
    third = (y >> 6) & 3
    char_row = (y >> 3) & 7
    pixel_row = y & 7
    return (third << 11) | (pixel_row << 8) | (char_row << 5) | col


def dither_reveal(src_pixels, reveal):
    """Keep source pixels that pass the Bayer threshold at the given reveal level."""
    out = bytearray(6144)
    for y in range(192):
        for x in range(256):
            off = scr_offset(x, y)
            bit = (src_pixels[off] >> (7 - (x & 7))) & 1
            if bit:
                threshold = (BAYER_8x8[y & 7][x & 7] + 0.5) / 64.0
                if reveal >= threshold:
                    out[off] |= 0x80 >> (x & 7)
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} source.scr output_prefix")
        print(f"  Produces output_prefix_1.scr .. output_prefix_6.scr")
        sys.exit(1)

    src_path = sys.argv[1]
    prefix = sys.argv[2]

    with open(src_path, "rb") as f:
        data = f.read()

    src_pixels = data[:6144]
    src_attrs = data[6144:6912] if len(data) >= 6912 else bytearray([0x07] * 768)

    reveals = [0.12, 0.30, 0.55, 0.80, 1.0, 1.0]

    for i, reveal in enumerate(reveals):
        fname = f"{prefix}_{i + 1}.scr"
        pixels = dither_reveal(src_pixels, reveal)
        with open(fname, "wb") as f:
            f.write(pixels)
            f.write(src_attrs)
        n_set = sum(bin(b).count('1') for b in pixels)
        print(f"  Frame {i + 1}: reveal={reveal:.0%}  pixels={n_set:5d} -> {fname}")


if __name__ == "__main__":
    main()
