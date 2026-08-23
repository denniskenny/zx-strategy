# ZX Strategy — Implementation Plan

How to get from the scaffold to the game in `docs/DESIGN.md`.

**Where we are: P0–P3 are done. P4 (combat and the real win condition) is
next.** The game today places two armies on any of the ten maps, lets the
player select a unit, shows what it can reach, moves it, spends its action and
ends the turn — but nothing can shoot, so nothing can win except by the
`DEBUG_STATE_WALK` keys, and the enemy never moves.

Two things drove the ordering:

1. **Walk the whole state graph first.** P0 made the entire campaign loop
   walkable — title → play → level end → next level → campaign complete →
   title — before a single unit existed. Every phase since has been a vertical
   slice added to a game that already ran end to end, which is why each one
   could be tested in the emulator the moment it was written.
2. **The board is tiny; exploit it.** 14x7 = **98 cells**. A full Dijkstra over
   the map is 98 nodes, a bitmap of the board is 13 bytes, and a byte per cell
   is 98 bytes. Algorithms that would be extravagant on a bigger grid are free
   here — and because the game is turn-based, they may take as long as they
   like (`docs/DESIGN.md` § Long operations). What is *not* free is **drawing**
   the result: the raster does not wait.


## Frame discipline

One hard rule, and it is about **drawing, not thinking**. The source is split
along exactly that line (`docs/DESIGN.md` § Logic and rendering):

| File | Budget | Holds |
|------|--------|-------|
| `src/logic.c` | **none** — as long as it takes | the board, the armies, placement, the movement fill, the orders |
| `src/render.c` | **~256 bytes of screen writes a frame** | every routine that writes to the screen, and nothing else |
| `src/game.c` | — | the frame loop, the states, the keyboard: it decides *when* the other two run |

The game is turn-based, so **computation is never chopped up to fit a frame**.
Work that overruns runs to completion and the loop misses a vsync, or several.
Anything long enough to notice puts a banner on the hint line and throws away
the input made while it ran (`docs/DESIGN.md` § Long operations). This is why
no phase below has a T-state target: there is nothing for one to protect.

What *does* still have to be spread across frames is **painting**, because the
raster will not wait. A movement highlight is up to 25 cells x 16 attribute
bytes and a full page flip is ~4 KB, so both go out N cells per frame —
`RANGE_CELLS` and `PAGE_CELLS` respectively, drained by `render_tick()`.

When adding anything, the file it belongs in answers the budget question for
you. If you find yourself wanting to draw from `logic.c`, mark the cell stale
instead (`mark_dirty()`, `recolour_page()`) and let the renderer schedule it.


## Decisions that shaped the data structures  ✓ all settled

Each of these changes the data structures, so all were settled before any of
them got expensive. **All are now recorded in `docs/DESIGN.md`**; the table is
kept as the index of where each rule came from, and every one of them is built.

| Question | Decision | Why |
|----------|----------|-----|
| Unit stacking | One unit per tile, occupied tiles impassable | Lets `occupancy[98]` be a single byte per cell and makes "who is here" O(1) |
| Adjacency | 4-way, cursor included | Matches per-tile movement costs; diagonals would need cost 1.4 or break the cost model |
| Action model | One action per unit; a move ending adjacent to an enemy may also attack | One `acted` bit per unit, set by either action. The attack half lands in P4 |
| Unit HP width | `uint8` — Base is 255, not 500 | Saves 38 bytes and every damage subtraction is 8-bit |
| Cursor vs paging | The cursor drives the page, flipping when it leaves | The far base is on another page; the player must be able to look at it |
| Turn order | All player units, then all enemy units; `ENTER` forfeits unspent actions | One `acted` bit clears per side, and the turn counter is per round |

**Stalemate** is settled too, and the opposite way to the turn cap this plan
once proposed: there is none. A side reduced to immobile out-of-range units
can neither win nor lose, so the player leaves with `X` and no `ST_OVER`
message, because nothing was decided (`docs/DESIGN.md` § Stalemate).


