/*
 * font_rt.c -- where print_at() gets its glyphs.
 *
 * FONT_BANK=1 DOES NOT WORK ON A 128K.  Read this before spending time
 * on it again.
 *
 * The shadow screen IS AT 0xC000, in page 7.  So is the only window a
 * RAM bank can be paged into.  render_compose() draws to whichever
 * screen is not being shown, so half the time print_at()'s destination
 * is 0xC000-0xDAFF -- and paging the font bank in replaces that screen
 * with the font.  The glyph writes then land in the font bank and the
 * reads collide with the screen.
 *
 * Measured, not reasoned about.  A 48K, which has one screen at 0x4000
 * and no paging, renders perfectly:
 *
 *     48k    ' UNIT   :Red Base  025/025 R0 M0'
 *     128k   ' ?NI?   :Red B?se   2?/ 2? R  ? '
 *
 * The corruption is only on the machine that double-buffers, which is
 * the signature of exactly this collision.
 *
 * There is a way out, but it is not worth having: page in, copy the ONE
 * glyph's eight bytes to low RAM, page back, then draw -- two page
 * switches per character, behind di/ei, on a routine that draws a status
 * line on every cursor step.  The 768 bytes are not worth that.
 *
 * Left in place, defaulting to 0, because the finding is worth more than
 * the code: any future attempt to put per-frame-readable data in a bank
 * runs into the same wall.
 *
 * THREE SOURCES, chosen by FONT= in the Makefile:
 *
 *   FONT=rom        the ROM's own font at 0x3D00.  THE DEFAULT, and
 *                   costs nothing: no array, no bank, no copy.
 *
 *   FONT=resident   a .ch8 in a 768-byte array in 0x8000-0xBFFF.  Works
 *                   perfectly and costs 768 bytes of the only region
 *                   that can hold code -- which is why it is not the
 *                   default.  `make FONT=resident` to look at it.
 *
 *   FONT=bank       the same font in a RAM bank.  BROKEN on a 128K; see
 *                   above.  Kept for the finding, not the feature.
 *
 * WHY THE STRING IS COPIED FIRST
 *
 * The strings live in MEM_TEXTPOOL, which is in page 7 at 0xFB46.
 * Paging the font bank in at 0xC000 swaps page 7 out, so the string
 * print_at() is walking DISAPPEARS mid-draw.  font_prepare() copies it
 * somewhere that is not paged before the bank comes in.
 *
 * That is the third time this project has been caught by "anything above
 * 0xC000 depends on what is currently paged": the cutscene could not
 * print while its bank was in, the strings could not be unpacked before
 * the map was settled, and now this.
 *
 * NO #if IN gfx.c.  print_at() calls these hooks unconditionally, because
 * gfx.c contains an __asm block and zcc silently drops #if directives
 * that appear after one -- see .claude/skills/zx-memory.  All the
 * conditional logic is here, in a file with no assembly in it.
 */

#include <stdint.h>

#include "../config/app_config.h"
#include "../include/gfx.h"
#include "../include/hw.h"

#define ROM_FONT_ADDR   0x3D00      /* the ROM's own, on every machine */

const uint8_t *font_base;

#if FONT_RESIDENT || FONT_BANK
#else

/* THE DEFAULT: the ROM's font, and nothing else at all.
 *
 * A custom font is 768 bytes of 0x8000-0xBFFF, and with 731 bytes clear
 * that was most of what remained.  The ROM's font is already in the
 * machine, costs nothing, and every Spectrum owner has read it for forty
 * years.  Switch with `make FONT=resident` when there is room.
 *
 * The hooks still exist so print_at() needs no conditional -- it has an
 * __asm block, and zcc drops #if directives that follow one. */
void font_init(void)
{
    font_base = (const uint8_t *)ROM_FONT_ADDR;
}

const char *font_prepare(const char *s)
{
    return s;
}

void font_release(void)
{
}

#endif

#if FONT_BANK

extern uint8_t page_reg;            /* render.c owns the shadow of 0x7FFD */

/* The bank the font block was loaded into; see FONT_BANK_NO in the
   Makefile, which must match what mktap was told. */
static volatile uint8_t font_bank = FONT_BANK_NO;

/* Long enough for the widest line the 32-column screen can hold, plus a
   terminator.  In bss, which is below 0xC000 and therefore never paged. */
static char safe[33];

/* Only a 128K has banks.  On a 48K there is nothing to page and the ROM
   font is what gets used, so both hooks must do nothing at all. */
static uint8_t banked;

void font_init(void)
{
    banked = is_128k;
    font_base = (const uint8_t *)(banked ? 0xC000 : ROM_FONT_ADDR);
}

const char *font_prepare(const char *s)
{
    uint8_t n = 0;

    if (!banked) return s;

    while (s[n] && n < 32) {        /* copy BEFORE the bank comes in */
        safe[n] = s[n];
        n++;
    }
    safe[n] = 0;

    __asm
        di
        ld  a, (_page_reg)
        and #0xF8
        ld  hl, #_font_bank
        or  (hl)                    ; only the bank bits change
        ld  bc, #0x7FFD
        out (c), a
        ld  (0x5B5C), a             ; BANKM: the ROM keeps a copy
    __endasm;
    return safe;
}

void font_release(void)
{
    if (!banked) return;

    __asm
        ld  a, (_page_reg)          ; back to whatever render.c had
        ld  bc, #0x7FFD
        out (c), a
        ld  (0x5B5C), a
        ei
    __endasm;
}

#elif FONT_RESIDENT

extern const uint8_t game_font[];

void font_init(void)
{
    font_base = game_font;
}

/* Nothing is paged, so nothing needs protecting: the string is read
   where it lies and these cost one `ret` each. */
const char *font_prepare(const char *s)
{
    return s;
}

void font_release(void)
{
}

#endif
