#ifndef _DEMO_H_
#define _DEMO_H_

/* ================================================================== */
/* demo.h — Floating bus vsync demonstration loop                     */
/* ================================================================== */

/* Draws a static info screen plus a moving tear-test bar, syncing
 * each frame with vsync_wait().  Never returns. */
void demo_run(void);

#endif /* _DEMO_H_ */
