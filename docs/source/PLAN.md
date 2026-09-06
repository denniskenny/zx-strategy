# ZX Strategy — Implementation Plan

How to get from the scaffold to the game in `docs/DESIGN.md`.

**Where we are: P0–P5, P7, P8 and P11 are done, one tap serves both
machines, and the three suites (render_paths, p0_state_walk, pixel_hash)
are green.  Only P6 — balance — is substantially outstanding, plus
Stalemate, which is the last thing in DESIGN.md that is not built.**

*(historic)* **P0–P3 and P7 are done, and the +3 crash is fixed. P4 —
combat and the real win condition — is next, with nothing blocking it.**

The game today places two armies on any of the ten maps, scrolls a pinned-cursor
view over the board, lets the player pick a unit up, shows what it can reach,
moves it, spends its action and ends the turn. It runs on 48K, 128K and +3.
What it cannot do is shoot — so nothing can win except through the
`DEBUG_STATE_WALK` keys, and the enemy never moves.

Two smaller things remain open and neither blocks P4:

- **No +3 coverage in the tests.** ZEsarUX will not run the tap on
  `--machine P341`; a `.sna` snapshot bypasses the ROM menu on every model.
  The +3 bug survived a fully green suite for a whole session because of this.
- **Tearing on a 48K.** Accepted by design: one screen, nowhere to compose.
  128K and +3 are tear-free throughout, state changes and scrolling alike.

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
| ~~Cursor vs paging~~ | **Superseded by P7**: the cursor is pinned and the world scrolls under it | The paging answer below was right for a static view; the design now asks for a moving one |
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
  side (green player, red enemy). The colour pipeline was reworked later — see
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
  which keeps only bit 6 and throws ink and paper away: a unit is green or red
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

### P4 — Combat and the real win condition  ✓ done

Damage with the cover formula, death, and the win check over the loser's
roster → `ST_OVER` with `player_won` set for real.

**Built beyond the original plan**, because the rules grew (docs/DESIGN.md
§ Adjacency and § Selection and highlighting):

- mobile units strike only from adjacent, so `attack_reach()` returns 1 for
  anything that moves and the Cannon is the only ranged unit;
- the defender counter-attacks for half unless it dies, and a counter can
  kill and end the level -- so `check_win()` runs on each death, not once;
- damage scales with the attacker's health, which makes fights snowball;
- **O/P cycling was never built and is not coming.** It is replaced by
  enemy-selection mode: targets are highlighted on the board, the arrows
  walk a looped list, SPACE confirms, ENTER or fire 2 cancels;
- the enemy scores whole exchanges (`gain - counter * AI_W_COUNTER`) rather
  than damage dealt, and refuses trades below `AI_MIN_TRADE`.

The P0 debug keys are still there, behind `DEBUG_KEYS=1`, because
`tests/p0_state_walk.py` needs them. They cost 99 bytes and are out of the
shipping tap.

**Untested: balance.** Every rule works and both suites pass, but nobody
has played ten levels through with the snowball and the counter-attacks in
place. That is P6.

- **Acceptance**: unit-test the damage table on the host (the numbers in
  `docs/DESIGN.md` § Cover); kill a base in the emulator and land in `ST_OVER`;
  win level 10 and land in `ST_WON`.

### 48K / 128K render paths  ✓ done (not a numbered phase)

`hw_detect()`'s `is_128k` now selects between two ways of putting a whole
screen up (`docs/DESIGN.md` § Two machines, two render paths). On a 128K a full
repaint is composed into the display file the ULA is not showing and revealed
with one write to `0x7FFD`; on a 48K it is drawn where it always was. The seam
is two functions — `render_compose()` and `render_show()` — and nothing below
them knows which machine it is running on.

**The prerequisite was memory, and it was close.** Banking page 7 in at
`0xC000` hides anything of ours up there, and the binary was overrunning
`0xC000` by 497 bytes. Reclaiming the never-called XOR-sprite path
(`xor_sprite_16`, `xor_sprite_8`, `plot`, `write_blit_px` — 614 bytes of code
and bss that nothing had ever called) brought the top symbol from `0xBE6E`
under the line with 402 bytes to spare. `make map` now runs
`tools/checkmem.py` and fails if that is ever lost again.

If it does get tight, the place to move data is `0x6000-0x7FFF`: contended, but
contention only applies while the ULA is drawing and this program draws in the
vblank window.

- **Built and verified, then disarmed.** Page 7 banks in, a screen composes
  off-display and the flip shows it — all confirmed on `--machine 128k`. But
  it collides with the vertical blank and is held off by `shadow_ok` in
  `src/render.c` until P7 deals with that.
- **The collision**: `vsync_wait()` writes the floating bus sync marker to
  `0x5AC0` — attribute row 22 of the *page 5* screen — and spins until it sees
  that byte on the bus. The bus carries what the ULA is fetching, so once page
  7 is on display the marker is never fetched and the wait never ends. The
  game hangs on the title with input dead, which is a remarkably quiet way for
  it to fail. Switching the path on means `src/vsync.c` has to know which
  screen is live and write the marker to `0xDAC0` when it is page 7 — a change
  to assembly that the sync has been tuned against, so it belongs with P7.
- **A second trap, already fixed**: `main()` locked paging (`out (0x7FFD),
  0x30`, bit 5) before the render path ever ran, so the page-in and the flip
  were both silently ignored while `is_128k` stayed 1 — every full screen went
  into a bank the ULA was not showing. `main()` now skips the lock on a 128K
  and `render.c` owns that port.
- **Cost**: the binary went *down* 614 bytes overall — the dead code removed
  was larger than the paths added.
