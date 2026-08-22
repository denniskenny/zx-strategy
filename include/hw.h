#ifndef _HW_H_
#define _HW_H_

/* ================================================================== */
/* hw.h — Hardware detection (128K, Kempston)                         */
/* ================================================================== */

#include <stdint.h>

/* Detect whether we're on a 128K Spectrum. Sets is_128k.
 * Also probes for a Kempston joystick interface. Sets has_kempston.
 * Must run BEFORE paging is locked. */
void hw_detect(void);

/* 1 if 128K detected, 0 otherwise */
extern uint8_t is_128k;

/* 1 if Kempston joystick interface detected, 0 otherwise */
extern uint8_t has_kempston;

#endif /* _HW_H_ */
