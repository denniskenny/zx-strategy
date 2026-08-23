---
name: zx-tiles
description: Add or edit terrain tiles and unit sprites for ZX Strategy — draw them in the ZX-Paintbrush strips (assets/tiles_{map,view}.zxp, assets/units_{map,view}.zxp), declare terrain in the Tiled tileset, and let the build ZX0-compress and wire them in with no C changes.
when_to_use: "add a tile" or "new terrain" or "new tile type" or "edit tile art" or "tile sheet" or "zxp tiles" or "change terrain colour" or "make swamp impassable" or "unit sprite" or "new unit type" or "edit unit art"
allowed-tools: Bash Read Write Edit
effort: low
---

# Terrain Tiles & Unit Sprites: Draw → Declare → Build

Graphics live in ZX-Paintbrush **strips**, one per renderer, at that renderer's
cell size — terrain and units use the same format and the same converter:

| Sheet | Cell size | Used by |
|-------|-----------|---------|
| `assets/tiles_map.zxp` | 16x16 px (2x2 chars) | `ST_MAP` campaign overview |
| `assets/tiles_view.zxp` | 32x32 px (4x4 chars) | `ST_PLAY` paged field view |
| `assets/units_map.zxp` | 16x16 px (2x2 chars) | units on `ST_MAP` |
| `assets/units_view.zxp` | 32x32 px (4x4 chars) | units on `ST_PLAY` |

Each holds **N cells side by side, in table order**. For terrain, column *i* is
terrain *i* is GID `firstgid + i` in `assets/maps/level_1.tmx`; for units,
column *i* is the *i*th entry of the unit table in `docs/DESIGN.md`. The build
converts each sheet with `tools/zxp_tiles_zx0.py` into a ZX0 blob plus a
per-cell attribute table; `load_tiles()` in `src/game.c` decompresses the
terrain sheets into RAM once at startup, and `draw_cell()` / `draw_view_cell()`
blit out of them.

```
assets/tiles_map.zxp  ──zxp_tiles_zx0.py──▶ include/tiles_map.h  (tiles_map_zx0[])
assets/tiles_view.zxp ──zxp_tiles_zx0.py──▶ include/tiles_view.h (tiles_view_zx0[])
assets/units_map.zxp  ──zxp_tiles_zx0.py──▶ include/units_map.h  (units_map_zx0[])   --attr-mode bright
assets/units_view.zxp ──zxp_tiles_zx0.py──▶ include/units_view.h (units_view_zx0[])  --attr-mode bright
assets/maps/level_1.tmx ──tmx2header.py──▶ include/level_1.h  (names, blocked, GIDs)
```

**Colour travels inside the blob, per character cell.** Each `NAME_zx0[]` is
the pixels for every tile followed by one attribute block per tile, compressed
as a single ZX0 stream, so a sheet costs one decompression and tile *t*'s
colours sit at `NAME_ATTR_OFF + t * NAME_ATTR_SIZE` in the unpacked buffer. A
32x32 tile therefore carries its own 4x4 block of attributes, not one flat
colour: a terrain tile can be several colours at once.

Everything the game needs about a terrain type is data: **art + colour** from
the `.zxp`, **name + passability** from the `.tmx`. Adding a type needs no C.

## Adding a tile type (the whole recipe)

1. **Draw it in both sheets.** Append one tile column to
   `assets/tiles_map.zxp` (16x16) and `assets/tiles_view.zxp` (32x32), and give
   its character cells an attribute in the sheet's attribute block. See *The
   .zxp format* below to do this without the GUI.
2. **Declare it in the tileset** — append a `<tile>` to `assets/maps/level_1.tmx`
   with the next id, in the same order as the sheet column:
   ```xml
   <tile id="4">
    <properties>
     <property name="impassable" type="bool" value="true"/>
     <property name="terrain" value="SWAMP"/>
    </properties>
   </tile>
   ```
   Bump the tileset's `tilecount`. Names become status-panel labels (truncated
   or padded to 8 characters) and `LEVEL_1_GID_SWAMP`.
3. **Bump the tile count** in the Makefile: `TILE_COUNT = 5`.
4. **Paint it into the map** — use the new GID in the layer CSV.
5. **Build**: `make assets && make`. Both converters print what they emitted;
   `src/game.c` has an `#error` that fires if the sheets and the tileset
   disagree on the count.

To *edit* an existing tile, only step 1 and `make` are needed. To recolour one,
change its attribute cells in the sheet — nothing else.

## Adding a unit type

Units are simpler — there is no `.tmx` side, so it is draw + count:

1. **Draw it in both unit sheets**, same column position in each:
   `assets/units_map.zxp` (16x16) and `assets/units_view.zxp` (32x32).
2. **Bump `UNIT_COUNT`** in the Makefile (currently 4).
3. **Extend the unit table** in `docs/DESIGN.md` in the same order —
   INFANTRY, TANK, CANNON, BASE — and the stat tables in
   `config/game_config.h` (`unit_range`, `unit_damage`, `unit_health`,
   `unit_movement`, and the start/per-level counts), which are indexed by the
   same id. `src/game.c` has a `#if` that fails the build if the sheets and
   `UNIT_TYPES` disagree, so a missed column is caught at compile time.

Two differences from terrain worth knowing:

