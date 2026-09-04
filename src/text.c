/*
 * text.c -- the string pool, unpacked once at boot.
 *
 * Every string the player reads is ZX0 in the program and lives raw
 * above MEM_END from text_init() onwards.  628 bytes of text compress to
 * 356, and the decompressor is dzx0 -- already linked for the tiles, the
 * maps and the music -- so the saving costs no decoder at all.
 *
 * That last point is the whole reason this replaced a word dictionary.
 * The dictionary packed the same text to 500 bytes but needed 116 bytes
 * of bespoke expander, which at 46 strings was more than it saved: the
 * measured net was TWELVE BYTES WORSE than storing the text raw.  A
 * scheme that reuses a decoder the program already carries starts 116
 * bytes ahead of one that does not.
 *
 * IN ITS OWN FILE, not in gfx.c beside print_at(), because gfx.c
 * contains an __asm block and zcc silently drops #if directives that
 * appear after one -- see .claude/skills/zx-memory.  The size check
 * below is exactly the kind of thing that would have vanished.
 *
 * BOTH MACHINES.  MEM_TEXTPOOL is above MEM_END: plain RAM on a 48K,
 * page 7 on a 128K, which is mapped throughout play.  The same placement
 * the music buffer already uses.
 *
 * ONE HAZARD.  A cutscene pages a bank in over 0xC000-0xFFFF, and the
 * pool is inside that window.  render_cutscene() pages in, decompresses,
 * and pages straight back out with nothing printed in between -- but a
 * print_at() added between those two calls would read the BANK where the
 * text should be, and draw rubbish with no clue why.  Do not print while
 * a bank is in.
 */

#include <stdint.h>

#include "../include/dzx0.h"
#include "../include/memmap.h"
#include "../include/strings.h"

extern const uint8_t text_zx0[];

/* The generated pool must fit the hole memmap.h reserved for it.  Both
   numbers move on their own -- adding a string grows one, editing
   memmap.h changes the other -- so the build has to check. */
#if TEXT_POOL_SIZE > MEM_TEXTPOOL_SIZE
#error "the strings no longer fit MEM_TEXTPOOL: raise MEM_TEXTPOOL_SIZE"
#endif

void text_init(void)
{
    dzx0_decompress(text_zx0, (uint8_t *)MEM_TEXTPOOL);
}
