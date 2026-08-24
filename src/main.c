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
    /* Bank 0 at 0xC000, screen 0, ROM 1 (48K BASIC).  Unconditional:
       harmless on a 48K where 0x7FFD is not decoded, and needed on
       every 128K-class machine to start from a known map.

       BIT 5 IS CLEAR — paging stays open.  src/render.c banks page 7 in
       for the shadow screen afterwards, and a +2A/+3 needs port 0x0FFD
       readable for its floating bus, which the lock would deny.

       BIT 4 IS SET.  It is the ROM select, and clearing it on a +2A/+3
       pages in +3DOS underneath the running program: no BASIC handler
       at 0x0038, no character set at 0x3D00.  That crashed every +3
       until hw_detect() stopped doing it.

       BANKM (0x5B5C) is updated to match.  The port is write-only, so
       the ROM keeps its own copy there and writes it back whenever it
       touches paging; leaving it stale means the ROM undoes us. */
    __asm
    ld bc, #0x7FFD
    ld a, #0x10
    out (c), a
    ld (0x5B5C), a      ; BANKM: the ROM keeps a copy of this port
    __endasm;

    game_run();
    return 0;
}
