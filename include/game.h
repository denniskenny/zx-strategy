#ifndef _GAME_H_
#define _GAME_H_

/* ================================================================== */
/* game.h — Main game loop with switchable states                     */
/* ================================================================== */

#include <stdint.h>

/* Game states.  One state is active per frame; the loop polls input,
 * updates the active state and switches when a state asks for it.
 *
 * Every state here is part of the game.  Music is not a state: it is a
 * blocking call the title screen makes (docs/DESIGN.md § Long
 * operations), which is why there is no ST_MUSIC. */
#define ST_TITLE    0
#define ST_PLAY     1
#define ST_MAP      2
#define ST_OVER     3   /* level ended: win advances, loss returns to title */
#define ST_WON      4   /* campaign complete: past the last level */
#define ST_CUTSCENE 5   /* a banked screen and the tune; 128K only    */

/* The active state (read-only for callers outside game.c). */
extern uint8_t game_state;

/* Frame-synced loop: vsync_wait() → update active state → poll input
 * → optional state switch.  Never returns. */
void game_run(void);

#endif /* _GAME_H_ */
