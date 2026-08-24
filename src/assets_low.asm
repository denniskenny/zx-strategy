; Compressed graphics for the contended 0x6000-0x7FFF window.
;
; Assembled STANDALONE, not linked into the C program, so none of it
; counts against the 16 KB code budget below 0xC000.  tools/mktap.py
; packages it as its own CODE block with a real header, which is why a
; plain LOAD ""CODE reads it -- and why a 48K, which cannot page at all,
; gets these graphics exactly like a 128K does.
;
; The C side sees only addresses; see include/assets_low.h.

    org     0x6000

sentinel:
    defb    0xA5, 0x5A, 0xC3, 0x3C
    defs    252, 0xE7
