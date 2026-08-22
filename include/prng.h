#ifndef _PRNG_H_
#define _PRNG_H_

#include <stdint.h>

/* 16-bit PRNG: an LFSR (taps 0,2,3,5) XORed with a Weyl sequence.
 * Each consumer keeps its own prng_t so streams stay independent
 * and reproducible. */
typedef struct {
    uint16_t lfsr;
    uint16_t weyl;
} prng_t;

/* Advance the state and return the next 16-bit value. */
uint16_t prng_next(prng_t *p);

#endif /* _PRNG_H_ */
