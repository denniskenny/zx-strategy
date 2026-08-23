#!/usr/bin/env python3
"""Convert a Tiled .tmx map into a C header of tile GIDs.

The map is emitted exactly as Tiled stored it — one byte per tile, row
major, holding the layer's GID — either raw (NAME_gids[]) or ZX0'd
(NAME_gids_zx0[], with --zx0).  Alongside it the tileset becomes a small
terrain table: NAME_GID_* constants, NAME_terrain_names[] (status labels)
and NAME_terrain_blocked[] (from each tile's "impassable" property), all
in tileset order.  Terrain index = GID - NAME_GID_FIRST, which is also
the tile's column in the .zxp tile sheets, so adding a terrain type needs
no C changes.  load_map() in src/game.c does that conversion in memory.

An optional point object named "start" in any object layer becomes
NAME_START_X / NAME_START_Y in tile coordinates.

Only orthogonal maps with a single CSV-encoded tile layer are supported:
that is what Tiled writes by default and all the ZX runtime needs.

Every map in a campaign normally shares one tileset, so --shared-terrain
omits the per-map terrain tables (the levels would otherwise carry ten
identical copies of the names and blocked flags).  NAME_TERRAIN_SIG is
still emitted either way: it hashes the tileset, so src/game.c can fail
the build when a level's tileset has drifted from the one whose tables it
is borrowing.

Usage:
    python3 tools/tmx2header.py map.tmx output.h [--name NAME]
                                [--zx0 /path/to/zx0] [--shared-terrain]
"""

import os
import subprocess
import sys
import xml.etree.ElementTree as ET


LABEL_W = 8     # status-panel label width in src/game.c


def terrain_sig(first_gid, terrains):
    """A cheap 16-bit hash of the tileset: order, names and passability.

    Two maps that agree on this can share one terrain table."""
    h = first_gid & 0xFFFF
    for gid in sorted(terrains):
        name, blocked = terrains[gid]
        for ch in name:
            h = (h * 33 + ord(ch)) & 0xFFFF
        h = (h * 33 + (1 if blocked else 0)) & 0xFFFF
    return h


def compress(data, zx0, name):
    """ZX0 the GID array; returns the compressed bytes."""
    raw = f"/tmp/{name}_map.bin"
    comp = f"/tmp/{name}_map.zx0"
    open(raw, "wb").write(bytes(data))
    if os.path.exists(comp):
        os.remove(comp)
    subprocess.run([zx0, "-f", raw, comp], check=True, capture_output=True)
    return open(comp, "rb").read()


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_tileset(root):
    """gid -> (terrain name, impassable) from each tile's properties."""
    terrains = {}
    for tileset in root.findall("tileset"):
        if tileset.get("source"):
            die("external tilesets (.tsx) are not supported; embed the "
                "tileset in the .tmx")
        first = int(tileset.get("firstgid", "1"))
        for tile in tileset.findall("tile"):
            gid = first + int(tile.get("id"))
            prop = tile.find("properties/property[@name='terrain']")
            if prop is None:
                die(f"tile gid {gid} has no 'terrain' property")
            blocked = tile.find("properties/property[@name='impassable']")
            terrains[gid] = (prop.get("value").strip().upper(),
                             blocked is not None
                             and blocked.get("value") == "true")
    if not terrains:
        die("no tileset tiles with a 'terrain' property found")

    gids = sorted(terrains)
    if gids != list(range(gids[0], gids[0] + len(gids))):
        die(f"tileset GIDs must be contiguous (got {gids}); the runtime uses "
            "GID - firstgid as the terrain index and tile sheet column")
    names = [terrains[g][0] for g in gids]
    if len(set(names)) != len(names):
        die(f"duplicate terrain names in the tileset: {names}")
    return gids[0], terrains


def parse_layer(root, cols, rows):
    layer = root.find("layer")
    if layer is None:
        die("no tile layer found")
    data = layer.find("data")
    if data is None or data.get("encoding") != "csv":
        die("layer data must be CSV encoded (Tiled: Map > Properties > "
            "Tile Layer Format = CSV)")
    gids = [int(v) for v in data.text.replace("\n", "").split(",") if v.strip()]
    if len(gids) != cols * rows:
        die(f"layer holds {len(gids)} tiles, expected {cols}x{rows}")
    return layer.get("name", "terrain"), gids


