---
name: tiled-maps
description: Author Tiled (.tmx) tile maps for the ZX app, convert them to C headers with tools/tmx2header.py, wire them into the Makefile, and load them at runtime by converting Tiled GIDs into the engine's terrain ids.
when_to_use: "new map" or "add a map" or "tiled" or "tmx" or "edit a level" or "new level" or "add a tile type" or "map header"
allowed-tools: Bash Read Write Edit
effort: low
---

# Tiled Maps: Author → Convert → Load

Maps live in `assets/maps/*.tmx` (Tiled, https://www.mapeditor.org). The build
converts each one into a C header of **raw Tiled GIDs**, ZX0-compressed; the
runtime decompresses one level at a time and converts those GIDs into its own
terrain ids in memory at load time.

**Naming**: one map per level, `level_N.tmx` → `include/level_N.h` →
`LEVEL_N_*` / `level_N_gids_zx0[]`. The campaign is `level_1` .. `level_10`,
all 14x7 and all sharing one tileset; `src/game.c` includes all ten and picks
one through `level_maps[]`.

## The pipeline

```
assets/maps/NAME.tmx            (you author this — Tiled or by hand)
   │  tools/tmx2header.py  (Makefile pattern rule)
   ▼
include/NAME.h                  (constants + terrain table + NAME_gids_zx0[])
   │  #include from src/game.c
   ▼
load_map()                      (ZX0 → terrain[], GID → terrain id in place)
```

Two-stage on purpose: the header is a faithful dump of what Tiled saved, so
re-ordering the tileset in Tiled cannot silently reinterpret existing map
data. The meaning lives in the tileset, which also generates the runtime's
terrain table:

| Generated | From |
|-----------|------|
| `NAME_GID_FIRST`, `NAME_TERRAIN_COUNT`, `NAME_GID_*` | tileset order |
| `NAME_terrain_names[]` (8-char status labels) | each tile's `terrain` property |
| `NAME_terrain_blocked[]` | each tile's optional `impassable` bool |
| `NAME_gids_zx0[]` (or `NAME_gids[]` without `--zx0`), `NAME_COLS/ROWS`, `NAME_START_X/Y` | layer + `start` object |
| `NAME_TERRAIN_SIG` | hash of the tileset, so levels can share one terrain table |

Terrain id = `GID - NAME_GID_FIRST`, which is also the tile's column in the
`.zxp` tile sheets (`.claude/skills/zx-tiles`), so terrain types are pure data.

| Piece | Location |
|-------|----------|
| Converter | `tools/tmx2header.py map.tmx out.h [--name NAME] [--zx0 PATH] [--shared-terrain]` |
| Makefile rules | `include/level_%.h: assets/maps/level_%.tmx` (before the catch-all `include/%.h: assets/maps/%.tmx`) |
| Worked example | `assets/maps/level_1.tmx` → `include/level_1.h` |
| Runtime loader | `load_map()` in `src/game.c`, level chosen by `level` |

## The campaign: ten levels, compressed, one shared tileset

Ten 14x7 maps would be 980 bytes of GIDs and ten identical copies of the
terrain tables, which a 48K machine notices. Two build flags avoid that:

- **`--zx0 PATH`** emits `NAME_gids_zx0[]` plus `NAME_RAW_SIZE` instead of
  `NAME_gids[]`. Each level drops from 98 bytes to 28-36. `load_map()`
  decompresses it *straight into* `terrain[]` — same size, no staging buffer —
  and converts the GIDs in place.
- **`--shared-terrain`** omits `NAME_terrain_names[]` / `NAME_terrain_blocked[]`.
  Levels 2-10 use level 1's, because every level embeds the same tileset.

That sharing is only safe while the tilesets agree, so the converter emits
`NAME_TERRAIN_SIG`, a hash of the tileset's order, names and `impassable`
flags. `src/game.c` `#error`s if any level's SIG differs from level 1's, and
again if any level's `COLS`/`ROWS` differ — `terrain[]` and both renderers are
sized from level 1.

**Ordering trap**: make picks the *first* matching pattern rule, so
`include/level_%.h` must appear before the catch-all `include/%.h:
assets/maps/%.tmx` in the Makefile. Behind it, levels silently build raw and
uncompressed, and the link then fails on the missing `level_N_gids_zx0`.

Adding level 11: author the `.tmx`, append `11` to `LEVELS` in the Makefile,
and add the blob and start tile to `level_maps[]` / `level_start[]` in
`src/game.c` (also extend the two `#if` guards).

Design rules the maps follow (see `docs/DESIGN.md`): `populate_map()` puts a
base in opposite corners, so **no corner may be water** and there must be a
land path between them; every level includes at least one `CITY`.

## Requirements on the .tmx

The converter is deliberately strict — it fails the build rather than emitting
data that looks plausible and plays wrong:

- **Orthogonal**, fixed size (not infinite).
- **Tile Layer Format = CSV.** Tiled's default is zlib-compressed base64; change
  it in *Map > Map Properties > Tile Layer Format*.
- **Tileset embedded in the `.tmx`**, not an external `.tsx`. In Tiled: right-click
  the tileset tab > *Embed Tileset*.
- **Every tileset tile carries a `terrain` string property** — the name becomes
  `NAME_GID_<TERRAIN>` and its status label. Existing names: `PLAIN`, `FOREST`,
  `WATER`, `HILLS`, `CITY`. Optional `impassable` (bool) stops units entering
  the tile — it does not stop the cursor, which goes anywhere on the map.
- **Tileset GIDs must be contiguous** and terrain names unique — the runtime
  uses `GID - firstgid` as an index into the terrain and tile-sheet tables.
- **GIDs must fit in a byte** (≤ 255 tiles, and no flipped/rotated tiles — Tiled
  encodes flips in the high bits of the GID).
- Only the **first tile layer** is read. Object layers are scanned for a point
  object named `start`, which becomes `NAME_START_X` / `NAME_START_Y` in tile
  coordinates (pixel position floor-divided by the tile size). That is where
  the **cursor** begins on the level — armies are placed by `populate_map()`
  around the two corners, not by the map — so it is worth putting somewhere the
  player can see something from.

## Hand-authoring template

Tiled is not needed to write a valid map — this is the whole format the
converter cares about (a 4x2 example):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<map version="1.10" tiledversion="1.10.2" orientation="orthogonal"
     renderorder="right-down" width="4" height="2" tilewidth="16" tileheight="16"
     infinite="0" nextlayerid="3" nextobjectid="2">
 <tileset firstgid="1" name="terrain" tilewidth="16" tileheight="16" tilecount="4" columns="0">
  <grid orientation="orthogonal" width="1" height="1"/>
  <tile id="0"><properties><property name="terrain" value="PLAIN"/></properties></tile>
  <tile id="1"><properties><property name="terrain" value="FOREST"/></properties></tile>
  <tile id="2"><properties><property name="terrain" value="WATER"/></properties></tile>
  <tile id="3"><properties><property name="terrain" value="HILLS"/></properties></tile>
 </tileset>
 <layer id="1" name="terrain" width="4" height="2">
  <data encoding="csv">
1,2,3,4,
4,3,2,1
</data>
 </layer>
 <objectgroup id="2" name="markers">
  <object id="1" name="start" x="16" y="0"><point/></object>
 </objectgroup>
</map>
```

GID = `firstgid + tile id`, so with `firstgid="1"` the tiles above are 1-4. The
CSV is row major, `width * height` values. Tiled opens this file fine, so a
hand-written map can still be edited in the GUI afterwards.

## Adding a new map (level)

1. **Author** `assets/maps/level_N.tmx` (copy `level_1.tmx` or the template).
   `NAME` below is the file stem, so `level_2` gives `LEVEL_2_*`.
2. **Register it** in the Makefile: append the number to `LEVELS`. The
   `include/level_%.h` rule and `GENERATED_HEADERS` (which expands
   `$(LEVEL_HEADERS)`) then pick it up, and `include/level_*.h` is already
   gitignored. A map that is *not* a level needs its own header added to
   `GENERATED_HEADERS` by hand.
3. **Use it** from C: `#include "../include/level_N.h"`, then add its blob and
   start tile to `level_maps[]` / `level_start[]` in `src/game.c` and extend
   the size/SIG `#if` guards.
4. **Build & check** (see Verifying).

## Size limits

`src/game.c` takes `GRID_COLS` / `GRID_ROWS` straight from the map header, so a
resized map propagates automatically — but two renderers constrain how big it
can get:

- `ST_MAP` draws the **whole world** as 2x2-character cells from `MAP_COL`,
  `MAP_ROW`, above the status panel. A `#error` guard in `src/game.c` fires if a
  map exceeds that; either shrink the map or write a scrolling/1x1 overview.
- `ST_PLAY` shows a `VIEW_COLS` x `VIEW_ROWS` **page** of the world, flipping
  pages as the cursor moves, so it is size-independent; cells past the world
  edge are blanked with `ATTR_VOID`.

`terrain[]` is one byte per tile in RAM, plus the same again for the GID array
in the header — a 32x24 map costs ~1.5 KB total, which is fine at
`-zorg=32768`.

## Adding a new terrain type

No C changes are needed — see `.claude/skills/zx-tiles` for the full recipe.
In short: draw the tile in both `.zxp` sheets, append a `<tile>` with `terrain`
(and `impassable` if it blocks movement) to this tileset in the same order,
bump `TILE_COUNT` in the Makefile, and paint the new GID into the layer. An
`#error` in `src/game.c` fires if the sheets and the tileset disagree on the
count.

## Verifying

```bash
make assets              # runs tmx2header.py; prints size + start tile
cat include/level_1.h    # eyeball the GIDs against the .tmx CSV
make                     # compiles; the #error guard catches oversized maps
make run                 # Fuse: a key stops the tune, SPACE plays, M = overview
```

The converter prints `WxH = N tiles, start (x,y)` on success, and a `note:` line
for any terrain type declared in the tileset but unused by the layer — useful
when a map is meant to exercise every tile type.

## Pitfalls

- **Base64/zlib layer data** — the most common failure; the converter says so
  explicitly. Switch the format to CSV in Tiled and re-save.
- **External tileset** — `.tsx` references are rejected; embed the tileset.
- **Flipped tiles** — Tiled sets flip flags in the GID's high bits, which trips
  the "must fit in a byte" check. Draw the tile you want instead.
- **Editing the generated header** — pointless, it is overwritten by `make
  assets` and gitignored. Edit the `.tmx`.
- **Stale header** — generated headers are listed in `HEADERS`, so a `.tmx`
  change triggers a relink; if you edited the converter itself, `make clean`.
- **`start` object missing** — `NAME_START_*` are simply not emitted, and C
  fails to compile where they are used. Add the point object, or hardcode the
  spawn.
