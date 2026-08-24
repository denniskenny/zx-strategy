; Place logic.c's code in the contended window below the program.
;
; logic.c is the right module to put here: AI and pathfinding run once
; per turn, not once per frame, so the ULA stealing a cycle from every
; fetch costs nothing anyone can see.  The render path stays at 0x8000.
;
; 0x6500 leaves the asset blobs at 0x6000 alone; tools/mkassets.py
; reports where they end.
    SECTION LOGIC
    org     0x6500
