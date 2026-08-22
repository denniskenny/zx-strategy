#!/usr/bin/env python3
"""
zxp2header.py - Convert ZX-Paintbrush .zxp files to C header files.

Produces static const uint8_t arrays suitable for Z88DK / SDCC.

ZXP format (text-based):
  Line 0: "ZX-Paintbrush image"
  Line 1: blank
  Lines 2..N: pixel rows as ASCII '0' and '1' characters
  Blank separator line
  Attribute lines: space-separated hex bytes, one line per character row

For multi-frame sprites, frames can be stacked vertically (default) or
arranged side-by-side (--horizontal).  Vertical: frame height = total rows
/ num_frames.  Horizontal: frame width = total width / num_frames.

Usage:
  zxp2header.py <input.zxp> <output.h> [--frames N] [--name NAME] [--sp1]
                [--horizontal] [--downscale]

The --sp1 flag outputs sprite data in SP1's column-major (mask, graphic)
pair format instead of row-major. Each column contains (height_chars + 1)
character rows (the extra row is blank for vertical rotation), with 8
(mask, graphic) pairs per character row.
"""

import argparse
import os
import sys


def parse_zxp(path):
    """Parse a .zxp file, returning (width, pixel_rows, attr_bytes)."""
    with open(path, "r") as f:
        lines = [l.rstrip("\r\n") for l in f.readlines()]

    # Skip header (first 2 lines)
    if len(lines) < 3:
        sys.exit(f"Error: {path} too short to be a valid .zxp file")

    # Find pixel data: lines of '0' and '1' characters
    # Skip header lines and any leading blanks
    pixel_lines = []
    attr_start = None
    i = 2
    while i < len(lines) and lines[i].strip() == "":
        i += 1
    while i < len(lines):
        line = lines[i]
        # A blank line separates pixels from attributes
        if line.strip() == "":
            attr_start = i + 1
            break
        # Validate pixel line
        if all(c in "01" for c in line) and len(line) > 0:
            pixel_lines.append(line)
        else:
            sys.exit(
                f"Error: unexpected content on line {i+1}: {line[:40]!r}"
            )
        i += 1

    if not pixel_lines:
        sys.exit(f"Error: no pixel data found in {path}")

    width = len(pixel_lines[0])
    for idx, pl in enumerate(pixel_lines):
        if len(pl) != width:
            sys.exit(
                f"Error: inconsistent width on pixel line {idx+2+1} "
                f"(expected {width}, got {len(pl)})"
            )

    # Parse attribute data (if present)
    attr_bytes = []
    if attr_start is not None:
        for j in range(attr_start, len(lines)):
            line = lines[j].strip()
            if not line:
                continue
            for token in line.split():
                try:
                    attr_bytes.append(int(token, 16))
                except ValueError:
                    sys.exit(
                        f"Error: invalid attribute hex value "
                        f"'{token}' on line {j+1}"
                    )

    return width, pixel_lines, attr_bytes


def pixels_to_bytes(pixel_lines, width):
    """Convert ASCII '0'/'1' pixel rows to packed bytes (MSB = leftmost)."""
    result = []
    for row in pixel_lines:
        row_bytes = []
        for col in range(0, width, 8):
            byte = 0
            for bit in range(8):
                px = col + bit
                if px < width and row[px] == "1":
                    byte |= 0x80 >> bit
            row_bytes.append(byte)
        result.extend(row_bytes)
    return result


def pixels_to_sp1(pixel_lines, width, height):
    """Convert pixel rows to SP1 column-major (mask, graphic) pairs.

    SP1 format per column (8 pixels wide):
      For each character row (height_chars + 1 for rotation buffer):
        For each pixel line within char row (8 lines):
          mask_byte, graphic_byte

    Mask: 0xFF = transparent, 0x00 = opaque.
    Mask is generated as inverse of graphic (1-pixel border expansion).
    """
    height_chars = (height + 7) // 8
    width_cols = (width + 7) // 8
    result = []

    for col in range(width_cols):
        col_x = col * 8
        for char_row in range(height_chars + 1):
            for line in range(8):
                pixel_y = char_row * 8 + line
                if char_row >= height_chars or pixel_y >= height:
                    # Extra rotation row or beyond sprite height
                    result.append(0xFF)  # mask: transparent
                    result.append(0x00)  # graphic: empty
                else:
                    # Build graphic byte from pixel data
                    gfx = 0
                    for bit in range(8):
                        px = col_x + bit
                        if px < width and pixel_lines[pixel_y][px] == "1":
                            gfx |= 0x80 >> bit
                    # Mask: inverse of graphic (simple; 1px border
                    # expansion is done at runtime by sprites_gen_mask)
                    mask = gfx ^ 0xFF
                    result.append(mask)
                    result.append(gfx)

    return result


