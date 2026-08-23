#!/usr/bin/env python3
"""zxp_tiles_zx0.py — Convert a ZX-Paintbrush tile strip into a ZX0 header.

The .zxp holds N tiles side by side, all the same size, e.g. four 16x16
map tiles in a 64x16 sheet.  Each tile is emitted row-major (w/8 bytes per
pixel row), the tiles are concatenated in sheet order, and the whole blob
is ZX0-compressed.  The runtime decompresses it once into RAM and blits
tiles out of it with write_blit().

Colour is authored in ZX-Paintbrush alongside the art and travels with
it, PER CHARACTER CELL — a 32x32 tile carries its own 4x4 block of
attributes, not one flat colour.  The attribute block is appended to the
pixel data and the whole thing is compressed as one ZX0 stream, so the
runtime gets both from a single decompression: pixels at offset 0, tile
t's attributes at NAME_ATTR_OFF + t * NAME_ATTR_SIZE.

Two modes, because the two kinds of sheet want different things:

  --attr-mode full    (default) keep the authored byte.  Terrain uses
                      this: what the artist coloured is what appears.

  --attr-mode bright  keep only bit 6, the BRIGHT flag, and discard
                      ink and paper.  Unit sheets use this: a unit is
                      cyan or red according to whose it is, so its ink
                      is not the artist's to choose — but which cells
                      are bright still is, and that is the shading.
                      The runtime ORs the side's colour over these.

Storing bright as a whole byte per cell rather than packing it eight to
a byte costs nothing worth having: the values are 0x00 and 0x40 in long
runs, which is exactly what ZX0 eats, and it keeps the runtime a single
OR against the side colour with no unpacking.

Usage:
    python3 tools/zxp_tiles_zx0.py IN.zxp OUT.h --name NAME --tiles N
                                   [--attr-mode full|bright]
                                   [--zx0 /path/to/zx0]

In full mode, refuses attribute 0x03 (and 0x02, which becomes 0x03 when
ORed with 1): that value is the floating bus sync marker — see
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


def tile_attrs(attrs, cols, tile, tw_ch, th_ch, name, mode):
    """One tile's attribute block, row-major: th_ch rows of tw_ch cells."""
    if not attrs:
        die("the sheet has no attribute data; colour the tiles in "
            "ZX-Paintbrush so each cell carries its ink/paper")
    out = bytearray()
    for cr in range(th_ch):
        for cc in range(tw_ch):
            idx = cr * cols + tile * tw_ch + cc
            if idx >= len(attrs):
                die(f"attribute data is short: expected {cols * th_ch} cells")
            a = attrs[idx]
            if mode == "bright":
                # Ink and paper belong to the runtime; only the artist's
                # choice of which cells glow survives.
                out.append(a & 0x40)
            else:
                if a in (VSYNC_MARKER, VSYNC_MARKER & 0xFE):
                    die(f"{name} tile {tile}, cell ({cc},{cr}) uses attribute "
                        f"0x{a:02X}, which collides with the floating bus "
                        f"sync marker 0x{VSYNC_MARKER:02X}")
                out.append(a)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--name", required=True, help="C identifier prefix")
    ap.add_argument("--tiles", type=int, required=True,
                    help="number of tiles, side by side in the sheet")
    ap.add_argument("--attr-mode", choices=("full", "bright"), default="full",
                    help="full: keep the authored attribute per cell. "
                         "bright: keep only the BRIGHT bit and let the "
                         "runtime supply ink and paper (unit sheets)")
    ap.add_argument("--zx0", default=os.environ.get("ZX0", "/tmp/ZX0/src/zx0"))
    args = ap.parse_args()

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
    blocks = [tile_attrs(attrs, w // 8, t, tw // 8, h // 8,
                         args.name, args.attr_mode)
              for t in range(args.tiles)]

    # Pixels for every tile, then attributes for every tile: one stream,
    # one decompression, and the attribute table at a known offset.
    pixel_bytes = b"".join(tiles)
    attr_bytes = b"".join(blocks)
    attr_off = len(pixel_bytes)
    attr_size = len(blocks[0])
    blob = pixel_bytes + attr_bytes
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
                f"   /* pixel bytes per tile */\n")
        f.write(f"#define {upper}_ATTR_SIZE {attr_size}"
                f"   /* attribute bytes per tile */\n")
        f.write(f"#define {upper}_ATTR_OFF  {attr_off}"
                f"   /* where the attributes start */\n")
        f.write(f"#define {upper}_RAW_SIZE  {len(blob)}"
                f"   /* decompressed size, pixels + attributes */\n\n")
        if args.attr_mode == "bright":
            f.write("/* Attributes are BRIGHT flags only (0x00 / 0x40), one\n"
                    "   byte per character cell: the runtime ORs the side's\n"
                    "   ink and paper over them. */\n")
        else:
            f.write("/* Attributes are the authored ink/paper/bright, one\n"
                    "   byte per character cell, row major within a tile. */\n")
        f.write(f"/* Tile t's block is at {args.name}[{upper}_ATTR_OFF"
                f" + t * {upper}_ATTR_SIZE]. */\n\n")
        f.write(f"/* {args.tiles} tiles of {tw}x{h} + {len(attr_bytes)} attribute"
                f" bytes, ZX0 ({len(zdata)} <- {len(blob)}). */\n")
        U = args.name.upper()
        f.write(f"/* --- Data: defined once, declared everywhere else ---\n"
                f"   This blob was `static const`, so every .c file that\n"
                f"   included this header got its OWN copy — 380 bytes of\n"
                f"   tiles_view carried three times before anyone noticed.\n"
                f"   Exactly one translation unit defines it:\n"
                f"\n"
                f"       #define {U}_DEFINE_DATA\n"
                f"       #include \"{args.name}.h\"\n"
                f"\n"
                f"   Undefined symbol at link time means nobody claimed it;\n"
                f"   duplicate means two files did. */\n")
        f.write(f"#ifndef {U}_DEFINE_DATA\n"
                f"extern const uint8_t {args.name}_zx0[{len(zdata)}];\n"
                f"#else\n"
                f"const uint8_t {args.name}_zx0[{len(zdata)}] = {{\n")
        for i in range(0, len(zdata), 16):
            chunk = zdata[i:i + 16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
            f.write(",\n" if i + 16 < len(zdata) else "\n")
        f.write("};\n#endif\n\n")
        f.write(f"#endif /* {guard} */\n")

    distinct = sorted({a for b in blocks for a in b})
    print(f"wrote {args.output}: {args.tiles} tiles of {tw}x{h}, "
          f"ZX0 {len(zdata)} B <- {len(blob)} B "
          f"({len(pixel_bytes)} pixel + {len(attr_bytes)} attr), "
          f"{args.attr_mode} attrs: "
          + " ".join(f"0x{a:02X}" for a in distinct))


if __name__ == "__main__":
    main()
