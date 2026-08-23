#ifndef _MEMMAP_H_
#define _MEMMAP_H_

/* ================================================================== */
/* memmap.h — what lives in the RAM below the program                 */
/* ================================================================== */
/* The linker puts everything above 0x8000, and the 128K render path
 * needs the whole program to stay under 0xC000 so page 7 can be banked
 * in for the shadow screen (docs/DESIGN.md § Two machines).  That is
 * 16 KB for code, rodata, data and bss together, and it is not enough.
 *
 * They go ABOVE the program, from 0xC000.  Not below it: 0x6000-0x7FFF
 * looks free, and is on a 48K, but with the loader's CLEAR 32767 that
 * region holds BASIC's program, its variables and the machine stack.
 * Putting 7 KB of buffers there survived on a 48K and a 128K by luck and
 * crashed a +3 straight back into BASIC with "Nonsense in BASIC" — the
 * +3 reserves more of it.  0xC000-0xFFFF is above RAMTOP and belongs to
 * nobody once the program is loaded.
 *
 * The cost of that choice: page 7 cannot be banked in at 0xC000 while
 * these live there, so the 128K shadow screen is off the table for as
 * long as this layout stands.  Correct on every machine beats faster on
 * one.
 *
 * Placing by hand means overlaps are possible, so every block is sized
 * from the thing that lives in it and the total is checked below.  Add a
 * block by appending to the chain, never by picking an address.
 *
 * tools/checkmem.py checks the other end of the same problem — that the
 * linker-placed part still clears 0xC000.
 */

/* Deliberately includes nothing.  The generated asset headers define
 * their blobs as `static const`, so pulling one in here would emit a
 * copy in every translation unit that includes this file — which is all
 * of them, via board.h.  The tile area is therefore reserved by SIZE,
 * and src/render.c checks the real sheets fit it.
 */

/* --- src/render.c: the play-view buffer --- */
#define MEM_VBUF        0xC000          /* 128 rows x 32 bytes  = 4096 */
#define MEM_VATTR       (MEM_VBUF  + 4096)      /* 16 x 32      =  512 */
#define MEM_VIEW_OFF    (MEM_VATTR + 512)       /* 128 x uint16 =  256 */

/* --- src/render.c: the unpacked tile sheets ---
   Reserved by size; render.c carves it up and checks it fits. */
#define MEM_TILES       (MEM_VIEW_OFF + 256)
#define MEM_TILES_SIZE  1664

/* --- src/logic.c: scratch with no deadline on it --- */
#define MEM_LOGIC       (MEM_TILES + MEM_TILES_SIZE)
#define MEM_Q           (MEM_LOGIC)             /* Dial's bucket queue */
#define MEM_PLACE_CAND  (MEM_Q + 128)           /* placement candidates */
#define MEM_CELL_COST   (MEM_PLACE_CAND + 32)   /* cost to enter a cell */

/* --- the board and the armies ---
   Read by the renderer while composing, which happens in the vblank
   window, so the contention here costs nothing.  Down here because the
   linker-placed part has to clear 0xC000 and no longer can. */
#define MEM_CELLS       (MEM_CELL_COST + 98)
#define MEM_TERRAIN     (MEM_CELLS)
#define MEM_OCCUPANCY   (MEM_TERRAIN   + 98)
#define MEM_COST        (MEM_OCCUPANCY + 98)

#define MEM_UNITS       (MEM_COST + 98)
#define MEM_U_TYPE      (MEM_UNITS)
#define MEM_U_CELL      (MEM_U_TYPE  + 40)
#define MEM_U_HP        (MEM_U_CELL  + 40)
#define MEM_U_FLAGS     (MEM_U_HP    + 40)

#define MEM_END         (MEM_U_FLAGS + 40)

#if MEM_END > 0x10000
#error "the hand-placed buffers have outgrown the RAM above 0xC000"
#endif
#if MEM_VBUF < 0xC000
#error "the buffers must stay above BASIC's RAMTOP; see the note above"
#endif

#endif /* _MEMMAP_H_ */
