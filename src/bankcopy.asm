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
;   0x5F00  bank number (byte)
;   0x5F01  length      (word)
;   0x5F03  destination offset within the bank (word, usually 0)
;   0x5F05  entry

    org     0x5F00

_bc_bank:   defb    1           ; 0x5F00
_bc_len:    defw    0           ; 0x5F01
_bc_dest:   defw    0           ; 0x5F03

    ; --- entry at 0x5F05 -------------------------------------------
    ;
    ; PRESERVE BANKM, do not force it.  The first version wrote 0x10 on
    ; the way out, which assumes the machine arrived here with bank 0 and
    ; the 48K ROM and nothing else set.  Two things go wrong if it did
    ; not:
    ;
    ;   * bit 5 is the PAGING DISABLE lock.  If the ROM had set it, the
    ;     OUT below is IGNORED -- the copy lands in bank 0 and reports
    ;     success -- and clearing it afterwards leaves the ROM in a state
    ;     it never chose.
    ;   * bit 4 is the ROM select.  Forcing it swaps a ROM under a
    ;     running interpreter, which is what broke the earlier attempts.
    ;
    ; Reading BANKM and putting it back makes the stub safe wherever it
    ; is called from, which is the only way to be sure when the caller is
    ; a ROM you did not write.
    di
    ld      a, (0x5B5C)
    ld      (_bc_save), a       ; whatever the ROM had

    ld      hl, _bc_bank
    ld      b, (hl)
    and     0xF8                ; keep ROM select, screen and the lock
    or      b                   ; only the bank bits change
    ld      bc, 0x7FFD
    out     (c), a
    ld      (0x5B5C), a

    ld      hl, 0x4000          ; from the screen
    ld      de, (_bc_dest)
    ld      a, d
    add     a, 0xC0             ; into the bank window
    ld      d, a
    ld      bc, (_bc_len)
    ldir

    ld      a, (_bc_save)       ; exactly as we found it
    ld      bc, 0x7FFD
    out     (c), a
    ld      (0x5B5C), a
    ei
    ret

_bc_save:   defb    0
