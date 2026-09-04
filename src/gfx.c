/*
 * gfx.c — Low-level ZX Spectrum graphics helpers
 */

#include "../config/app_config.h"
#include "../include/gfx.h"
#include "../include/memmap.h"

/* Where drawing lands.  Normally the displayed screen at 0x4000; on a
   128K it can be pointed at the shadow screen so a whole screen can be
   composed off-display and then shown with a page flip.  Set once per
   repaint by gfx_target(), never per byte, so the indirection costs an
   address load and nothing in the inner loops. */
uint8_t *gfx_pix  = (uint8_t *)0x4000;
uint8_t *gfx_attr = (uint8_t *)0x5800;

void gfx_target(uint8_t *pixels)
{
    gfx_pix  = pixels;
    gfx_attr = pixels + PIX_SIZE;
}

uint16_t scr_off(uint8_t x, uint8_t y)
{
    return ((uint16_t)(y & 0xC0) << 5) |
           ((uint16_t)(y & 0x07) << 8) |
           ((uint16_t)(y & 0x38) << 2) |
           (x >> 3);
}

void set_attr_rect(uint8_t col, uint8_t row, uint8_t w, uint8_t h,
                   uint8_t attr)
{
    uint8_t r, c;
    uint8_t *base = gfx_attr;

    for (r = 0; r < h; r++) {
        if (row + r >= 24) break;
        for (c = 0; c < w; c++) {
            if (col + c >= 32) break;
            base[(row + r) * 32 + col + c] = attr;
        }
    }
}

void blit_attr_rect(uint8_t col, uint8_t row, uint8_t w, uint8_t h,
                    const uint8_t *src, uint8_t or_mask)
{
    uint8_t r, c;
    uint8_t *base = gfx_attr;

    for (r = 0; r < h; r++) {
        if (row + r >= 24) { src += w; continue; }
        for (c = 0; c < w; c++) {
            if (col + c < 32)
                base[(row + r) * 32 + col + c] = (uint8_t)(*src | or_mask);
            src++;
        }
    }
}

void screen_clear(uint8_t attr)
{
    uint16_t i;
    for (i = 0; i < PIX_SIZE; i++) gfx_pix[i] = 0;
    for (i = 0; i < ATTR_SZ; i++) gfx_attr[i] = attr;
}

void border(uint8_t colour) __z88dk_fastcall __naked
{
    (void)colour;
    __asm
        ld  a, l
        and #0x07
        out (#0xFE), a
        ret
    __endasm;
}

/* --- XOR 16x16 sprite + 2x2 attr rect --- */

/* --- XOR 8x8 sprite + 1x1 attr cell --- */

/* --- Direct-write blit with left-edge clipping --- */
void write_blit(int8_t col, uint8_t y, const uint8_t *data,
                uint8_t w, uint8_t h)
{
    uint8_t row, c, skip, draw_w, start_col;

    if (col < 0) {
        skip = (uint8_t)(-col);
        if (skip >= w) return;
        draw_w = w - skip;
        start_col = 0;
    } else {
        skip = 0;
        start_col = (uint8_t)col;
        if (start_col >= 32) return;
        draw_w = w;
        if (start_col + draw_w > 32)
            draw_w = 32 - start_col;
    }

    for (row = 0; row < h; row++) {
        uint8_t py = y + row;
        uint16_t off;
        const uint8_t *src;
        if (py >= 192) continue;
        off = scr_off(start_col << 3, py);
        src = data + (uint16_t)row * w + skip;
        for (c = 0; c < draw_w; c++)
            gfx_pix[off + c] = src[c];
    }
}

/* --- Clear (zero) a rect of screen bytes with left-edge clipping --- */
void clear_blit(int8_t col, uint8_t y, uint8_t w, uint8_t h)
{
    uint8_t row, c, draw_w, start_col;

    if (col < 0) {
        uint8_t skip = (uint8_t)(-col);
        if (skip >= w) return;
        draw_w = w - skip;
        start_col = 0;
    } else {
        start_col = (uint8_t)col;
        if (start_col >= 32) return;
        draw_w = w;
        if (start_col + draw_w > 32)
            draw_w = 32 - start_col;
    }

    for (row = 0; row < h; row++) {
        uint8_t py = y + row;
        uint16_t off;
        if (py >= 192) continue;
        off = scr_off(start_col << 3, py);
        for (c = 0; c < draw_w; c++)
            gfx_pix[off + c] = 0;
    }
}

/* ROM character set: 96 chars (space..copyright), 8 bytes each */
/* Where the glyphs are, and what has to happen around reading them.
 *
 * The ROM CANNOT be overwritten -- 0x0000-0x3FFF is read-only on every
 * Spectrum -- so a replacement font is one held in RAM with the printer
 * pointed at it.  A .ch8 has the ROM's layout, 8 rows per glyph from
 * character 32, so the indexing below is unchanged.
 *
 * font_prepare()/font_release() are hooks rather than #if, because this
 * file has an __asm block in it and zcc drops #if directives that follow
 * one.  In the resident build they cost a `ret` each; in the banked build
 * they page the font in and copy the string somewhere that is not paged.
 * src/font_rt.c has the reasoning. */
extern const uint8_t *font_base;
const char *font_prepare(const char *s);
void font_release(void);

#define ROM_FONT font_base

void print_at(uint8_t col, uint8_t row, const char *s)
{
    uint8_t px, py, i;
    uint16_t off;
    const uint8_t *glyph;

    px = col << 3;
    py = row << 3;
    s = font_prepare(s);

    while (*s) {
        if (col >= 32) break;
        glyph = ROM_FONT + (((uint8_t)*s - 32) << 3);
        off = scr_off(px, py);
        for (i = 0; i < 8; i++) {
            gfx_pix[off] = glyph[i];
            off += 256;  /* next pixel row within char cell */
        }
        s++;
        col++;
        px += 8;
    }
    font_release();
}
