#ifndef _BOARD_H_
#define _BOARD_H_

/* ================================================================== */
/* board.h — the board, the armies, and the rules that move them      */
/* ================================================================== */
/* The state `src/logic.c` owns and `src/render.c` reads.  Logic never
 * touches the screen and rendering never changes the game, so this
 * header is the whole contract between them (docs/DESIGN.md § Logic and
 * rendering).
 *
 * Everything here is a plain extern rather than an accessor: on a Z80 a
 * global is an absolute address and a getter is a call, and this is read
 * from inner loops.
 */

#include <stdint.h>

#include "../config/game_config.h"
#include "level_1.h"

/* --- The board ------------------------------------------------------
 * Sized from level 1; every level is the same size and shares its
 * tileset, which src/logic.c checks at compile time. */
#define GRID_COLS   LEVEL_1_COLS
#define GRID_ROWS   LEVEL_1_ROWS
#define CELL_COUNT  (GRID_COLS * GRID_ROWS)

/* Terrain ids are tile indices: the .tmx tileset order, the .zxp tile
   column order and the generated tables all line up. */
#define TER_COUNT   LEVEL_1_TERRAIN_COUNT

#define LEVEL_COUNT 10

/* A cell index, not an (x, y) pair: one byte per position, neighbours
   at +/-1 and +/-GRID_COLS. */
extern uint8_t terrain[CELL_COUNT];     /* terrain id per cell          */
extern uint8_t cell_cost[CELL_COUNT];   /* cost to ENTER, 0 = impassable */

/* y * GRID_COLS and cell % GRID_COLS without the multiply or divide. */
extern const uint8_t row_base[GRID_ROWS];
extern const uint8_t col_of[CELL_COUNT];

uint8_t cell_of(uint8_t x, uint8_t y);

/* --- The armies ------------------------------------------------------
 * Structure of arrays, not an array of structs: SDCC pays a multiply
 * for every unit[i].field, while a parallel byte array is one indexed
 * load.  A slot with u_type == NO_UNIT is free; dead units release
 * their slot in place rather than being compacted out, because
 * occupancy[] stores indices into these arrays. */
#define NO_UNIT     0xFF

#define U_SIDE      0x01    /* 0 = player, 1 = enemy */
#define U_ACTED     0x02    /* spent: has moved (or, from P4, attacked) */

#define SIDE_PLAYER 0
#define SIDE_ENEMY  1

extern uint8_t u_type[UNITS_MAX];
extern uint8_t u_cell[UNITS_MAX];       /* row_base[y] + x, never a pair */
extern uint8_t u_hp[UNITS_MAX];
extern uint8_t u_flags[UNITS_MAX];
extern uint8_t unit_count;              /* slots used, 0..UNITS_MAX */

/* Which unit stands where, or NO_UNIT.  This is what makes "one unit
   per tile" an O(1) fact rather than a scan of the roster. */
extern uint8_t occupancy[CELL_COUNT];

/* --- Movement range --------------------------------------------------
 * cost[] is the whole result of the flood fill: a cell is reachable
 * exactly when cost[cell] != NO_COST, so it doubles as the highlight
 * set.  Meaningful only while a unit is selected — every reader gates
 * on that. */
#define NO_COST     0xFF

extern uint8_t cost[CELL_COUNT];

void movement_range(uint8_t start, uint8_t budget);

/* --- What the player is doing ---------------------------------------- */

/* The cursor: where the player is looking, not something standing on
   the board.  cur_* is the campaign overview's own cursor. */
extern uint8_t cursor_x, cursor_y;
extern uint8_t cur_x, cur_y;

/* The unit the player has picked up, or NO_UNIT, and where it stands. */
extern uint8_t selected;
extern uint8_t sel_x, sel_y;

extern uint8_t level;           /* 1-based; selects the campaign map    */
extern uint16_t turn;
extern uint8_t player_won;      /* the outcome ST_OVER is reporting     */

/* --- Orders ----------------------------------------------------------
 * These change the board and ask the renderer to catch up; none of them
 * draws anything itself. */
void load_map(void);            /* terrain + costs + both armies        */
void select_unit(uint8_t u);    /* pick up, and flood its range         */
void deselect(void);            /* put down                             */
void move_selected(void);       /* order it to the cursor's cell        */
void end_turn(void);            /* give every unit its action back      */

#endif /* _BOARD_H_ */
