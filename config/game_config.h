#ifndef _GAME_CONFIG_H_
#define _GAME_CONFIG_H_

/* ================================================================== */
/* game_config.h — Rules configuration for ZX Strategy                 */
/* ================================================================== */
/* The tunable side of the game rules, kept out of the code so a
 * balance change is a header edit and a rebuild.  Hardware and screen
 * constants live in app_config.h; unit stats (range, damage, health,
 * movement) and terrain costs are specified in docs/DESIGN.md and are
 * not here yet — this file currently owns the ARMY COMPOSITION only.
 *
 * populate_map() reads these to build both sides at level start: one
 * base per side at opposite corners, the rest placed within
 * UNITS_PLACE_RADIUS tiles of it on non-water tiles.  See
 * docs/DESIGN.md, "Game Init". */

#include <stdint.h>

/* --- Unit types ---------------------------------------------------- */
/* Ids are indices, and the order is load-bearing: it is the sprite
 * column order in assets/units_view.zxp and assets/units_map.zxp, and
 * the row order of every table below.  Adding a type means adding a
 * sprite column to both sheets and bumping UNIT_COUNT in the Makefile.
 * See .claude/skills/zx-tiles. */
#define UNIT_INFANTRY   0
#define UNIT_TANK       1
#define UNIT_CANNON     2
#define UNIT_BASE       3
#define UNIT_TYPES      4

/* --- Army composition ---------------------------------------------- */
/* How many of each type a side starts level 1 with, and how many more
 * it gains on each odd-numbered level after that ("each subsequent
 * odd-numbered level will have an additional unit of each type").  Both
 * sides get the same army: the map decides the advantage, not the
 * roster.  Asymmetric sides would mean a second pair of tables indexed
 * by side.
 *
 * The base is the win condition, so it does NOT scale — one per side,
 * always, however deep the campaign runs. */
#define UNITS_INFANTRY_START    3
#define UNITS_TANK_START        2
#define UNITS_CANNON_START      1
#define UNITS_BASE_START        1

#define UNITS_INFANTRY_PER_LEVEL 1
#define UNITS_TANK_PER_LEVEL     1
#define UNITS_CANNON_PER_LEVEL   1
#define UNITS_BASE_PER_LEVEL     0

/* Levels the campaign runs to: assets/maps/level_1.tmx .. level_10.tmx,
 * which is also LEVEL_COUNT in src/game.c.  This bounds the unit
 * arrays, so raising it costs RAM before the maps are even authored. */
#define GAME_LEVELS     10

/* Units of one type on a given level (level 1 = the first).  The gain
 * lands on the odd-numbered levels — 3, 5, 7, 9 — so ten levels add
 * four units per type, not nine, and the army still fits around its
 * base.  No cast: this has to stay usable in #if, and every result
 * fits a byte. */
#define UNITS_AT_LEVEL(start, gain, level) \
    ((start) + (gain) * (((level) - 1) / 2))

/* Worst-case army size, for sizing the runtime unit arrays. */
#define UNITS_PER_SIDE_MAX                                             \
    (UNITS_AT_LEVEL(UNITS_INFANTRY_START, UNITS_INFANTRY_PER_LEVEL,    \
                    GAME_LEVELS)                                       \
     + UNITS_AT_LEVEL(UNITS_TANK_START, UNITS_TANK_PER_LEVEL,          \
                      GAME_LEVELS)                                     \
     + UNITS_AT_LEVEL(UNITS_CANNON_START, UNITS_CANNON_PER_LEVEL,      \
                      GAME_LEVELS)                                     \
     + UNITS_AT_LEVEL(UNITS_BASE_START, UNITS_BASE_PER_LEVEL,          \
                      GAME_LEVELS))

/* Both sides on the board at once. */
#define UNITS_MAX       (UNITS_PER_SIDE_MAX * 2)

/* The same numbers as tables, for code that walks the unit types
 * rather than naming them.  Row order = the UNIT_* ids above. */
static const uint8_t unit_start_count[UNIT_TYPES] = {
    UNITS_INFANTRY_START,
    UNITS_TANK_START,
    UNITS_CANNON_START,
    UNITS_BASE_START
};

static const uint8_t unit_level_gain[UNIT_TYPES] = {
    UNITS_INFANTRY_PER_LEVEL,
    UNITS_TANK_PER_LEVEL,
    UNITS_CANNON_PER_LEVEL,
    UNITS_BASE_PER_LEVEL
};

/* --- Placement -------------------------------------------------------
 * populate_map() puts the non-base units within this many tiles of
 * their base (Chebyshev distance, so a square block).  The base sits in
 * a corner, so only the quarter-square inside the map is usable:
 * (R+1)^2 cells, minus the one the base occupies.
 *
 * R = 4 leaves 24 slots, which is the level-10 army (18) plus room for
 * water.  Raising the roster past that means raising R, and R is capped
 * by the map: at R = 6 the two 14x7 corners' blocks start to overlap. */
#define UNITS_PLACE_RADIUS  4
#define UNITS_PLACE_SLOTS   ((UNITS_PLACE_RADIUS + 1) * \
                             (UNITS_PLACE_RADIUS + 1) - 1)

/* An army has to fit the map it is placed on.  This catches a roster
 * that cannot be placed long before the placement loop spins looking
 * for a free tile — and it counts every cell in the block, so a corner
 * with water in it is tighter still. */
#if (UNITS_PER_SIDE_MAX - UNITS_BASE_START) > UNITS_PLACE_SLOTS
#error "army too large to place around one base; lower the per-level gain or raise UNITS_PLACE_RADIUS"
#endif

#endif /* _GAME_CONFIG_H_ */
