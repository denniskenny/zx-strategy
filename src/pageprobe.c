/*
 * pageprobe.c -- P8 step 1: is the 0xC000 window survivable?
 *
 * Page 7 must be mapped at 0xC000 to draw into the shadow screen, and it
 * is currently mapped once at startup and left there.  That is what
 * costs this program every byte above 0xC000 and caps the 128k build at
 * 16 KB (docs/PLAN.md P8).
 *
 * The way out is to map it only for the duration of a copy.  Before any
 * of that is built, this answers the one question it rests on: does
 * memory above 0xC000 survive being paged away and back?
 *
 * A pattern is written high in the window, page 7 is banked in, the
 * screen area is scribbled on, bank 0 comes back, and the pattern is
 * checked.  `page_probe` is 1 if it survived.  Nothing depends on the
 * answer yet; the title screen reports it so all three machines can be
 * asked, which is the only way to ask a +3.
 *
 * IN THE 48k BUILD ONLY, and that is the point rather than a compromise:
 * the 48k build is the one with code and bss above 0xC000, so running
 * its tap on a 128K or +3 tests exactly the case P8 depends on.
 *
 * NO PREPROCESSOR DIRECTIVES BELOW, and none near the asm.  zcc passes
 * #if inside an __asm-bearing function straight through unevaluated; the
 * first attempt at this file gated its call site that way and the
 * directive was ignored.  The Makefile picks this file or the stub.
 *
 * Interrupts are off across the window because a +2A/+3 ROM handler may
 * page, and because anything of ours up there would not be present to
 * return to.
 */

#include <stdint.h>

#include "../include/hw.h"
#include "../include/pageprobe.h"

uint8_t page_probe;

void page_probe_run(void)
{
    if (!is_128k) return;

    __asm
        di
        ld  hl, #0xFF00         ; a byte the shadow screen never reaches
        ld  (hl), #0x5A

        ld  bc, #0x7FFD
        ld  a, #0x17            ; page 7 in, ROM bit (4) preserved
        out (c), a
        ld  (0x5B5C), a         ; BANKM, or the ROM writes its copy back
        ld  a, #0xA5
        ld  (0xC000), a         ; scribble where the screen would go
        ld  (0xDAFF), a         ; and at its far end

        ld  a, #0x10            ; bank 0 back
        out (c), a
        ld  (0x5B5C), a

        ld  hl, #0xFF00
        ld  a, (hl)
        cp  #0x5A               ; did our byte survive the round trip?
        ld  a, #0x00
        jr  nz, _pp_done
        inc a
    _pp_done:
        ld  (_page_probe), a
        ei
    __endasm;
}
