#ifndef _GFX_H_
#define _GFX_H_

/* ================================================================== */
/* gfx.h — Low-level graphics helpers                                 */
/* ================================================================== */

#include <stdint.h>

/* Convert (x,y) pixel coords to screen-RAM byte offset */
uint16_t scr_off(uint8_t x, uint8_t y);

/* Where everything below draws.  Defaults to the displayed screen at
 * 0x4000; a 128K can point it at the shadow screen to compose a whole
 * screen off-display.  Attributes follow the pixels by PIX_SIZE, as
 * they do in the hardware layout. */
extern uint8_t *gfx_pix;
extern uint8_t *gfx_attr;
void gfx_target(uint8_t *pixels);

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

/* Direct-write blit with left-edge clipping.
 * col is signed (negative = partially off-screen left).
 * w = width in character columns (bytes per row).
 * h = height in pixel rows. data is row-major (h rows of w bytes). */
void write_blit(int8_t col, uint8_t y, const uint8_t *data,
                uint8_t w, uint8_t h);

/* Clear (zero) a rect of screen bytes with left-edge clipping. */
void clear_blit(int8_t col, uint8_t y, uint8_t w, uint8_t h);

/* Print a null-terminated string at character position (col, row)
 * using the ROM font. Does not set attributes. */
/* Chooses where the glyphs come from: the resident array, or a RAM bank
   on a 128K with the ROM font as the 48K fallback.  Call once, after
   is_128k and page_reg are valid. */
void font_init(void);

void print_at(uint8_t col, uint8_t row, const char *s);

#endif /* _GFX_H_ */
