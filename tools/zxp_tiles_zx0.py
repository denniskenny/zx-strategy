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


ZX0_NOTE = '/* The BYTES do not belong in the program.  They are read once, at\n   boot, to decompress into the tile buffers, so they live in the\n   standalone asset block at 0x6000 instead of spending the 16 KB\n   code budget below 0xC000.\n\n   tools/mkassets.py parses the array below out of this header and\n   emits two things: the binary that mktap.py ships as its own CODE\n   block, and a `defc` that resolves the extern to its address at\n   zero cost.  Nothing defines %s, so no translation\n   unit ever carries a copy -- the array is here to be read by the\n   tool, not by the compiler. */\n'


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


def tile_bytes(pixels, x0, tw, th, y0=0):
    """Row-major bytes for one tile (MSB = leftmost pixel)."""
    out = bytearray()
    for y in range(y0, y0 + th):
        row = pixels[y]
        for bx in range(tw // 8):
            b = 0
            for bit in range(8):
                if row[x0 + bx * 8 + bit] == "1":
                    b |= 0x80 >> bit
            out.append(b)
    return bytes(out)


def frames_layout(pixels, tiles, frames):
    """Where each (unit, frame) tile sits, and how big it is.

       With --frames the sheet is a GRID: one column per frame, one row
       per unit, so unit u frame f is at (f * tw, u * th).  Without it the
       sheet is the horizontal strip it has always been and frame 0 is the
       only frame -- the two layouts are different shapes, so the flag
       chooses rather than extends.  Existing sheets are untouched. */"""
    h = len(pixels)
    w = len(pixels[0])
    if frames < 2:
        tw = w // tiles
        return tw, h, [[(t * tw, 0)] for t in range(tiles)]
    tw = w // frames
    th = h // tiles
    if w % frames or h % tiles:
        die(f"sheet {w}x{h} does not divide into {frames} frames "
            f"by {tiles} units")
    return tw, th, [[(f * tw, u * th) for f in range(frames)]
                    for u in range(tiles)]


def dilate(pixels, cols, tw, th, y0=0):
    """The sprite grown by one pixel all round, diagonals included.

       That extra pixel is the whole point of the mask: it puts a black rim
       between the sprite and whatever it stands on, so a unit stays legible
       over busy terrain instead of dissolving into it (docs/DESIGN.md
       § Sprite masks and animation).

       `cols` may be several x-origins, in which case the frames are
       UNIONED before dilating -- one mask that covers every frame, which
       is what lets both frames share it whatever they look like."""
    if not isinstance(cols, (list, tuple)):
        cols = (cols,)
    grown = [[False] * tw for _ in range(th)]
    for y in range(th):
        for x in range(tw):
            if not any(pixels[y0 + y][x0 + x] == "1" for x0 in cols):
                continue
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < th and 0 <= nx < tw:
                        grown[ny][nx] = True
    return grown


def mask_bytes(pixels, cols, tw, th, y0=0):
    """One tile's mask, INVERTED, row-major, MSB leftmost.

       Inverted because the blit is

           screen = (screen AND NOT mask) OR sprite

       and doing the NOT here makes the inner loop AND then OR with no
       complement per byte -- free in the tool, one instruction saved in
       the one loop that cannot afford them.

       So a bit is SET where the screen should be kept and CLEAR over the
       sprite and its outline."""
    grown = dilate(pixels, cols, tw, th, y0)
    out = bytearray()
    for y in range(th):
        for bx in range(tw // 8):
            b = 0
            for bit in range(8):
                if not grown[y][bx * 8 + bit]:
                    b |= 0x80 >> bit
            out.append(b)
    return bytes(out)


def check_margin(pixels, x0, tw, th, name, tile, y0=0):
    """Refuse artwork with ink on the outer edge of its cell.

       Dilating it would spill outside the cell, where the blit cannot
       reach, and the outline would simply be missing on that side -- which
       reads as a drawing mistake rather than a clipping one.  Stopping is
       better than quietly emitting a slightly wrong mask."""
    edges = []
    if any(pixels[y0][x0 + x] == "1" for x in range(tw)):
        edges.append("top")
    if any(pixels[y0 + th - 1][x0 + x] == "1" for x in range(tw)):
        edges.append("bottom")
    if any(pixels[y0 + y][x0] == "1" for y in range(th)):
        edges.append("left")
    if any(pixels[y0 + y][x0 + tw - 1] == "1" for y in range(th)):
        edges.append("right")
    if edges:
        die(f"{name} tile {tile} has ink on its {', '.join(edges)} edge: "
            f"the mask needs a 1-pixel margin inside the cell to dilate "
            f"into, or the outline goes missing on that side")


def tile_attrs(attrs, cols, tile, tw_ch, th_ch, name, mode,
               cc0=None, cr0=0):
    """One tile's attribute block, row-major: th_ch rows of tw_ch cells.

       (cc0, cr0) is the tile's top-left in CHARACTER cells.  It defaults
       to the horizontal strip -- tile n at column n * tw_ch, row 0 -- so
       existing sheets are unaffected; the grid layout passes both."""
    if cc0 is None:
        cc0 = tile * tw_ch
    if not attrs:
        die("the sheet has no attribute data; colour the tiles in "
            "ZX-Paintbrush so each cell carries its ink/paper")
    out = bytearray()
    for cr in range(th_ch):
        for cc in range(tw_ch):
            idx = (cr0 + cr) * cols + cc0 + cc
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
    ap.add_argument("--frames", type=int, default=1,
                    help="frames per unit; >1 makes the sheet a grid, one "
                         "column per frame and one row per unit")
    ap.add_argument("--mask", action="store_true",
                    help="also emit a dilated, inverted mask blob")
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

    if args.frames < 2 and w % args.tiles:
        die(f"sheet width {w} is not divisible by {args.tiles} tiles")
    tw, th, grid = frames_layout(pixels, args.tiles, args.frames)
    if tw % 8 or th % 8:
        die(f"tile size {tw}x{th} must be a whole number of 8x8 characters")

    # Frame 1 is what every machine gets, and is the sheet as far as the
    # rest of the pipeline is concerned.
    tiles = [tile_bytes(pixels, grid[t][0][0], tw, th, grid[t][0][1])
             for t in range(args.tiles)]

    # Masks, when asked for.  Kept as their OWN blob rather than appended
    # to the pixels: they are mostly solid runs, so ZX0 crushes them far
    # harder on their own than mixed in with sprite detail, and a build
    # that does not want them then costs nothing at all.
    masks = None
    if args.mask:
        # Every frame's ink must clear the cell edge, because the mask is
        # dilated from all of them together.
        for t in range(args.tiles):
            y0 = grid[t][0][1]
            for x0, _ in grid[t]:
                check_margin(pixels, x0, tw, th, args.name, t, y0)
        # ONE mask per sprite, from the frames COMBINED: union them, then
        # dilate the combined outline.  Both frames are inside it by
        # construction, so no frame can draw a pixel without its black rim
        # however different the two are -- which an explosion's frames very
        # much are.  The cost is a rim sized to the larger frame, so the
        # smaller one carries a slightly thicker edge.
        masks = b"".join(mask_bytes(pixels, [x0 for x0, _ in grid[t]],
                                    tw, th, grid[t][0][1])
                         for t in range(args.tiles))

    # Frame 2, pixels only -- it shares frame 1's mask and attributes.
    # Its own blob because it has its own destination: a RAM bank, which
    # only a 128K has (docs/PLAN.md P11).  The `_bank_` in the name is
    # what keeps tools/mkassets.py from sweeping it into the contended
    # block with everything else.
    frame2 = None
    if args.frames > 1:
        frame2 = b"".join(tile_bytes(pixels, grid[t][1][0], tw, th,
                                     grid[t][1][1])
                          for t in range(args.tiles))
    # Frame 1's attributes, positioned from the same grid the pixels use.
    # Passing h // 8 here was what made the two disagree: the attribute
    # slicing kept reading the whole sheet height as one tile while the
    # pixels had already moved to rows.
    blocks = [tile_attrs(attrs, w // 8, t, tw // 8, th // 8,
                         args.name, args.attr_mode,
                         grid[t][0][0] // 8, grid[t][0][1] // 8)
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

    f2data = None
    if frame2 is not None:
        f2raw = "/tmp/%s_f2.bin" % args.name
        f2comp = "/tmp/%s_f2.zx0" % args.name
        open(f2raw, "wb").write(frame2)
        if os.path.exists(f2comp):
            os.remove(f2comp)
        subprocess.run([args.zx0, "-f", f2raw, f2comp], check=True,
                       capture_output=True)
        f2data = open(f2comp, "rb").read()

    mdata = None
    if masks is not None:
        mraw = "/tmp/%s_mask.bin" % args.name
        mcomp = "/tmp/%s_mask.zx0" % args.name
        open(mraw, "wb").write(masks)
        if os.path.exists(mcomp):
            os.remove(mcomp)
        subprocess.run([args.zx0, "-f", mraw, mcomp], check=True,
                       capture_output=True)
        mdata = open(mcomp, "rb").read()

    MASK_RAW = len(masks) if masks is not None else 0
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
        f.write(f"#define {upper}_TILE_ROWS {th // 8}"
                f"   /* character rows    */\n")
        f.write(f"#define {upper}_TILE_H    {th}"
                f"   /* pixel rows        */\n")
        f.write(f"#define {upper}_TILE_SIZE {tw // 8 * th}"
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
        f.write(ZX0_NOTE % f"{U}_ZX0_INLINE")
        f.write(f"extern const uint8_t {args.name}_zx0[{len(zdata)}];\n"
                f"#ifdef {U}_ZX0_INLINE\n"
                f"const uint8_t {args.name}_zx0[{len(zdata)}] = {{\n")
        for i in range(0, len(zdata), 16):
            chunk = zdata[i:i + 16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
            f.write(",\n" if i + 16 < len(zdata) else "\n")
        f.write("};\n#endif\n\n")

        if mdata is not None:
            f.write(f"/* Mask: the sprite dilated one pixel, INVERTED, so\n"
                    f"   the blit is (screen AND mask) OR sprite with no\n"
                    f"   complement per byte.  {MASK_RAW} bytes -> {len(mdata)}\n"
                    f"   ZX0: mostly solid runs, which is why it is a blob of\n"
                    f"   its own rather than appended to the pixels.\n"
                    f"   Decompress to {upper}_MASK_RAW_SIZE bytes. */\n")
            f.write(f"#define {upper}_MASK_RAW_SIZE  {MASK_RAW}\n")
            f.write(ZX0_NOTE % f"{U}_MASK_ZX0_INLINE")
            f.write(f"extern const uint8_t {args.name}_mask_zx0[{len(mdata)}];\n"
                    f"#ifdef {U}_MASK_ZX0_INLINE\n"
                    f"const uint8_t {args.name}_mask_zx0[{len(mdata)}] = {{\n")
            for i in range(0, len(mdata), 16):
                chunk = mdata[i:i + 16]
                f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
                f.write(",\n" if i + 16 < len(mdata) else "\n")
            f.write("};\n#endif\n\n")

        if f2data is not None:
            f.write(f"/* Frame 2, pixels only: shares frame 1's mask and\n"
                    f"   attributes.  An ordinary asset in the contended\n"
                    f"   block, decompressed above MEM_TILES like the\n"
                    f"   sheets -- which is real RAM on a 48K and page 7 on\n"
                    f"   a 128K, so BOTH machines animate.  It was going to\n"
                    f"   be banked until the free space above MEM_END was\n"
                    f"   measured: 1334 bytes, and this needs 640.\n"
                    f"   {len(frame2)} bytes -> {len(f2data)} ZX0. */\n")
            f.write(f"#define {upper}_F2_RAW_SIZE  {len(frame2)}\n")
            f.write(ZX0_NOTE % f"{U}_F2_ZX0_INLINE")
            f.write(f"extern const uint8_t {args.name}_f2_zx0"
                    f"[{len(f2data)}];\n"
                    f"#ifdef {U}_F2_ZX0_INLINE\n"
                    f"const uint8_t {args.name}_f2_zx0"
                    f"[{len(f2data)}] = {{\n")
            for i in range(0, len(f2data), 16):
                chunk = f2data[i:i + 16]
                f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
                f.write(",\n" if i + 16 < len(f2data) else "\n")
            f.write("};\n#endif\n\n")

        f.write(f"#endif /* {guard} */\n")

    distinct = sorted({a for b in blocks for a in b})
    print(f"wrote {args.output}: {args.tiles} tiles of {tw}x{th}, "
          f"ZX0 {len(zdata)} B <- {len(blob)} B "
          f"({len(pixel_bytes)} pixel + {len(attr_bytes)} attr), "
          f"{args.attr_mode} attrs: "
          + " ".join(f"0x{a:02X}" for a in distinct))
    if f2data is not None:
        print(f"  frame 2: ZX0 {len(f2data)} B <- {len(frame2)} B  (frame 2)")
    if mdata is not None:
        print(f"  mask: ZX0 {len(mdata)} B <- {MASK_RAW} B "
              f"({100 - 100 * len(mdata) // MASK_RAW}% saved)")


if __name__ == "__main__":
    main()
