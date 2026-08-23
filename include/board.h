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
#include "memmap.h"

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
#define terrain   ((uint8_t *)MEM_TERRAIN)     /* terrain id per cell   */
/* Hand-placed below the program; see include/memmap.h. */
#define cell_cost ((uint8_t *)MEM_CELL_COST)   /* cost to ENTER, 0 = impassable */

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

/* Hand-placed below the program; see include/memmap.h. */
#define u_type    ((uint8_t *)MEM_U_TYPE)
#define u_cell    ((uint8_t *)MEM_U_CELL)   /* row_base[y] + x, never a pair */
#define u_hp      ((uint8_t *)MEM_U_HP)
#define u_flags   ((uint8_t *)MEM_U_FLAGS)

#if UNITS_MAX > 40
#error "the unit arrays no longer fit the blocks memmap.h reserves for them"
#endif
extern uint8_t unit_count;              /* slots used, 0..UNITS_MAX */

/* Which unit stands where, or NO_UNIT.  This is what makes "one unit
   per tile" an O(1) fact rather than a scan of the roster. */
#define occupancy ((uint8_t *)MEM_OCCUPANCY)

/* --- Movement range --------------------------------------------------
 * cost[] is the whole result of the flood fill: a cell is reachable
 * exactly when cost[cell] != NO_COST, so it doubles as the highlight
 * set.  Meaningful only while a unit is selected — every reader gates
 * on that. */
#define NO_COST     0xFF

#define cost      ((uint8_t *)MEM_COST)

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
/* --- Combat ----------------------------------------------------------
 * Attack range is a DISTANCE, not a path: terrain does not block it and
 * no search is needed (docs/DESIGN.md § Attack Range).
 *
 * A unit that has already moved this turn may still strike, but only at
 * an ADJACENT enemy — the "move into contact" exception. attack_reach()
 * is what encodes that: full range while the unit is fresh, 1 once it
 * has spent its action moving. */
uint8_t attack_reach(uint8_t u);
uint8_t is_target(uint8_t cell);   /* selected can hit whoever is here  */
void attack(uint8_t cell);         /* ...so do it, and spend the action */
uint8_t damage_at(uint8_t attacker, uint8_t cell);

/* Set when a side has lost its base or its last unit.  logic.c cannot
   change game state — that belongs to the loop — so it raises this and
   src/game.c turns it into ST_OVER on the next pass. */
extern uint8_t outcome_ready;

/* --- The enemy turn --------------------------------------------------
 * Driven a unit at a time by src/game.c so the player can watch it
 * happen; logic.c decides, the loop paces and draws.
 *
 * enemy_begin() builds the threat map — one pass over the player's
 * units instead of a range test per candidate cell per unit — and
 * enemy_step() then acts for the next enemy that still has its action,
 * returning the cell it ended on, or NO_CELL when the army is done. */
#define NO_CELL     0xFF

#define threat    ((uint8_t *)MEM_THREAT)

void enemy_begin(void);
uint8_t enemy_step(void);

void load_map(void);            /* terrain + costs + both armies        */
void select_unit(uint8_t u);    /* pick up, and flood its range         */
void deselect(void);            /* put down                             */
void move_selected(void);       /* order it to the cursor's cell        */
void end_turn(void);            /* give every unit its action back      */

#endif /* _BOARD_H_ */
