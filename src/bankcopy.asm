; bankcopy.asm -- move a freshly loaded block into a RAM bank.
;
; The loader problem, solved the only way that works here: BASIC does the
; LOAD, and THIS does the paging.  BASIC's own OUT cannot be trusted to
; page -- three attempts failed, one of them while booting and rendering
; perfectly with the data in the wrong bank (.claude/skills/zx-loader).
;
; So the tape does:
;
;   LOAD ""CODE            the blob, into the SCREEN at 0x4000
;   RANDOMIZE USR 0x5B00   this, which pages and copies
;
; The screen is the staging buffer because it is the only free 6912 bytes
; on the machine, and the cutscene is about to overwrite it anyway.  The
; load is visible as noise, which is honest: something is loading.
;
; Assembled STANDALONE at a FIXED address so the BASIC loader can name it
; without the build having to read the link map -- which does not exist
; until after the link, by which point the tap is being written.
;
; It lives at 0x5F00, just ABOVE the loader's CLEAR 24319.  That is the
; only reliably safe place: everything below RAMTOP belongs to the ROM or
; to BASIC, and two earlier homes proved it --
;
;   0x5AFA  the tail of the ATTRIBUTE FILE (0x5800-0x5AFF).  The ROM
;           writes to the screen while loading and wiped the parameters.
;
;   0x5B00  the printer buffer -- on a 48K.  On a 128K that range holds
;           the machine's own system variables, BANKM at 0x5B5C among
;           them, so the stub overwrote the ROM's state and the machine
;           crashed.  "Traditional home for a loader stub" is 48K advice.
;
; Above RAMTOP nothing else has a claim, which is the whole point of
; CLEAR.
;
; Parameters are POKEd in by the loader rather than passed, because BASIC
; can POKE and cannot pass arguments to USR.
;
;   0x5F00  pointer to the next table entry (word)
;   0x5F05  entry -- call once per block; it advances itself
;   0x5F80  the table: 5 bytes per block, bank / dest / length
;
; SELF-ADVANCING, so BASIC can drive it from a FOR loop:
;
;     FOR I=1 TO N: LOAD ""CODE: RANDOMIZE USR 24325: NEXT I
;
; The alternative was a LOAD, five POKEs and a USR per block, and at ten
; cutscene screens that BASIC program reached 1472 bytes -- past RAMTOP at
; 0x5EFF, over this stub at 0x5F00 and into the assets at 0x6000.  The tap
; loaded every block and then never reached the game.
;
; A loader whose SIZE GROWS WITH THE CONTENT is the bug; one line does not
; care how many blocks there are.  mktap.py appends the table to this
; stub's own block, so the two always agree.

    org     0x5F00

_bc_ptr:    defw    0x5F80      ; next table entry
            defs    3           ; keeps the entry point at 0x5F05

    ; --- entry at 0x5F05 -------------------------------------------
    di
    ld      hl, (_bc_ptr)
    ld      a, (hl)             ; bank
    ld      (_bc_bank), a
    inc     hl
    ld      e, (hl)
    inc     hl
    ld      d, (hl)             ; DE = offset within the bank
    inc     hl
    ld      c, (hl)
    inc     hl
    ld      b, (hl)             ; BC = length
    inc     hl
    ld      (_bc_ptr), hl       ; ...ready for the next block

    push    bc
    push    de

    ; PRESERVE BANKM, do not force it.  Bit 5 is the paging lock -- if the
    ; ROM set it the OUT is ignored and the copy lands in bank 0 looking
    ; successful -- and bit 4 is the ROM select, which swaps a ROM under a
    ; running interpreter.  Change only the bank bits.
    ld      a, (0x5B5C)
    ld      (_bc_save), a
    and     0xF8
    ld      hl, _bc_bank
    or      (hl)
    ld      bc, 0x7FFD
    out     (c), a
    ld      (0x5B5C), a

    pop     de
    pop     bc

    ld      a, d
    add     a, 0xC0             ; DE = 0xC000 + offset
    ld      d, a
    ld      hl, 0x4000          ; from the screen, where BASIC staged it
    ldir

    ld      a, (_bc_save)       ; exactly as we found it
    ld      bc, 0x7FFD
    out     (c), a
    ld      (0x5B5C), a
    ei
    ret

_bc_bank:   defb    0
_bc_save:   defb    0
