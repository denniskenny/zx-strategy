#ifndef _GAME_H_
#define _GAME_H_

/* ================================================================== */
/* game.h — Main game loop with switchable states                     */
/* ================================================================== */

#include <stdint.h>

/* Game states.  One state is active per frame; the loop polls input,
 * updates the active state and switches when a state asks for it. */
#define ST_TITLE    0
#define ST_PLAY     1
#define ST_MAP      2
#define ST_PAUSE    3
#define ST_GALLERY  4
#define ST_MUSIC    5

/* The active state (read-only for callers outside game.c). */
extern uint8_t game_state;

/* Frame-synced loop: vsync_wait() → update active state → poll input
 * → optional state switch.  Never returns. */
void game_run(void);

#endif /* _GAME_H_ */
