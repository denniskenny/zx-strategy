/*
 * game.c — Main game loop with switchable states
 *
 * One frame = vsync_wait() → update the active state → poll input →
 * optional state switch.  All drawing happens between the sync and the
 * end of the border/vblank window, so nothing tears.
 *
 * States (see include/game.h):
 *
 *   ST_TITLE    front end; machine/vsync report, waits for START
 *   ST_PLAY     the game proper: an 8x4 page of the world, flipped when
 *               the party walks off its edge
 *   ST_MAP      campaign overview, opened with M from ST_PLAY and
 *               dismissed with SPACE; free cursor over the terrain grid
 *   ST_OVER     a level ended: a win loads the next one, a loss quits
 *   ST_WON      the last level was won; the campaign is over
 *
 * Controls:  Q/A/O/P or Kempston  move the cursor
 *            SPACE                the one "go on" key: starts a game,
 *                                 picks a unit up and puts it down,
 *                                 orders a move, closes the overview,
 *                                 and takes the level-end screen on.
 *                                 Fire 1 does the same on the screens a
 *                                 joystick has to get through.
 *            ENTER / Z / fire 1   end the turn (ST_PLAY only, because
 *                                 SPACE is busy giving orders there)
 *            X / fire 2           back (drops the held unit first)
 *            M                    campaign overview (ST_PLAY only)
 *
 * The tune plays itself whenever the title screen is entered, and any
 * key stops it; there is no key that starts it.
 *
 * The worlds come from assets/maps/level_N.tmx (Tiled), N = 1..10.
 * The build ZX0's each one into include/level_N.h as Tiled GIDs;
 * load_map() decompresses the current level straight into terrain[]
 * and converts the GIDs into terrain ids in place.
 *
 * Terrain graphics come from two ZX-Paintbrush tile strips, ZX0'd by the
 * build: assets/tiles_map.zxp (2x2-character overview tiles) and
 * assets/tiles_view.zxp (4x4-character play tiles).  Both are
 * decompressed once into RAM by load_tiles(); tile N of each sheet is
 * terrain N, and each tile's colour comes from the sheet's attributes.
 * See .claude/skills/zx-tiles.
 *
 * The border turns RED while the frame's work runs and BLACK while
 * waiting for the beam, so the red band is the CPU budget used.
 */

#include <string.h>

#include "../config/app_config.h"
#include "../config/game_config.h"
#include "../include/dzx0.h"
#include "../include/game.h"
#include "../include/gfx.h"
#include "../include/hw.h"
#include "../include/input.h"
#include "../include/level_1.h"
#include "../include/level_2.h"
#include "../include/level_3.h"
#include "../include/level_4.h"
#include "../include/level_5.h"
#include "../include/level_6.h"
#include "../include/level_7.h"
#include "../include/level_8.h"
#include "../include/level_9.h"
#include "../include/level_10.h"
#include "../include/music.h"
#include "../include/prng.h"
#include "../include/tiles_map.h"
#include "../include/tiles_view.h"
#include "../include/units_map.h"
#include "../include/units_view.h"
#include "../include/vsync.h"

/* --- Attributes (never 0x02 or 0x03: the vsync marker owns those) --- */
#define ATTR_TITLE  0x45    /* bright cyan ink, black paper   */
#define ATTR_TEXT   0x47    /* bright white ink, black paper  */
#define ATTR_HINT   0x46    /* bright yellow ink, black paper */
#define ATTR_BG     0x07    /* white ink, black paper         */
#define ATTR_CURSOR 0x78    /* black ink, white paper         */
#define ATTR_VOID   0x00    /* off-world cells: black on black */

/* Units share one sprite per type; the side is carried by the cell
   attribute, so these two are the only thing telling the armies apart
   (docs/DESIGN.md: "Enemy units are red, player units are cyan"). */
#define ATTR_UNIT_P 0x45    /* bright cyan ink, black paper   */
#define ATTR_UNIT_E 0x42    /* bright red ink, black paper    */

/* A player unit that has spent its action loses its brightness, which
   is how the board shows what is left to move.  The enemy has no
   matching dim form: non-bright red is 0x02, one of the two values the
   floating bus sync marker reserves.  It does not need one — the
   player never waits on an enemy unit's remaining action. */
#define ATTR_UNIT_P_DONE 0x05   /* cyan ink, black paper, not bright */

/* Ground the selected unit can reach: the terrain art stays, its paper
   turns blue.  Distinct from the cursor (white paper) and from every
   terrain and unit colour, all of which use black paper. */
#define ATTR_RANGE  0x4F    /* bright white ink, blue paper   */

/* The "working" banner, deliberately not the hint line's yellow. */
#define ATTR_BUSY   0x42    /* bright red ink, black paper    */

/* Terrain ids are tile indices: the .tmx tileset order, the .zxp tile
   column order and the generated tables all line up. */
#define TER_COUNT   LEVEL_1_TERRAIN_COUNT

#if (TILES_MAP_TILES != TER_COUNT) || (TILES_VIEW_TILES != TER_COUNT)
#error "tile sheets and the .tmx tileset disagree on the terrain count"
#endif

/* The cost/cover tables in game_config.h are indexed by the same
   terrain id, so a tile appended to the tileset without a row there
   would read past the end of both. */
#if (TER_TYPES != TER_COUNT)
#error "game_config.h terrain tables and the .tmx tileset disagree on the terrain count"
#endif

/* Unit sprites are indexed by UNIT_* id, one sheet column each, and a
   unit occupies exactly one terrain cell in both views. */
#if (UNITS_MAP_TILES != UNIT_TYPES) || (UNITS_VIEW_TILES != UNIT_TYPES)
#error "unit sheets and game_config.h disagree on the unit type count"
#endif
#if (UNITS_MAP_TILE_W != TILES_MAP_TILE_W) \
 || (UNITS_MAP_TILE_H != TILES_MAP_TILE_H)
#error "units_map.zxp tiles are not the size of a campaign-map cell"
#endif
#if (UNITS_VIEW_TILE_W != TILES_VIEW_TILE_W) \
 || (UNITS_VIEW_TILE_H != TILES_VIEW_TILE_H)
#error "units_view.zxp tiles are not the size of a play-view cell"
#endif

/* Action bits — keyboard and Kempston are folded into one byte so a
   single edge test debounces both. */
