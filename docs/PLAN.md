# ZX Strategy — Implementation Plan

How to get from the current scaffold to the game in `docs/DESIGN.md`.

Two things drive the ordering:

1. **Walk the whole state graph first.** Every state in the design exists in
   `include/game.h` today, but `ST_OVER` and `ST_WON` are unreachable because
   nothing can win. Phase 0 makes the entire campaign loop walkable — title →
   play → level end → next level → campaign complete → title — before a single
   unit exists. After that, every phase is a vertical slice added to a game
   that already runs end to end.
2. **The board is tiny; exploit it.** 14x7 = **98 cells**. A full Dijkstra over
   the map is 98 nodes, a bitmap of the board is 13 bytes, and a byte per cell
   is 98 bytes. Algorithms that would be extravagant on a bigger grid are free
   here — what is *not* free is doing them inside the vblank window.


## Frame discipline

The one hard rule, from `docs/DESIGN.md` § The loop:

| Where | Budget | Use it for |
|-------|--------|------------|
| `update_state()` (in vblank) | ~256 bytes of screen writes, ~28 000 T | one or two cells repainted, one AI step |
| `enter_*()` (outside vblank) | a few frames of stall is fine | full repaints, map load, army placement |

So: **computation is chunked, never blocking.** A movement-range flood fill is
cheap enough for one frame; painting its highlight is not (20 cells x 16 attr
bytes = 320 bytes), so highlights paint N cells per frame exactly like the
existing page flip does with `PAGE_CELLS`.


## Decisions needed before Phase 1

Each of these changes the data structures, so all six were settled before any
of them got expensive. **All are now recorded in `docs/DESIGN.md`**; the table
is kept as the index of where each rule came from.

| Question | Recommendation | Why |
|----------|----------------|-----|
| ~~Unit stacking~~ | **Decided: one unit per tile, occupied tiles impassable** | Lets `occupancy[98]` be a single byte per cell and makes "who is here" O(1) |
| ~~Adjacency~~ | **Decided: 4-way, cursor included** | Matches per-tile movement costs; diagonals would need cost 1.4 or break the cost model |
| ~~Action model~~ | **Decided: one action per unit; a move ending adjacent to an enemy may also attack** | One `acted` bit per unit, set by either action |
| ~~Unit HP width~~ | **Decided: `uint8`** — Base is 255, not 500 | Saves 38 bytes and every damage subtraction is 8-bit |
| ~~Cursor vs paging~~ | **Decided: the cursor drives the page**, flipping when it leaves | The far base is on another page; the player must be able to look at it |
| ~~Turn order~~ | **Decided: all player units, then all enemy units; `SELECT` forfeits unspent actions** | One `acted` bit clears per side, and the turn counter is per round |

**Stalemate** is settled too, and the opposite way to the turn cap this plan
once proposed: there is none. A side reduced to immobile out-of-range units
can neither win nor lose, so the player leaves with `X` and no `ST_OVER`
message, because nothing was decided (`docs/DESIGN.md` § Stalemate).


## Data structures

Structure-of-arrays, not array-of-structs: SDCC on Z80 pays a multiply for
every `unit[i].field`, but an indexed byte array is `LD A,(HL)`.

```c
/* --- units: parallel arrays, index 0..UNITS_MAX-1 (38) --- */
uint8_t  u_type[UNITS_MAX];    /* UNIT_INFANTRY..UNIT_BASE, 0xFF = slot free */
uint8_t  u_cell[UNITS_MAX];    /* y * 14 + x, one byte, no coordinate pair    */
uint8_t  u_hp[UNITS_MAX];      /* 1..255; 0 is death                          */
uint8_t  u_flags[UNITS_MAX];   /* bit0 side (0 player, 1 enemy), bit1 acted   */
                               /* 4 x 38 = 152 bytes                          */

/* --- board-sized scratch, one byte per cell (98 each) --- */
uint8_t occupancy[98];         /* unit index, or 0xFF                          */
uint8_t cost[98];              /* Dial's output: movement cost, 0xFF = unreached */
uint8_t came_from[98];         /* direction stepped from, for path replay      */
uint8_t threat[98];            /* how many enemy units can hit this cell       */

/* --- Dial's bucket queue --- */
uint8_t q[128];                /* cells, monotonically non-decreasing cost     */
uint8_t bucket_end[MAX_MOVE + 1];

/* --- avoid the multiply in y * GRID_COLS + x --- */
static const uint8_t row_base[7] = { 0, 14, 28, 42, 56, 70, 84 };
```

Total 679 bytes of new RAM. There is room: the binary is ~12.5 KB at
`-zorg=32768`.

**Cell index, not (x,y).** One byte per position, neighbours are `±1` and
`±14`, and the only care needed is the left/right edge wrap — check `x` via a
`col_of[98]` table or `cell % 14` at setup, not in the inner loop.

**Deaths**: mark `u_type = 0xFF` and clear `occupancy`. Do *not* swap-remove —
compaction would invalidate the indices stored in `occupancy`, and iterating 38
slots is 38 byte compares.


## Algorithms

### Movement range — Dial's algorithm, not Dijkstra

Movement costs are 1 or 2 and budgets are ≤ 3, so a priority queue is
overkill. Dial's bucket queue processes cells in non-decreasing cost order with
no comparisons at all:

