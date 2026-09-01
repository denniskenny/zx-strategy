/*
 * music.c -- the tunes, unpacked on the way in.
 *
 * Song data is the most compressible thing in this program: patterns
 * repeat and half of every row is a $01 sustain byte.  The compressed
 * blobs live below 0xC000 with the code; the unpacked data lives above
 * MEM_END, which is uncontended, is where a 128K's paging means code can
 * never go, and had 1.7 KB spare.  Worth ~430 bytes of the only region
 * that can hold code.
 *
 * ASSEMBLED AT ITS DESTINATION.  A tune's order table is a list of
 * ABSOLUTE pointers -- `DEFW PAT0, DEFW PAT1, ...` -- so compressing the
 * module's bytes and unpacking them elsewhere leaves every pointer wrong
 * by the relocation distance.  tools/gen_tritone_module.py --org
 * assembles the data at MEM_MUSIC so the pointers are correct on arrival,
 * and checks in the build that the image survives a ZX0 round trip and
 * that every order-table entry lands inside the block.
 *
 * ONE BUFFER, because only one tune plays at a time: both are blocking
 * calls from a static screen.  A second buffer would cost bytes to state
 * what the control flow already guarantees.
 *
 * The engine is shared and links once (assets/music/tritone_engine.asm);
 * only the data is per-tune.  A third tune costs its blob and nothing
 * else, provided it fits MEM_MUSIC_SIZE.
 */

#include <stdint.h>

#include "../include/dzx0.h"
#include "../include/memmap.h"
#include "../include/music.h"

/* Emitted by tools/gen_tritone_module.py into each tune's linkable
   module.  DEFB in a code_user section, not a C array: a C array named
   *_zx0 is swept into the contended asset block by mkassets.py, which is
   both unnecessary and far too small to hold them. */
extern const uint8_t grenadiers_zx0[];
extern const uint8_t lowlands_zx0[];

/* Not a #define: zcc drops preprocessor directives that sit above a
   function containing an __asm block -- see .claude/skills/zx-memory.  A
   const pointer is immune and costs nothing. */
static uint8_t *const music_buf = (uint8_t *)MEM_MUSIC;

/* HL = the order table, by fastcall.  Resets the shared entropy counter
   first, which is what every tune's module used to carry a copy of. */
static void tri_play(const uint8_t *song) __z88dk_fastcall __naked
{
    (void)song;
    __asm
        EXTERN TRI_PLAY
        EXTERN _tritone_ticks
        push ix                 ; preserve sdcc's frame pointer
        ld   de, #0
        ld   (_tritone_ticks), de
        call TRI_PLAY           ; HL already holds the song
        pop  ix
        ret
    __endasm;
}

void grenadiers_play(void)
{
    dzx0_decompress(grenadiers_zx0, music_buf);
    tri_play(music_buf);
}

void lowlands_play(void)
{
    dzx0_decompress(lowlands_zx0, music_buf);
    tri_play(music_buf);
}