#define ACT_UP      0x01
#define ACT_DOWN    0x02
#define ACT_LEFT    0x04
#define ACT_RIGHT   0x08
#define ACT_SELECT  0x10
#define ACT_BACK    0x20
#define ACT_SPACE   0x40
#define ACT_M       0x80    /* the campaign overview, in ST_PLAY only */

#define ACT_DIRS    (ACT_UP | ACT_DOWN | ACT_LEFT | ACT_RIGHT)

/* Moving between screens is SPACE.  Fire 1 is accepted alongside it
   because a Kempston stick has no space bar and would otherwise be
   unable to start a game or take the level-end screen on — and fire 1
   arrives in ACT_SELECT, which scan_input() cannot separate from Z or
   ENTER without another action bit (all eight are spoken for).  So
   ENTER still works on these screens; the screens advertise SPACE. */
#define ACT_GO      (ACT_SPACE | ACT_SELECT)

/* Keyboard half-rows not covered by input.h. */
#define KEY_ENTER_ROW 0xBFFE    /* ENTER = bit 0            */
#define KEY_SPACE_ROW 0x7FFE    /* SPACE = bit 0, M = bit 2 */

/* Staging buffer for decompressed asset data.  Low RAM above the screen
   is free: code and data start at 0x8000 (-zorg=32768). */
#define SCRATCH_BUF ((uint8_t *)0x6000)

/* --- Campaign overview: the whole world in tiles_map.zxp cells.  Size
       comes from the Tiled map, cell size from the tile sheet. --- */
#define GRID_COLS   LEVEL_1_COLS
#define GRID_ROWS   LEVEL_1_ROWS
#define CELL_W      TILES_MAP_TILE_W
#define CELL_ROWS   TILES_MAP_TILE_ROWS
#define MAP_COL     2
#define MAP_ROW     3

/* --- Play view: an 8x4 page of tiles_view.zxp cells.  A full page is
       32x16 characters — 4 KB of screen writes, far more than one vblank
       window allows — so the view PAGES rather than scrolls: the party
       walks around inside a fixed page (2 cells redrawn per step) and the
       page flips only when it steps off the edge. --- */
#define VIEW_COLS   8
#define VIEW_ROWS   4
#define VIEW_CELLS  (VIEW_COLS * VIEW_ROWS)
#define VIEW_CW     TILES_VIEW_TILE_W
#define VIEW_CH     TILES_VIEW_TILE_ROWS
#define VIEW_COL    ((32 - VIEW_COLS * VIEW_CW) / 2)
#define VIEW_ROW    1

/* Cells repainted per frame during a page flip.  Two 4x4 tiles is ~256
   bytes of screen writes, which fits the post-vsync_wait() budget with
   room to spare; the whole page takes VIEW_CELLS / this many frames. */
#define PAGE_CELLS  2

/* Cells RECOLOURED per frame, when a movement range goes on or comes
   off.  An attribute-only cell is 16 bytes against a full repaint's
   144, so eight of them is 128 bytes — half the budget above, and the
   whole page in four frames. */
#define RANGE_CELLS 8

/* Status panel: four lines, ending one row above the key legend.  The
   play view stops at row 16 and the overview at row 17, so ROW_UNIT is
   the last row either renderer leaves free. */
#define ROW_UNIT    17
#define ROW_TURN    18
#define ROW_COORD   19
#define ROW_TERRAIN 20
#define PANEL_TOP   ROW_UNIT
#define PANEL_ROWS  4

/* The key legend, and the row a long operation borrows to say it is
   working.  Every state paints this row on entry, so a banner needs
   nothing saved: whoever comes next writes over it. */
#define ROW_HINT    21

/* The legends a banner has to be able to put back, each 31 columns so
   it overwrites whatever the banner left.  ST_TITLE keeps its key list
   up at rows 10-11 and leaves this row blank. */
#if DEBUG_STATE_WALK
#define PLAY_HINT   "SPC MOVE ENTER TURN MAP W/L END"
#else
#define PLAY_HINT   "SPACE MOVE  ENTER END TURN  MAP"
#endif
#define TITLE_HINT  "                               "

/* Both renderers must fit above the status panel. */
#if (MAP_COL + GRID_COLS * CELL_W > 32) \
 || (MAP_ROW + GRID_ROWS * CELL_ROWS > PANEL_TOP)
#error "level_1.tmx is too large for the campaign overview"
#endif
#if (VIEW_COL < 0) || (VIEW_ROW + VIEW_ROWS * VIEW_CH > PANEL_TOP)
#error "the play view does not fit on screen; shrink the page or the tiles"
#endif

/* --- The campaign: one ZX0'd GID array per level, all the same size
       and all sharing level_1's terrain tables (the build enforces the
       shared tileset with _TERRAIN_SIG, checked below).  A level costs
       ~35 bytes here instead of the 98 it occupies in terrain[]. --- */
#define LEVEL_COUNT 10

static const uint8_t *const level_maps[LEVEL_COUNT] = {
    level_1_gids_zx0, level_2_gids_zx0, level_3_gids_zx0,
    level_4_gids_zx0, level_5_gids_zx0, level_6_gids_zx0,
    level_7_gids_zx0, level_8_gids_zx0, level_9_gids_zx0,
    level_10_gids_zx0
};

/* Where the party starts on each level (the .tmx "start" object). */
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

/* terrain[] and the two renderers are sized from level 1, and levels
   2-10 borrow its terrain names and passability, so a level that
   disagrees on either would silently mis-draw or read out of bounds. */
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

/* Frames between cursor steps while a direction is held. */
#define NAV_DELAY   5

uint8_t game_state;

static uint8_t next_state;

static uint8_t acts, last_acts, prev_stable, edge;
static uint8_t nav_delay;

static uint8_t terrain[GRID_COLS * GRID_ROWS];
static uint8_t cur_x, cur_y;        /* map cursor  */
static uint8_t cursor_x, cursor_y;    /* play state  */
static uint8_t redraw_status;
static uint8_t page_x, page_y;      /* top-left world cell of the page */
static uint8_t cells_left;          /* cells still to repaint on a flip */
static uint8_t key_idle, key_down;
static uint8_t level;       /* 1-based; selects level_maps[] */
static uint8_t player_won;  /* outcome of the level ST_OVER is reporting */
static uint16_t turn;

