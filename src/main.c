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

       BIT 5 IS CLEAR: it locks paging, and src/render.c needs the port
       afterwards to bank page 7 in for the shadow screen.

       Clearing it once before crashed every +3 — but that was not the
       lock's doing.  hw_detect() was clearing BIT 4, the ROM select,
       with interrupts enabled: on a +2A/+3 that pages in +3DOS, whose
       0x0038 is not a BASIC interrupt handler.  The lock had merely
       been hiding it by making the later writes no-ops.  hw_detect()
       now preserves bit 4 and runs behind di/ei, so the hazard is gone
       at source and the port can be left open.

       BIT 4 IS SET here for the same reason: this write must not change
       the ROM either. */
    if (vsync_mode != VSYNC_MODE_128K) {
        /* 48K or 128K: bank 0 at 0xC000, ROM 1, and LOCKED.  Nothing
           needs the port afterwards now that the shadow screen is gone,
           and the lock pins the bank our buffers live in. */
        __asm
        ld bc, #0x7FFD
        ld a, #0x30         ; bank 0, screen 0, ROM 1 (48K), LOCKED
        out (c), a
        __endasm;
    } else {
        /* +2A/+3.  Paging must be LOCKED here, and that costs the
           mode-2 floating bus, which is why this used to be skipped
           altogether.  The trade is not optional:

           this program keeps 7 KB of buffers above 0xC000 (see
           include/memmap.h), and on a +3 that window is the ROM's too —
           it pages banks in for the RAM disk and +3DOS workspace
           whenever it likes.  Leaving paging open let it do exactly
           that underneath us: no crash, just a tile sheet that came
           back part garbage and a title screen that never arrived.

           Locking pins bank 0 there for good.  Port 0x0FFD then reads
           0xFF for ever, so vsync_wait() would spin on a marker it can
           never see — the mode is forced down to HALT to match.  A +3
           therefore syncs on HALT and has no shadow screen; it renders
           correctly, which beats both. */
        vsync_mode = VSYNC_MODE_HALT;
        __asm
        ld bc, #0x7FFD
        ld a, #0x30         ; bank 0, screen 0, ROM 1 (48K), LOCKED
        out (c), a
        __endasm;
    }

    game_run();
    return 0;
}
