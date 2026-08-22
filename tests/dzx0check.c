/*
 * dzx0check.c — ZX0 decompression regression harness
 *
 * Decompresses the Great Old One blob into the same staging buffer the
 * demo uses, then writes a result block at 0xF000 that can be read back
 * over ZRCP:
 *
 *   0xF000  magic 0x5A ('Z') once the run completed
 *   0xF001  low byte  of the number of bytes written
 *   0xF002  high byte of the number of bytes written
 *   0xF003+ first 16 decompressed bytes
 *
 * Build: make dzx0check      Read: read-memory 61440 32
 */

#include <stdint.h>
#include "../include/dzx0.h"
#include "../include/goo_data.h"

#define SCRATCH ((uint8_t *)0x6000)
#define RESULT  ((uint8_t *)0xF000)

int main(void)
{
    uint16_t i, written = 0;

    for (i = 0; i < GOO_CROP_SIZE; i++) SCRATCH[i] = 0xAA;
    for (i = 0; i < 32; i++) RESULT[i] = 0;

    dzx0_decompress(goo_final, SCRATCH);

    for (i = 0; i < GOO_CROP_SIZE; i++)
        if (SCRATCH[i] != 0xAA) written++;

    RESULT[0] = 0x5A;
    RESULT[1] = (uint8_t)(written & 0xFF);
    RESULT[2] = (uint8_t)(written >> 8);
    for (i = 0; i < 16; i++) RESULT[3 + i] = SCRATCH[i];

    __asm
        ld a, 7
        out (0xFE), a
    __endasm;

    for (;;) { }
    return 0;
}
