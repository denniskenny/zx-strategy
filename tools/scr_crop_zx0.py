#!/usr/bin/env python3
"""Crop .scr files to their non-zero bounding box, ZX0-compress, and emit a C header.

Finds the union bounding box across all input frames, extracts that region
in row-major order, compresses each with ZX0, and writes a single C header
with the data arrays and crop constants.

The placement constants are prefixed with the output header's name —
include/foo_data.h gives FOO_CROP_COL, FOO_CROP_ROW, FOO_CROP_W, FOO_CROP_H
and FOO_CROP_SIZE — or with --name if you want something else.  The pixel
arrays are named by the name:file pairs.

With --mirror, only the left half (up to column 15) is stored.  The runtime
reconstructs the right half by bit-reversing each byte and reversing column
order.  The header then also emits PREFIX_MIRROR_COL for the right-half start
column.

Usage:
    python3 tools/scr_crop_zx0.py [--mirror] [--name PREFIX] \
        output.h zx0_path name1:file1.scr [...]

Requires the zx0 compressor binary at zx0_path.
"""

import os
import subprocess
import sys
import tempfile


def scr_offset(x, y):
    col = x >> 3
    third = (y >> 6) & 3
    char_row = (y >> 3) & 7
    pixel_row = y & 7
    return (third << 11) | (pixel_row << 8) | (char_row << 5) | col


def find_bbox(frames):
    """Find union bounding box (byte-column, pixel-row) across all frames."""
    min_y, max_y = 192, 0
    min_col, max_col = 32, 0
    for data in frames:
        for y in range(192):
            for col in range(32):
                if data[scr_offset(col * 8, y)] != 0:
                    if y < min_y: min_y = y
                    if y > max_y: max_y = y
                    if col < min_col: min_col = col
                    if col > max_col: max_col = col
    return min_col, min_y, max_col, max_y


def crop_frame(data, min_col, min_y, max_col, max_y):
    """Extract bounding box region in row-major order."""
    out = bytearray()
    for y in range(min_y, max_y + 1):
        for col in range(min_col, max_col + 1):
            out.append(data[scr_offset(col * 8, y)])
    return bytes(out)


def main():
    args = sys.argv[1:]
    mirror = False
    prefix = None
    while args and args[0].startswith("--"):
        if args[0] == "--mirror":
            mirror = True
            args = args[1:]
        elif args[0] == "--name":
            prefix = args[1].upper()
            args = args[2:]
        else:
            print(f"Unknown option {args[0]}")
            sys.exit(1)

    if len(args) < 3:
        print(f"Usage: {sys.argv[0]} [--mirror] [--name PREFIX] "
              "output.h zx0_path name1:file1.scr [...]")
        sys.exit(1)

    dst = args[0]
    zx0_bin = args[1]

    # The placement constants are named after the header unless --name
    # says otherwise: include/foo_data.h -> FOO_CROP_COL and friends.
    if prefix is None:
        stem = os.path.basename(dst).rsplit(".", 1)[0]
        if stem.endswith("_data"):
            stem = stem[:-len("_data")]
        prefix = stem.upper()
    entries = []
    for arg in args[2:]:
        name, path = arg.split(":", 1)
        with open(path, "rb") as f:
            data = f.read()[:6144]
        entries.append((name, data))

    # Union bounding box
    frames = [data for _, data in entries]
    min_col, min_y, max_col, max_y = find_bbox(frames)

    mirror_col = None
    if mirror:
        center = 14  # axis of symmetry between cols 14 and 15
        max_col = min(max_col, center)
        mirror_col = center + 1

    w = max_col - min_col + 1
    h = max_y - min_y + 1
    crop_size = w * h

    print(f"Bounding box: rows {min_y}-{max_y}, cols {min_col}-{max_col}"
          + (f" (mirror from col {mirror_col})" if mirror else ""))
    print(f"Crop: {h} rows x {w} byte-cols = {crop_size} bytes (was 6144)")

    # Crop and compress each frame
    compressed = []
    with tempfile.TemporaryDirectory() as tmpdir:
        for name, data in entries:
            cropped = crop_frame(data, min_col, min_y, max_col, max_y)
            bin_path = os.path.join(tmpdir, f"{name}.bin")
            zx0_path = os.path.join(tmpdir, f"{name}.zx0")
            with open(bin_path, "wb") as f:
                f.write(cropped)
            subprocess.run([zx0_bin, "-f", bin_path, zx0_path],
                           check=True, capture_output=True)
            with open(zx0_path, "rb") as f:
                zdata = f.read()
            compressed.append((name, zdata))
            print(f"  {name}: {len(zdata)} bytes ZX0")

    # Write header
    guard = "_" + os.path.basename(dst).replace(".", "_").upper() + "_"
    with open(dst, "w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write(f"/* Generated from {len(entries)} .scr frame(s) by "
                "tools/scr_crop_zx0.py — do not edit. */\n\n")
        f.write(f"#define {prefix}_CROP_COL  {min_col}\n")
        f.write(f"#define {prefix}_CROP_ROW  {min_y}\n")
        f.write(f"#define {prefix}_CROP_W    {w}\n")
        f.write(f"#define {prefix}_CROP_H    {h}\n")
        f.write(f"#define {prefix}_CROP_SIZE {crop_size}\n")
        if mirror_col is not None:
            f.write(f"#define {prefix}_MIRROR_COL {mirror_col}\n")
        f.write("\n")
        for name, zdata in compressed:
            f.write(f"/* ZX0 compressed cropped screen data ({len(zdata)} bytes) */\n")
            f.write(f"static const unsigned char {name}[] = {{\n")
            for i in range(0, len(zdata), 16):
                chunk = zdata[i : i + 16]
                f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
                if i + 16 < len(zdata):
                    f.write(",")
                f.write("\n")
            f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")

    total = sum(len(z) for _, z in compressed)
    print(f"\nWritten {dst}: {len(compressed)} arrays, {total} bytes total")


if __name__ == "__main__":
    main()