def parse_start(root, tw, th):
    for group in root.findall("objectgroup"):
        for obj in group.findall("object"):
            if (obj.get("name") or "").lower() == "start":
                return (int(float(obj.get("x", "0")) // tw),
                        int(float(obj.get("y", "0")) // th))
    return None


def main():
    args = [a for a in sys.argv[1:]]
    name = None
    zx0 = None
    shared = False
    if "--shared-terrain" in args:
        args.remove("--shared-terrain")
        shared = True
    if "--name" in args:
        i = args.index("--name")
        name = args[i + 1]
        del args[i:i + 2]
    if "--zx0" in args:
        i = args.index("--zx0")
        zx0 = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        print(f"Usage: {sys.argv[0]} map.tmx output.h [--name NAME] "
              "[--zx0 /path/to/zx0] [--shared-terrain]")
        sys.exit(1)

    src, dst = args
    if name is None:
        name = os.path.splitext(os.path.basename(src))[0]

    root = ET.parse(src).getroot()
    if root.get("orientation") != "orthogonal":
        die("only orthogonal maps are supported")
    if root.get("infinite") == "1":
        die("infinite maps are not supported; set a fixed map size in Tiled")

    cols = int(root.get("width"))
    rows = int(root.get("height"))
    tw = int(root.get("tilewidth"))
    th = int(root.get("tileheight"))

    first_gid, terrains = parse_tileset(root)
    layer_name, gids = parse_layer(root, cols, rows)

    unknown = sorted({g for g in gids if g not in terrains})
    if unknown:
        die(f"layer uses gids not in the tileset: {unknown}")
    if max(gids) > 255:
        die("tile GIDs must fit in a byte for the ZX runtime")
    unused = sorted(t[0] for g, t in terrains.items() if g not in set(gids))
    if unused:
        print(f"  note: terrain types not used by the map: {', '.join(unused)}")

    start = parse_start(root, tw, th)
    sig = terrain_sig(first_gid, terrains)
    zdata = compress(gids, zx0, name) if zx0 else None
    upper = name.upper()
    guard = f"_{os.path.basename(dst).replace('.', '_').upper()}_"

    with open(dst, "w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"/* Generated from {src} by tools/tmx2header.py — do not edit. */\n\n")
        f.write(f"#define {upper}_COLS {cols}\n")
        f.write(f"#define {upper}_ROWS {rows}\n")
        if start is not None:
            f.write(f"#define {upper}_START_X {start[0]}\n")
            f.write(f"#define {upper}_START_Y {start[1]}\n")
        f.write(f"\n/* Terrain table, in tileset order.  Terrain index =\n"
                f"   GID - {upper}_GID_FIRST = tile column in the .zxp"
                f" sheets. */\n")
        f.write(f"#define {upper}_GID_FIRST {first_gid}\n")
        f.write(f"#define {upper}_TERRAIN_COUNT {len(terrains)}\n")
        f.write(f"#define {upper}_TERRAIN_SIG 0x{sig:04X}"
                "   /* tileset hash: order, names, passability */\n")
        for gid in sorted(terrains):
            f.write(f"#define {upper}_GID_{terrains[gid][0]} {gid}\n")
        if shared:
            f.write("\n/* --shared-terrain: the names/blocked tables live"
                    " with the first map of\n   the campaign; every map"
                    " with the same _TERRAIN_SIG can use them. */\n")
        else:
            f.write(f"\n/* Status-panel labels, padded to {LABEL_W}"
                    " characters. */\n")
            f.write(f"static const char *const {name}_terrain_names"
                    f"[{len(terrains)}] = {{\n")
            for gid in sorted(terrains):
                label = terrains[gid][0][:LABEL_W].ljust(LABEL_W)
                f.write(f"    \"{label}\",\n")
            f.write("};\n")
            f.write("\n/* 1 = the party cannot enter (Tiled property"
                    " \"impassable\"). */\n")
            f.write(f"static const uint8_t {name}_terrain_blocked"
                    f"[{len(terrains)}] = {{\n    ")
            f.write(", ".join("1" if terrains[g][1] else "0"
                              for g in sorted(terrains)))
            f.write("\n};\n")
        if zdata is None:
            f.write(f"\n/* Layer \"{layer_name}\": {cols}x{rows} GIDs,"
                    " row major. */\n")
            f.write(f"static const uint8_t {name}_gids[{cols} * {rows}]"
                    " = {\n")
            for r in range(rows):
                row = gids[r * cols:(r + 1) * cols]
                f.write("    " + ", ".join(str(g) for g in row))
                f.write(",\n" if r + 1 < rows else "\n")
            f.write("};\n\n")
        else:
            f.write(f"\n/* Layer \"{layer_name}\": {cols}x{rows} GIDs,"
                    f" row major, ZX0\n   ({len(zdata)} <-"
                    f" {cols * rows} bytes).  load_map() decompresses this"
                    " straight\n   into terrain[] and converts the GIDs"
                    " in place. */\n")
            f.write(f"#define {upper}_RAW_SIZE ({cols} * {rows})\n")
            f.write(f"static const uint8_t {name}_gids_zx0[{len(zdata)}]"
                    " = {\n")
            for i in range(0, len(zdata), 16):
                chunk = zdata[i:i + 16]
                f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
                f.write(",\n" if i + 16 < len(zdata) else "\n")
            f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")

    size = (f"ZX0 {len(zdata)} B <- {cols * rows} B" if zdata is not None
            else f"{cols * rows} tiles raw")
    print(f"Written {dst}: {cols}x{rows}, {size}"
          + (f", start ({start[0]},{start[1]})" if start else "")
          + (", shared terrain table" if shared else ""))


if __name__ == "__main__":
    main()