- **Note for testing**: on a 128K, a ZRCP read of `0x4000` is the *back*
  buffer, not what is on screen. Read `0xC000` when bit 3 of `page_reg` is set.
  `.claude/skills/zesarux-test` has the details.

### P7 — The scrolling view  ✓ done

*Numbered after P6 because it was asked for last, but it belongs here in the
order: it should land before P5, for the reason under Knock-on.*

`docs/DESIGN.md` § Cursor and movement replaces the paging view: the cursor
stops moving and the world moves under it. This is the largest rendering change
the project has had, and it is worth being clear that it buys feel rather than
capability — the board is equally playable today. Sized and staged accordingly.

**Settled by the design**: the cursor is pinned to a fixed screen cell; a
direction pushes the world one tile the other way; input is locked for the
length of the scroll; off the board is **sea**, not black; `ST_MAP` is
untouched and remains the fast way across the board.

#### What it costs

The view is 32x16 characters — full width, rows 1-16:

| | bytes |
|---|---|
| one character of scroll | 4 096 pixel + 512 attribute = **4 608** |
| one cursor step (a 4-character tile) | 4 sub-steps = **18 432** |
| the vblank window, for comparison | ~28 000 T, about **1 750** bytes |

So a single sub-step is 2.6x what fits in the window, and a cursor step is ten
times it. Two consequences follow and neither is avoidable by being clever
about the copy:

- **Presenting cannot be tear-free on a 48K** without chasing the raster down
  the screen. Accepted for now, to be revisited.
- **Smooth scrolling costs 4x jumping.** A full view redraw is also 4 608
  bytes, so moving the window a whole tile and repainting it is a quarter of
  the work. The scroll is bought purely for the look of it. Worth remembering
  if it turns out slow: the fallback is already written.

At roughly a frame per sub-step, a cursor step is ~80 ms and crossing the board
is about a second. That is the number to judge once it is on screen.

#### What the double buffer actually does

It is worth being precise, because it is easy to expect too much of it.

- **It does** take composition off the raster's clock. Building the view —
  terrain, unit sprites, the movement highlight, attributes, clipping at the
  sea — happens in RAM with no deadline, and only the finished bytes go to the
  screen. That is § Logic and rendering applied one level down: separate the
  slow, careful part from the part with a deadline.
- **It does not**, by itself, stop tearing on a 48K. Presenting is still a
  4 608-byte blit racing the beam.
- **On a 128K it can stop tearing completely.** Page 7 at 0xC000 is a second
  display file, selected by bit 3 of port 0x7FFD: compose into the shadow
  screen and flip the bit, and nothing is copied at all. `hw_detect()` already
  sets `is_128k`, so the machine is known. This is the single biggest win
  available here and it should shape the interface even if the 48K path lands
  first — `present()` is a page flip on one machine and a blit on the other.

**Buffer layout is a real decision, not a detail.** The screen is interleaved,
so presenting a *linear* 32x16 buffer is 128 separate 32-byte runs. Holding the
buffer in screen order instead makes it a handful of long runs. Decide before
writing the assembly, because it changes what the assembly looks like.

#### Steps, each one playable

1. **The model, still jumping.**  ✓ **done.** Signed view origin (`int8_t
   page_x/page_y`), cursor pinned to `CURSOR_VX/VY` = (3,1), sea beyond the
   board, window follows the cursor. No buffer and no assembly yet.
   - `set_page()` inverted: the window goes where the pinned cursor lands,
     with **no clamping** — that is what lets the cursor reach a corner, and
     both bases are in one.
   - The progressive page flip is gone: `start_page_flip()` and `cells_left`
     retired, because a step changes every cell and there is nothing left to
     reuse. `move_play_cursor()` repaints the window in one go.
   - **It tears**, as predicted: ~4 608 bytes against a ~256-byte window. The
     alternative was 16 frames a step, which is a third of a second to move
     one cell. Step 3 is what fixes it.
   - Verified: the cursor holds screen cell (3,1) across every step while the
     world slides under it; `(0,0)` is reachable with the window at origin
     `(-3,-1)` and 17 of 32 cells drawn as sea; both suites still pass.
2. **Compose and present.**  ✓ **done.** The view is built in a linear RAM
   buffer at 0x6000 (128 rows x 32 bytes, plus 512 attributes) and presented
   in one pass.
   - **This is where the speed came from, not the scroll.** Drawing straight
     to the screen cost **1 024 `scr_off()` calls** for a full repaint — one
     per pixel row per cell — because the display is interleaved and every
     row needs its address worked out. The buffer is linear, so composing
     needs *no* address arithmetic at all, and presenting needs 128 table
     lookups (`VIEW_OFF[]`, built once at startup) instead of 1 024
     computations.
   - `VIEW_COL` is 0 and tiles are a whole number of characters wide, so
     every copy is byte-aligned and nothing is ever shifted. A `#error`
     keeps that true.
   - `cell_art()` now returns the unit sprite *instead of* the terrain rather
     than over it. Sprites are opaque, so the terrain underneath was being
     composed and then completely overwritten — wasted work on every
     occupied cell, and a starting board has fourteen.
3. **Scroll.**  ✓ **done.** `scroll_view()` pushes the buffer one character at
   a time, four sub-steps per cursor step, presenting between each.
   - Shifting a linear buffer is a `memmove` per row; a vertical push is one
     move for the lot, because rows are contiguous.
   - The incoming edge is a **slice**: a column is a strided read (every
     fourth byte of the tile), a row is contiguous. That is the capability
     nothing had before.
   - Attributes are recomposed wholesale at the end rather than sliced in —
     512 bytes, and far simpler than tracking what the pinned cursor's flood
     and the range highlight should look like mid-slide.
   - **Verified the strong way**: after a scroll in each of the four
     directions, the screen is *byte-identical* to a from-scratch recompose
     at the same origin.
