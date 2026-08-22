/*
 * gfx.c — Low-level ZX Spectrum graphics helpers
 */

#include "../config/app_config.h"
#include "../include/gfx.h"

uint16_t scr_off(uint8_t x, uint8_t y)
{
    return ((uint16_t)(y & 0xC0) << 5) |
           ((uint16_t)(y & 0x07) << 8) |
           ((uint16_t)(y & 0x38) << 2) |
           (x >> 3);
}

void plot(uint8_t *buf, uint8_t x, uint8_t y)
{
    if (y >= 192) return;
    buf[scr_off(x, y)] ^= (0x80 >> (x & 7));
}

void set_attr_rect(uint8_t col, uint8_t row, uint8_t w, uint8_t h,
                   uint8_t attr)
{
    uint8_t r, c;
    uint8_t *base = ATTR;

    for (r = 0; r < h; r++) {
        if (row + r >= 24) break;
        for (c = 0; c < w; c++) {
            if (col + c >= 32) break;
            base[(row + r) * 32 + col + c] = attr;
        }
    }
}

void screen_clear(uint8_t attr)
{
    uint16_t i;
    for (i = 0; i < PIX_SIZE; i++) SCREEN[i] = 0;
    for (i = 0; i < ATTR_SZ; i++) ATTR[i] = attr;
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
uint8_t xor16_x, xor16_y, xor16_attr;
const uint8_t *xor16_spr;

void xor_sprite_16(void)
{
    uint8_t row, py;
    uint16_t off;

    for (row = 0; row < 16; row++) {
        py = xor16_y + row;
        if (py >= 192) continue;
        off = scr_off(xor16_x, py);
        SCREEN[off]     ^= xor16_spr[row * 2];
        SCREEN[off + 1] ^= xor16_spr[row * 2 + 1];
    }
    set_attr_rect(xor16_x >> 3, xor16_y >> 3, 2, 2, xor16_attr);
}

/* --- XOR 8x8 sprite + 1x1 attr cell --- */
uint8_t xor8_x, xor8_y, xor8_attr;
const uint8_t *xor8_spr;

void xor_sprite_8(void)
{
    uint8_t row, py;
    uint16_t off;

    for (row = 0; row < 8; row++) {
        py = xor8_y + row;
        if (py >= 192) continue;
        off = scr_off(xor8_x, py);
        SCREEN[off] ^= xor8_spr[row];
    }
    set_attr_rect(xor8_x >> 3, xor8_y >> 3, 1, 1, xor8_attr);
}

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
            SCREEN[off + c] = src[c];
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
            SCREEN[off + c] = 0;
    }
}

/* --- Direct-write blit at pixel X with sub-byte shifting --- */
void write_blit_px(int16_t px, uint8_t y, const uint8_t *data,
                   uint8_t w, uint8_t h)
{
    uint8_t shift, row, c;
    int8_t base_col;

    shift = (uint8_t)(px & 7);
    base_col = (int8_t)(px >> 3);

    if (shift == 0) {
        write_blit(base_col, y, data, w, h);
        return;
    }

    for (row = 0; row < h; row++) {
        uint8_t py = y + row;
        uint8_t carry = 0;
        const uint8_t *src;
        if (py >= 192) continue;
        src = data + (uint16_t)row * w;
        for (c = 0; c <= w; c++) {
            int8_t sc = base_col + (int8_t)c;
            uint8_t src_byte = (c < w) ? src[c] : 0;
            uint8_t val = (src_byte >> shift) | carry;
            carry = (uint8_t)(src_byte << (8 - shift));
            if (sc < 0) continue;
            if ((uint8_t)sc >= 32) break;
            SCREEN[scr_off((uint8_t)sc << 3, py)] = val;
        }
    }
}

/* ROM character set: 96 chars (space..copyright), 8 bytes each */
#define ROM_FONT ((const uint8_t *)0x3D00)

void print_at(uint8_t col, uint8_t row, const char *s)
{
    uint8_t px, py, i;
    uint16_t off;
    const uint8_t *glyph;

    px = col << 3;
    py = row << 3;

    while (*s) {
        if (col >= 32) break;
        glyph = ROM_FONT + (((uint8_t)*s - 32) << 3);
        off = scr_off(px, py);
        for (i = 0; i < 8; i++) {
            SCREEN[off] = glyph[i];
            off += 256;  /* next pixel row within char cell */
        }
        s++;
        col++;
        px += 8;
    }
}
