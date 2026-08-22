#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

/* ================================================================== */
/* app_config.h — Configurable constants for ZX Map                   */
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

/* --- Demo tuning --- */
#define BAR_Y       96      /* pixel row of the moving tear-test bar */
#define BAR_SPEED   4       /* pixels per frame */

#endif /* _APP_CONFIG_H_ */