4. **128K shadow screen.**  ✓ **done, on 48K, 128K and +3.** Every whole
   screen is composed into the display file the ULA is not showing and revealed
   with one write to `0x7FFD`. A 48K takes the direct path unchanged.
   - **The buffers had to leave the paged window.** They are at **0x6000**,
     below the program: every byte kept above 0xC000 is a byte page 7 cannot
     have. Putting them *inside* page 7 above the screen was tried first and
     works on a 128K, but not a +3. 0x6000 frees the whole bank for everyone.
   - **0x6000 was rejected earlier on a wrong diagnosis.** It was blamed for a
     +3 crash that turned out to be `hw_detect()` clearing bit 4 of `0x7FFD`.
     Fixing that at source made the region available again — two rounds of
     moving 7 KB of memory were spent chasing the wrong cause.
   - **BANKM is why the +3 saw nothing.** `0x7FFD` is write-only, so the ROM
     keeps its copy at `0x5B5C` and writes it back whenever it touches paging.
     Setting bit 3 without updating BANKM let the ROM undo the flip within a
     frame: page 7 appeared never to display, so the title and the map came up
     blank while the board — composed into page 5 — looked perfect. **A 128K
     tolerated it and the suite stayed green.**
   - Paging is left OPEN (bit 5 clear) and bit 4 always set. `screens_init()`
     still proves a page-in survives before arming, so a locked port degrades
     to the 48K path rather than composing where nobody is looking.
   - **The scroll is tear-free too**, on any machine with a second screen.
     Every sub-step composes off-display and is revealed whole, so a cursor
     step is four clean reveals rather than four visible repaints. A 48K has
     nowhere to hide the work and still tears — that is the machine, not the
     design.
   - What made that safe is `copy_chrome()`. Flipping per sub-step shows both
     screens in turn, and the header, panel and legend were painted into one
     of them only — the board would have appeared over the previous state's
     furniture on alternate frames, a strobe rather than a tear. Copying the
     chrome **once** before the first sub-step is enough: four flips show each
     screen twice, so both are correct for the whole slide.

5. **Assembly.**  ✓ **done for the present.** `present_pixels()` is now Z80:
   128 rows of 32 unrolled `LDI`, with the screen offset per row read from
   `VIEW_OFF[]`.
   - `LDI` is 16 T-states a byte against `LDIR`'s 21, and there is no memcpy
     call per row — ~20 000 T a present, four times a cursor step.
   - **BC cannot hold the row counter**, because `LDI` decrements it. The
     counter lives in memory; ~40 T a row against the 160 the unrolling saves.
   - Measured end to end: the P0 walk fell from **27.6 s to 18.6 s**.
   - **Verified byte-identical**: after a scroll in each direction the screen
     still matches a from-scratch recompose exactly, pixels and attributes.
   - Two things had to be true first. The attribute half of the present was
     16 `memcpy` calls where the attribute area is *flat* — one 512-byte copy
     — and that saved 60 bytes, which is what made room for the unrolling.
   - Addresses are baked in (inline assembly cannot see C expressions) behind
     an `#error` that fails the build if `memmap.h` moves them.
   - Earlier attempts failed to compile: SDCC rejected `.rept`/`.endm` and the
     `0 (iy)` indexed operand. Explicit `LDI` lines and `ex de,hl` juggling
     work. Copy the shape of `border()` in `src/gfx.c` rather than
     re-deriving it.

   The buffer shift (`push_h`/`push_v`) is still C. It is a `memmove` per row,
   which z88dk already turns into `LDIR`, so the win there is only the call
   overhead — a fraction of what the present gave.

#### Settle before step 1

- **Which screen cell the cursor sits on.** An 8x4 window has no centre cell;
  (3,1) and (4,2) are both "middle". It also decides how much of the board the
  player sees ahead of the cursor versus behind it.
- **Scroll granularity.** Four sub-steps a tile is the smooth option; two
  characters at a time is twice as fast and may look no worse in motion.
- **What the incoming edge needs.** A horizontal scroll brings in one character
  column, which may be the middle slice of a 4-character unit sprite — so the
  composer needs to render a *slice* of a cell, which nothing does today.

#### Knock-on

- **Do this before P5.** A scrolling view means the enemy turn has to bring
  each acting unit into view, or the player watches an empty stretch of sea
  while the turn happens somewhere else. Cheap to build into P5; expensive to
  retrofit.
- `draw_view_cell()` / `attr_view_cell()` take signed world coordinates once
  the window can leave the board.
- `set_page()`, `start_page_flip()` and `cells_left` retire with the paging
  view. The dirty-cell and recolour passes stay: they are what keeps a *static*
  view up to date, which is still most of the time.
- The movement-range highlight has to be composed into the buffer rather than
  written to the screen.

#### Risks

| Risk | Mitigation |
|------|------------|
| The present is too slow and the scroll crawls | Step 1 leaves a working jump-scroll to fall back to; measure at step 2 before committing to the assembly |
| Tearing on 48K | Accepted by decision; the 128K shadow screen path is the real answer and step 4 puts the seam in the right place |
| RAM for the buffer | Hand-placed in 0xC000-0xDC48 via `include/memmap.h`. **`make memmap` prints the whole picture** — linker-placed and hand-placed together, which neither the map file nor the header shows on its own. Today: 85 bytes free below 0xC000, 9 144 above. The squeeze is on *code*, not data, so moving more arrays up buys almost nothing |
| The board reads as tiny once surrounded by sea | An 8x4 window on a 14x7 island shows a lot of water. If it looks wrong, the answer is bigger maps, which is a `.tmx` change and not a code one |