/* Tile pixels, decompressed once from the ZX0 blobs in the headers. */
static uint8_t map_tiles[TILES_MAP_RAW_SIZE];
static uint8_t view_tiles[TILES_VIEW_RAW_SIZE];
static uint8_t unit_map_tiles[UNITS_MAP_RAW_SIZE];
static uint8_t unit_view_tiles[UNITS_VIEW_RAW_SIZE];

/* --- The armies ------------------------------------------------------
   Structure of arrays, not an array of structs: SDCC pays a multiply
   for every unit[i].field, while a parallel byte array is one indexed
   load.  A slot with u_type == NO_UNIT is free; dead units release
   their slot in place rather than being compacted out, because
   occupancy[] stores indices into these arrays (docs/PLAN.md).  */
#define CELL_COUNT  (GRID_COLS * GRID_ROWS)
#define NO_UNIT     0xFF

#define U_SIDE      0x01    /* 0 = player, 1 = enemy */
#define U_ACTED     0x02    /* spent for this turn; unused until P3 */

#define SIDE_PLAYER 0
#define SIDE_ENEMY  1

static uint8_t u_type[UNITS_MAX];
static uint8_t u_cell[UNITS_MAX];   /* row_base[y] + x, never a pair */
static uint8_t u_hp[UNITS_MAX];
static uint8_t u_flags[UNITS_MAX];
static uint8_t unit_count;          /* slots used, 0..UNITS_MAX */

/* Which unit stands where, or NO_UNIT.  This is what makes "one unit
   per tile" an O(1) fact rather than a scan of the roster. */
static uint8_t occupancy[CELL_COUNT];

/* y * GRID_COLS without the multiply. */
static const uint8_t row_base[GRID_ROWS] = { 0, 14, 28, 42, 56, 70, 84 };

/* cell % GRID_COLS without the divide.  The flood fill needs a popped
   cell's column to know whether stepping east or west would wrap it
   onto the next row — the one edge case of addressing the board as a
   flat array instead of a coordinate pair. */
static const uint8_t col_of[CELL_COUNT] = {
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

/* --- Movement range --------------------------------------------------
   Dial's algorithm, not Dijkstra: movement costs are 1 or 2 and the
   budget is at most MAX_MOVE, so a priority queue would be a
   comparison-heavy way of ordering four values.  A bucket queue pops
   cells in non-decreasing cost order with no comparisons at all —
   bucket c holds the cells reached at cost c, and the buckets are
   walked in order (docs/PLAN.md, "Movement range").

   cost[] is the whole result: a cell is reachable exactly when
   cost[cell] != NO_COST, so it doubles as the highlight set and no
   separate list is kept.  It is meaningful only while a unit is
   selected — every reader gates on that. */
#define NO_COST     0xFF

/* A queued cell always has cost >= its Manhattan distance from the
   start, so only the diamond of radius MAX_MOVE is ever pushed: 25
   cells at MAX_MOVE = 3.  No single bucket can hold more than that
   however the terrain is arranged, which is what bounds q[]. */
#define Q_BUCKET    32
#define Q_DIAMOND   (2 * MAX_MOVE * MAX_MOVE + 2 * MAX_MOVE + 1)

#if Q_DIAMOND > Q_BUCKET
#error "Q_BUCKET is too small for MAX_MOVE; a bucket could overflow"
#endif

static uint8_t cost[CELL_COUNT];
static uint8_t q[(MAX_MOVE + 1) * Q_BUCKET];

/* Where each bucket starts, and where its next entry goes.  A pointer
   pair rather than a count per bucket because a push then costs one
   16-bit load and a store: indexing q[nc * Q_BUCKET + n] instead makes
   SDCC emit a five-shift multiply and a pair of 16-bit adds every time,
   and pushing is the innermost thing the fill does. */
static uint8_t *const bucket_start[MAX_MOVE + 1] = {
    q, q + Q_BUCKET, q + 2 * Q_BUCKET, q + 3 * Q_BUCKET
};
static uint8_t *bucket_end[MAX_MOVE + 1];

#if MAX_MOVE != 3
#error "bucket_start[] is written out for MAX_MOVE = 3; regenerate it"
#endif

/* Cost of entering each CELL, or 0 for "cannot be entered".  Folding
   terrain[] -> terrain_move_cost[] / level_1_terrain_blocked[] into one
   per-cell byte turns three table lookups in the fill's innermost loop
   into one.  Rebuilt by load_map(), because terrain only changes when
   a level does — and derived from the .tmx flags rather than repeating
   them, so passability still has exactly one source (game_config.h
   explains why it is not restated there). */
static uint8_t cell_cost[CELL_COUNT];

/* The unit the player has picked up, or NO_UNIT.  Selection survives
   the cursor wandering off — the point of it is to look at the ground
   before committing — so it is cleared only by SPACE, X, the end of the
   turn, or the unit dying. */
static uint8_t selected = NO_UNIT;
static uint8_t sel_x, sel_y;    /* where it stands, kept to avoid a divide */

/* World cells whose PICTURE is stale, redrawn on the next frame: a unit
   sprite that left one cell and arrived in another.  Input is handled
   outside the vblank window, so an order cannot draw its own result.
   Two is all a move needs, and two draw_view_cell() calls is exactly
   what a page flip already spends per frame. */
#define DIRTY_MAX   2
static uint8_t dirty_x[DIRTY_MAX], dirty_y[DIRTY_MAX], dirty_n;

/* Page cells whose COLOUR is stale, recoloured a few per frame: a
   movement range going on or coming off.  0 when there is nothing
   outstanding. */
static uint8_t attrs_left;

/* Status-panel labels, padded to 8 characters like the terrain names
   the map converter generates. */
static const char *const unit_names[UNIT_TYPES] = {
    "INFANTRY",
    "TANK    ",
    "CANNON  ",
    "BASE    "
};

/* Placement scratch: the free cells around one base, drawn from at
   random.  A base sits in a corner, so its radius block holds at most
   UNITS_PLACE_SLOTS usable cells (game_config.h does that arithmetic
   and refuses a roster that cannot fit). */
static uint8_t place_cand[UNITS_PLACE_SLOTS];
static prng_t place_rng;

/* The eight keyboard half-rows, in the usual port order. */
static const uint16_t key_rows[8] = {
    0xFEFE, 0xFDFE, 0xFBFE, 0xF7FE, 0xEFFE, 0xDFFE, 0xBFFE, 0x7FFE
};

/* --------------------------------------------------------------- input */

static uint8_t scan_actions(void)
{
    uint8_t k = scan_input();
    uint8_t a = 0;

    if (k & INPUT_UP)    a |= ACT_UP;
    if (k & INPUT_DOWN)  a |= ACT_DOWN;
    if (k & INPUT_LEFT)  a |= ACT_LEFT;
    if (k & INPUT_RIGHT) a |= ACT_RIGHT;
    if (k & INPUT_FIRE1) a |= ACT_SELECT;   /* Z / Kempston fire 1 */
    if (k & INPUT_FIRE2) a |= ACT_BACK;     /* X / Kempston fire 2 */

    if (!(read_keys(KEY_ENTER_ROW) & 0x01)) a |= ACT_SELECT;
    if (!(read_keys(KEY_SPACE_ROW) & 0x01)) a |= ACT_SPACE;
    if (!(read_keys(KEY_SPACE_ROW) & 0x04)) a |= ACT_M;

    return a;
}

#if DEBUG_STATE_WALK
/* P0 state walk: W wins the level, L loses it.  Kept out of the action
   byte — every bit of it is taken, and this is temporary scaffolding
   that comes out with the real win check in P4. */
#define DBG_WIN     0x01
#define DBG_LOSE    0x02

static uint8_t dbg_last, dbg_prev, dbg_edge;

static void poll_debug(void)
{
    uint8_t a = 0, stable;

    if (!(read_keys(KEY_QWERT) & 0x02))     a |= DBG_WIN;    /* W */
    if (!(read_keys(KEY_ENTER_ROW) & 0x02)) a |= DBG_LOSE;   /* L */

    stable = (uint8_t)(a & dbg_last);
    dbg_last = a;
    dbg_edge = (uint8_t)(stable & ~dbg_prev);
    dbg_prev = stable;
}
#endif

/* An action counts only when the same bit is seen in two consecutive
   frames, then fires on its rising edge: held keys act once, and a
   single noisy read is ignored. */
static void poll_input(void)
{
    uint8_t stable;

    acts = scan_actions();
    stable = (uint8_t)(acts & last_acts);
    last_acts = acts;
    edge = (uint8_t)(stable & ~prev_stable);
    prev_stable = stable;
}

/* Forget the current key state so the key that caused a transition is
   not read again by the state we land in. */
static void flush_input(void)
{
    last_acts = prev_stable = 0xFF;
    edge = 0;
    nav_delay = NAV_DELAY;
#if DEBUG_STATE_WALK
    dbg_last = dbg_prev = 0xFF;
    dbg_edge = 0;
#endif
}

/* --- Long operations -------------------------------------------------
   The game is turn-based, so nothing animates and nothing waits on a
   clock: work that overruns a frame costs a pause and nothing else, and
   game logic is never chopped up to fit the frame (docs/DESIGN.md
   § Long operations).

   What the player must not get is a game that looks dead, or one that
   acts on a key pressed half a second ago.  So an operation long enough
   to see says what it is doing, and throws away whatever was pressed
   while it ran.

   Pass a message padded to the full 31 columns; it overwrites the hint
   line in place. */
static void busy_on(const char *msg)
{
    print_at(1, ROW_HINT, msg);
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_BUSY);
}

