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
 * They go at 0x6000, BELOW the program and out of the paged window
 * entirely.  That is the whole point: 0xC000-0xFFFF is a bank on a
 * 128K-class machine, and every byte we keep there is a byte page 7
 * cannot have.  With this region empty of ours, page 7 holds the
 * SHADOW SCREEN on a 128K *and* a +2A/+3 — see docs/DESIGN.md
 * § Two machines.
 *
 * 0x6000-0x7FFF is CONTENDED RAM, and that costs nothing that matters:
 * contention only bites while the ULA is drawing, and the present runs
 * in the vblank window when it is not.
 *
 * It is also below RAMTOP, so it is BASIC's on paper.  The loader's
 * CLEAR leaves the stack near 0x7FA0 growing down, and MEM_END is
 * checked against that below — the gap is the only thing between these
 * buffers and the return addresses.
 *
 * This was tried once before and blamed for a +3 crash.  That was
 * wrong: the crash was hw_detect() clearing bit 4 of 0x7FFD, the ROM
 * select, with interrupts enabled.  Fixed at source, and this region
 * came back with it.
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
/* 0xDB00, ABOVE the program, and it must stay there.

   These buffers used to sit at 0x6000, which looked like free RAM below
   the code.  It is not free: 0x6000-0x7FFF is where the compressed asset
   block is loaded (see tools/mkassets.py), and the two overlapped.  The
   sheet blobs survived by luck -- they are decompressed at boot, before
   the renderer first writes VBUF -- while the per-level gids, decompressed
   on every level load, were read back out of render garbage.
   render_paths.py passed throughout; only p0_state_walk caught it.

   On a 128K 0xDB00 is page 7, above the shadow screen at 0xC000-0xDAFF;
   on a 48K it is plain RAM.  Same addresses either way, and everything
   here is written at runtime, so nothing needs loading into it. */
#ifndef MEM_VBUF
#define MEM_VBUF        0xDB00
#endif
                                        /* 128 rows x 32 bytes  = 4096 */
#define MEM_VATTR       (MEM_VBUF  + 4096)      /* 16 x 32      =  512 */
#define MEM_VIEW_OFF    (MEM_VATTR + 512)       /* 128 x uint16 =  256 */

/* --- src/render.c: the unpacked tile sheets ---
   Reserved by size; render.c carves it up and checks it fits. */
#define MEM_TILES       (MEM_VIEW_OFF + 256)
/* Unpacked sheets: terrain map + terrain view + unit map + unit view,
   plus one mask for the view units.  The view sheet gained the explosion
   sprite, so both it and its mask are 5 tiles now, not 4. */
#define MEM_TILES_SIZE  3104    /* +640 for the second animation frame */

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

/* One byte per cell: how many player units can strike it.  Built once
   per enemy turn, then read O(1) per candidate cell (docs/PLAN.md
   § Enemy decisions). */
#define MEM_THREAT      (MEM_U_FLAGS + 40)

#define MEM_END         (MEM_THREAT + 98)

/* The buffers now live ABOVE the program, at 0xDB00, so the old check
   against the stack no longer applies: with -zorg 0x6000 the stack is at
   ~0x5FA4, below all of this, and 0x6000-0xBFFF is code.  What must hold
   instead is that the buffers clear the shadow screen (page 7 holds it at
   0xC000-0xDAFF on a 128K) and stay inside the address space. */
#if MEM_VBUF >= 0xC000 && MEM_VBUF < 0xDB00
#error "the buffers would overlap the 128K shadow screen at 0xC000-0xDAFF"
#endif
#if MEM_END > 0x10000
#error "the hand-placed buffers run off the top of RAM"
#endif
#if MEM_VBUF < 0xC000 && MEM_END > 0x7D00
#error "the low buffers are encroaching on the stack below 0x7FA0"
#endif
#if MEM_VBUF < 0x5B00
#error "the buffers must clear BASIC's system variables and program"
#endif

#endif /* _MEMMAP_H_ */