def downscale_bytes(data, src_w, src_h, factor):
    """Nearest-neighbour downscale of packed pixel bytes.

    src_w/src_h in pixels.  factor=2 gives half-size, factor=4 gives quarter.
    Returns packed bytes for the smaller sprite.
    """
    dst_w = src_w // factor
    dst_h = src_h // factor
    src_bpr = src_w // 8
    dst_bpr = max(dst_w // 8, 1)
    out = []
    for dy in range(dst_h):
        sy = dy * factor
        row_bytes = []
        for db in range(dst_bpr):
            byte = 0
            for bit in range(8):
                dpx = db * 8 + bit
                if dpx >= dst_w:
                    break
                spx = dpx * factor
                sbyte_idx = sy * src_bpr + spx // 8
                sbit = 7 - (spx & 7)
                if data[sbyte_idx] & (1 << sbit):
                    byte |= 0x80 >> bit
            row_bytes.append(byte)
        out.extend(row_bytes)
    return out


def format_c_array(name, data, bytes_per_row=16):
    """Format a byte array as a C static const uint8_t declaration."""
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), bytes_per_row):
        chunk = data[i : i + bytes_per_row]
        hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
        if i + bytes_per_row < len(data):
            hex_vals += ","
        lines.append(f"    {hex_vals}")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Convert ZX-Paintbrush .zxp to C header"
    )
    parser.add_argument("input", help="Input .zxp file")
    parser.add_argument("output", help="Output .h file")
    parser.add_argument(
        "--frames",
        type=int,
        default=1,
        help="Number of animation frames (stacked vertically)",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="Base name for C identifiers (default: derived from filename)",
    )
    parser.add_argument(
        "--sp1",
        action="store_true",
        help="Output in SP1 column-major (mask, graphic) format",
    )
    parser.add_argument(
        "--downscale",
        action="store_true",
        help="Also emit 16x16 and 8x8 nearest-neighbour downscaled arrays "
             "(row-major only, requires 32x32 source frames)",
    )
    parser.add_argument(
        "--horizontal",
        action="store_true",
        help="Frames are arranged side-by-side (default: stacked vertically)",
    )
    args = parser.parse_args()

    full_width, pixel_lines, attr_bytes = parse_zxp(args.input)
    total_rows = len(pixel_lines)

    if args.horizontal:
        if full_width % args.frames != 0:
            sys.exit(
                f"Error: {full_width} pixel width not divisible by "
                f"{args.frames} frames"
            )
        frame_width = full_width // args.frames
        frame_height = total_rows
    else:
        if total_rows % args.frames != 0:
            sys.exit(
                f"Error: {total_rows} pixel rows not divisible by "
                f"{args.frames} frames"
            )
        frame_width = full_width
        frame_height = total_rows // args.frames

    # Derive C identifier base name
    if args.name:
        base = args.name
    else:
        base = os.path.splitext(os.path.basename(args.input))[0]
        # Sanitise for C identifier
        base = "".join(c if c.isalnum() or c == "_" else "_" for c in base)

    # Build header guard
    guard = f"_{base.upper()}_H_"

    height_chars = (frame_height + 7) // 8
    width_cols = (frame_width + 7) // 8

    header_lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "/* Auto-generated from "
        + os.path.basename(args.input)
        + " by zxp2header.py */",
        "",
        "#include <stdint.h>",
        "",
        f"#define {base.upper()}_WIDTH  {frame_width}",
        f"#define {base.upper()}_HEIGHT {frame_height}",
        f"#define {base.upper()}_FRAMES {args.frames}",
    ]

    if args.sp1:
        header_lines.append(
            f"#define {base.upper()}_SP1_COLS  {width_cols}"
        )
        header_lines.append(
            f"#define {base.upper()}_SP1_ROWS  {height_chars}"
        )
        # Bytes per column: (height_chars + 1) * 8 * 2
        col_bytes = (height_chars + 1) * 8 * 2
        header_lines.append(
            f"#define {base.upper()}_SP1_COL_BYTES  {col_bytes}"
        )

    header_lines.append("")

    if args.downscale and (args.sp1 or frame_width != 32
                          or frame_height != 32):
        sys.exit(
            "Error: --downscale requires 32x32 row-major frames "
            "(no --sp1)"
        )

    # Generate per-frame arrays
    for frame in range(args.frames):
        if args.horizontal:
            col_start = frame * frame_width
            col_end = col_start + frame_width
            frame_pixels = [row[col_start:col_end] for row in pixel_lines]
        else:
            start_row = frame * frame_height
            end_row = start_row + frame_height
            frame_pixels = pixel_lines[start_row:end_row]

        if args.sp1:
            data = pixels_to_sp1(frame_pixels, frame_width, frame_height)
        else:
            data = pixels_to_bytes(frame_pixels, frame_width)

        if args.frames == 1:
            arr_name = f"{base}_bitmap" if not args.sp1 else f"{base}_sp1"
        else:
            arr_name = f"{base}_f{frame + 1}"

        header_lines.append(format_c_array(arr_name, data))
        header_lines.append("")

        if args.downscale:
            ds16 = downscale_bytes(data, 32, 32, 2)
            ds8 = downscale_bytes(data, 32, 32, 4)
            header_lines.append(format_c_array(f"{arr_name}_16", ds16))
            header_lines.append("")
            header_lines.append(format_c_array(f"{arr_name}_8", ds8))
            header_lines.append("")

    # Attribute array (if present)
    if attr_bytes:
        header_lines.append(format_c_array(f"{base}_attr", attr_bytes))
        header_lines.append("")

    header_lines.append(f"#endif /* {guard} */")
    header_lines.append("")

    with open(args.output, "w") as f:
        f.write("\n".join(header_lines))

    fmt = "SP1 column-major" if args.sp1 else "row-major"
    layout = "horizontal" if args.horizontal else "vertical"
    print(
        f"Generated {args.output}: {frame_width}x{frame_height}, "
        f"{args.frames} frame(s) [{layout}], "
        f"{len(attr_bytes)} attribute byte(s) [{fmt}]"
    )


if __name__ == "__main__":
    main()