/* Put the legend back and forget the keyboard.  Only needed when the
   work does NOT end in a state change: enter_state() repaints the whole
   screen and flushes input already. */
static void busy_off(const char *hint)
{
    print_at(1, ROW_HINT, hint);
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_HINT);
    flush_input();
}

/* True if any key on the whole keyboard, or the joystick, is down.
   Each row is masked to its 5 key bits — the upper bits carry EAR and
   the floating bus, not keyboard state. */
static uint8_t any_key(void)
{
    uint8_t i;

    if (scan_input()) return 1;
    for (i = 0; i < 8; i++)
        if ((read_keys(key_rows[i]) & 0x1F) != 0x1F) return 1;
    return 0;
}

/* -------------------------------------------------------------- drawing */

static void print_num(uint8_t col, uint8_t row, uint16_t v, uint8_t digits)
{
    char buf[6];
    uint8_t i;

    buf[digits] = 0;
    for (i = digits; i > 0; i--) {
        buf[i - 1] = (char)('0' + (v % 10));
        v /= 10;
    }
    print_at(col, row, buf);
}

static void print_char(uint8_t col, uint8_t row, char c)
{
    char buf[2];

    buf[0] = c;
    buf[1] = 0;
    print_at(col, row, buf);
}

static void draw_header(const char *title)
{
    screen_clear(ATTR_BG);
    print_at(1, 0, title);
    set_attr_rect(0, 0, 32, 1, ATTR_TITLE);
}

/* Unpack the tile and unit strips into RAM.  ~1.1 KB of pixels cost
   ~500 bytes of ZX0 in the binary and one decompression at startup. */
static void load_tiles(void)
{
    dzx0_decompress(tiles_map_zx0, map_tiles);
    dzx0_decompress(tiles_view_zx0, view_tiles);
    dzx0_decompress(units_map_zx0, unit_map_tiles);
    dzx0_decompress(units_view_zx0, unit_view_tiles);
}

/* ------------------------------------------------------------- armies */

static uint8_t cell_of(uint8_t x, uint8_t y)
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

/* Every cell the unit at `start` can reach on a budget of `budget`
   movement points, written into cost[].  Dial's algorithm: bucket c is
   scanned only once c is the cheapest cost left outstanding, so the
   first cost written for a cell is its cheapest and the entry that put
   it there is never revisited.

   Terrain the .tmx marks impassable and cells that already hold a unit
   are both skipped, because "one unit per tile" makes an occupied tile
   as solid as water for as long as its occupant stands there
   (docs/DESIGN.md § Units).  Movement is 4-way, so entering a cell
   costs the same whichever side it is entered from and a single
   per-terrain cost is the whole cost model. */
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

