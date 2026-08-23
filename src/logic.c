/*
 * logic.c — the board, the armies, and the rules that move them
 *
 * This is the half of the game with no deadline.  It is turn-based:
 * between orders nothing animates and nothing is waiting on a clock, so
 * a routine here may take as long as it needs and the frame loop simply
 * misses a vsync or several (docs/DESIGN.md § Logic and rendering).
 * That freedom is the point — it buys heuristics, flood fills and
 * whole-board scans that would be unaffordable under a frame budget.
 *
 * The one thing logic never does is draw.  It changes the board and
 * marks what the renderer owes the screen; src/render.c pays that off
 * against the raster's schedule, not this file's.
 *
 * Everything shared is declared in include/board.h and defined here.
 */

/* logic.c owns the campaign maps 2-10; render.c owns the tile sheets
   and level 1.  One definer each, and the claims must precede every
   include — see the note in any generated header. */
#define LEVEL_2_DEFINE_DATA
#define LEVEL_3_DEFINE_DATA
#define LEVEL_4_DEFINE_DATA
#define LEVEL_5_DEFINE_DATA
#define LEVEL_6_DEFINE_DATA
#define LEVEL_7_DEFINE_DATA
#define LEVEL_8_DEFINE_DATA
#define LEVEL_9_DEFINE_DATA
#define LEVEL_10_DEFINE_DATA

#include <string.h>

#include "../config/app_config.h"
#include "../config/game_config.h"
#include "../include/board.h"
#include "../include/dzx0.h"
#define LEVEL_2_DEFINE_DATA
#define LEVEL_3_DEFINE_DATA
#define LEVEL_4_DEFINE_DATA
#define LEVEL_5_DEFINE_DATA
#define LEVEL_6_DEFINE_DATA
#define LEVEL_7_DEFINE_DATA
#define LEVEL_8_DEFINE_DATA
#define LEVEL_9_DEFINE_DATA
#define LEVEL_10_DEFINE_DATA

#include "../include/level_1.h"
#include "../include/memmap.h"
#include "../include/level_2.h"
#include "../include/level_3.h"
#include "../include/level_4.h"
#include "../include/level_5.h"
#include "../include/level_6.h"
#include "../include/level_7.h"
#include "../include/level_8.h"
#include "../include/level_9.h"
#include "../include/level_10.h"
#include "../include/prng.h"
#include "../include/render.h"

/* The cost/cover tables in game_config.h are indexed by the same
   terrain id as the tileset, so a tile appended to one but not the
   other would read past the end of both. */
#if (TER_TYPES != TER_COUNT)
#error "game_config.h terrain tables and the .tmx tileset disagree on the terrain count"
#endif

/* --- The campaign: one ZX0'd GID array per level, all the same size
       and all sharing level_1's terrain tables (the build enforces the
       shared tileset with _TERRAIN_SIG, checked below).  A level costs
       ~35 bytes here instead of the 98 it occupies in terrain[]. --- */
static const uint8_t *const level_maps[LEVEL_COUNT] = {
    level_1_gids_zx0, level_2_gids_zx0, level_3_gids_zx0,
    level_4_gids_zx0, level_5_gids_zx0, level_6_gids_zx0,
    level_7_gids_zx0, level_8_gids_zx0, level_9_gids_zx0,
    level_10_gids_zx0
};

/* Where the cursor starts on each level (the .tmx "start" object). */
static const uint8_t level_start[LEVEL_COUNT][2] = {
    { LEVEL_1_START_X,  LEVEL_1_START_Y  },
    { LEVEL_2_START_X,  LEVEL_2_START_Y  },
    { LEVEL_3_START_X,  LEVEL_3_START_Y  },
    { LEVEL_4_START_X,  LEVEL_4_START_Y  },
    { LEVEL_5_START_X,  LEVEL_5_START_Y  },
    { LEVEL_6_START_X,  LEVEL_6_START_Y  },
    { LEVEL_7_START_X,  LEVEL_7_START_Y  },
    { LEVEL_8_START_X,  LEVEL_8_START_Y  },
    { LEVEL_9_START_X,  LEVEL_9_START_Y  },
    { LEVEL_10_START_X, LEVEL_10_START_Y }
};

