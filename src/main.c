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

    /* Lock 128K paging into 48K-compatible mode.
       Harmless on 48K (port 0x7FFD is not decoded).
       Skipped on +2A/+3 machines using the mode-2 floating bus: with
       paging locked, port 0x0FFD always reads 0xFF and the sync would
       hang. */
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