static void movement_range(uint8_t start, uint8_t budget)
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
    dirty_n = 0;
    attrs_left = 0;

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
static void load_map(void)
{
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

/* A unit's own colour: its side, dimmed once it has spent its action
   for the turn.  This is the board's only record of what the player
   still has left to move, so it has to survive every repaint — which
   is why it is computed here rather than painted once at the moment
   the unit acts. */
static uint8_t unit_attr(uint8_t u)
{
    if (u_flags[u] & U_SIDE)   return ATTR_UNIT_E;
    return (u_flags[u] & U_ACTED) ? ATTR_UNIT_P_DONE : ATTR_UNIT_P;
}

/* The colour a cell wears when the cursor is not on it: yellow if it
   holds the selected unit, otherwise the occupant's side colour, and
   otherwise the terrain's own attribute from the tile sheet. */
static uint8_t map_cell_attr(uint8_t cell)
{
    uint8_t u = occupancy[cell];

    if (u == NO_UNIT) return tiles_map_attr[terrain[cell]];
    if (u == selected) return ATTR_HINT;
    return unit_attr(u);
}

/* As above, plus the movement range: the play view is where orders are
   given, so it is the only view that shows the ground a held unit can
   reach.  cost[] is only meaningful while something is selected, hence
   the guard.  Reachable cells are never occupied — the fill refuses
   them — so testing the occupant first cannot hide a highlight. */
static uint8_t view_cell_attr(uint8_t cell)
{
    uint8_t u = occupancy[cell];

    if (u != NO_UNIT) return (u == selected) ? ATTR_HINT : unit_attr(u);
    if (selected != NO_UNIT && cost[cell] != NO_COST) return ATTR_RANGE;
    return tiles_view_attr[terrain[cell]];
}

/* Terrain, then the unit standing on it.  The sprites are opaque, so a
   unit hides its tile rather than sitting over it — masked sprites are
   a later pass (docs/PLAN.md, Risks). */
static void draw_cell(uint8_t cx, uint8_t cy, uint8_t attr)
{
    uint8_t col = (uint8_t)(MAP_COL + cx * CELL_W);
    uint8_t row = (uint8_t)(MAP_ROW + cy * CELL_ROWS);
    uint8_t cell = cell_of(cx, cy);
    uint8_t u = occupancy[cell];

    write_blit((int8_t)col, (uint8_t)(row << 3),
               map_tiles + (uint16_t)terrain[cell] * TILES_MAP_TILE_SIZE,
               TILES_MAP_TILE_W, TILES_MAP_TILE_H);
    if (u != NO_UNIT)
        write_blit((int8_t)col, (uint8_t)(row << 3),
                   unit_map_tiles + (uint16_t)u_type[u] * UNITS_MAP_TILE_SIZE,
                   UNITS_MAP_TILE_W, UNITS_MAP_TILE_H);
    set_attr_rect(col, row, CELL_W, CELL_ROWS, attr);
}

static void draw_map(void)
{
    uint8_t x, y;

    for (y = 0; y < GRID_ROWS; y++)
        for (x = 0; x < GRID_COLS; x++)
            draw_cell(x, y, map_cell_attr(cell_of(x, y)));
}

/* Row 17: the unit under the cursor — type, HP against its type's
   maximum, attack range and movement.  Enemy units report the same
   numbers as friendly ones; knowing what is about to shoot you is not
   a privilege, and the line's colour says whose it is.  An empty cell
   blanks the whole field, so nothing is left over from the last unit
   the cursor crossed. */
static void draw_unit_line(uint8_t cell)
{
    uint8_t u = occupancy[cell];
    uint8_t t;

    print_at(1, ROW_UNIT, "UNIT   :");

    if (u == NO_UNIT) {
        print_at(10, ROW_UNIT, "-                     ");
        set_attr_rect(0, ROW_UNIT, 32, 1, ATTR_TEXT);
        return;
    }

    t = u_type[u];
    print_at(10, ROW_UNIT, unit_names[t]);
    print_num(19, ROW_UNIT, u_hp[u], 3);
    print_char(22, ROW_UNIT, '/');
    print_num(23, ROW_UNIT, unit_health[t], 3);
    print_char(27, ROW_UNIT, 'R');
    print_num(28, ROW_UNIT, unit_range[t], 1);
    print_char(30, ROW_UNIT, 'M');
    print_num(31, ROW_UNIT, unit_movement[t], 1);

    /* Same colour rule as the sprite, so a spent unit reads as spent in
       the panel too. */
    set_attr_rect(0, ROW_UNIT, 32, 1, unit_attr(u));
}

/* Unit / turn / coordinate / terrain panel, shared by the play and map
   states.  Both report whatever their own cursor is over. */
static void draw_status(const char *label, uint8_t x, uint8_t y)
{
    uint8_t cell = cell_of(x, y);
    uint8_t t = terrain[cell];

    draw_unit_line(cell);

    print_at(1, ROW_TURN, "TURN   :");
    print_num(10, ROW_TURN, turn, 3);

    print_at(1, ROW_COORD, label);
    print_num(10, ROW_COORD, x, 2);
    print_char(12, ROW_COORD, ',');
    print_num(13, ROW_COORD, y, 2);

    print_at(1, ROW_TERRAIN, "TERRAIN:");
    print_at(10, ROW_TERRAIN, level_1_terrain_names[t]);
    print_at(19, ROW_TERRAIN, "COVER");
    print_num(25, ROW_TERRAIN, terrain_cover[t], 2);
    print_char(27, ROW_TERRAIN, '%');

    set_attr_rect(0, ROW_TURN, 32, PANEL_ROWS - 1, ATTR_TEXT);
}

/* Page the view onto whichever VIEW_COLS x VIEW_ROWS block of the world
   holds the cursor. */
static void set_page(void)
{
    page_x = (uint8_t)(cursor_x - cursor_x % VIEW_COLS);
    page_y = (uint8_t)(cursor_y - cursor_y % VIEW_ROWS);
}

/* One cell of the play view: a terrain tile from tiles_view.zxp with
   its occupant blitted over it, or blank for cells outside the world.
   The cursor's cell keeps its picture and inverts, so the cursor reads
   over terrain and over a unit sprite alike. */
static void draw_view_cell(uint8_t vx, uint8_t vy)
{
    uint8_t col = (uint8_t)(VIEW_COL + vx * VIEW_CW);
    uint8_t row = (uint8_t)(VIEW_ROW + vy * VIEW_CH);
    uint8_t wx = (uint8_t)(page_x + vx);
    uint8_t wy = (uint8_t)(page_y + vy);

    if (wx >= GRID_COLS || wy >= GRID_ROWS) {
        clear_blit((int8_t)col, (uint8_t)(row << 3),
                   VIEW_CW, TILES_VIEW_TILE_H);
        set_attr_rect(col, row, VIEW_CW, VIEW_CH, ATTR_VOID);
        return;
    }

    {
        uint8_t cell = cell_of(wx, wy);
        uint8_t u = occupancy[cell];
        uint8_t at_cursor = (wx == cursor_x && wy == cursor_y);

        write_blit((int8_t)col, (uint8_t)(row << 3),
                   view_tiles + (uint16_t)terrain[cell] * TILES_VIEW_TILE_SIZE,
                   TILES_VIEW_TILE_W, TILES_VIEW_TILE_H);
        if (u != NO_UNIT)
            write_blit((int8_t)col, (uint8_t)(row << 3),
                       unit_view_tiles +
                           (uint16_t)u_type[u] * UNITS_VIEW_TILE_SIZE,
                       UNITS_VIEW_TILE_W, UNITS_VIEW_TILE_H);
        set_attr_rect(col, row, VIEW_CW, VIEW_CH,
                      at_cursor ? ATTR_CURSOR : view_cell_attr(cell));
    }
}

/* The same cell, colour only.  Laying a movement range down or taking
   it away changes up to 25 cells' attributes and not one pixel of their
   art, so this costs 16 bytes a cell where draw_view_cell() costs 144.
   That is the difference between recolouring the page in four frames
   and repainting it in thirteen. */
static void attr_view_cell(uint8_t vx, uint8_t vy)
{
    uint8_t wx = (uint8_t)(page_x + vx);
    uint8_t wy = (uint8_t)(page_y + vy);
    uint8_t a;

    if (wx >= GRID_COLS || wy >= GRID_ROWS)     a = ATTR_VOID;
    else if (wx == cursor_x && wy == cursor_y)  a = ATTR_CURSOR;
    else                                        a = view_cell_attr(cell_of(wx, wy));

    set_attr_rect((uint8_t)(VIEW_COL + vx * VIEW_CW),
                  (uint8_t)(VIEW_ROW + vy * VIEW_CH), VIEW_CW, VIEW_CH, a);
}

static void draw_view(void)
{
    uint8_t i;

    for (i = 0; i < VIEW_CELLS; i++)
        draw_view_cell((uint8_t)(i % VIEW_COLS), (uint8_t)(i / VIEW_COLS));
}

/* ---------------------------------------------------------------- states */

/* Play the tune.  The Tritone player blocks with interrupts off until a
   key is pressed — it owns the speaker and cannot share the frame loop —
   which used to make it a state of its own.  It does not need to be: it
   is a long operation like any other, so it borrows the hint row to say
   so and flushes the key that stopped it (docs/DESIGN.md § Long
   operations).  Nothing else on the title screen is disturbed, so
   nothing has to be repainted afterwards. */
static void play_music(void)
{
    busy_on("PLAYING - PRESS A KEY          ");
    lowlands_play();
    busy_off(TITLE_HINT);
}

static void enter_title(void)
{
    draw_header("ZX STRATEGY");

    print_at(1, 3, "MACHINE :");
    print_at(11, 3, is_128k ? "128K" : "48K");

    print_at(1, 4, "KEMPSTON:");
    print_at(11, 4, has_kempston ? "YES" : "NO");

    print_at(1, 5, "VSYNC   :");
    switch (vsync_mode) {
        case VSYNC_MODE_48K:
            print_at(11, 5, "FLOATING BUS 0X40FF");
            break;
        case VSYNC_MODE_128K:
            print_at(11, 5, "FLOATING BUS 0X0FFD");
            break;
        default:
            print_at(11, 5, "HALT FALLBACK      ");
            break;
    }
    set_attr_rect(0, 3, 32, 3, ATTR_TEXT);

    print_at(1, 10, "SPACE / FIRE   START");
    set_attr_rect(0, 10, 32, 1, ATTR_HINT);

    print_at(1, 20, "QAOP / KEMPSTON TO MOVE");
    set_attr_rect(0, 20, 32, 1, ATTR_TEXT);

    /* The hint row is where the tune's banner goes, and where it is
       cleared back to. */
    print_at(1, ROW_HINT, TITLE_HINT);
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_HINT);

    /* Row 22 is left blank on purpose — it holds the floating bus sync
       marker written by vsync_wait(). */

    /* The tune plays itself, as the last thing entering the title does:
       the screen above is already painted, so the player has something
       to read while it runs.  It blocks until a key is pressed, so the
       title is unresponsive until then — the banner says so, and the
       keypress that stops it is flushed rather than being taken as the
       one that starts a game. */
    play_music();
}