/* terrain[] and both renderers are sized from level 1, and levels 2-10
   borrow its terrain names and passability, so a level that disagrees
   on either would silently mis-draw or read out of bounds. */
#if (LEVEL_2_COLS != GRID_COLS)  || (LEVEL_2_ROWS != GRID_ROWS)  \
 || (LEVEL_3_COLS != GRID_COLS)  || (LEVEL_3_ROWS != GRID_ROWS)  \
 || (LEVEL_4_COLS != GRID_COLS)  || (LEVEL_4_ROWS != GRID_ROWS)  \
 || (LEVEL_5_COLS != GRID_COLS)  || (LEVEL_5_ROWS != GRID_ROWS)  \
 || (LEVEL_6_COLS != GRID_COLS)  || (LEVEL_6_ROWS != GRID_ROWS)  \
 || (LEVEL_7_COLS != GRID_COLS)  || (LEVEL_7_ROWS != GRID_ROWS)  \
 || (LEVEL_8_COLS != GRID_COLS)  || (LEVEL_8_ROWS != GRID_ROWS)  \
 || (LEVEL_9_COLS != GRID_COLS)  || (LEVEL_9_ROWS != GRID_ROWS)  \
 || (LEVEL_10_COLS != GRID_COLS) || (LEVEL_10_ROWS != GRID_ROWS)
#error "every level must be the same size as level_1.tmx"
#endif
#if (LEVEL_2_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_3_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_4_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_5_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_6_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_7_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_8_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_9_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)  \
 || (LEVEL_10_TERRAIN_SIG != LEVEL_1_TERRAIN_SIG)
#error "a level's tileset differs from level_1's; they cannot share a terrain table"
#endif

/* ------------------------------------------------------------- state */


uint8_t unit_count;

uint8_t cursor_x, cursor_y;
uint8_t cur_x, cur_y;
uint8_t selected = NO_UNIT;
uint8_t sel_x, sel_y;

uint8_t level;
uint16_t turn;
uint8_t player_won;
uint8_t outcome_ready;

/* --- Locators for the hand-placed arrays -----------------------------
   terrain[], occupancy[], cost[], cell_cost[] and the unit arrays live
   at fixed addresses (include/memmap.h), so the linker emits no symbol
   for any of them.  Both the test harnesses and the debugger look names
   up in zxstrategy.map, and without these they simply cannot find the
   board any more — `symbol _terrain not found`.

   Two bytes each to keep everything inspectable, which is a bargain:
   the alternative is a game whose state cannot be read from outside. */
uint8_t *const at_terrain   = terrain;
uint8_t *const at_occupancy = occupancy;
uint8_t *const at_cost      = cost;
uint8_t *const at_cell_cost = cell_cost;
uint8_t *const at_u_type    = u_type;
uint8_t *const at_u_cell    = u_cell;
uint8_t *const at_u_hp      = u_hp;
uint8_t *const at_u_flags   = u_flags;

/* y * GRID_COLS without the multiply. */
const uint8_t row_base[GRID_ROWS] = { 0, 14, 28, 42, 56, 70, 84 };

/* cell % GRID_COLS without the divide.  The flood fill needs a popped
   cell's column to know whether stepping east or west would wrap it
   onto the next row — the one edge case of addressing the board as a
   flat array instead of a coordinate pair. */
const uint8_t col_of[CELL_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
};

#if (GRID_COLS != 14) || (GRID_ROWS != 7)
#error "row_base[] and col_of[] are written out for a 14x7 map; regenerate them"
#endif