### The +3 problem  ✓ SOLVED

The game crashed on a real +2A/+3 with "Nonsense in BASIC" immediately after
loading, and before that rendered its text as garbage. Both were the same
fault, in `hw_detect()`, and neither could be seen on a 48K or a 128K.

**Bit 4 of port 0x7FFD is the ROM select.** `hw_detect()` probes for 128K by
bank-switching, and every value it wrote — `0x01`, `0x02`, `xor a` — had bit 4
clear. It only ever cared about bits 0-2; the ROM came along as collateral.

- On a **128K** bit 4 picks ROM 0 (128 editor) or ROM 1 (48K BASIC). Landing on
  ROM 0 is survivable — it has a working interrupt handler — but its `0x3D00`
  is not a character set, so `print_at()` drew noise.
- On a **+2A/+3** the ROM number is two bits: `0x1FFD` bit 2 above `0x7FFD`
  bit 4. A 48K-format tap loads from **48 BASIC, which is ROM 3**, so clearing
  bit 4 drops it to **ROM 2: +3DOS**.
- And `hw_detect()` ran with **interrupts enabled**, so an IM 1 interrupt could
  vector to `0x0038` inside +3DOS. That is the crash.
- `main()` restored the ROM with `0x30` afterwards, but only when the mode-2
  floating bus was *not* in use — i.e. never on the machine that needed it.

The fix is two lines of intent: `di`/`ei` around the probe, and bit 4 set in
every value it writes (`0x11`, `0x12`, `0x10`). The ROM is now exactly as the
loader left it.

**It also fixed 128K vsync.** A 128K used to fall back to HALT sync, which had
been written off as a ZEsarUX emulation gap. It was not: interrupts were
firing during `vsync_detect()`'s timed probe, which runs immediately after
`hw_detect()`. A 128K now detects the floating bus and its sync marker checks
pass.

Two lessons worth more than the fix:

- **Three confident diagnoses were wrong first** — the paging lock, the buffers
  colliding with BASIC at 0x6000, the bank left selected at 0xC000. Each was
  reasoned from the two machines that could be tested, and each survived a
  fully green suite. The symptom that finally localised it was one the user
  supplied: *before the title paints*, which cut the search to five functions.
- **A green test run says nothing about a machine the tests cannot load.**
  `tests/render_paths.py` covers 48K and 128K; ZEsarUX could not run the tap on
  `--machine P341` at all. That gap is still open and is the obvious next
  investment: a `.sna` snapshot bypasses the ROM menu on every model.

### P5 — Enemy turn  ✓ done, both machines

*Was "done in the 48k build, blocked in the 128k one".  The blocker was
285 bytes; the 128k build now has ~1 000 clear and ships as ONE tap for
both machines.  The sub-plan below is kept because its analysis of where
the bytes went is still the best record of it.*

Threat map built once per turn, per-unit AI, unit-by-unit pacing with the view
travelling to whoever is acting, input discarded until control returns.

- `enemy_begin()` fills `threat[]` — one pass over the player's units, then a
  byte read per candidate cell instead of a range test per unit per cell.
- `pick_target()` prefers a kill, then the Base, then the lowest survivor.
- `ai_move()` scores every reachable cell on distance, threat and cover, with
  the weights in `config/game_config.h` so tuning is a rebuild.
- `game.c` paces one unit every 14 frames and brings the window to it. logic.c
  still never draws: it returns the cell and the loop does the rest.

**It does not fit the 128k target.** P5 costs 1 266 bytes and that build has
1 001 to spare, against a hard 16 KB ceiling it can never exceed — page 7 sits
at 0xC000, so code cannot live above it.

Two things were measured and are *not* the answer:

- **Strings are ~716 bytes in total**, padding included. Deleting every
  literal in the program still leaves the 128k build 285 bytes short.
- **P5's own code is a few hundred bytes.** Halving it does not close the gap.

The one item large enough is **`CRT_FONT_64`: 768 unused bytes** pulled in by
z88dk's zx crt. Nothing in this program references it — `print_at()` reads the
ROM font at 0x3D00. `-pragma-define:CRT_ENABLE_STDIO=0` does not remove it and
every alternative `-startup=` value fails to build. Finding that switch is the
bounded piece of work that unblocks the 128k target.

Beyond that, the 128k build's route to more room is **paging tile and
animation data into the spare RAM banks** (1, 3, 4 and 6 are unused). Data
pages cleanly; code does not.

### P8 — One binary, both machines  (sub-plan)

There are two taps because the shadow screen wants page 7 mapped at 0xC000,
which caps code at 16 KB, and the 48k target gives that up to reach 0xFFFF.

#### The paging window: a dead end, and why

The first version of this plan proposed mapping page 7 only for the duration
of a copy — `di`, page in, copy from a low buffer, page bank 0 back, `ei` —
so code could live above 0xC000 *and* a 128K keep its shadow screen.

**The hardware allows it. This renderer does not.** `SCREEN_1` *is* 0xC000:

```c
#define SCREEN_1    ((uint8_t *)0xC000)     /* page 7, banked in below */
gfx_target(back ? SCREEN_1 : SCREEN_0);
```

