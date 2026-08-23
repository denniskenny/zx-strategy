#!/usr/bin/env python3
"""zxp_tiles_zx0.py — Convert a ZX-Paintbrush tile strip into a ZX0 header.

The .zxp holds N tiles side by side, all the same size, e.g. four 16x16
map tiles in a 64x16 sheet.  Each tile is emitted row-major (w/8 bytes per
pixel row), the tiles are concatenated in sheet order, and the whole blob
is ZX0-compressed.  The runtime decompresses it once into RAM and blits
tiles out of it with write_blit().

Each tile's colour comes from the sheet's attribute cells, so ink/paper
are authored in ZX-Paintbrush alongside the art: NAME_attr[i] is tile i's
attribute.  All cells of a tile must share one attribute (the ZX can only
give a whole character cell one colour anyway).

--attr HH overrides that for sheets whose colour the runtime decides:
every tile is emitted with the given attribute and the sheet's own
attribute cells are ignored.  The unit sheets use it, because a unit
sprite is drawn cyan or red by side and the colour ZX-Paintbrush saved is
just whatever the art was drawn in.  Terrain sheets do NOT: there the
sheet attribute is the tile's colour on screen, so a stray cell is a real
mistake and the strict check catches it.

Usage:
    python3 tools/zxp_tiles_zx0.py IN.zxp OUT.h --name NAME --tiles N
                                   [--attr HH] [--zx0 /path/to/zx0]

Refuses attribute 0x03 (and 0x02, which becomes 0x03 when ORed with 1):
that value is the floating bus sync marker — see
.claude/skills/floating-bus-vsync.
"""

import argparse
import os
import subprocess
import sys

VSYNC_MARKER = 0x03


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_zxp(path):
    """Return (pixel_rows, attr_bytes) from a ZX-Paintbrush text file."""
    lines = [l.rstrip("\r\n") for l in open(path)]
    if len(lines) < 3:
        die(f"{path} is too short to be a .zxp file")

    i = 2
    while i < len(lines) and lines[i].strip() == "":
        i += 1
    pixels = []
    while i < len(lines) and lines[i].strip() != "":
        if not all(c in "01" for c in lines[i]):
            die(f"{path}:{i + 1}: expected a row of 0/1 pixels")
        pixels.append(lines[i])
        i += 1
    if not pixels:
        die(f"{path}: no pixel data")

    attrs = []
    for line in lines[i + 1:]:
        for tok in line.split():
            attrs.append(int(tok, 16))
    return pixels, attrs