/* --- Dial's bucket queue ---------------------------------------------
   A queued cell always has cost >= its Manhattan distance from the
   start, so only the diamond of radius MAX_MOVE is ever pushed: 25
   cells at MAX_MOVE = 3.  No single bucket can hold more than that
   however the terrain is arranged, which is what bounds q[]. */
#define Q_BUCKET    32
#define Q_DIAMOND   (2 * MAX_MOVE * MAX_MOVE + 2 * MAX_MOVE + 1)

#if Q_DIAMOND > Q_BUCKET
#error "Q_BUCKET is too small for MAX_MOVE; a bucket could overflow"
#endif

#define q ((uint8_t *)MEM_Q)

#if ((MAX_MOVE + 1) * Q_BUCKET) > 128
#error "the Dial queue no longer fits the block memmap.h reserves for it"
#endif

/* Where each bucket starts, and where its next entry goes.  A pointer
   pair rather than a count per bucket because a push then costs one
   16-bit load and a store: indexing q[nc * Q_BUCKET + n] instead makes
   SDCC emit a five-shift multiply and a pair of 16-bit adds every time,
   and pushing is the innermost thing the fill does. */
static uint8_t *bucket_start[MAX_MOVE + 1];
static uint8_t *bucket_end[MAX_MOVE + 1];

/* Filled once rather than initialised: q now lives at a hand-picked
   address (include/memmap.h) and SDCC will not take a cast pointer in a
   static initialiser. */
static void buckets_init(void)
{
    uint8_t i;

    for (i = 0; i <= MAX_MOVE; i++)
        bucket_start[i] = q + (uint16_t)i * Q_BUCKET;
}

/* Placement scratch: the free cells around one base, drawn from at
   random.  A base sits in a corner, so its radius block holds at most
   UNITS_PLACE_SLOTS usable cells (game_config.h does that arithmetic
   and refuses a roster that cannot fit). */
#define place_cand ((uint8_t *)MEM_PLACE_CAND)

#if UNITS_PLACE_SLOTS > 32
#error "place_cand no longer fits the block memmap.h reserves for it"
#endif
static prng_t place_rng;

/* ------------------------------------------------------------- armies */

uint8_t cell_of(uint8_t x, uint8_t y)
{
    return (uint8_t)(row_base[y] + x);
}

/* How many of one type a side fields on the current level.  The gain
   lands on odd levels, which is what UNITS_AT_LEVEL encodes. */
static uint8_t units_of_type(uint8_t type)
{
    return (uint8_t)(unit_start_count[type] +
                     unit_level_gain[type] * ((level - 1) >> 1));
}

/* Take a slot and stand a unit in it.  Returns 0 if the roster is
   full, which the level-10 army cannot do — UNITS_MAX is sized from
   the same tables — but a raised roster and a stale UNITS_MAX would. */
static uint8_t spawn(uint8_t type, uint8_t cell, uint8_t side)
{
    uint8_t i = unit_count;

    if (i >= UNITS_MAX) return 0;

    u_type[i]  = type;
    u_cell[i]  = cell;
    u_hp[i]    = unit_health[type];
    u_flags[i] = side;          /* U_ACTED clear: everyone starts fresh */
    occupancy[cell] = i;
    unit_count = (uint8_t)(i + 1);
    return 1;
}

/* The free cell nearest (x, y) by Chebyshev distance, scanning the
   whole 98-cell board.  Bases want their map corner, but a corner that
   is water — or that a converter change makes water later — must not
   silently drop a base, so the search is unconditional and the corner
   is simply the usual answer.  Writes the cell's coordinates back. */
