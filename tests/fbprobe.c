/*
 * fbprobe.c — Floating bus probe
 *
 * Diagnostic harness: fills the screen with a known attribute pattern,
 * writes the sync marker to attr row 22, then samples port 0x40FF a
 * large number of times and builds a 256-entry byte histogram at
 * 0xF000 (one saturating counter per value observed on the bus).
 *
 * Read the histogram back over ZRCP:  read-memory 61440 256
 *
 *   - Only 0xFF non-zero        → no floating bus on this machine/emulator
 *   - Attribute values present  → floating bus works; a hang in
 *                                 vsync_wait() is a loop-timing issue
 *
 * Build:  make -C .. tests/fbprobe.tap   (see Makefile target `probe`)
 */

#include <stdint.h>

#define HIST ((uint8_t *)0xF000)

static void fill_screen(void)
{
    uint16_t i;
    for (i = 0; i < 6144; i++) ((uint8_t *)0x4000)[i] = 0;
    for (i = 0; i < 768; i++)  ((uint8_t *)0x5800)[i] = 0x07;
    /* marker: attr row 22, cols 0-2; col 3 = +2A/+3 preload */
    ((uint8_t *)0x5800)[22 * 32 + 0] = 0x03;
    ((uint8_t *)0x5800)[22 * 32 + 1] = 0x03;
    ((uint8_t *)0x5800)[22 * 32 + 2] = 0x03;
    ((uint8_t *)0x5800)[22 * 32 + 3] = 0x00;
    for (i = 0; i < 256; i++) HIST[i] = 0;
}

static void probe(void) __naked
{
    __asm
        ld  bc, 0                ; 65536 samples
_fb_loop:
        ld  a, 0x40
        in  a, (0xFF)            ; read port 0x40FF
        ld  l, a
        ld  h, 0xF0              ; HL = 0xF000 + value
        ld  a, (hl)
        inc a
        jr  z, _fb_sat           ; saturate at 255
        ld  (hl), a
_fb_sat:
        dec bc
        ld  a, b
        or  c
        jr  nz, _fb_loop
        ret
    __endasm;
}

int main(void)
{
    fill_screen();
    probe();
    /* signal completion: border white, then spin */
    __asm
        ld  a, 7
        out (0xFE), a
    __endasm;
    for (;;) { }
    return 0;
}
