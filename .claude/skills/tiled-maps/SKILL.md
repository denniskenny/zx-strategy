---
name: tiled-maps
description: Author Tiled (.tmx) tile maps for the ZX app, convert them to C headers with tools/tmx2header.py, wire them into the Makefile, and load them at runtime by converting Tiled GIDs into the engine's terrain ids.
when_to_use: "new map" or "add a map" or "tiled" or "tmx" or "edit the overworld" or "new level" or "add a tile type" or "map header"
allowed-tools: Bash Read Write Edit
effort: low
---

# Tiled Maps: Author → Convert → Load

Maps live in `assets/maps/*.tmx` (Tiled, https://www.mapeditor.org). The build
converts each one into a C header of **raw Tiled GIDs**; the runtime converts
those GIDs into its own terrain ids in memory at load time.

## The pipeline

```
assets/maps/NAME.tmx            (you author this — Tiled or by hand)
   │  tools/tmx2header.py  (Makefile pattern rule)
   ▼
include/NAME.h                  (constants + terrain table + NAME_gids[])
   │  #include from src/game.c
   ▼
load_map()                      (GID → terrain id conversion, in memory)
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
| `NAME_gids[]`, `NAME_COLS/ROWS`, `NAME_START_X/Y` | layer + `start` object |

Terrain id = `GID - NAME_GID_FIRST`, which is also the tile's column in the
`.zxp` tile sheets (`.claude/skills/zx-tiles`), so terrain types are pure data.

| Piece | Location |
|-------|----------|
| Converter | `tools/tmx2header.py map.tmx out.h [--name NAME]` |
| Makefile rule | `include/%.h: assets/maps/%.tmx` |
| Worked example | `assets/maps/overworld.tmx` → `include/overworld.h` |
| Runtime loader | `load_map()` in `src/game.c` |

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
  `WATER`, `HILLS`. Optional `impassable` (bool) blocks the party.
- **Tileset GIDs must be contiguous** and terrain names unique — the runtime
  uses `GID - firstgid` as an index into the terrain and tile-sheet tables.
- **GIDs must fit in a byte** (≤ 255 tiles, and no flipped/rotated tiles — Tiled
  encodes flips in the high bits of the GID).
- Only the **first tile layer** is read. Object layers are scanned for a point
  object named `start`, which becomes `NAME_START_X` / `NAME_START_Y` in tile
  coordinates (pixel position floor-divided by the tile size).

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

## Adding a new map

1. **Author** `assets/maps/NAME.tmx` (copy the worked example or the template).
2. **Register the header** in the Makefile so `make assets` builds it and
   `make clean` removes it:
   ```make
   GENERATED_HEADERS = include/goo_data.h include/overworld.h include/NAME.h
   ```
   The pattern rule needs nothing else — `--name NAME` comes from the stem, so
   the array is `NAME_gids[]` and the constants are `NAME_*`.
3. **Ignore the generated header** — add `include/NAME.h` to `.gitignore`
   alongside the others (generated files are not committed).
4. **Use it** from C:
   ```c
   #include "../include/NAME.h"
   ```
   then convert in `load_map()` exactly as `src/game.c` does for the overworld.
5. **Build & check** (see Verifying).

## Size limits

`src/game.c` takes `GRID_COLS` / `GRID_ROWS` straight from the map header, so a
resized map propagates automatically — but two renderers constrain how big it
can get:

- `ST_MAP` draws the **whole world** as 2x2-character cells from `MAP_COL`,
  `MAP_ROW`, above the status panel. A `#error` guard in `src/game.c` fires if a
  map exceeds that; either shrink the map or write a scrolling/1x1 overview.
- `ST_PLAY` shows a `VIEW_COLS` x `VIEW_ROWS` **page** of the world, flipping
  pages as the party walks, so it is size-independent; cells past the world edge
  are blanked with `ATTR_VOID`.

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
make assets                 # runs tmx2header.py; prints size + start tile
cat include/overworld.h     # eyeball the GIDs against the .tmx CSV
make                        # compiles; the #error guard catches oversized maps
make run                    # Fuse: ENTER to play, M for the overview
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
