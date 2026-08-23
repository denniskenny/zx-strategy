; === Reclaim the 64-column font ==============================================
;
; z88dk's zx console keeps a pointer to a 4x8 font in __zx_64col_font, and
; console_vars.asm declares
;
;       EXTERN  CRT_FONT_64
;
; purely to store its address in that two-byte variable.  The EXTERN is
; enough to make the linker pull in CRT_FONT_64.asm -- 768 bytes of glyph
; data -- even though nothing in this program ever prints through the
; console.  print_at() reads the ROM's own character set at 0x3D00.
;
; 768 bytes is a large fraction of the 16 KB the 128k target has to fit
; inside, so define the symbol here instead and let it resolve to the ROM
; font.  The linker then has no unresolved reference, does not load the
; module, and __zx_64col_font ends up pointing somewhere valid rather than
; at nothing -- which matters if anything ever does reach the console.
;
; If a future change genuinely needs a 4x8 font, delete this file; the real
; one comes back on its own.

                MODULE  no_font64
                PUBLIC  CRT_FONT_64

                defc    CRT_FONT_64 = 0x3D00
