#ifndef _GFX_H_
#define _GFX_H_

/* ================================================================== */
/* gfx.h — Low-level graphics helpers                                 */
/* ================================================================== */

#include <stdint.h>

/* Convert (x,y) pixel coords to screen-RAM byte offset */
uint16_t scr_off(uint8_t x, uint8_t y);

/* Toggle a single pixel (XOR) */
void plot(uint8_t *buf, uint8_t x, uint8_t y);

/* Set a rectangle of attribute cells */
void set_attr_rect(uint8_t col, uint8_t row, uint8_t w, uint8_t h,
                   uint8_t attr);

/* Copy a w x h block of attribute cells from row-major `src`, ORing
 * each with `or_mask`.  With or_mask 0 that is a straight copy of an
 * authored colour block; with src holding only BRIGHT flags it paints
 * a sprite in `or_mask`'s ink while keeping the artist's shading. */
void blit_attr_rect(uint8_t col, uint8_t row, uint8_t w, uint8_t h,
                    const uint8_t *src, uint8_t or_mask);

/* Zero all pixel RAM and fill all attributes with attr */
void screen_clear(uint8_t attr);

/* Set the border colour (0-7) */
void border(uint8_t colour) __z88dk_fastcall __naked;

/* Fast XOR 16x16 sprite + set 2x2 attr rect.
 * Set these globals, then call xor_sprite_16(). */
extern uint8_t xor16_x, xor16_y, xor16_attr;
extern const uint8_t *xor16_spr;
void xor_sprite_16(void);

/* Fast XOR 8x8 sprite + set 1x1 attr cell.
 * Set these globals, then call xor_sprite_8(). */
extern uint8_t xor8_x, xor8_y, xor8_attr;
extern const uint8_t *xor8_spr;
void xor_sprite_8(void);

/* Direct-write blit with left-edge clipping.
 * col is signed (negative = partially off-screen left).
 * w = width in character columns (bytes per row).
 * h = height in pixel rows. data is row-major (h rows of w bytes). */
void write_blit(int8_t col, uint8_t y, const uint8_t *data,
                uint8_t w, uint8_t h);

/* Clear (zero) a rect of screen bytes with left-edge clipping. */
void clear_blit(int8_t col, uint8_t y, uint8_t w, uint8_t h);

/* Direct-write blit at pixel X (handles sub-byte shifting). */
void write_blit_px(int16_t px, uint8_t y, const uint8_t *data,
                   uint8_t w, uint8_t h);

/* Print a null-terminated string at character position (col, row)
 * using the ROM font. Does not set attributes. */
void print_at(uint8_t col, uint8_t row, const char *s);

#endif /* _GFX_H_ */
