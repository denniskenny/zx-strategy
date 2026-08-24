#ifndef _PAGEPROBE_H_
#define _PAGEPROBE_H_

#include <stdint.h>

/*
 * P8 step 1.  Two implementations, chosen by the MAKEFILE via PROBE_SRC:
 * src/pageprobe.c in the 48k build, src/pageprobe_stub.c in the 128k one.
 *
 * The split is in SRCS rather than in #if on purpose.  zcc does not
 * evaluate preprocessor directives inside a function body containing an
 * __asm block — they reach the back end verbatim — so gating the call in
 * main() (which does contain __asm) silently did nothing and produced
 * `undefined symbol: _page_probe_run` from source that plainly excluded
 * it.  The call site here is unconditional; the stub is what makes it
 * free.  See src/pageprobe.c.
 */

extern uint8_t page_probe;      /* 1 = memory above 0xC000 survived */
void page_probe_run(void);

#endif /* _PAGEPROBE_H_ */
