# ZX Strategy

A ZX Spectrum 48K/128K application written in C (z88dk + SDCC), built around a
**floating bus vsync** so screen updates happen while the beam is off the
display.

It doubles as a **project template**: floating bus vsync, hardware detection,
graphics/input/PRNG helpers, a ZX0 asset pipeline with a runtime decompressor,
the Tritone (Beepola) beeper music pipeline, and the Claude skills that document
all of it.

The current program is a frame-synced game loop with switchable states
(title, play, campaign map, level end, campaign complete), polling
keyboard and Kempston input once per frame.

## Requirements

- [z88dk](https://z88dk.org) (`Z88DK` defaults to `$HOME/z88dk`)
- Fuse (`make run`) — reference emulator
- Optional: ZEsarUX + Python 3 for headless inspection (see `.claude/skills/zesarux-test`)

## Build & run

```bash
make            # → zxstrategy.tap (builds assets + music as needed)
make assets     # generated headers + music modules only
make run        # build + launch Fuse (48K)
make map        # rebuild with zxstrategy.map symbol map (for ZRCP debugging)
make probe      # build tests/fbprobe.tap, the floating bus histogram probe
make dzx0check  # build tests/dzx0check.tap, the ZX0 decompression harness
make clean
```

Override the toolchain or add compiler flags:

```bash
make Z88DK=/path/to/z88dk
make USER_CFLAGS="-m"
```

## Controls

| Key | Kempston | Action |
|-----|----------|--------|
| Q / A / O / P | up / down / left / right | Move the cursor (repeats when held) |
| SPACE | fire 1 | Go on — start the game, pick a unit up, order its move, close the overview, take the level-end screen on |
| ENTER, Z | fire 1 | End the turn (in play only — SPACE is giving orders there) |
| X | fire 2 | Back — drop the held unit, then return to the title |
| M | — | Campaign map (while playing) |

The Tritone tune plays itself whenever the title screen is entered — there is
no key that starts it. It blocks with interrupts off, so the title is
unresponsive until any key stops it; from cold, starting a game is one key to
stop the tune and then `SPACE`.

`SPACE` is the one key that moves you forward. Fire 1 is accepted alongside it
on the screens a joystick has to get through, because a Kempston stick has no
space bar.

Actions from the keyboard and the joystick are folded into one byte, so a single
edge test debounces both: a bit must be seen in two consecutive frames and then
fires on its rising edge.

The border flashes **green** while a state repaints its screen — the work SPACE
and ENTER cause — and is black otherwise. It used to mark every frame's work as
a budget meter, but a cursor step is now a multi-frame scroll and the meter
strobed on every step.

## Game loop

`game_run()` in `src/game.c` is one frame per iteration: `vsync_wait()` →
update the active state → poll input → optional state switch. States are
`ST_TITLE`, `ST_PLAY`, `ST_MAP`, `ST_OVER` and `ST_WON`; each has a
`render_*()` that paints its screen once, and only play and map do per-frame
work. To add a state: add the `ST_` id in `include/game.h`, a `render_*()` in
`src/render.c`, a case in `enter_state()`, and its transitions in
`handle_input()`. The states, their screens and their transitions are
specified in [`docs/DESIGN.md`](docs/DESIGN.md).

**The source is split by deadline, not by topic.** `src/logic.c` has none — the
game is turn-based, so the board, the armies and the movement fill may take as
long as they like. `src/render.c` has a hard one — roughly 256 bytes of screen
writes between `vsync_wait()` returning and the raster catching up — so big
repaints are paid off across frames by `render_tick()`. `src/game.c` decides
when each runs. Logic never draws and rendering never changes the game, which
is what makes the renderer replaceable by hand-written Z80 later. See
[§ Logic and rendering](docs/DESIGN.md).

Because the game is turn-based, logic that overruns a frame is allowed to: it
runs to completion behind a banner on the hint line, with the input made during
it discarded. Loading a level and playing the tune both work that way — see
§ Long operations in the design doc.

`ST_PLAY` is entered from the title and is the game proper: an **8x4 page** of
4x4-character terrain cells filling the screen width, both armies standing on
the grid loaded from the Tiled map, a free cursor, and a four-line status panel
below. **SPACE** picks up the unit under the cursor, washes the ground it can
reach in blue, and sends it there; **ENTER** ends the turn (fire 1 acts, it does not end turns). **M** opens
`ST_MAP`, the whole-world overview with its own cursor and the play cursor's
cell marked; **SPACE** dismisses it back to play.

## Layout

```
Makefile                 build + asset/music pipelines (-zorg=32768)
config/app_config.h      screen/keyboard constants + floating bus marker config
config/game_config.h     unit types and how many of each an army starts with
config/basic_config.mk   build config included by the Makefile
include/                 public headers
src/main.c               startup order: hw_detect → vsync_detect → paging lock
src/vsync.c              floating bus vsync (all assembly, __naked)
src/hw_detect.c          128K + Kempston detection
src/gfx.c                screen address maths, blits, XOR sprites, ROM-font text
src/input.c              keyboard half-rows + Kempston
src/prng.c               16-bit LFSR/Weyl PRNG
src/dzx0.c               ZX0 decompression (wraps z88dk's dzx0_standard)
src/game.c               the frame loop + states (decides WHEN)
src/logic.c              the board, armies and rules (no deadline)
src/render.c             everything that writes to the screen (has one)
include/board.h          the contract between logic and render
include/render.h         what a Z80 rewrite of the renderer must keep
assets/tiles_map.zxp     16x16 terrain tiles for the campaign overview
assets/tiles_view.zxp    32x32 terrain tiles for the play view
assets/units_map.zxp     16x16 unit sprites for the campaign overview
assets/units_view.zxp    32x32 unit sprites for the play view
assets/maps/level_*.tmx  the ten campaign levels, authored in Tiled
assets/music/            Tritone template/engine + one example tune
tools/                   asset + music converters, ZRCP profiler
tests/fbprobe.c          floating bus diagnostic histogram
tests/dzx0check.c        ZX0 decompression regression harness
docs/DESIGN.md           design doc: game states and transitions
.claude/skills/          floating-bus-vsync, compile-scr, tiled-maps,
                         zx-tiles, tritone-music, zesarux-test
```

## Assets (ZX0)

Drop a file in `assets/`, add the generated header to `GENERATED_HEADERS`, and
the Makefile pattern rules do the rest:

- `assets/NAME.scr` → `include/NAME.h` — whole screen, ZX0-compressed as
  `NAME_zx0[]`
- `assets/NAME.zxp` → `include/NAME.h` — ZX-Paintbrush sprite, row-major
- `assets/maps/NAME.tmx` → `include/NAME.h` — Tiled map, raw GIDs as
  `NAME_gids[]`
- `assets/tiles_*.zxp` → `include/tiles_*.h` — terrain tile strip: one ZX0
  stream holding the pixels for every tile followed by a **per-character-cell**
  attribute block per tile, so colour is authored with the art and a tile can
  be several colours at once
- `assets/units_*.zxp` → `include/units_*.h` — unit sprite strip, same
  converter but built with `--attr-mode bright`: ink and paper are discarded
  (a unit is green or red by side) and only the BRIGHT flags survive, which is
  the sprite's shading

At runtime: `dzx0_decompress(NAME_zx0, SCREEN);`. The compressor is
`$Z88DK/bin/z88dk-zx0` by default (`make ZX0=/path/to/zx0` to override).
Converters live in `tools/` (`zx0_to_header.py`, `zxp2header.py`, `zxp2zx0.py`,
`scr2header.py`, `scr_crop_zx0.py`, `scr_dither_reveal.py`).

The worked example is the tile sheets: `tools/zxp_tiles_zx0.py` packs each
strip into one ZX0 blob plus a per-tile attribute table, and `load_tiles()`
unpacks all four into RAM once at startup. For a full-screen graphic,
`tools/scr_crop_zx0.py` crops a `.scr` to its bounding box before compressing
and emits `PREFIX_CROP_*` placement constants named after the output header;
nothing in the game needs one today, so no Makefile rule invokes it.

**ZX0 v1 vs v2**: the stream format differs and the decompressor must match the
compressor. z88dk ships ZX0 v1.5 and the matching `dzx0_standard`, so
`src/dzx0.c` wraps the library routine rather than vendoring the widely copied
68-byte v2 decoder — which silently corrupts RAM when fed v1 data. `make
dzx0check` verifies the pair. See `.claude/skills/compile-scr`.

## Maps (Tiled)

The campaign is ten maps, `assets/maps/level_1.tmx` .. `level_10.tmx`, authored
in [Tiled](https://www.mapeditor.org) (orthogonal, CSV layer data, tileset
embedded in the `.tmx`). All ten are 14x7 and share one tileset whose tiles
carry a `terrain` property — `PLAIN`, `FOREST`, `WATER`, `HILLS`, `CITY`. A
point object named `start` marks where the cursor begins; the armies are placed
by `populate_map()` around the two opposite corners, not by the map.

`tools/tmx2header.py` converts each at build time into `include/level_N.h`,
ZX0-compressing the GID array (98 bytes → 28-36):

```c
#define LEVEL_1_COLS 14           /* ROWS, START_X, START_Y ...        */
#define LEVEL_1_GID_FIRST 1       /* terrain id = GID - GID_FIRST      */
#define LEVEL_1_TERRAIN_COUNT 5
#define LEVEL_1_TERRAIN_SIG 0xFD41 /* tileset hash: order/names/blocked */
#define LEVEL_1_GID_PLAIN 1       /* ... FOREST 2, WATER 3, HILLS 4, CITY 5 */
static const char *const level_1_terrain_names[5] = { ... };  /* labels  */
static const uint8_t level_1_terrain_blocked[5] = { ... };    /* movement */
static const uint8_t level_1_gids_zx0[36] = { ... };          /* the map  */
```

The data stays the **raw Tiled GIDs**, so re-ordering the tileset in Tiled can
never silently change what the data means. `load_map()` in `src/logic.c`
decompresses the current level straight into `terrain[]` (both are `COLS*ROWS`
bytes) and converts the GIDs in place (terrain id = `GID - LEVEL_1_GID_FIRST`),
then parks the cursor on that level's `START_*`. Status labels come from the
tileset's `terrain` properties and impassability from its `impassable` bools,
so a new terrain type needs no C changes.

Levels 2-10 are built with `--shared-terrain`: since every level uses the same
tileset, only `level_1.h` carries the name and passability tables and the rest
borrow them. `LEVEL_*_TERRAIN_SIG` hashes each tileset and `src/logic.c`
`#error`s if one has drifted — as it does if a level is a different size from
level 1, which `terrain[]` and both renderers are sized from.

To add level 11: author `assets/maps/level_11.tmx`, append `11` to `LEVELS` in
the Makefile, and add it to `level_maps[]` / `level_start[]` in `src/logic.c`.
See `.claude/skills/tiled-maps` for the full recipe, including a
hand-authorable `.tmx` template and how to add a terrain type.

## Terrain tiles (ZX-Paintbrush + ZX0)

Two tile strips hold the terrain art, one per renderer:

| Sheet | Tile | Used by |
|-------|------|---------|
| `assets/tiles_map.zxp` | 16x16 px (2x2 chars) | `ST_MAP` overview |
| `assets/tiles_view.zxp` | 32x32 px (4x4 chars) | `ST_PLAY` field view |

Each sheet holds the tiles side by side **in tileset order** — column *i* is
terrain *i*. `tools/zxp_tiles_zx0.py` slices them and ZX0-compresses the pixels
and the attributes together as one stream (720 → 348 bytes for the view sheet),
so **ink/paper is authored in ZX-Paintbrush** next to the art and arrives with
it. Each tile keeps its own block of attributes, one byte per character cell,
at `NAME_ATTR_OFF + t * NAME_ATTR_SIZE` in the unpacked buffer — a 32x32 tile
can be up to sixteen colours. `load_tiles()` decompresses all four sheets into
RAM once at startup and the renderers blit out of them.

Tile size drives the layout: `CELL_W`, `VIEW_CW` and friends in `include/render.h`
come from the generated headers.

**Why the field view pages instead of scrolling:** an 8x4 page of 4x4 tiles is
32x16 characters, ~4 KB of screen writes. Even with a hand-written blit that is
several frames' worth of work, so a cursor-centred scrolling window is off the
table at this tile size. Instead the cursor moves inside a fixed page — two
cells redrawn per step — and the page flips only when it steps off the edge,
repainting `PAGE_CELLS` tiles per frame so no frame overruns the vblank window.

Adding or editing a tile is data-only — see `.claude/skills/zx-tiles`.

## Music (Tritone / Beepola)

```
assets/music/NAME.txt  →  NAME.asm  →  NAME_linkable.asm  →  NAME_play()
         txt2tritone.py      gen_tritone_module.py --name NAME
```

The ~300-line Tritone player is factored into one shared module,
`assets/music/tritone_engine.asm`, extracted with `--engine` from
`assets/music/tritone_template.asm` — a Beepola "Tritone v2" export with its
song data stripped, which also supplies the engine/drums that `txt2tritone.py`
splices each new tune onto. Swap that file for another Beepola export to change
the engine, drum kit or volume mode for every tune at once.

Per-tune modules hold song data only. Declare tunes in `include/music.h`, add
the module to `MUSIC_LINKABLE`, and call `NAME_play()` from a **static screen**
— it blocks until a key is pressed. `assets/music/lowlands.txt` is the single
example melody. See `.claude/skills/tritone-music`.

## Floating bus notes

`vsync_wait()` supports three modes, chosen once at boot by `vsync_detect()`:

| Mode | Machine | Port | Loop |
|------|---------|------|------|
| 1 | 48K / 128K / +2 | 0x40FF | 35 T-states |
| 2 | +2A / +3 | 0x0FFD | 42 T-states |
| 0 | anything else | — | `ei / halt / di` fallback |

A unique attribute marker (`0x03`, black paper on magenta ink) is written across
attribute row 22 every frame; when the timed loop reads it back off the bus the
beam has just reached that row, leaving ~28 000 T-states of border and vblank
for tear-free drawing.

Constraints worth remembering when extending the app:

- Keep code in non-contended RAM (`-zorg=32768`) or the timed loop jitters.
- Never use attribute `0x03` (or any value that becomes `0x03` when ORed with 1)
  anywhere on screen.
- Keep attribute row 22 pixel-blank, and keep row 23 col 0 (the +2A/+3 preload
  byte) different from the marker.
- Run `hw_detect()` and `vsync_detect()` before locking 128K paging.

Full details, including the emulator support matrix, are in
`.claude/skills/floating-bus-vsync/SKILL.md`.

Reference: Ast A. Moore, *The Definitive Programmer's Guide to Using the
Floating Bus Trick on the ZX Spectrum*.
