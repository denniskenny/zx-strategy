#ifndef _VSYNC_H_
#define _VSYNC_H_

/* ================================================================== */
/* vsync.h — Floating bus vsync with model auto-detection             */
/* ================================================================== */

#include <stdint.h>

/* 0 = HALT fallback, 1 = 48K/128K/+2 floating bus, 2 = +2A/+3 floating bus */
#define VSYNC_MODE_HALT 0
#define VSYNC_MODE_48K  1
#define VSYNC_MODE_128K 2

extern uint8_t vsync_mode;

/* Attribute row 22 of the screen currently being DISPLAYED — the marker
   the floating bus sync watches for.  src/render.c repoints this when a
   128K flips display files; nothing else should touch it. */
extern uint8_t *vsync_marker_addr;

/* Detect which floating bus technique works on this machine.
 * Call once at startup, after hw_detect() and BEFORE locking
 * paging (port 0x7FFD bit 5) if +2A/+3 support is desired. */
void vsync_detect(void) __naked;

/* Wait for the beam to reach the sync marker near the bottom of the
 * active display.  Returns with ~28 000 T-states available before the
 * beam re-enters the top of the display area.  Falls back to HALT
 * (~14 000 T available) when the floating bus is not supported.
 * Refreshes the marker attributes on every call. */
void vsync_wait(void) __naked;

#endif /* _VSYNC_H_ */
