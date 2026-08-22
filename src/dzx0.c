/*
 * dzx0.c -- ZX0 decompression
 *
 * ZX0 by Einar Saukas.  This wraps z88dk's own `dzx0_standard()` rather
 * than vendoring a copy of the Z80 routine, because the ZX0 *stream
 * format changed between v1 and v2* and the decompressor must match the
 * compressor exactly:
 *
 *   - z88dk ships ZX0 v1.5 (`$Z88DK/bin/z88dk-zx0`) and the matching
 *     v1 `dzx0_standard` in its library.
 *   - The widely copied 68-byte "standard" routine found online is the
 *     v2 decoder.  Feeding it v1 data does not fail cleanly: it runs
 *     away, overwrites RAM and crashes.
 *
 * If you point ZX0= at a v2 compressor (e.g. a github checkout), you
 * must supply a v2 decoder here too.
 */

#include <compress/zx0.h>

#include "../include/dzx0.h"

void dzx0_decompress(const uint8_t *src, uint8_t *dst)
{
    dzx0_standard((void *)src, (void *)dst);
}