static uint8_t nearest_free_cell(uint8_t *x, uint8_t *y)
{
    uint8_t bx = *x, by = *y;
    uint8_t cx, cy, dx, dy, d;
    uint8_t best = NO_UNIT, best_d = 0xFF;

    for (cy = 0; cy < GRID_ROWS; cy++) {
        for (cx = 0; cx < GRID_COLS; cx++) {
            uint8_t cell = cell_of(cx, cy);

            if (occupancy[cell] != NO_UNIT) continue;
            if (level_1_terrain_blocked[terrain[cell]]) continue;

            dx = (cx > bx) ? (uint8_t)(cx - bx) : (uint8_t)(bx - cx);
            dy = (cy > by) ? (uint8_t)(cy - by) : (uint8_t)(by - cy);
            d  = (dx > dy) ? dx : dy;

            if (d < best_d) {
                best_d = d;
                best = cell;
                *x = cx;
                *y = cy;
                if (d == 0) return cell;    /* the corner itself */
            }
        }
    }
    return best;
}

/* One side: its base in a corner, the rest of its roster scattered over
   the free cells within UNITS_PLACE_RADIUS of that base.  Candidates
   are collected once and drawn from without replacement, so placement
   never spins looking for a free tile and never stacks two units; if
   the block holds less land than the roster needs, the overflow is
   placed outward from the base rather than dropped. */
static void place_side(uint8_t side, uint8_t bx, uint8_t by)
{
    uint8_t x, y, x0, x1, y0, y1, n = 0;
    uint8_t type, count;
    uint8_t base_cell = nearest_free_cell(&bx, &by);

    if (base_cell == NO_UNIT) return;       /* a map with no land at all */
    spawn(UNIT_BASE, base_cell, side);

    x0 = (bx > UNITS_PLACE_RADIUS) ? (uint8_t)(bx - UNITS_PLACE_RADIUS) : 0;
    y0 = (by > UNITS_PLACE_RADIUS) ? (uint8_t)(by - UNITS_PLACE_RADIUS) : 0;
    x1 = (uint8_t)(bx + UNITS_PLACE_RADIUS);
    y1 = (uint8_t)(by + UNITS_PLACE_RADIUS);
    if (x1 > GRID_COLS - 1) x1 = GRID_COLS - 1;
    if (y1 > GRID_ROWS - 1) y1 = GRID_ROWS - 1;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            uint8_t cell = cell_of(x, y);

            if (occupancy[cell] != NO_UNIT) continue;
            if (level_1_terrain_blocked[terrain[cell]]) continue;
            if (n < UNITS_PLACE_SLOTS) place_cand[n++] = cell;
        }
    }

    for (type = 0; type < UNIT_TYPES; type++) {
        if (type == UNIT_BASE) continue;    /* one per side, already up */

        count = units_of_type(type);
        while (count--) {
            uint8_t cell;

            if (n) {
                uint8_t k = (uint8_t)(prng_next(&place_rng) % n);

                cell = place_cand[k];
                place_cand[k] = place_cand[--n];
            } else {
                /* The radius block ran out of land — level 8's enemy
                   corner is most of a lake.  Spill outward to the
                   nearest free cell instead of dropping the unit:
                   both sides field the same army whatever the map
                   looks like, because the map is meant to decide the
                   advantage and not the roster (docs/DESIGN.md,
                   "Game Init"). */
                uint8_t sx = bx, sy = by;

                cell = nearest_free_cell(&sx, &sy);
                if (cell == NO_UNIT) return;    /* no land left at all */
            }
            spawn(type, cell, side);
        }
    }
}

/* Try to reach cell N from the cell being scanned, at cost c.
 *
 * A macro rather than a function, which is not the usual trade: this is
 * the innermost thing the fill does — around ninety times per call —
 * and as a function SDCC spent over a third of each invocation on the
 * call itself.  A byte parameter goes on the stack, which forces an IX
 * frame to read it (push ix / ld ix,0 / add ix,sp ... pop ix), and c
 * and the budget had to become file statics to reach it, turning two
 * register reads into two memory loads.  Inlined, all four live in
 * registers and the call disappears.
 *
 * Everything it touches is a single-subscript table read, so the macro
 * evaluates N exactly once and needs no other hygiene. */
