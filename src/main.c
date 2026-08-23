/*
 * main.c — Entry point for ZX Strategy
 *
 * Detects hardware and the floating bus technique BEFORE locking
 * paging, then runs the game loop.
 */

#include "../include/hw.h"
#include "../include/vsync.h"
#include "../include/game.h"

int main(void)
{
    /* Both the 128K bank-switching test and the +2A/+3 floating bus
       probe need paging enabled, so they must run first. */
    hw_detect();
    vsync_detect();

    /* Put a 128K into a known 48K-compatible map: bank 0 at 0xC000,
       screen 0, ROM 1 (48K BASIC).  Harmless on 48K, where 0x7FFD is not
       decoded.

       Skipped on +2A/+3 machines using the mode-2 floating bus, which
       needs port 0x0FFD readable — this write is what used to lock it
       out, and the sync would hang.

       BIT 5 LOCKS PAGING, and it is set on purpose.  Clearing it lets
       src/render.c bank page 7 in for the shadow screen, which is what
       a 128K needs — but it also lets those writes reach a +2A/+3,
       where they disturb the map the loader established and drop the
       machine back into BASIC with "Nonsense in BASIC".  The lock is
       what was protecting those machines, silently, all along.

       So the shadow screen stays off until this can be made +2A/+3
       safe, which needs a +3 to test against: detecting one reliably is
       the missing piece, since a +3 that falls back to HALT sync is
       indistinguishable from a 128K here.  A tear-free scroll on one
       model is not worth a crash on another. */
    if (vsync_mode != VSYNC_MODE_128K) {
        __asm
        ld bc, #0x7FFD
        ld a, #0x30         ; bank 0, screen 0, ROM 1 (48K), lock
        out (c), a
        __endasm;
    }

    game_run();
    return 0;
}