`gfx_target()` aims `gfx_pix`/`gfx_attr` straight at page 7 and then everything
draws there — chrome, `print_at`, `set_attr_rect`, incremental
`draw_view_cell()`, the cursor stamp. Page 7 must stay mapped across arbitrary
game code, which is exactly what forbids code above 0xC000. Moving the page-in
changes nothing.

Confining paging to a copy would mean composing the whole screen low first.
That is 6912 bytes; below 0xC000 there are **758** free between MEM_END
(~0x7CAA) and the stack. Not close.

Step 1 was still worth doing and its result stands — **the window survives
paging on 48K, 128K and +3** (`src/pageprobe.c`, reported as PAGEWIN). What it
proved was a fact about the hardware. What it did not prove, and what the plan
conflated with it, was that this program's renderer could live with paging
confined to a copy.

#### The way that works: ship the 128k build for everyone

If all code is below 0xC000 anyway, **page 7 can stay mapped permanently, just
as it does today, and no paging window is needed at all.**

The cost is near zero, because the 48k build barely uses what it pays for:

| build | code top | vs 0xC000 |
|---|---|---|
| 48k | 0xC0A7 | **167 bytes over** — 16 217 clear of 0x10000 |
| 128k | 0xBFFB | 5 bytes under |

And `shadow_ok = 0` — the fallback written for 128Ks with locked paging — *is*
the 48K path. **Verified: `zxstrategy128.tap` runs correctly on ZEsarUX
`--machine 48k`.** PC in our code at 0xB4D5, banner row 0x45, hardware report
rows 3-5 at 0x47.

#### Constraints

1. **All code, rodata and bss below 0xC000.** Universal 16 KB ceiling at
   0x8000-0xBFFF, enforced by `checkmem --limit 0xC000` for every target.
2. **Stack below 0xC000** — already ~0x7FA0.
3. **Every 0x7FFD write keeps bit 4** and **updates BANKM (0x5B5C)**, both
   directions, no exceptions.
4. **No IM2 vector table above 0xC000** if the ROM handler is ever left.
5. 0xC000+ is data and screens only, never code, on any machine.

#### Memory map

```
0x4000-0x5AFF  screen 0 (page 5)                     ULA
0x5B00-0x5BFF  system vars (BANKM at 0x5B5C)
0x5C00-0x5FFF  ROM workspace
0x6000-0x7CAA  hand-placed: VBUF, VATTR, VIEW_OFF, tiles, logic arrays
0x7CAA-0x7FA0  spare (758 bytes)
0x7FA0         stack, grows down
0x8000-0xBFFF  ALL CODE + rodata + bss   <-- 16 KB, universal ceiling
0xC000-0xDAFF  48K: spare / scratch;  128K: page 7 = shadow screen
0xDB00-0xFFFF  uniform data region (9.4 KB) on BOTH machines
```

0xDB00-0xFFFF is the point: the common denominator, real RAM on a 48K and
page-7 RAM on a 128K, so the same addresses work on both with no conditional
accessors. It is already where the buffers live.

#### Bootstrap

1. BASIC: `CLEAR 32767 : LOAD ""CODE : RANDOMIZE USR 32768`
2. `hw_detect()` first — `di`/`ei`, bit 4 preserved, BANKM updated.
3. `vsync_detect()` — needs paging live, so it follows.
4. Restore a known map: bank 0 at 0xC000, ROM bit set, BANKM in step.
5. `screens_init()` — if `is_128k`, sentinel-test paging; on success map page 7
   and leave it, `shadow_ok = 1`. Otherwise `shadow_ok = 0` and draw in place.
   **This code already exists and is the path verified on a 48K.**
6. Decompress assets into 0xDB00+ — identical on both machines.
7. `game_run()`

#### Steps

1. **Trim 167 bytes from the 48k path.** `src/pageprobe.c` has served its
   purpose and is most of it; the debug keys are the rest.
2. **Delete the TARGET split**: one APP, `MEM_LIMIT = 0xC000` always,
   `BUILD_SHADOW` gone, `PROBE_SRC` gone.
3. **Point the test suite at the single tap**, which closes the asymmetry that
   has hidden faults here before — the suite currently drives the 48k build
   while a 128K owner should be handed the other one.
4. **Verify on all three machines**, 48K included: it is now running code paths
   it never has.

#### P9 — lowering -zorg (landed, but under review)

Buffers moved to 0xDB00 and `-zorg=24576`: **5 bytes clear became 8197**.
Both suites pass on 48K and 128K. `tests/lowmem.tap` is GREEN on 48K,
128K **and +3**, so the layout is safe on every machine — the +3 was the
one in doubt and is now cleared.

Two beliefs were tested, both wrong in opposite directions:

- **The floating bus does not need non-contended code.** The Makefile had
  asserted it did.  vsync still settles on 0x40FF and the marker still
  lands on the displayed screen.
- **Contention is expensive.** p0_state_walk went 19.7-21.1s to **30.4s**,
  ~50%% slower, because half the program now sits in contended
  0x6000-0x7FFF.

**Open decision:** 8 KB against half the speed. The better version is
selective — only cold code (logic.c: AI and pathfinding, once a turn, not
once a frame) at 0x6000, render path staying at 0x8000. z88dk has the
section machinery (CODE_0..n appear in the map, used for 128K banking)
but wiring a C module to a chosen address is unvalidated.

#### P10 — splitting the tap (research done, not built)

Needed to put asset blobs anywhere outside the one contiguous block
`-create-app` emits.  `org` in a user module is not enough — z80asm places
the section and appmake silently drops the bytes (see the zx-memory skill).

**appmake +zx already has the pieces**, so a tap builder need not be
hand-rolled from scratch:

```
--noloader     don't create the loader block
--noheader     don't create the header
--merge FILE   merge a custom loader from an external TAP
--blockname N  name of the code block
--clearaddr N  address to CLEAR at
--usraddr N    USR address to run from
```

Plus `-split-bin` on the assembler side (one binary per section) and
`--exclude-sections` / `--bankspace` / `--main-fence` on appmake's.

**And z88dk's banking is already wired for this.** The map shows
`__CODE_0_head = $C000`, `__CODE_1_head = $1C000` — addresses encoded as
`(bank << 16) | addr`, i.e. CODE_1 is bank 1 at 0xC000. Putting a blob in
`SECTION CODE_1` should place it in a bank and have appmake emit it as its
own block, with no custom loader at all. **That is the route to try first**,
since banked data is where this is heading anyway.

**But it does not help a 48K.** Bank blocks are 128K/+3 only. Freeing the
16 KB budget on a 48K still needs blobs in *contended low RAM*, which still
needs the custom two-LOAD loader. Two different problems sharing one
mechanism; do not conflate them.

**Tested.** `SECTION CODE_1` in a user module works as far as the tap:

```
_bank_probe_blob = $1C000      bank 1, offset 0xC000
tap 16471 -> 17499 bytes       the 1 KB block really ships
```

Unlike the `org` attempt, the bytes are there. But the tap looks like this:

```
HEADER BASIC  Loader       30 bytes   addr 0x000A
HEADER CODE   zxstrategy   16391      addr 0x8000
  data block                1024      <-- NO HEADER
```

**The bank block is headerless and the 30-byte loader only does one LOAD,
so nothing loads it.** appmake emits banked blocks for a program that
loads its own banks at runtime, which is the usual 128K pattern.

So a custom loader IS needed, and now we know the shape of the problem:
a headerless block cannot be read by `LOAD ""CODE`, which expects a
header. The loader must either get appmake to emit headers for the extra
blocks, or call the ROM's LD-BYTES with the flag byte set for headerless
data.

#### The layout this has to serve

- **One contended-RAM block at 0x6000-0x7FFF, loaded by EVERY machine.**
  Compressed graphics, read once at boot. This is the one that frees the
  16 KB code budget and it is not optional on a 48K.
- **Zero or more bank blocks, loaded only on a 128K/+3.** Extended
  graphics a 48K simply never sees; the game picks at runtime off
  `is_128k`.

The first is the important one and the harder one, because it must work
on a machine with no paging at all. Do not let the banked extras drive
the design.

#### What it costs

The **16 KB ceiling becomes universal**, so the banked-data work below stops
being optional for animations and more tiles. A 48K gets 0xC000-0xDAFF as
spare rather than code, which could later hold a second buffer for 48K tear
reduction.

### P11 — Animated sprites  ✓ done, and on BOTH machines

*Was "128K only, plan only, not started".  The second frame turned out to
fit in MEM_TILES on a 48K too, so the machines were never split.  Frozen
by FREEZE_ANIM=1 for tests/pixel_hash.py -- see
.claude/skills/test-design, because freezing it also hid animate() from
every test in the file.*

Two frames per unit, the second one 128K-only, driven by the at-rest rule
already settled in docs/DESIGN.md § Sprite masks and animation. Fix the
outstanding tile-rendering bugs first — this touches the same paths.

#### The sheet becomes a grid

`assets/units_view.zxp` today is a horizontal strip of four 32x32 tiles.
It becomes **two columns**: column 1 the units, column 2 each unit's second
frame, so unit *n* is row *n* and frame *f* is column *f*.

`tools/zxp_tiles_zx0.py` **only slices horizontally** — `tile_bytes()`
takes `x0 = t * tw` and reads the full sheet height. Two-dimensional
slicing is the first piece of work, and it is a change to the tool's
central assumption rather than an addition to it. Everything else in the
pipeline reads whatever the tool emits, so nothing downstream needs to
know.

#### One mask for both frames

Agreed, to save 512 bytes and a second decompression. It puts a real
constraint on the artwork:

**Frame 2 must keep its ink inside frame 1's outline.** The mask preserves
background wherever it is set, and the sprite is ORed over the top — so a
frame-2 pixel outside frame 1's dilated silhouette still draws, but with no
black rim around it. One limb sticking out further on the second frame and
that limb loses its outline, on that frame only, which reads as a
flickering edge rather than as a mask problem.

So `--mask` should **check frame 2 against frame 1's mask and fail the
build** if it has ink outside it, the same way the margin check already
refuses ink on a cell edge. The tool has the data; nobody should be finding
this by looking at a flickering knee.

#### Where the two frames live

Frame 1 goes where the sheet goes today: compressed in the **contended
block at 0x6000**, decompressed into `MEM_TILES` above 0xDB00.

Frame 2 into a **bank**, so a 48K neither loads nor pays for it, and
`is_128k` picks at runtime. **This is the hard part, and P10 already found
out why:**

- `SECTION CODE_1` reaches the tap, but as a **headerless block** that the
  BASIC loader does not load — appmake emits banked blocks for a program
  that loads its own banks at runtime.
- `LOAD ""CODE` cannot read a headerless block; it wants a header.

Three ways out, in the order I would try them:

1. **`tools/mktap.py` gives the bank block a header**, and the BASIC loader
   pages the bank in before loading into 0xC000 and pages back after —
   BASIC can do that with `OUT 32765`. The catch is that a 48K would run
   the same loader: the `OUT` is harmless there but the `LOAD` is not
   wanted, so the loader has to branch on machine type in BASIC, or the
   48K has to tolerate 512 bytes landing in spare RAM at 0xC000. The
   latter is probably fine and much simpler — worth checking before
   building the former.