## Data structures

Structure-of-arrays, not array-of-structs: SDCC on Z80 pays a multiply for
every `unit[i].field`, but an indexed byte array is `LD A,(HL)`.

This is what is actually in `src/game.c`, not a proposal:

```c
/* --- units: parallel arrays, index 0..UNITS_MAX-1 (38) --- */
uint8_t  u_type[UNITS_MAX];    /* UNIT_INFANTRY..UNIT_BASE, 0xFF = slot free */
uint8_t  u_cell[UNITS_MAX];    /* y * 14 + x, one byte, no coordinate pair    */
uint8_t  u_hp[UNITS_MAX];      /* 1..255; 0 is death                          */
uint8_t  u_flags[UNITS_MAX];   /* bit0 side (0 player, 1 enemy), bit1 acted   */
                               /* 4 x 38 = 152 bytes                          */

/* --- board-sized, one byte per cell --- */
uint8_t occupancy[98];         /* unit index, or 0xFF                          */
uint8_t cost[98];              /* Dial's output: movement cost, 0xFF = unreached */
uint8_t cell_cost[98];         /* cost to ENTER this cell, 0 = impassable      */

/* --- Dial's bucket queue: 4 buckets of 32 --- */
uint8_t  q[(MAX_MOVE + 1) * 32];
uint8_t *bucket_end[MAX_MOVE + 1];          /* write cursor per bucket        */
static uint8_t *const bucket_start[MAX_MOVE + 1] = { q, q+32, q+64, q+96 };

/* --- ROM tables that kill a multiply and a divide --- */
static const uint8_t row_base[7]  = { 0, 14, 28, 42, 56, 70, 84 };
static const uint8_t col_of[98]   = { 0,1,2,...,13, 0,1,2,... };
```

606 bytes of game state, plus `terrain[98]` and 1 620 bytes of decompressed
tile and sprite buffers — 2 324 bytes of RAM all told. The binary is 15 750
bytes at `-zorg=32768`, so there is plenty of room above it.

Two structures the plan originally listed are **not** built:

- `came_from[98]` — for replaying a path backwards to animate a move. Nothing
  animates a move; the unit is redrawn at its destination. 98 bytes unspent
  until something wants to watch a unit walk.
- `threat[98]` — the enemy's threat map, which belongs to P5 and arrives with
  it.

**Cell index, not (x,y).** One byte per position, neighbours are `±1` and
`±14`, and the only care needed is the left/right edge wrap — `col_of[]` gives
a cell's column in one indexed read, which is what the fill's inner loop needs.

**`cell_cost[]` is derived, never authored.** `load_map()` folds
`terrain_move_cost[]` and the `.tmx` passability flags into one byte per cell,
so the fill takes a single lookup where it would otherwise take three, and
passability still has exactly one source.

**Deaths**: mark `u_type = 0xFF` and clear `occupancy`. Do *not* swap-remove —
compaction would invalidate the indices stored in `occupancy`, and iterating 38
slots is 38 byte compares.


## Algorithms

### Movement range — Dial's algorithm, not Dijkstra  ✓ built (P3)

Movement costs are 1 or 2 and budgets are ≤ 3, so a priority queue is
overkill. Dial's bucket queue processes cells in non-decreasing cost order with
no comparisons at all:

```
cost[all] = 0xFF; cost[start] = 0; push start into bucket 0
for c = 0 .. budget:
    for each cell in bucket c:
        if cost[cell] != c: continue                  /* stale entry */
        for each legal neighbour n:
            if cell_cost[n] == 0 or occupancy[n] != 0xFF: continue
            nc = c + cell_cost[n]
            if nc <= budget and nc < cost[n]:
                cost[n] = nc; push n into bucket nc
```

A movement-3 unit reaches at most **25** cells — the Manhattan disc of radius
3 — and far fewer hemmed into a corner or crossing forest. `cost[]` is the
whole result: a cell is reachable exactly when `cost[cell] != 0xFF`, so it
doubles as the highlight set and nothing else is stored.

