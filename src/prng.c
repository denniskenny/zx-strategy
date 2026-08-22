/*
 * prng.c — 16-bit PRNG (LFSR taps 0,2,3,5 XOR Weyl sequence).
 */

#include "../include/prng.h"

uint16_t prng_next(prng_t *p)
{
    uint16_t bit = ((p->lfsr >> 0) ^ (p->lfsr >> 2) ^
                    (p->lfsr >> 3) ^ (p->lfsr >> 5)) & 1;
    p->lfsr = (p->lfsr >> 1) | (bit << 15);
    p->weyl += 0x9E35;
    return p->lfsr ^ p->weyl;
}
