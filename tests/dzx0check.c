/*
 * dzx0check.c — ZX0 decompression regression harness
 *
 * Decompresses the unit sprite sheet — a blob the game itself unpacks
 * at startup — into a staging buffer, then writes a result block at
 * 0xF000 that can be read back over ZRCP:
 *
 *   0xF000  magic 0x5A ('Z') once the run completed
 *   0xF001  low byte  of the 16-bit sum of the decompressed block
 *   0xF002  high byte of that sum
 *   0xF003+ first 16 decompressed bytes
 *
 * Any ZX0 blob in the build would do; this one is used because dzx0 is
 * load-bearing for every tile sheet and level map, so a regression here
 * is a regression in something the game cannot start without.
 *
 * The check is a SUM over the whole block, not a count of bytes that
 * changed from the 0xAA fill.  Counting looks like it measures how much
 * was written, but it silently under-reports every byte the data itself
 * happens to set to 0xAA — this sheet contains two — while a sum catches
 * both a short write (the tail stays 0xAA) and corruption anywhere in
 * the middle.  Compare it against the host reference.
 *
 * Build: make dzx0check      Read: read-memory 61440 32
 */

#include <stdint.h>
#include "../include/dzx0.h"
#include "../include/units_view.h"

#define SCRATCH ((uint8_t *)0x6000)
#define RESULT  ((uint8_t *)0xF000)
#define RAW_SIZE UNITS_VIEW_RAW_SIZE

int main(void)
{
    uint16_t i, sum = 0;

    for (i = 0; i < RAW_SIZE; i++) SCRATCH[i] = 0xAA;
    for (i = 0; i < 32; i++) RESULT[i] = 0;

    dzx0_decompress(units_view_zx0, SCRATCH);

    for (i = 0; i < RAW_SIZE; i++) sum = (uint16_t)(sum + SCRATCH[i]);

    RESULT[0] = 0x5A;
    RESULT[1] = (uint8_t)(sum & 0xFF);
    RESULT[2] = (uint8_t)(sum >> 8);
    for (i = 0; i < 16; i++) RESULT[3 + i] = SCRATCH[i];

    __asm
        ld a, 7
        out (0xFE), a
    __endasm;

    for (;;) { }
    return 0;
}