static void enter_play(void)
{
    draw_header("THE FIELD");
    set_page();
    draw_view();
    cells_left = 0;
    attrs_left = 0;         /* draw_view() already used the real colours */
    dirty_n = 0;
    draw_status("CURSOR :", cursor_x, cursor_y);

    print_at(1, ROW_HINT, PLAY_HINT);
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_HINT);
}

static void enter_map(void)
{
    draw_header("CAMPAIGN MAP");
    draw_map();
    draw_cell(cursor_x, cursor_y, ATTR_HINT);   /* where the play cursor is */
    draw_cell(cur_x, cur_y, ATTR_CURSOR);
    draw_status("CURSOR :", cur_x, cur_y);

    print_at(1, ROW_HINT, "QAOP LOOK AROUND  SPACE CLOSE");
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_HINT);
}

/* A level ended.  player_won says which message to show; the exit is
   handled in handle_input(), which is where the level advances. */
static void enter_over(void)
{
    draw_header(player_won ? "VICTORY" : "DEFEAT");

    print_at(1, 10, player_won ? "LEVEL TAKEN    :" : "LEVEL LOST     :");
    print_num(18, 10, level, 2);
    set_attr_rect(0, 10, 32, 1, ATTR_TEXT);

    print_at(1, ROW_HINT, player_won ? "SPACE FOR THE NEXT LEVEL       "
                                     : "SPACE TO RETURN TO THE TITLE   ");
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_HINT);
}

/* Past the last level: the campaign is over.  Any key goes back to the
   title, behind a release-then-press debounce so the key that won the
   game cannot dismiss this immediately. */