#define RELAX(N)                                                    \
    do {                                                            \
        uint8_t rn = (N);                                           \
        uint8_t sc = cell_cost[rn];     /* 0 = water, or no land */ \
                                                                    \
        if (sc && occupancy[rn] == NO_UNIT) {   /* one per tile */  \
            uint8_t nc = (uint8_t)(c + sc);                         \
                                                                    \
            if (nc <= budget && nc < cost[rn]) {                    \
                cost[rn] = nc;                                      \
                *bucket_end[nc] = rn;                               \
                bucket_end[nc]++;                                   \
            }                                                       \
        }                                                           \
    } while (0)

void movement_range(uint8_t start, uint8_t budget)
{
    uint8_t c;

    memset(cost, NO_COST, CELL_COUNT);
    for (c = 0; c <= MAX_MOVE; c++) bucket_end[c] = bucket_start[c];

    /* q[] has a bucket per point of MAX_MOVE and no more; a unit that
       could outrun them would write past the end. */
    if (budget > MAX_MOVE) budget = MAX_MOVE;

    cost[start] = 0;
    *bucket_end[0] = start;
    bucket_end[0]++;

    for (c = 0; c <= budget; c++) {
        const uint8_t *p = bucket_start[c];
        const uint8_t *e = bucket_end[c];   /* pushes from this bucket
                                               land in later ones, so
                                               the end cannot move */

        for (; p != e; p++) {
            uint8_t cell = *p;
            uint8_t x;

            /* A cheaper path reached this cell after it was queued, so
               its entry in this bucket is stale. */
            if (cost[cell] != c) continue;

            /* North and south simply fall off the array; west and east
               need the column, the one place a flat cell index can wrap
               onto the wrong row. */
            x = col_of[cell];
            if (cell >= GRID_COLS)              RELAX((uint8_t)(cell - GRID_COLS));
            if (cell < CELL_COUNT - GRID_COLS)  RELAX((uint8_t)(cell + GRID_COLS));
            if (x != 0)                         RELAX((uint8_t)(cell - 1));
            if (x != GRID_COLS - 1)             RELAX((uint8_t)(cell + 1));
        }
    }
}

/* Build both armies for the current level.  Called from load_map(),
   after terrain[] is in place — placement reads it for passability.
   The player takes the bottom-left corner and the enemy the top-right,
   the "opposite corners" of docs/DESIGN.md; the enemy base is
   therefore always on a different page of the play view, which is the
   reason the cursor is what flips pages. */
static void populate_map(void)
{
    uint8_t i;

    for (i = 0; i < UNITS_MAX; i++) u_type[i] = NO_UNIT;
    for (i = 0; i < CELL_COUNT; i++) occupancy[i] = NO_UNIT;
    unit_count = 0;

    /* Unit indices are about to be reused, so anything holding one is
       stale — including the player's selection from the level just won,
       and the repaints it had outstanding. */
    selected = NO_UNIT;
    render_discard();

    /* Seeded from the level, so a level's layout is the same every time
       it is played but different from its neighbours' — reproducible
       for testing, varied across the campaign. */
    place_rng.lfsr = (uint16_t)(0xACE1u + level * 0x1111u);
    place_rng.weyl = (uint16_t)(0x1234u + level);

    place_side(SIDE_PLAYER, 0, GRID_ROWS - 1);
    place_side(SIDE_ENEMY, GRID_COLS - 1, 0);
}

/* Load the current level.  Tiled stores a GID per tile; the header
   keeps them verbatim (ZX0'd), and the terrain id is the tile's index
   in the tileset — which is also its column in both .zxp sheets.  This
   is the only place that conversion happens, and out-of-range GIDs fall
   back to terrain 0. */
/* How far the selected unit can reach RIGHT NOW.  A unit that has
   already acted has moved, and a move only earns an adjacent strike. */