```
cost[all] = 0xFF; cost[start] = 0; push start into bucket 0
for c = 0 .. MAX_MOVE:
    for each cell in bucket c:
        if cost[cell] != c: continue                  /* stale entry */
        for each of the 4 neighbours n:
            if impassable[terrain[n]] or occupancy[n] != 0xFF: continue
            nc = c + move_cost[terrain[n]]
            if nc <= MAX_MOVE and nc < cost[n]:
                cost[n] = nc; came_from[n] = dir; push n into bucket nc
```

O(cells + edges) = at most 98 + 350 steps, and in practice a movement-3 unit
reaches at most 24 cells on open plains (9 from a corner, fewer again through
forest). One frame, comfortably. `cost[]` doubles as the highlight
set (`cost[i] != 0xFF`) and as the path source: `came_from[]` replays the route
backwards for animation without a second search.

The same routine serves the enemy: it is the only pathfinder in the game.

### Attack range — no pathfinding at all

Attack range is a distance, not a path (design § Attack Range). For a unit at
`c` with range `r`, walk the Manhattan disc around it — 41 cells at r=4 — and
test `occupancy`. Or, cheaper for target *cycling*: iterate the ≤19 enemy units
and test `|dx| + |dy| <= r`. 19 subtractions beats 41 cell lookups, and it
yields the target list already ordered for O/P cycling.

### Enemy decisions — one threat map per turn, then O(1) lookups

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

Cost per unit: one Dial's (≤ 450 steps) plus ≤ 24 candidate cells x 19 targets.
Well under a frame — but **run one unit per frame anyway**, as an AI state
machine driven from `update_state()`. Two reasons: the frame loop keeps
servicing vsync, and the player sees the enemy act unit by unit instead of the
board teleporting.


## Phases

Each phase ends in a working, playable-to-that-point build, verified in the
emulator with `.claude/skills/zesarux-test`.

### P0 — Walk every state (no rules)  ✱ highest priority

`ST_OVER` and `ST_WON` are implemented but unreachable. Add a temporary
`DEBUG_STATE_WALK` in `config/app_config.h` that, in `ST_PLAY`, maps two keys
to "win this level" and "lose this level" (set `player_won`, enter `ST_OVER`).

- **Deliverable**: title → play → win → level 2 loads → … → level 10 → win →
  `ST_WON` → title, all reachable by hand.
- **Acceptance**: read `level` and `terrain[]` over ZRCP at levels 1, 5 and 10
  and confirm each matches its `.tmx`; confirm `ST_WON` only appears after 10.
- **Removed in P4**, when real win detection replaces it.

### P1 — Units as data

`config/game_config.h` army composition → the SoA arrays → `populate_map(level)`
→ both renderers draw units.

- Bases at opposite corners, others within `UNITS_PLACE_RADIUS`, skipping
  impassable and occupied cells.
- `draw_view_cell()` / `draw_cell()` gain a unit layer: blit the terrain tile,
  then the unit sprite from `units_view`/`units_map`, then set the cell
  attribute by side (cyan player, red enemy — the sheets' 0x47 is a neutral
  default that the runtime overrides).
- **Acceptance**: 6 units per side visible on level 1 in both views; a
  screenshot per view; no cell shows two units.

### P2 — Selection and information

Cursor replaces the `@` party; SPACE selects the unit under it; the status
panel shows type / HP / range / movement. Cursor movement drives page flips.

- **Acceptance**: select and deselect every unit on the board; the panel
  matches the config table; walking the cursor off a page flips it.

### P3 — Movement

Dial's + highlight (painted N cells/frame) + move + `occupancy` update + acted
flag + end turn clears all acted flags and bumps the turn counter.

- **Acceptance**: an infantry on plains reaches exactly 3 tiles, 1 through
  forest+forest; water and occupied cells are never in the set; T-state
  measurement of the flood fill under 28 000.

### P4 — Combat and the real win condition

Target list by Manhattan distance, O/P cycling, SPACE confirms, damage with the
cover formula `(dmg * (100 - cover) + 99) / 100`, death, then the win check
over the loser's roster → `ST_OVER` with `player_won` set for real. Delete the
P0 debug keys.

- **Acceptance**: unit-test the damage table on the host (the numbers in
  `docs/DESIGN.md` § Cover); kill a base in the emulator and land in `ST_OVER`;
  win level 10 and land in `ST_WON`.

### P5 — Enemy turn

Threat map, per-unit AI state machine, one unit per frame, end-of-turn handback.

- **Acceptance**: an enemy adjacent to a player unit attacks rather than
  moving; an enemy with no target in range closes the distance and prefers
  cells with `threat == 0`; a full enemy turn never drops a frame (border
  timing stays inside the window).

### P6 — Balance and polish

Weights, a level indicator in the status panel, and
whatever the ten maps show up as unplayable.


## Risks

| Risk | Mitigation |
|------|------------|
| Highlight repaint blows the frame budget | Paint N cells/frame, reuse the `PAGE_CELLS` pattern |
| SDCC multiplies in inner loops | Cell indices + `row_base[]`; no `y * GRID_COLS` at runtime |
| Binary growth on 48K | ~12.5 KB now; measure per phase, and the maps are already ZX0'd |
| AI stalls the frame loop | One unit per frame, always; never loop the whole army in one call |
| Sprites are opaque, so units overwrite terrain | Accept for P1; masked sprites need `gfx.c`'s XOR path and a mask strip |