static void enter_won(void)
{
    draw_header("CAMPAIGN COMPLETE");

    print_at(4, 9,  "EVERY LEVEL TAKEN");
    print_at(4, 11, "LEVELS WON     :");
    print_num(21, 11, LEVEL_COUNT, 2);
    set_attr_rect(0, 9, 32, 3, ATTR_TEXT);

    print_at(1, ROW_HINT, "PRESS A KEY                    ");
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_HINT);

    key_idle = 0;
    key_down = 0;
}

static void enter_state(uint8_t s)
{
    game_state = s;

    switch (s) {
        case ST_TITLE:   enter_title();   break;
        case ST_PLAY:    enter_play();    break;
        case ST_MAP:     enter_map();     break;
        case ST_OVER:    enter_over();    break;
        case ST_WON:     enter_won();     break;
    }
    flush_input();
}

static void set_state(uint8_t s)
{
    next_state = s;
}

/* Held directions repeat every NAV_DELAY frames.  Returns 1 when a step
   is due and writes the target cell to *nx / *ny. */
static uint8_t nav_step(uint8_t x, uint8_t y, uint8_t *nx, uint8_t *ny)
{
    *nx = x;
    *ny = y;

    if (!(acts & ACT_DIRS)) {
        nav_delay = 0;
        return 0;
    }
    if (nav_delay) {
        nav_delay--;
        return 0;
    }

    if ((acts & ACT_LEFT)  && x > 0)              (*nx)--;
    if ((acts & ACT_RIGHT) && x < GRID_COLS - 1)  (*nx)++;
    if ((acts & ACT_UP)    && y > 0)              (*ny)--;
    if ((acts & ACT_DOWN)  && y < GRID_ROWS - 1)  (*ny)++;

    nav_delay = NAV_DELAY;
    return (uint8_t)(*nx != x || *ny != y);
}

static void move_cursor(void)
{
    uint8_t nx, ny;

    if (!nav_step(cur_x, cur_y, &nx, &ny)) return;

    draw_cell(cur_x, cur_y,
              (cur_x == cursor_x && cur_y == cursor_y)
                  ? ATTR_HINT                       /* the play cursor */
                  : map_cell_attr(cell_of(cur_x, cur_y)));
    cur_x = nx;
    cur_y = ny;
    draw_cell(cur_x, cur_y, ATTR_CURSOR);
    redraw_status = 1;
}

/* The cursor roams the grid, stopped only by its edges: it is where the
   player is looking, not something standing on the board, so water and
   occupied tiles do not block it (docs/DESIGN.md § Movement Range).
   Passability is a constraint on the unit being ordered, and is checked
   when the order is issued.  A step inside the page repaints just the
   two cells that changed; a step off it starts a page flip. */
static void move_play_cursor(void)
{
    uint8_t nx, ny, ox, oy;

    if (cells_left) {           /* frozen while the page repaints */
        nav_delay = NAV_DELAY;
        return;
    }
    if (!nav_step(cursor_x, cursor_y, &nx, &ny)) return;

    ox = (uint8_t)(cursor_x - page_x);
    oy = (uint8_t)(cursor_y - page_y);
    cursor_x = nx;
    cursor_y = ny;
    redraw_status = 1;

    if (nx < page_x || nx >= page_x + VIEW_COLS ||
        ny < page_y || ny >= page_y + VIEW_ROWS) {
        set_page();
        cells_left = VIEW_CELLS;
        attrs_left = 0;         /* the flip repaints every attribute */
    } else {
        draw_view_cell(ox, oy);                     /* leave the old cell */
        draw_view_cell((uint8_t)(nx - page_x),
                       (uint8_t)(ny - page_y));     /* and enter the new  */
    }
}

/* Queue a world cell for a full redraw next frame.  Orders are given
   from handle_input(), which runs outside the vblank window, so nothing
   there may draw its own result. */
static void mark_dirty(uint8_t x, uint8_t y)
{
    if (dirty_n < DIRTY_MAX) {
        dirty_x[dirty_n] = x;
        dirty_y[dirty_n] = y;
        dirty_n++;
    }
}

/* Pick a unit up: work out the ground it can reach and start showing
   it.  The range is recomputed at every selection rather than cached
   for the turn, because units block each other and the board has
   usually moved since the last time this unit was looked at
   (docs/DESIGN.md § Units). */
static void select_unit(uint8_t u)
{
    selected = u;
    sel_x = cursor_x;
    sel_y = cursor_y;
    movement_range(u_cell[u], unit_movement[u_type[u]]);
    attrs_left = VIEW_CELLS;
}

/* Put the held unit down.  Only colours change — the unit has not
   moved — so the page is recoloured rather than redrawn, and
   view_cell_attr() stops reporting the range the moment `selected`
   clears. */
static void deselect(void)
{
    if (selected == NO_UNIT) return;
    selected = NO_UNIT;
    attrs_left = VIEW_CELLS;
}

/* Order the held unit onto the cursor's cell.  The caller has already
   established that the cell is in range, which by construction means it
   is empty, passable, and no further than the unit's movement.

   Moving is the unit's action for the turn, so it is spent and put down
   in the same breath.  The design's "a move ending next to an enemy may
   also attack" is the one exception, and it needs an attack to exist
   first: it arrives with combat in P4. */
static void move_selected(void)
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
    attrs_left = VIEW_CELLS;
}

/* End the player's turn.  Every unit gets its action back and the
   counter moves on; anything the player did not spend is forfeit, which
   is what makes SELECT a decision rather than a formality
   (docs/DESIGN.md § Enemy turn).  The enemy half of the round is P5, so
   for now the counter goes straight round again. */
static void end_turn(void)
{
    uint8_t i;

    deselect();
    for (i = 0; i < unit_count; i++)
        u_flags[i] &= (uint8_t)~U_ACTED;
    turn++;
    attrs_left = VIEW_CELLS;    /* spent units brighten again */
}