uint8_t attack_reach(uint8_t u)
{
    if (u_flags[u] & U_ACTED) return 1;
    return unit_range[u_type[u]];
}

/* Can the held unit hit whatever stands here?  Manhattan distance, no
   line of sight, no pathfinding — terrain neither blocks nor bends an
   attack, so this is arithmetic rather than a search. */
uint8_t is_target(uint8_t cell)
{
    uint8_t v, dx, dy, from;

    if (selected == NO_UNIT) return 0;
    v = occupancy[cell];
    if (v == NO_UNIT) return 0;
    if (((u_flags[v] ^ u_flags[selected]) & U_SIDE) == 0) return 0;

    from = u_cell[selected];
    dx = col_of[cell] > col_of[from] ? (uint8_t)(col_of[cell] - col_of[from])
                                     : (uint8_t)(col_of[from] - col_of[cell]);
    dy = cell > from ? (uint8_t)((cell - from) / GRID_COLS)
                     : (uint8_t)((from - cell) / GRID_COLS);
    return (uint8_t)(dx + dy) <= attack_reach(selected);
}

/* Has the side that just lost a unit lost the game?  A base counts as a
   unit, so "base destroyed" and "no units left" are one scan over the
   loser's roster (docs/DESIGN.md § Win Conditions). */
static void check_win(uint8_t loser_side)
{
    uint8_t i, alive = 0, base = 0;

    for (i = 0; i < unit_count; i++) {
        if (u_type[i] == NO_UNIT) continue;
        if ((u_flags[i] & U_SIDE) != loser_side) continue;
        alive = 1;
        if (u_type[i] == UNIT_BASE) base = 1;
    }
    if (alive && base) return;

    player_won = (uint8_t)(loser_side == SIDE_ENEMY ? 1 : 0);
    outcome_ready = 1;
}

/* Strike whoever is on this cell.  Cover is a percentage off the
   attacker's damage and ROUNDS UP, so an attack that lands always takes
   at least one point while cover is under 100% — no unit is ever
   unkillable by standing somewhere good. */
void attack(uint8_t cell)
{
    uint8_t v = occupancy[cell];
    uint8_t cover = terrain_cover[terrain[cell]];
    uint16_t raw = unit_damage[u_type[selected]];
    uint8_t dmg = (uint8_t)((raw * (100u - cover) + 99u) / 100u);
    uint8_t side;

    if (dmg == 0) dmg = 1;

    u_flags[selected] |= U_ACTED;

    if (u_hp[v] > dmg) {
        u_hp[v] = (uint8_t)(u_hp[v] - dmg);
        side = 0xFF;
    } else {
        /* Dead.  The slot is marked free in place and occupancy cleared;
           it is never compacted out, because occupancy[] holds indices
           into these arrays (docs/PLAN.md § Data structures). */
        side = (uint8_t)(u_flags[v] & U_SIDE);
        u_type[v] = NO_UNIT;
        u_hp[v] = 0;
        occupancy[cell] = NO_UNIT;
        mark_dirty(col_of[cell], (uint8_t)(cell / GRID_COLS));
    }

    deselect();
    recolour_page();
    if (side != 0xFF) check_win(side);
}

void load_map(void)
{
    buckets_init();
    outcome_ready = 0;

    uint8_t i, t;

    /* The compressed GIDs unpack straight into terrain[] — same 98
       bytes — and are converted in place, so no staging buffer. */
    dzx0_decompress(level_maps[level - 1], terrain);

    for (i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        t = (uint8_t)(terrain[i] - LEVEL_1_GID_FIRST);
        terrain[i] = (t < TER_COUNT) ? t : 0;
    }

    /* Flatten terrain into the per-cell entry cost the flood fill
       reads, with impassable terrain as cost 0.  Doing it once per
       level here is what lets the fill's inner loop take a single
       lookup where it would otherwise take three. */
    for (i = 0; i < CELL_COUNT; i++) {
        t = terrain[i];
        cell_cost[i] = level_1_terrain_blocked[t] ? 0 : terrain_move_cost[t];
    }

    cursor_x = level_start[level - 1][0];
    cursor_y = level_start[level - 1][1];
    cur_x = cursor_x;
    cur_y = cursor_y;

    populate_map();
}

