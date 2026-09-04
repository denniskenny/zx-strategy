#ifndef _GAME_CONFIG_H_
#define _GAME_CONFIG_H_

/* ================================================================== */
/* game_config.h — Rules configuration for ZX Strategy                 */
/* ================================================================== */
/* The tunable side of the game rules, kept out of the code so a
 * balance change is a header edit and a rebuild.  Hardware and screen
 * constants live in app_config.h.  This file owns the ARMY
 * COMPOSITION, the per-type UNIT STATS and the per-terrain COSTS AND
 * COVER; all three are transcribed from docs/DESIGN.md, which stays
 * the source of truth for the numbers and the reasoning behind them.
 *
 * populate_map() reads these to build both sides at level start: one
 * base per side at opposite corners, the rest placed within
 * UNITS_PLACE_RADIUS tiles of it on non-water tiles.  See
 * docs/DESIGN.md, "Game Init". */

#include <stdint.h>

/* --- Sound effects --------------------------------------------------
 * White noise, and the only knob that matters is the PERIOD of a speaker
 * half-cycle in loop counts: the routine holds for `base + (random &
 * mask)` counts each way, at ~26 T-states a count.  So
 *
 *     Hz  ~=  67300 / counts
 *
 * A WIDE mask is what stops it settling into a tone -- the spread matters
 * as much as the centre.  Narrow it and it starts to whistle, which is
 * what the first version of this did by accident.
 *
 * `len` is how many cycles the burst lasts.  A low voice takes far longer
 * per cycle, so equal `len` does NOT mean equal duration; these are tuned
 * for roughly comparable lengths.
 */
#define SFX_MOVE        0       /* a unit moves:  ~180-560 Hz, a thud   */
#define SFX_ATTACK      1       /* a strike:      ~400-1680 Hz, a crack */
#define SFX_BOOM        2       /* a death:       ~37-224 Hz, a crunch  */
#define SFX_VOICES      3

static const uint16_t sfx_base[SFX_VOICES] = {     88,     40,    172 };
static const uint16_t sfx_mask[SFX_VOICES] = { 0x00FF, 0x007F, 0x05FF };
static const uint8_t  sfx_len[SFX_VOICES]  = {      3,      8,     24 };

/* HOW FAR THE PITCH WANDERS between one firing and the next.
 *
 * `mask` above already randomises the period cycle BY cycle, which is
 * what makes each burst noise rather than a tone.  But every burst
 * started from the same `base`, so every move sounded like exactly the
 * same move -- and MOVE fires on every step of every unit, which is
 * where a repeated sound is most obviously a repeated sound.
 *
 * This picks one offset per BURST, so successive footsteps differ from
 * each other while each one still holds together.  A power-of-two mask,
 * not a modulo: the Z80 has no divide.
 *
 * The bases above were LOWERED by half the spread, so the average pitch
 * is what it was before tuning -- 120 and 300.  Change a spread and
 * change its base by half as much in the other direction, or the voice
 * moves as well as widening.
 *
 * ATTACK is deliberately 0.  It is the confirmation that a shot landed,
 * and a confirmation that sounds different every time reads as a
 * different event.  The two sounds that describe a PHYSICAL thing -- a
 * footfall, a blast -- are the ones that want the variation. */
static const uint16_t sfx_vary[SFX_VOICES] = { 0x003F, 0x0000, 0x00FF };
/* MOVE is deliberately the shortest burst of the three -- a tick, not a
 * thud.  It fires on every step of every unit, so it is the one sound the
 * player hears constantly, and anything with a tail makes holding a
 * direction feel like wading.  At ~3.7 ms a cycle three of them is about
 * 11 ms; ten was 37 ms and dragged. */

/* Turns a level is "meant" to take.  The score for winning is this less
 * the turns spent, floored at zero -- so 20 is both the par and the best
 * possible score, and a level dragged past it is worth nothing rather
 * than a negative.  Raise it to be kinder about slow play. */
#define SCORE_PAR   20

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

/* The view sheet carries one sprite that is NOT a unit type: an explosion,
 * used as a transient effect where something died.  It is a sprite index,
 * not a type -- nothing has one of these, it has no health, damage, range
 * or movement, and it must never appear in the tables below or in
 * unit_count.  Hence a separate count for the sheet.
 *
 * The MAP sheet does not have it: an explosion is a moment on the board,
 * and the map view is a schematic of where things are. */
#define SPRITE_EXPLOSION  4
#define VIEW_SPRITES      5

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