Only cells within the disc are ever queued, which is what bounds each bucket at
32 entries and `q[]` at 128 bytes.

The same routine will serve the enemy: it is the only pathfinder in the game.

### Attack range — no pathfinding at all  (P4)

Attack range is a distance, not a path (design § Attack Range). For a unit at
`c` with range `r`, walk the Manhattan disc around it — 41 cells at r=4 — and
test `occupancy`. Or, cheaper for target *cycling*: iterate the ≤19 enemy units
and test `|dx| + |dy| <= r`. 19 subtractions beats 41 cell lookups, and it
yields the target list already ordered for O/P cycling.

### Enemy decisions — one threat map per turn, then O(1) lookups  (P5)

The expensive-looking rule is "avoid player unit attack ranges". Computed
naively that is (enemy units) x (candidate cells) x (player units) = ~5 000
range tests per turn. Instead build a **threat map once per enemy turn**:

```
clear threat[98]
for each living player unit p:
    for each cell within p's attack range:  threat[cell]++
```

19 units x 41 cells = 779 increments, once. After that, scoring a candidate
cell is a single `threat[cell]` read.

Per enemy unit, then:

1. Targets in range from where it stands? Attack the best one — lowest
   `hp - damage_after_cover`, preferring a kill, preferring the Base.
2. Otherwise run Dial's from its cell and score every reachable cell:
   `score = -distance_to_nearest_target * W1 - threat[cell] * W2 + cover[terrain[cell]] * W3`
   and step to the best. Weights live in `config/game_config.h` so tuning is a
   rebuild, not a code change.
3. Mark it acted.

Cost per unit: one Dial's (~42 000 T) plus ≤ 24 candidate cells x 19 targets —
call it a frame each, and a fifth of a second for a full 19-unit army.

The enemy turn is a **long operation** (`docs/DESIGN.md` § Long operations): it
runs behind a banner with input discarded, and is under no obligation to fit
the frame. It should still step **unit by unit**, pausing long enough for each
move to register, but for the one reason that survives: the player has to see
what the enemy did rather than watch the board teleport. That is a pacing
decision now, not a scheduling constraint, so it belongs to whatever reads
best — not to `update_state()`'s budget.


## Phases

Each phase ends in a working, playable-to-that-point build, verified in the
emulator with `.claude/skills/zesarux-test`.

### P0 — Walk every state (no rules)  ✓ done

`ST_OVER` and `ST_WON` are implemented but unreachable. Add a temporary
`DEBUG_STATE_WALK` in `config/app_config.h` that, in `ST_PLAY`, maps two keys
to "win this level" and "lose this level" (set `player_won`, enter `ST_OVER`).

- **Deliverable**: title → play → win → level 2 loads → … → level 10 → win →
  `ST_WON` → title, all reachable by hand.
- **Acceptance**: read `level` and `terrain[]` over ZRCP at levels 1, 5 and 10
  and confirm each matches its `.tmx`; confirm `ST_WON` only appears after 10.
- **Removed in P4**, when real win detection replaces it.

### P1 — Units as data  ✓ done

`config/game_config.h` army composition → the SoA arrays → `populate_map(level)`
→ both renderers draw units.

- Bases at opposite corners, others within `UNITS_PLACE_RADIUS`, skipping
  impassable and occupied cells — and spilling outward when that block holds
  less land than the roster needs, which level 8's enemy corner does. Dropping
  the overflow instead cost that side four units and all of its cannons.
- `src/game.c` picks up `config/game_config.h` for the first time, so it also
  gains the guard the other generated tables have: `#if (TER_TYPES != TER_COUNT)`
  catches a terrain type added to the tileset but not to the cost/cover tables.
- `u_hp[i]` is seeded from `unit_health[type]`; the rest of the stats table is
  unread until P2's status panel and P3's movement.
- `draw_view_cell()` / `draw_cell()` gain a unit layer: blit the terrain tile,
  then the unit sprite from `units_view`/`units_map`, then colour the cell by
  side (cyan player, red enemy). The colour pipeline was reworked later — see
  *Colour* below — so the side's ink is now ORed over the sheet's BRIGHT flags
  rather than replacing a flat default.