/* Pick a unit up: work out the ground it can reach and start showing
   it.  The range is recomputed at every selection rather than cached
   for the turn, because units block each other and the board has
   usually moved since the last time this unit was looked at
   (docs/DESIGN.md § Units). */
void select_unit(uint8_t u)
{
    selected = u;
    sel_x = cursor_x;
    sel_y = cursor_y;
    movement_range(u_cell[u], unit_movement[u_type[u]]);
    recolour_page();
}

/* Put the held unit down.  Only colours change — the unit has not
   moved — so the page is recoloured rather than redrawn, and
   attr_view_cell() stops reporting the range the moment `selected`
   clears. */
void deselect(void)
{
    if (selected == NO_UNIT) return;
    selected = NO_UNIT;
    recolour_page();
}

/* Order the held unit onto the cursor's cell.  The caller has already
   established that the cell is in range, which by construction means it
   is empty, passable, and no further than the unit's movement.

   Moving is the unit's action for the turn, so it is spent and put down
   in the same breath.  The design's "a move ending next to an enemy may
   also attack" is the one exception, and it needs an attack to exist
   first: it arrives with combat in P4. */
void move_selected(void)
{
    uint8_t u = selected;
    uint8_t from = u_cell[u];
    uint8_t to = cell_of(cursor_x, cursor_y);

    occupancy[from] = NO_UNIT;
    occupancy[to] = u;
    u_cell[u] = to;
    u_flags[u] |= U_ACTED;

    selected = NO_UNIT;
    mark_dirty(sel_x, sel_y);           /* the sprite leaves here */
    mark_dirty(cursor_x, cursor_y);     /* and arrives here       */
    recolour_page();
}

/* --- The enemy turn --------------------------------------------------
 * The expensive-looking rule is "avoid the player's attack ranges".
 * Done naively that is every enemy x every candidate cell x every
 * player unit. Done once into threat[], it is a single byte read per
 * candidate afterwards (docs/PLAN.md § Enemy decisions). */

static uint8_t ai_next;         /* next roster slot to consider */

/* Manhattan distance between two cells.  The board is small enough that
   this beats a lookup table and it is the only distance the game has —
   attacks ignore terrain entirely. */
static uint8_t cell_dist(uint8_t a, uint8_t b)
{
    uint8_t ax = col_of[a], bx = col_of[b];
    uint8_t ay = (uint8_t)(a / GRID_COLS), by = (uint8_t)(b / GRID_COLS);

    return (uint8_t)((ax > bx ? ax - bx : bx - ax) +
                     (ay > by ? ay - by : by - ay));
}

void enemy_begin(void)
{
    uint8_t i, c;

    memset(threat, 0, CELL_COUNT);
    for (i = 0; i < unit_count; i++) {
        if (u_type[i] == NO_UNIT) continue;
        if (u_flags[i] & U_SIDE) continue;          /* player units only */
        for (c = 0; c < CELL_COUNT; c++)
            if (cell_dist(u_cell[i], c) <= unit_range[u_type[i]])
                threat[c]++;
    }
    ai_next = 0;
}

/* The best thing this unit can hit from where it stands, or NO_CELL.
   Prefer a kill, then the Base, then the lowest survivor. */