/* Per-frame work for the active state, run inside the vblank window. */
static void update_state(void)
{
    switch (game_state) {
        case ST_PLAY:
            move_play_cursor();
            /* Page flips repaint PAGE_CELLS tiles per frame so no frame
               overruns the vblank window. */
            if (cells_left) {
                uint8_t n = PAGE_CELLS;
                while (n-- && cells_left) {
                    uint8_t i = (uint8_t)(VIEW_CELLS - cells_left);
                    draw_view_cell((uint8_t)(i % VIEW_COLS),
                                   (uint8_t)(i / VIEW_COLS));
                    cells_left--;
                }
            }
            /* Cells an order left stale — the two ends of a move.  Only
               drawn if the page still shows them and a flip is not
               already redrawing everything anyway. */
            if (dirty_n) {
                while (dirty_n) {
                    uint8_t i = --dirty_n;

                    if (!cells_left &&
                        dirty_x[i] >= page_x &&
                        dirty_x[i] < page_x + VIEW_COLS &&
                        dirty_y[i] >= page_y &&
                        dirty_y[i] < page_y + VIEW_ROWS)
                        draw_view_cell((uint8_t)(dirty_x[i] - page_x),
                                       (uint8_t)(dirty_y[i] - page_y));
                }
            }
            /* Recolour the page a few cells at a time: a movement range
               appearing or clearing.  Held off while either heavier
               repaint is outstanding — a flip rewrites every attribute
               itself, and a move has already spent this frame's budget
               on its two full cells. */
            else if (attrs_left && !cells_left) {
                uint8_t n = RANGE_CELLS;
                while (n-- && attrs_left) {
                    uint8_t i = (uint8_t)(VIEW_CELLS - attrs_left);
                    attr_view_cell((uint8_t)(i % VIEW_COLS),
                                   (uint8_t)(i / VIEW_COLS));
                    attrs_left--;
                }
            }
            if (redraw_status) {
                draw_status("CURSOR :", cursor_x, cursor_y);
                redraw_status = 0;
            }
            break;

        case ST_MAP:
            move_cursor();
            if (redraw_status) {
                draw_status("CURSOR :", cur_x, cur_y);
                redraw_status = 0;
            }
            break;

        default:
            break;
    }
}

/* State transitions driven by the debounced action edges. */
static void handle_input(void)
{
    switch (game_state) {
        case ST_TITLE:
            if (edge & ACT_GO) {
                turn = 1;
                level = 1;
                /* Decompressing a map and placing two armies runs long.
                   No banner is put back: enter_play() repaints the
                   screen and enter_state() flushes the keyboard. */
                busy_on("DEPLOYING...                   ");
                load_map();
                set_state(ST_PLAY);
            }
            break;

        case ST_PLAY:
            /* SPACE is the one order key: with nothing held it picks
               the unit under the cursor up, and with a unit held it
               either sends it to the highlighted cell the cursor is on
               or puts it down again.  Only the player's own units can
               be held, and only ones that still have their action; an
               enemy under the cursor is reported in the panel but never
               selectable. */
            /* Orders wait for a page flip to finish, exactly as cursor
               movement does: the flip is part way through repainting
               the page, so a move made now could have its two cells
               drawn before the unit had left one and reached the
               other. */
            if ((edge & ACT_SPACE) && !cells_left) {
                uint8_t cell = cell_of(cursor_x, cursor_y);
                uint8_t u = occupancy[cell];

                if (selected == NO_UNIT) {
                    if (u != NO_UNIT &&
                        (u_flags[u] & (U_SIDE | U_ACTED)) == SIDE_PLAYER)
                        select_unit(u);
                } else if (u == NO_UNIT && cost[cell] != NO_COST) {
                    move_selected();
                } else {
                    /* The unit's own cell, or ground it cannot reach. */
                    deselect();
                }
                redraw_status = 1;
            }
            /* ENTER, not SPACE: SPACE is giving orders on this screen,
               and it is the only screen where the two differ. */
            if (edge & ACT_SELECT) {
                end_turn();
                redraw_status = 1;
            }
            /* X drops the held unit first and only quits on a second
               press, so the exit cannot be hit while giving an order. */
            if (edge & ACT_BACK) {
                if (selected != NO_UNIT) deselect();
                else                     set_state(ST_TITLE);
            }
#if DEBUG_STATE_WALK
            /* Stand in for the win check until P4 gives us one. */
            if (dbg_edge & (DBG_WIN | DBG_LOSE)) {
                player_won = (uint8_t)((dbg_edge & DBG_WIN) ? 1 : 0);
                set_state(ST_OVER);
            }
#endif
            break;

        case ST_MAP:
            /* Read-only overview: SPACE (or back) dismisses it. */
            if (edge & (ACT_GO | ACT_BACK))
                set_state(ST_PLAY);
            break;

        case ST_OVER:
            /* A loss ends the campaign.  A win advances: level 11 does
               not exist, so passing the last level is the campaign
               being complete rather than another map to load. */
            if (edge & ACT_GO) {
                if (!player_won) {
                    set_state(ST_TITLE);
                } else if (++level > LEVEL_COUNT) {
                    set_state(ST_WON);
                } else {
                    turn = 1;
                    busy_on("DEPLOYING...                   ");
                    load_map();
                    set_state(ST_PLAY);
                }
            }
            break;

        case ST_WON:
            /* Wait for the key that won the level to be released, then
               for a fresh press: two consecutive samples in each state
               debounce both.  Sampling once per frame rather than in a
               tight loop keeps ULA bus noise out of the reads. */
            if (key_idle < 2) {
                key_idle = any_key() ? 0 : (uint8_t)(key_idle + 1);
            } else {
                key_down = any_key() ? (uint8_t)(key_down + 1) : 0;
                if (key_down >= 2) set_state(ST_TITLE);
            }
            break;
    }

    /* M opens the overview.  It means nothing anywhere else — the tune
       plays itself when the title screen is entered. */
    if ((edge & ACT_M) && game_state == ST_PLAY) {
        cur_x = cursor_x;
        cur_y = cursor_y;
        set_state(ST_MAP);
    }
}

void game_run(void)
{
    turn = 0;
    level = 1;
    load_tiles();
    load_map();

    next_state = ST_TITLE;
    enter_state(ST_TITLE);

    for (;;) {
        /* ---- Sync to the beam --------------------------------------
           After this returns the beam is at attribute row 22, so the
           bottom border + vblank + top border (~28 000 T-states on a
           48K) are free for tear-free updates. */
        vsync_wait();

        border(2);              /* RED: start of the frame's work */
        update_state();
        border(0);              /* BLACK: work done, idle from here */

        poll_input();
#if DEBUG_STATE_WALK
        poll_debug();
#endif
        handle_input();

        if (next_state != game_state) {
            /* Every state repaints its own screen on entry, so returning
               from a full-screen state needs no extra bookkeeping. */
            enter_state(next_state);
        }
    }
}
