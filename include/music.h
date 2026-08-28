#ifndef _MUSIC_H_
#define _MUSIC_H_

/* ================================================================== */
/* music.h — Tritone beeper tunes (Shiru's engine, Beepola exports)   */
/* ================================================================== */

#include <stdint.h>

/* Each tune is a linkable module generated from assets/music/NAME.txt
 * by the Makefile (txt2tritone.py → gen_tritone_module.py).  All tunes
 * share the single engine module assets/music/tritone_engine.asm.
 *
 * NAME_play() BLOCKS: it loops the tune with interrupts disabled and
 * returns on any key or joystick press, so only call it from a static
 * screen — never from a frame loop that also drives vsync.
 *
 * To add a tune: write assets/music/NAME.txt, append
 * assets/music/NAME_linkable.asm to MUSIC_LINKABLE in the Makefile,
 * and declare void NAME_play(void) here.
 * See .claude/skills/tritone-music. */
void lowlands_play(void);

/* The British Grenadiers -- traditional, public domain.  Two-part melody
   with a drum tattoo; the title screen's tune.  Blocks until a key, like
   every Tritone tune: it owns the speaker. */
void grenadiers_play(void);

/* Rows played by the last tune — shared entropy counter, valid right
 * after any *_play() returns.  Handy as a PRNG seed. */
extern uint16_t tritone_ticks;

#endif /* _MUSIC_H_ */