- **Only BRIGHT is authored.** Both sides share one sprite and its ink is set
  by whose it is, so the unit sheets are converted with `--attr-mode bright`:
  the converter keeps bit 6 of each cell and throws ink and paper away.
  Changing a unit cell between `0x07` and `0x47` in the sheet *does* change the
  screen — that is the sprite's shading — but changing `0x47` to `0x46` does
  nothing.
- **The enemy is always bright, whatever the sheet says.** Non-bright red on
  black is `0x02`, which the floating bus sync marker reserves, so enemy ink
  already carries BRIGHT and ORing the sheet's flag over it cannot dim
  anything. Shading shows on player units only. A spent player unit is
  deliberately flattened to dim as its "already moved" signal.
- **Sprites are opaque**, like tiles: blitting one over a terrain cell replaces
  the whole cell rather than compositing. Masked or XOR'd units over terrain
  need `gfx.c`'s XOR sprite path plus a second mask strip — decide that before
  the art gets detailed, because it changes how the sheets are drawn.

`load_tiles()` unpacks all four sheets into `map_tiles[]`, `view_tiles[]`,
`unit_map_tiles[]` and `unit_view_tiles[]`, pixels and attributes together.
`attr_view_cell()` and `attr_map_cell()` are the only places that decide a
cell's colour: bare ground gets the terrain block copied straight in, a unit
gets `blit_attr_rect(..., ATTR_UNIT_P)` over its BRIGHT block, and the cursor,
selection and movement range flood the cell with one colour.

## The .zxp format

ZX-Paintbrush files are plain text, so a tile strip can be written or patched
with a script (and still opened in ZX-Paintbrush afterwards):

```
ZX-Paintbrush image
<blank line>
0011...   one line per pixel row, '1' = ink, width = tiles * tile_width
...
<blank line>
44 44 04 04 45 45 47 47     one line per character row, hex attributes
44 44 04 04 45 45 47 47
```

The attribute block is `height/8` lines of `width/8` hex bytes, one per
character cell. **Cells within a tile may differ** — the converter keeps each
one, so a 32x32 terrain tile can be four colours down its height, or sixteen
different cells if you like. Colour a tile by editing its cells here; nothing
else needs to change.

Patch or preview a sheet like this:

```bash
# preview as ASCII art
python3 - <<'PY'
for l in open("assets/tiles_view.zxp").read().split("\n")[2:]:
    if l and set(l) <= {"0", "1"}:
        print(l.replace("0", ".").replace("1", "#"))
PY
```

## Rules the converter enforces

- Sheet width must divide evenly by `--tiles`, and the tile size must be a
  whole number of 8x8 characters.
- Every tile needs attribute cells; they may vary freely within a tile.
- **No attribute 0x03 or 0x02** in `--attr-mode full` — 0x03 is the floating
  bus sync marker and 0x02 becomes 0x03 when the +2A/+3 bus ORs it with 1. The
  converter names the offending tile *and cell*. See
  `.claude/skills/floating-bus-vsync`, and update the attribute inventory there
  when you introduce a new colour. `--attr-mode bright` cannot trip this,
  since only bit 6 survives.
- Tiles are stored row-major, `tile_w/8` bytes per pixel row, concatenated in
  sheet order, then the attribute blocks in the same order, then the lot is
  ZX0'd as one stream.

## Changing tile *size*

`src/game.c` derives its geometry from the headers — `CELL_W`, `CELL_ROWS`,
`VIEW_CW`, `VIEW_CH` and the centring of the play window all come from
`TILES_*_TILE_W/ROWS`. Redraw a sheet at a different tile size and the layout
follows, guarded by two `#error` checks (overview fits above the status panel,
play view fits on screen).

Watch the **frame budget** when tiles get bigger. `write_blit()` is C, and the
full 8x4 page of 4x4 tiles is ~4 KB of screen writes — many times the ~28 000
T-states available after `vsync_wait()` (even `LDI` needs 16 T per byte). Two
consequences baked into `ST_PLAY`:

- The view **pages** instead of scrolling. A step inside the page repaints only
  the two cells that changed (~256 bytes); the page flips only when the cursor
  steps off the edge.
- A flip repaints `PAGE_CELLS` (2) cells per frame and freezes movement until it
  finishes, so no frame overruns the window. Raise the tile size or
  `VIEW_COLS`/`VIEW_ROWS` and you lower `PAGE_CELLS`, never the other way round.

If you want a cursor-centred scrolling view instead, it has to come with small
tiles (2x2 chars) — that is the only way a whole window fits in a frame.

## Verifying

```bash
make assets     # tile count, size, pixel/attribute split, ZX0 size, colours used
make            # #error catches sheet/tileset count mismatches
make run        # Fuse: SPACE for the field view, M for the overview
```

If a tile looks shifted or mirrored, check the sheet width and `--tiles`: the
converter splits purely by column, so a stray pixel row of the wrong length or
a miscounted tile shifts everything after it.

## Related

- `.claude/skills/tiled-maps` — the map itself, and the terrain table the
  tileset generates.
- `.claude/skills/compile-scr` — the other `.zxp`/`.scr` converters
  (`zxp2header.py` for row-major sprites, `zxp2zx0.py` for full-width banners).
- `.claude/skills/floating-bus-vsync` — attribute constraints and the frame
  budget the renderers have to live inside.
