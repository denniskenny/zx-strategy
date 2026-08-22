/*
 * vsync.c — Floating bus vsync with model auto-detection
 *
 * The floating bus trick exploits the fact that reading an unattached
 * ULA port returns whatever byte the ULA last fetched from screen RAM.
 * A unique attribute marker is placed near the bottom of the display
 * (row 22, cols 0-2); a precisely timed loop reads only attribute
 * fetches, so a match means the beam has just reached row 22 — leaving
 * the bottom border + vblank + top border (~28 000 T-states) free for
 * tear-free screen updates.
 *
 * Three modes, detected once at startup by vsync_detect():
 *
 *   Mode 1 — 48K/128K/+2  (port 0x40FF, 35 T-state loop)
 *   Mode 2 — +2A/+3       (port 0x0FFD, 42 T-state loop)
 *   Mode 0 — HALT fallback (ei / halt / di, ~14 000 T-states free)
 *
 * Requirements:
 *   - This code must live in NON-CONTENDED RAM (>= 0x8000).  The
 *     Makefile builds with -zorg=32768.
 *   - VSYNC_MARKER must be unique on screen, not 0xFF, bit 0 set.
 *   - The +2A/+3 path needs paging enabled (bit 5 of port 0x7FFD = 0)
 *     at detection time, so call vsync_detect() before locking paging.
 *
 * Reference: Ast A. Moore, "The Definitive Programmer's Guide to
 * Using the Floating Bus Trick on the ZX Spectrum".
 */

#include "../config/app_config.h"
#include "../include/vsync.h"
#include "../include/hw.h"

uint8_t vsync_mode = 0;

void vsync_detect(void) __naked
{
    __asm

    ;; ---- Write the sync marker across attr row 22 ----
    ld  hl, VSYNC_MARKER_ADDR
    ld  b, VSYNC_MARKER_CELLS
    ld  a, VSYNC_MARKER
_vsd_mark:
    ld  (hl), a
    inc l
    djnz _vsd_mark

    ;; ---- Test 48K floating bus (port 0xFF) ----
    ;; Read up to 10 000 times (~2 frames).  Any non-0xFF value means
    ;; the floating bus is active on this port.  No timing needed here.
    ld  bc, 10000
_vsd_48k_loop:
    in  a, (0xFF)
    cp  0xFF
    jr  nz, _vsd_48k_ok
    dec bc
    ld  a, b
    or  c
    jr  nz, _vsd_48k_loop

    ;; Port 0xFF failed.  If 128K, try the +2A/+3 port.
    ld  a, (_is_128k)
    or  a
    jr  z, _vsd_halt

    ;; ---- Test +2A/+3 floating bus (port 0x0FFD) ----
    ;; Returns values ORed with 1.  Always 0xFF if paging is locked.
    ld  bc, 10000
_vsd_128k_loop:
    ld  a, 0x0F
    in  a, (0xFD)          ; port 0x0FFD
    cp  0xFF
    jr  nz, _vsd_128k_ok
    dec bc
    ld  a, b
    or  c
    jr  nz, _vsd_128k_loop

    ;; Both ports failed — emulator without floating bus support,
    ;; or a paging-locked +2A/+3.
_vsd_halt:
    xor a
    ld  (_vsync_mode), a
    ret

_vsd_48k_ok:
    ld  a, 1
    ld  (_vsync_mode), a
    ret

_vsd_128k_ok:
    ld  a, 2
    ld  (_vsync_mode), a
    ret

    __endasm;
}

void vsync_wait(void) __naked
{
    __asm

    ;; ---- Branch on detected mode ----
    ld  a, (_vsync_mode)
    or  a
    jr  z, _vs_halt

    ;; ---- Refresh the marker every frame (~1 000 T with contention) --
    ;; Guarantees the marker survives any full-screen clear: at worst
    ;; one frame falls through to the next marker match.
    ld  d, VSYNC_MARKER     ; D = marker for the timed loops below
    ld  hl, VSYNC_MARKER_ADDR
    ld  b, VSYNC_MARKER_CELLS
_vs_mark:
    ld  (hl), d
    inc l
    djnz _vs_mark

    ;; ---- Branch on mode 1 vs 2 ----
    ld  a, (_vsync_mode)
    dec a
    jr  z, _vs_48k

    ;; ============================================================
    ;; Mode 2: +2A/+3 floating bus  (port 0x0FFD, 42 T per iter)
    ;; ============================================================
    ;; On +2A/+3 the idle bus returns the last CONTENDED memory read,
    ;; so `ld a,(col3)` preloads a known non-marker value.  The ULA
    ;; value comes back ORed with 1 — the marker already has bit 0 set.
_vs_128k:
    ld  e, 0x0F            ; E = port MSB (D = marker, set above)
_vs_128k_loop:
    ld  a, (VSYNC_PRELOAD_ADDR)     ; [13] contended read (preload)
    ld  a, e               ; [4]  A = 0x0F
    in  a, (0xFD)          ; [11] read port 0x0FFD
    cp  d                  ; [4]  marker?
    jp  nz, _vs_128k_loop  ; [10] 42 T total
    ret

    ;; ============================================================
    ;; Mode 1: 48K/128K/+2 floating bus  (port 0x40FF, 35 T per iter)
    ;; ============================================================
    ;; `dec hl` pads the loop to exactly 35 T-states so every IN lands
    ;; on an attribute fetch, never a bitmap byte or an idle interval.
_vs_48k:
    ld  e, 0x40            ; E = port MSB (D = marker, set above)
_vs_48k_loop:
    dec hl                 ; [6]  padding
    ld  a, e               ; [4]  A = 0x40
    in  a, (0xFF)          ; [11] read port 0x40FF
    cp  d                  ; [4]  marker?
    jp  nz, _vs_48k_loop   ; [10] 35 T total
    ret

    ;; ============================================================
    ;; Mode 0: HALT fallback
    ;; ============================================================
    ;; startup=31 boots with interrupts disabled.  Briefly enable
    ;; them for the HALT, then disable again.
_vs_halt:
    ei
    halt
    di
    ret

    __endasm;
}