def tile_bytes(pixels, x0, tw, th):
    """Row-major bytes for one tile (MSB = leftmost pixel)."""
    out = bytearray()
    for y in range(th):
        row = pixels[y]
        for bx in range(tw // 8):
            b = 0
            for bit in range(8):
                if row[x0 + bx * 8 + bit] == "1":
                    b |= 0x80 >> bit
            out.append(b)
    return bytes(out)


def tile_attr(attrs, cols, tile, tw_ch, th_ch, name):
    """The attribute shared by every cell of a tile."""
    if not attrs:
        die("the sheet has no attribute data; colour the tiles in "
            "ZX-Paintbrush so each tile carries its ink/paper")
    seen = set()
    for cr in range(th_ch):
        for cc in range(tw_ch):
            idx = cr * cols + tile * tw_ch + cc
            if idx >= len(attrs):
                die(f"attribute data is short: expected {cols * th_ch} cells")
            seen.add(attrs[idx])
    if len(seen) != 1:
        die(f"tile {tile} of {name} uses several attributes "
            f"({', '.join(f'0x{a:02X}' for a in sorted(seen))}); "
            "give every cell of a tile the same ink/paper")
    attr = seen.pop()
    if attr in (VSYNC_MARKER, VSYNC_MARKER & 0xFE):
        die(f"tile {tile} uses attribute 0x{attr:02X}, which collides with "
            f"the floating bus sync marker 0x{VSYNC_MARKER:02X}")
    return attr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--name", required=True, help="C identifier prefix")
    ap.add_argument("--tiles", type=int, required=True,
                    help="number of tiles, side by side in the sheet")
    ap.add_argument("--attr", type=lambda v: int(v, 16), default=None,
                    help="hex attribute for every tile; ignores the sheet's "
                         "own attribute cells (for sheets the runtime "
                         "recolours, e.g. the unit sprites)")
    ap.add_argument("--zx0", default=os.environ.get("ZX0", "/tmp/ZX0/src/zx0"))
    args = ap.parse_args()

    if args.attr is not None and args.attr in (VSYNC_MARKER,
                                               VSYNC_MARKER & 0xFE):
        die(f"--attr 0x{args.attr:02X} collides with the floating bus sync "
            f"marker 0x{VSYNC_MARKER:02X}")

    pixels, attrs = parse_zxp(args.input)
    h = len(pixels)
    w = len(pixels[0])
    for y, row in enumerate(pixels):
        if len(row) != w:
            die(f"{args.input}: pixel row {y} is {len(row)} wide, expected {w}")

    if w % args.tiles:
        die(f"sheet width {w} is not divisible by {args.tiles} tiles")
    tw = w // args.tiles
    if tw % 8 or h % 8:
        die(f"tile size {tw}x{h} must be a whole number of 8x8 characters")

    tiles = [tile_bytes(pixels, t * tw, tw, h) for t in range(args.tiles)]
    if args.attr is None:
        tile_attrs = [tile_attr(attrs, w // 8, t, tw // 8, h // 8, args.name)
                      for t in range(args.tiles)]
    else:
        tile_attrs = [args.attr] * args.tiles

    blob = b"".join(tiles)
    raw = "/tmp/%s_tiles.bin" % args.name
    comp = "/tmp/%s_tiles.zx0" % args.name
    open(raw, "wb").write(blob)
    if os.path.exists(comp):
        os.remove(comp)
    subprocess.run([args.zx0, "-f", raw, comp], check=True,
                   capture_output=True)
    zdata = open(comp, "rb").read()

    upper = args.name.upper()
    guard = f"_{os.path.basename(args.output).replace('.', '_').upper()}_"
    with open(args.output, "w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"/* Generated from {args.input} by tools/zxp_tiles_zx0.py"
                " — do not edit. */\n\n")
        f.write(f"#define {upper}_TILES     {args.tiles}\n")
        f.write(f"#define {upper}_TILE_W    {tw // 8}"
                f"   /* character columns */\n")
        f.write(f"#define {upper}_TILE_ROWS {h // 8}"
                f"   /* character rows    */\n")
        f.write(f"#define {upper}_TILE_H    {h}"
                f"   /* pixel rows        */\n")
        f.write(f"#define {upper}_TILE_SIZE {tw // 8 * h}"
                f"   /* bytes per tile    */\n")
        f.write(f"#define {upper}_RAW_SIZE  {len(blob)}"
                f"   /* decompressed size */\n\n")
        f.write("/* Tile attributes, authored in ZX-Paintbrush. */\n"
                if args.attr is None else
                "/* Tile attributes: fixed by --attr, not read from the "
                "sheet — the runtime picks the colour. */\n")
        f.write(f"static const uint8_t {args.name}_attr[{args.tiles}] = {{\n    ")
        f.write(", ".join(f"0x{a:02X}" for a in tile_attrs))
        f.write("\n};\n\n")
        f.write(f"/* {args.tiles} tiles of {tw}x{h}, row major, ZX0"
                f" ({len(zdata)} <- {len(blob)} bytes). */\n")
        f.write(f"static const uint8_t {args.name}_zx0[{len(zdata)}] = {{\n")
        for i in range(0, len(zdata), 16):
            chunk = zdata[i:i + 16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
            f.write(",\n" if i + 16 < len(zdata) else "\n")
        f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")

    print(f"wrote {args.output}: {args.tiles} tiles of {tw}x{h}, "
          f"ZX0 {len(zdata)} B <- {len(blob)} B, attrs "
          + " ".join(f"0x{a:02X}" for a in tile_attrs))


if __name__ == "__main__":
    main()