2. **Load it low and copy it up.** The block loads into the contended
   window like any other asset, and a 128K copies it into the bank at boot
   and reuses the space. Costs a 48K the tape time and the asset-block
   bytes for something it never uses, which is what banking was meant to
   avoid.
3. **Two taps again.** Rejected in P8 for good reasons; recorded only so
   nobody re-proposes it as new.

#### Route 1 is dead: BASIC cannot be trusted to page

**Tested, three ways, all failed.** `tools/mktap.py --bank` and
`src/bankprobe.c` exist and work; the loading does not.

| loader | `bank_probe` | |
|---|---|---|
| `OUT 32765,17` / `16` | **2** | bank 1 readable, wrong contents |
| `OUT 32765,1` / `0` | **0** | never reached our code |
| `POKE 23388,n` + `OUT 32765,n` | **0** | never reached our code, PC in the ROM ISR at 90s |

The first is explainable: bit 4 is the ROM select, and setting it swaps
the 48K ROM in underneath the 128K editor mid-`LOAD`. Clearing it, with or
without keeping BANKM in step, stops the machine reaching our program at
all, and I could not establish why within the time spent.

**What matters more than the cause:** in the first case the game **booted
and rendered perfectly** -- title bar painted, PC in our code, shadow
screen armed -- while the data was simply absent. Every outward sign was
green. That is the third time on this project, after the `org` section and
the headerless `CODE_1` block, and it is exactly why the probe was written
rather than trusting the boot.

Do not revive this without a probe. `--bank` is left in mktap.py but
**nothing should use it**.

#### Option 1 is arithmetically out of reach

Freeing the 0xC000 window -- what the reference project does, and what
would make banking straightforward -- was measured rather than judged:

```
buffers to relocate        8138 bytes   (0xDB00..0xFACA)
contended window total     8096 bytes   (0x6000..0x7FA0)
                             -42 bytes over, before anything else
```

The buffers are larger than the whole contended window, so freeing
0xC000+ means they fill it and BOTH current occupants move above 0x8000:

```
needed above 0x8000        5690 bytes   (assets 1390 + logic.c ~4300)
free above 0x8000          3182 bytes
SHORTFALL                  2508 bytes
```

And `logic.c` cannot go back on its own -- it is ~4300 bytes against 3182
free, which is why it was moved in the first place.

**Every window is committed.** 0xC000+ holds the buffers and the shadow
screen, 0x6000-0x7FA0 holds the assets and cold code, 0x8000-0xC000 is
80% full. The reference can bank because it keeps 0xC000+ empty; that is a
choice made early and it is mutually exclusive with the one this project
made to buy code space.

#### So frame 2 is an ordinary asset, and BOTH machines animate

It goes above MEM_TILES with the other decompressed sheets -- **real RAM on
a 48K, page 7 on a 128K, the same addresses either way** -- so there is no
bank, no paging, no custom loader, no .sna, and no `is_128k` branch. One
code path, and a 48K animates too.

```
MEM_END .. top of RAM   0xFACA..0x10000   1334 bytes free
frame 2 decompressed                        640 bytes
left over                                   694 bytes
```

**This was the answer all along and step 3 need not have happened.** The
measurement that settles it is one line -- free space above MEM_END -- and
it went unrun because "128K only" was taken from the brief as a
constraint rather than as a guess. Measure the cheap thing first.

The 694 bytes left are the real ceiling: a THIRD frame needs another 640
and does not fit. Two frames is the design, not a starting point.

#### For the future: bootstrap should be able to load into pages

The one thing worth building before the banks are wanted in earnest is a
loader that can put a block into a page at 0xC000 -- `LOAD ""CODE` into a
low staging buffer, then `RANDOMIZE USR` a copier that pages, copies, and
pages back in machine code. Doing it that way round is what BASIC cannot
break, and it turns 64 KB of banks from a dead end into a warehouse. Worth
having in the bootstrap before something actually needs it, rather than
under the pressure of a feature that is waiting on it.

#### The 64 KB of banks: bank things you SWAP, keep things you ALTERNATE

Banks 1, 3, 4 and 6 are unused and hold 64 KB. **Banking gives storage,
not addressable memory**: reading a bank means paging it at 0xC000, which
evicts the buffers at 0xDB00 and the shadow screen with them, so every
banked byte has to be copied down into a low buffer before it can be used.

So the deciding test is not "is it big" but **"is it only needed one at a
time"**:

| | low RAM needed |
|---|---|
| alternate tileset -> the existing MEM_TILES | **none** |
| another tune -> the existing music buffer | **none** |
| extra levels -> MEM_TERRAIN, rewritten per level anyway | **none** |
| a second sprite frame live *alongside* the first | **640 bytes** |

The first three are SWAPS. The buffer already exists and is already
rewritten at a state change; the bank only changes where the bytes come
from. Genuinely free, and you need room for the largest single item rather
than for the whole warehouse.

The last is ADDITIVE, and that is why frame 2 does not belong in a bank:
animation alternates between two frames every tick, so neither can replace
the other, and the 640 bytes of low RAM are owed whatever you do. Once
that is paid the bank adds nothing but a loader and a paging window.

**When you next want the 64 KB, look for what is big AND needed one at a
time.** Levels and music qualify. Sprites in an animation loop never will.

#### Getting data into a bank: BASIC loads, our code pages

The route that sidesteps everything that failed above. A loader can