/* --- Unit stats ------------------------------------------------------
 * docs/DESIGN.md, "Units".  One row per UNIT_* id, in that order, so a
 * stat is unit_range[u_type[i]] with no search.  Every column is a
 * byte: health tops out at the Base's 255 by design, which is why
 * u_hp[] is uint8_t and damage subtracts straight from it.
 *
 * Cannon and Base have Movement 0 — they are placed and never move,
 * which is what makes the stalemate rule in docs/DESIGN.md necessary. */
static const uint8_t unit_range[UNIT_TYPES]    = {  3,  2,  4,  0 };
static const uint8_t unit_damage[UNIT_TYPES]   = {  5, 10,  8,  0 };
static const uint8_t unit_health[UNIT_TYPES]   = { 10, 15, 20, 25 };
static const uint8_t unit_movement[UNIT_TYPES] = {  3,  2,  0,  0 };

/* The widest movement budget in the roster, which is the number of
 * buckets Dial's algorithm needs (docs/PLAN.md, "Movement range").
 * Raising any unit's Movement past this grows q[]/bucket_end[]. */
#define MAX_MOVE        3

/* The longest attack range, which bounds the Manhattan disc the enemy
 * threat map stamps per player unit. */
#define MAX_RANGE       4

/* --- Terrain ---------------------------------------------------------
 * docs/DESIGN.md, "Tiles".  Terrain id = GID - firstgid = the tile
 * column in both .zxp sheets, so this order is load-bearing in exactly
 * the same way UNIT_* is: append, never insert.
 *
 * Passability is NOT repeated here — the map converter emits it per
 * level as level_N_terrain_blocked[] from the Tiled "impassable"
 * property, and duplicating it would let the two disagree.  Water's
 * movement cost is a placeholder for that reason: nothing may enter it,
 * so the cost is never read. */
#define TER_PLAIN       0
#define TER_FOREST      1
#define TER_WATER       2
#define TER_HILLS       3
#define TER_CITY        4
#define TER_TYPES       5

/* --- Enemy scoring ---------------------------------------------------
 * A candidate cell is worth
 *     -distance_to_nearest_target * AI_W_DIST
 *     -threat[cell]               * AI_W_THREAT
 *     +cover[terrain[cell]]       * AI_W_COVER
 * Distance dominates so the army actually advances; threat and cover
 * decide between cells that are equally close.  Cover is a percentage,
 * so its weight is the smallest of the three by a wide margin.
 * Tuning these is a rebuild, not a code change. */
#define AI_W_DIST        8
#define AI_W_THREAT      4
#define AI_W_COVER       8      /* divisor, not multiplier: cover / 8 */
#define AI_SCORE_BASE  200      /* keeps the sum inside a byte */

/* --- What the enemy thinks an exchange is worth -----------------------
 * Under docs/DESIGN.md § Adjacency an attack is a TRADE: the defender
 * hits back for half unless it dies.  So the enemy scores
 *
 *     score = gain - counter * AI_W_COUNTER
 *
 * and not just the damage it deals.  Without this it walks into
 * exchanges it should refuse, and wounded-damage then grinds its own
 * units down for the rest of the level.
 */

/* A kill takes no counter at all, so it is worth more than its damage
 * number -- this is what makes the enemy finish what it starts rather
 * than spreading damage around. */
#define AI_KILL          200

/* On top of the score, so a base is worth walking into a bad trade for.
 * It is the win condition; nothing else on the board is. */
#define AI_BASE_BONUS    150

/* How much the counter weighs against the damage dealt.  1 is an even
 * trade, higher makes the enemy cautious, 0 makes it ignore counters
 * altogether -- which is how it behaved before these rules existed, and
 * is worth keeping reachable for comparison. */
#define AI_W_COUNTER     1

/* Refuse an exchange scoring at or below this and move instead.  0 means
 * "never take a trade that loses more than it deals".  Raise it to make
 * the enemy hold out for good trades; drop it below zero to let it throw
 * units away. */
#define AI_MIN_TRADE     0

static const uint8_t terrain_move_cost[TER_TYPES] = { 1, 2, 0, 2, 1 };

/* Percent of incoming damage cancelled by standing here.  The formula
 * is (damage * (100 - cover) + 99) / 100 — rounding up, so cover under
 * 100 never makes a unit unkillable. */
static const uint8_t terrain_cover[TER_TYPES]     = { 0, 50, 0, 25, 75 };

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