static uint8_t pick_target(uint8_t u)
{
    uint8_t i, best = NO_CELL, best_score = 0;

    for (i = 0; i < unit_count; i++) {
        uint8_t cell, cover, score;
        uint16_t raw;

        if (u_type[i] == NO_UNIT) continue;
        if (u_flags[i] & U_SIDE) continue;          /* player units only */
        cell = u_cell[i];
        if (cell_dist(u_cell[u], cell) > unit_range[u_type[u]]) continue;

        cover = terrain_cover[terrain[cell]];
        raw = unit_damage[u_type[u]];
        score = (uint8_t)((raw * (100u - cover) + 99u) / 100u);
        if (score >= u_hp[i]) score = 200;          /* a kill outranks all */
        if (u_type[i] == UNIT_BASE) score = (uint8_t)(score / 2 + 150);
        if (best == NO_CELL || score > best_score) {
            best = cell;
            best_score = score;
        }
    }
    return best;
}

/* Nowhere to shoot from here: walk somewhere better.  cost[] already
   holds the reachable set from movement_range(), so this is a scan of
   98 cells and a byte read of threat[] each, not a search. */
static void ai_move(uint8_t u)
{
    uint8_t c, from = u_cell[u], best = from, best_score = 0;

    for (c = 0; c < CELL_COUNT; c++) {
        uint8_t score, pen;
        uint8_t i, near = 255;

        if (cost[c] == NO_COST) continue;           /* cannot reach it */
        if (c != from && occupancy[c] != NO_UNIT) continue;

        for (i = 0; i < unit_count; i++) {
            uint8_t d;
            if (u_type[i] == NO_UNIT || (u_flags[i] & U_SIDE)) continue;
            d = cell_dist(c, u_cell[i]);
            if (d < near) near = d;
        }
        if (near == 255) return;                    /* no player units left */

        /* All of this stays inside a byte on purpose: 16-bit arithmetic
           costs SDCC a great deal of code for a score that only has to
           order 98 candidates. */
        pen = (uint8_t)(near * AI_W_DIST + threat[c] * AI_W_THREAT);
        score = pen >= AI_SCORE_BASE ? 0
              : (uint8_t)(AI_SCORE_BASE - pen
                          + terrain_cover[terrain[c]] / AI_W_COVER);
        if (best == from || score > best_score) {
            best_score = score;
            best = c;
        }
    }

    if (best != from) {
        occupancy[from] = NO_UNIT;
        occupancy[best] = u;
        u_cell[u] = best;
        mark_dirty(col_of[from], (uint8_t)(from / GRID_COLS));
        mark_dirty(col_of[best], (uint8_t)(best / GRID_COLS));
    }
}

/* Act for the next enemy that still has its action.  Returns the cell it
   ended on so the caller can bring the view to it, or NO_CELL when the
   army has finished. */
uint8_t enemy_step(void)
{
    while (ai_next < unit_count) {
        uint8_t u = ai_next++;
        uint8_t target;

        if (u_type[u] == NO_UNIT) continue;
        if (!(u_flags[u] & U_SIDE)) continue;       /* enemy units only */
        if (u_flags[u] & U_ACTED) continue;
        if (u_type[u] == UNIT_BASE) continue;       /* bases do not move */

        /* attack() and is_target() both read `selected`, so the unit is
           held for the duration exactly as a player-driven order is. */
        selected = u;
        target = pick_target(u);
        if (target != NO_CELL) {
            attack(target);                         /* also clears selected */
        } else {
            movement_range(u_cell[u], unit_movement[u_type[u]]);
            ai_move(u);
            u_flags[u] |= U_ACTED;
            selected = NO_UNIT;
            recolour_page();
        }
        return u_cell[u];
    }
    return NO_CELL;
}

/* End the player's turn.  Every unit gets its action back and the
   counter moves on; anything the player did not spend is forfeit, which
   is what makes SELECT a decision rather than a formality
   (docs/DESIGN.md § Enemy turn).  The enemy half of the round is P5, so
   for now the counter goes straight round again. */
void end_turn(void)
{
    uint8_t i;

    deselect();
    for (i = 0; i < unit_count; i++)
        u_flags[i] &= (uint8_t)~U_ACTED;
    turn++;
    recolour_page();            /* spent units brighten again */
}
