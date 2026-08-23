#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

/* ================================================================== */
/* app_config.h — Configurable constants for ZX Strategy                   */
/* ================================================================== */

#include <stdint.h>

/* --- Screen layout --- */
#define SCREEN   ((uint8_t *)0x4000)
#define ATTR     ((uint8_t *)0x5800)
#define PIX_SIZE 6144
#define ATTR_SZ  768

/* --- Keyboard half-row port addresses --- */
#define KEY_QWERT  0xFBFE   /* Q=bit0 */
#define KEY_ASDFG  0xFDFE   /* A=bit0 */
#define KEY_POIUY  0xDFFE   /* P=bit0  O=bit1 */
#define KEY_SHZXCV 0xFEFE   /* CapsShift=bit0  Z=bit1  X=bit2 */

/* --- Kempston joystick --- */
#define KEMP_PORT  0x001F   /* active-high: R=0 L=1 D=2 U=3 Fire=4 */

/* --- Floating bus sync marker ------------------------------------- */
/* Attribute written across the whole of row 22 and matched by the
 * timed floating bus loop.  Must be unique on screen, must not be
 * 0xFF and must have bit 0 set (+2A/+3 ORs the bus value with 1).
 * 0x03 = black paper, magenta ink → invisible on a blank row.
 * No other attribute used by this app (nor any of them ORed with 1)
 * equals 0x03.
 *
 * A full 32-cell row is used rather than a few cells: the ULA only
 * puts a byte on the bus during its fetch slots, so a wide marker is
 * hit far sooner (and matters a lot under emulators, whose floating
 * bus windows are narrower than real hardware). Row granularity is
 * all the sync needs — the beam is at row 22 either way. */
#define VSYNC_MARKER       0x03
#define VSYNC_MARKER_ADDR  0x5AC0   /* attr row 22, col 0  */
#define VSYNC_MARKER_CELLS 32       /* whole row           */

/* +2A/+3 only: contended address read each iteration so the idle bus
 * holds a known NON-marker value.  Attr row 23, col 0. */
#define VSYNC_PRELOAD_ADDR 0x5AE0

/* --- Debug: P0 state walk ------------------------------------------
 * There is no combat yet, so nothing can win or lose a level and
 * ST_OVER / ST_WON are unreachable.  With this set, ST_PLAY takes two
 * extra keys — W wins the level, L loses it — so the whole campaign
 * loop (play → ST_OVER → next level → ... → ST_WON → title) can be
 * walked and tested before a single unit exists.
 *
 * DELETE THIS, and the code it guards, in P4 when the real win check
 * over the loser's roster replaces it.  See docs/PLAN.md. */
/* Overridable from the Makefile: the 128k target builds without it.
   That build has a hard 16 KB ceiling and the debug keys are the one
   thing in here that is not the game.  tests/p0_state_walk.py drives
   the campaign through them, so the 48k build — the one the tests
   use — keeps them. */
#ifndef DEBUG_STATE_WALK
#define DEBUG_STATE_WALK 1
#endif

#endif /* _APP_CONFIG_H_ */
