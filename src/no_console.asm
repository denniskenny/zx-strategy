; no_console.asm -- refuse the console driver z88dk links by default.
;
; THE PRIZE: 878 bytes in 92 symbols, for a console this game never uses.
; It draws with its own print_at(), straight to the display file.
;
; WHY IT WAS THERE.  CRT_ENABLE_STDIO=0 does not remove it: the zx crt0
; says
;
;       ; We use the generic driver by default
;       defc    TAR__fputc_cons_generic = 1
;
; and crt/classic/crt_runtime_selection.inc then does
;
;       IF !DEFINED_fputc_cons
;           IF !TAR__fputc_cons_generic
;                EXTERN fputc_cons_native
;           ELSE
;                EXTERN fputc_cons_generic      ; <- 878 bytes
;           ENDIF
;       ENDIF
;
; SATISFY THE EXTERN, do not try to suppress it.  Defining fputc_cons
; ourselves and pragma-defining DEFINED_fputc_cons looks tidier and does
; not work: the crt then reaches line 539 with
;
;       defc _fputc_cons = fputc_cons
;
; and no EXTERN for a symbol it assumed it had defined, so the link
; fails.  Providing fputc_cons_GENERIC instead resolves the EXTERN from
; here, and the library module -- with all 92 of its symbols -- is never
; pulled off the shelf.
;
; This is the same bargain as src/no_font64.asm, which reclaimed the
; 768-byte 64-column font the same way: the linker is not being told to
; drop something, it is being given what it was looking for.
;
; WHAT THIS COSTS.  Any call to printf, puts, putchar or fputc now
; prints NOTHING rather than failing to link -- the routine returns
; without doing anything.  That is the trade: the game does not use
; them, and if a debugging printf is ever wanted back, take this module
; out of SRCS and the driver returns with it.
;
; A `ret` is a defensible body because the character is genuinely
; discarded; the alternative, leaving the symbol undefined, would make
; the link fail on a call that would have been harmless.

    SECTION code_crt0_sccz80

    PUBLIC  fputc_cons_generic

; The generic console driver's entry point.  Discards the character.
fputc_cons_generic:
    ret