```
LOAD ""CODE            into a low staging buffer -- no paging
RANDOMIZE USR copier   pages the bank in, copies, pages back
```

with the paging in machine code, `di` and BANKM handled the way
src/pageprobe.c proved works. Repeat per block and the staging buffer is
reused, so 64 KB costs one buffer. **BASIC never touches 0x7FFD**, which
is what broke all three attempts.

#### Steps

1. Fix the outstanding tile-rendering bugs. This work touches
   `cell_layers()` and both slice paths; doing it on top of a known-good
   renderer is the difference between one bug and two.
2. Two-dimensional slicing in `zxp_tiles_zx0.py`, plus the frame-2 mask
   check. Verifiable on its own: build the sheet, read the blob sizes.
3. Prove a bank block loads, with a sentinel, on a 128K **and a +3**, and
   that a 48K still boots. Nothing depends on it until this passes.
4. Frame 2 into the bank, `is_128k` selecting the frame pointer.
5. The at-rest animation tick (§ Sprite masks and animation), which is the
   only part that touches the frame budget.

#### What it costs

512 bytes of bank on a 128K, nothing on a 48K, and `cell_layers()` gains a
frame index — it already returns the sprite pointer, so animation is a
different offset rather than a different path. That is the payoff for
having consolidated the three draw paths into it.

### A replacement font  ✓ built, and switched OFF

`make FONT=resident` puts *Adventure C - The Ship of Doom* (Artic, 1982)
in place of the ROM's font: `tools/mkfont.py`, `src/font.c`,
`src/font_rt.c`, and `assets/fonts/artic_ship_of_doom.ch8`.

**Not in the shipping build.**  768 bytes of 0x8000-0xBFFF is too much
for a typeface when that region had 1 499 bytes in it -- the font alone
was half of everything left.  `FONT=rom` is the default and costs
nothing; the ROM's font is already in the machine.

`FONT=bank` exists and is BROKEN on a 128K: the shadow screen and the
bank window are the same sixteen kilobytes, so paging the font in
replaces the screen being drawn to.  Kept for the finding.  See the top
of `src/font_rt.c` and `.claude/skills/zx0-layout` § What a bank can and
cannot hold.

The ROM cannot be overwritten, of course; "replacing the font" is holding
one in RAM and pointing print_at() at it.

### Removing the stdio console driver  (~570 bytes, not started)

`fputc_cons_generic` (438) + `generic_console_printc` (133) are linked into
every build and never called: the game has `print_at()`.  They are pulled
in by the zx target's own CRT, not by our code:

```asm
; lib/target/zx/classic/spec_crt0.asm:92
    ; We use the generic driver by default
    defc    TAR__fputc_cons_generic = 1
```

**Unconditional -- no IFNDEF, no CRT_* guard**, and `spec_crt0.asm` is the
only file in the zx target that references the driver.  That is why
`nostreams`, `nofileio`, `CLIB_STDIO_HEAP_SIZE=0`, `CRT_ENABLE_CLOSE=0`
and `CLIB_EXIT_STACK_SIZE=0` between them bought EIGHT bytes: all of them
are downstream of a decision already taken.  Line 96,
`defc TAR__clib_exit_stack_size = 32`, is unconditional for the same
reason.

**The only route: copy `spec_crt0.asm`, delete line 92, and point zcc at
the copy.**  The cost is owning a 300-line CRT file forever, including
whatever z88dk changes in it upstream.  Worth it when the program region
is genuinely full; not before.

### P6 — Balance and polish

Weights, a level indicator in the status panel, and whatever the ten maps
show up as unplayable.

Now the substantial phase rather than a tidy-up, because P4's rules changed
what the numbers mean and none of them have been retuned:

- **Does the Cannon dominate?** It is the only unit that shoots without
  being counter-attacked, so it never enters the health spiral. Three
  advantages on one unit whose only cost is being immobile. Its damage of
  30 is the dial.
- **Is the snowball too steep?** Wounded units hit softer, so the side
  that wins the first exchange tends to win all of them. Fun or brutal is
  a question for play, not for reading.
- **Is `AI_W_COUNTER 1` right?** 0 is the pre-rules control case; 2+ makes
  the enemy passive. See docs/DESIGN.md § The knobs.
- **§ Stalemate is still unbuilt** -- the last thing in DESIGN that is not.
  It needs a position where neither side can reach the other, which this
  AI may never produce; build it if play shows it.

Four bugs in this phase surfaced in play and none in the test suites, which
read states and pixels rather than rules. Expect balance work to be the
same: it cannot be tested, only played.


## Risks

| Risk | Mitigation |
|------|------------|
| Highlight repaint tears | Paint N cells/frame in the vblank window, reusing the `PAGE_CELLS` pattern — this is a raster constraint and does not go away |
| SDCC multiplies in inner loops | Cell indices + `row_base[]`; no `y * GRID_COLS` at runtime |
| Binary growth on 48K | 15 750 bytes at `-zorg=32768` plus 2 324 bytes of RAM; measure per phase, and every asset is already ZX0'd |
| A long operation looks like a crash | Banner before the work, input flushed after (`docs/DESIGN.md` § Long operations) |
| Sprites are opaque, so units overwrite terrain | **Still true, still accepted.** A unit hides its tile rather than standing on it. Masked sprites need an XOR blit and a second mask strip per sheet; the old `xor_sprite_*` routines were deleted to buy space under 0xC000, so restore them from git history rather than rewriting |
| A new colour collides with the sync marker | The converter rejects `0x02`/`0x03` per cell; `make assets` prints every colour each sheet uses. Keep the inventory in `.claude/skills/floating-bus-vsync` current |
