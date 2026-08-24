/*
 * lowmem.c -- can this machine take CLEAR 24575 and code at 0x6000?
 *
 * Lowering -zorg from 0x8000 to 0x6000 would give code 24 KB instead of
 * 16 KB (docs/PLAN.md P9), but it pushes BASIC's program, variables and
 * stack down into 0x5C00-0x5FFF.  A +3 has crashed on exactly that
 * before, so this asks the question on its own before any buffer moves.
 *
 * Three things have to hold, and the border says which failed even if
 * the screen never paints:
 *
 *   RED     we did not get here at all
 *   YELLOW  running at 0x6000, but the stack is not where we expect
 *   CYAN    running at 0x6000 with the stack below it
 *   GREEN   and 0xDB00-0xFFFF holds every byte written to it
 *
 * Anything but GREEN means the idea is dead on that machine.
 *
 *   make lowmem
 *   fuse --machine plus3 tests/lowmem.tap        <-- the one that matters
 */

#include <stdint.h>

uint16_t sp_val;

#define BORDER(c)   (*(volatile uint8_t *)0x5C48 = (uint8_t)((c) << 3), out_border(c))

static void out_border(uint8_t c) __z88dk_fastcall
{
    (void)c;
    __asm
        ld  a, l
        and #0x07
        out (0xFE), a
    __endasm;
}

/* Print one character cell of solid ink so a blank screen is
   distinguishable from a hang, without dragging in any font. */
static void blot(uint8_t col, uint8_t row, uint8_t attr)
{
    uint8_t i;
    uint8_t *p = (uint8_t *)(0x4000 + (row & 7) * 32 + ((row >> 3) << 11) + col);
    for (i = 0; i < 8; i++) p[i << 8] = 0xFF;
    *((uint8_t *)0x5800 + row * 32 + col) = attr;
}

int main(void)
{
    uint16_t sp;
    uint8_t ok = 1;
    volatile uint8_t *low;

    BORDER(2);                          /* red: reached main() */

    __asm
        ld  hl, #0
        add hl, sp
        ld  (_sp_val), hl
    __endasm;

    sp = sp_val;
    blot(0, 0, 0x47);

    /* The stack must be below where code now starts, or BASIC has put it
       somewhere that will be overwritten the moment we recurse. */
    if (sp > 0x6000) { BORDER(6); ok = 0; }     /* yellow */

    /* The sentinel goes at 0xDB00, NOT low: with -zorg 0x6000 the low
       region IS the program, and the first version of this probe filled
       0x6100-0x7F00 and overwrote its own code.  It reported CYAN and
       stopped, which looked like the machine refusing the layout when it
       was the test destroying itself.  0xDB00+ is where the buffers
       would actually go, so that is what is worth proving. */
    low = (volatile uint8_t *)0xDB00;
    *low = 0xA5;
    if (*low != 0xA5) { BORDER(2); ok = 0; }
    else {
        uint16_t i;
        for (i = 0xDB00; i; i++) *(uint8_t *)i = (uint8_t)(i & 0xFF);
        blot(2, 0, 0x47);
        for (i = 0xDB00; i; i++)
            if (*(uint8_t *)i != (uint8_t)(i & 0xFF)) { ok = 0; break; }
    }

    if (ok) BORDER(4);                          /* green: all three hold */
    blot(4, 0, ok ? 0x44 : 0x42);
    for (;;) ;
}