- **Acceptance**: **7** units per side visible on level 1 in both views
  (3 infantry + 2 tanks + 1 cannon + 1 base — `UNITS_AT_LEVEL` at level 1 is
  just the start counts); a screenshot per view; no cell shows two units.

### P2 — Selection and information  ✓ done

A free cursor replaced the scaffold's `@` party marker; SPACE selects the unit
under it; the status panel shows type / HP / range / movement. Cursor movement
drives page flips.

- The panel grew a fourth line above the existing three (`ROW_UNIT` = 17), so
  both renderers now have to fit above `PANEL_TOP` rather than above `ROW_TURN`.
- The cursor is not a thing standing on the board, so terrain stopped blocking
  it — only the edge of the map does (`docs/DESIGN.md` § Movement Range).
  Passability constrains the unit being *ordered*, and is checked then.
- **Acceptance**: select and deselect every unit on the board; the panel
  matches the config table; walking the cursor off a page flips it.

### P3 — Movement  ✓ done

Dial's + highlight (painted N cells/frame) + move + `occupancy` update + acted
flag + end turn clears all acted flags and bumps the turn counter.

- The highlight is **attribute-only**: a range cell's art does not change, just
  its paper, so recolouring a cell costs 16 bytes where redrawing it costs 144.
  That is a separate, cheaper repaint pass from the page flip's
  (`RANGE_CELLS` = 8 a frame against `PAGE_CELLS` = 2), and the two are
  mutually exclusive per frame.
- `attr_view_cell()` is the single place that decides a play-view cell's colour
  — cursor, selected, occupied, in range, or bare terrain — so every repaint
  path (flip, cursor step, recolour) agrees without any of them knowing about
  the others. `attr_map_cell()` is its opposite number for the overview.
- **Acceptance**: an infantry on plains reaches exactly 3 tiles, 1 through
  forest+forest; water and occupied cells are never in the set. Verified
  against an independent Dijkstra over five boards — open plains, forest,
  water, an occupied cell, and mixed hills/city — written into the emulator
  over ZRCP and compared cell by cell.
- **Cost**: the worst case (movement 3, open plains, 25 cells reached) is
  **41 616 T**, about six tenths of a frame, so selecting a unit drops one
  frame and no banner is warranted. That is down from 68 248 T for the first
  working version: `memset` for the clear, a per-cell `cell_cost[]` folding
  three table lookups into one, bucket end-pointers instead of an indexed
  multiply, and inlining the relaxation to kill an IX-frame function call
  ninety times a fill. Recorded because P5 calls the same fill up to 19 times
  a turn, not because anything requires it to be smaller.

### Colour — per-cell attributes  ✓ done (not a numbered phase)

Asked for after P3 and done on its own: the build imports the attribute grid
from each `.zxp` and the runtime paints it, so colour is authored with the art
instead of being one flat byte per tile.

- Each sheet is now **one ZX0 stream**: the pixels for every tile, then one
  attribute block per tile. One decompression per sheet, and tile *t*'s colours
  at `NAME_ATTR_OFF + t * NAME_ATTR_SIZE`.
- Terrain uses `--attr-mode full` — the authored ink/paper/bright per character
  cell, so a 32x32 tile can be sixteen colours. Units use `--attr-mode bright`,
  which keeps only bit 6 and throws ink and paper away: a unit is cyan or red
  by side, but *which cells are lit* is the artist's, and that is the shading.
- `blit_attr_rect(col,row,w,h,src,or_mask)` in `gfx.c` does both jobs — mask 0
  copies an authored block, a side colour paints a shaded sprite.
- **The one thing the hardware refuses**: enemy units cannot be dimmed.
  Non-bright red on black is `0x02`, and `0x02 | 1` is the floating bus sync
  marker, so enemy ink carries BRIGHT permanently and the sheet's flags cannot
  take it away. Shading reads on player units only. The converter rejects
  `0x02`/`0x03` per cell in full mode, naming the tile and the cell.

### The three-way split  ✓ done (not a numbered phase)

`game.c` had grown to 1 655 lines and mixed two kinds of code with opposite
constraints. Split along the budget line (`docs/DESIGN.md` § Logic and
rendering):

- **`src/logic.c`** (~500 lines) — the board, the armies, placement, Dial's
  fill, the orders. Touches no screen memory at all, which is checkable:
  `grep 'print_at\|set_attr_rect\|write_blit' src/logic.c` comes back empty.
- **`src/render.c`** (~560 lines) — every routine that writes to the screen,
  plus the tile buffers and the repaint queues. Changes no game state, which
  is equally checkable.
- **`src/game.c`** (~520 lines) — the loop, the states, the keyboard.
- **`include/board.h`** is the contract between the first two;
  **`include/render.h`** is what a Z80 rewrite of the renderer must preserve.

Two things came out of it beyond the tidying. `render_tick()` now owns the
whole repaint schedule in one place instead of being inlined in
`update_state()`, and the three debts it services are explicitly mutually
exclusive per frame. And logic no longer sets render's counters directly — it
calls `mark_dirty()` / `recolour_page()` / `render_discard()`, so "what is
stale" and "when it gets painted" stopped being the same decision.

**Cost: +1 142 bytes** (15 750 → 16 892). Cross-module calls cannot be
optimised the way file-local statics could, and the new seam functions have
real prologues. About 180 bytes of it is `level_1.h`'s tables, which are
`static const` in a generated header and so get emitted once per including
translation unit — three copies now. Worth fixing if the binary ever gets
tight: have `tmx2header.py` emit `extern` declarations with one definition.
The movement fill is unaffected (40 470 T against 41 616 before — it and its
`RELAX` macro stayed in the same file).

### P4 — Combat and the real win condition

Target list by Manhattan distance, O/P cycling, SPACE confirms, damage with the
cover formula `(dmg * (100 - cover) + 99) / 100`, death, then the win check
over the loser's roster → `ST_OVER` with `player_won` set for real. Delete the
P0 debug keys.

- **Acceptance**: unit-test the damage table on the host (the numbers in
  `docs/DESIGN.md` § Cover); kill a base in the emulator and land in `ST_OVER`;
  win level 10 and land in `ST_WON`.

### P5 — Enemy turn

Threat map, per-unit AI, unit-by-unit pacing, end-of-turn handback. The whole
turn is one long operation: banner up, input discarded, banner down and the
legend back when control returns to the player.

- **Acceptance**: an enemy adjacent to a player unit attacks rather than
  moving; an enemy with no target in range closes the distance and prefers
  cells with `threat == 0`; each enemy move is visible as it happens rather
  than the board changing all at once; keys pressed during the enemy turn do
  nothing once it ends.

### P6 — Balance and polish

Weights, a level indicator in the status panel, and
whatever the ten maps show up as unplayable.


## Risks

| Risk | Mitigation |
|------|------------|
| Highlight repaint tears | Paint N cells/frame in the vblank window, reusing the `PAGE_CELLS` pattern — this is a raster constraint and does not go away |
| SDCC multiplies in inner loops | Cell indices + `row_base[]`; no `y * GRID_COLS` at runtime |
| Binary growth on 48K | 15 750 bytes at `-zorg=32768` plus 2 324 bytes of RAM; measure per phase, and every asset is already ZX0'd |
| A long operation looks like a crash | Banner before the work, input flushed after (`docs/DESIGN.md` § Long operations) |
| Sprites are opaque, so units overwrite terrain | **Still true, still accepted.** A unit hides its tile rather than standing on it. Masked sprites need `gfx.c`'s XOR path and a second mask strip per sheet — decide before the art gets detailed |
| A new colour collides with the sync marker | The converter rejects `0x02`/`0x03` per cell; `make assets` prints every colour each sheet uses. Keep the inventory in `.claude/skills/floating-bus-vsync` current |
