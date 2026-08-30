#!/usr/bin/env python3
"""Turn assets/logo.zxp into two raw blocks the TAPE loads straight into
the display file.

    tools/mklogo.py assets/logo.zxp logo_third.bin logo_attr.bin

WHY RAW, AND WHY NOT IN THE PROGRAM

The logo is shown once while the game loads and is then thrown away.  It
does not need to be compressed and it does not need to exist in memory:
the ROM's own LOAD can put it in the screen, and nothing in the program
ever refers to it.

The first version compiled a ZX0 blob into the binary and decompressed it
in render_logo() -- 358 bytes of the scarce 0x8000-0xC000 region plus the
blit, for a picture that is overwritten seconds later.  This costs nothing
but tape.

WHERE IT GOES

The BOTTOM THIRD, character rows 16-23:

  * the bank blocks stage through 0x4000-0x49D7 (2519 bytes of cutscene
    each), which covers the top third and the first two pixel lines of the
    middle one -- the bottom third is never touched;
  * the ROM prints one "Bytes:" line per block and there are 14 blocks,
    which fits in the 22 lines above without scrolling.  If a fifteenth
    block is ever added, check that again: a scroll would drag the logo up
    and out of its rows.

A third's pixel lines are 256 bytes apart, so three character rows are NOT
contiguous -- hence the whole 2048-byte third goes, with the logo placed
inside it, plus 256 bytes of attributes for rows 16-23.
"""

import sys

THIRD = 0x800           # 2048 bytes of bitmap, character rows 16-23
ATTRS = 8 * 32          # ...and their attributes
ROW_IN_THIRD = 1        # put the logo on character rows 17-19


def main():
    src, out_pix, out_attr = sys.argv[1], sys.argv[2], sys.argv[3]

    lines = [l.rstrip('\r\n') for l in open(src)]
    px = [l for l in lines if l and set(l) <= set('01')]
    if not px:
        sys.exit('mklogo: no pixel rows in ' + src)
    w, h = len(px[0]), len(px)
    if w % 8 or h % 8:
        sys.exit('mklogo: %dx%d is not a whole number of characters' % (w, h))
    cw, ch = w // 8, h // 8

    # Attributes are TWO HEX DIGITS.  Insist on that rather than trusting
    # the separator count.
    #
    # This used to skip one line after the pixels and int(tok, 16) whatever
    # followed.  A .zxp has a trailing all-zero row before its attribute
    # block, so the first "token" was a 192-character string of noughts --
    # which parses cleanly as 0 and became a 73rd attribute at the FRONT,
    # shifting every real one a cell to the right.  The count check passed
    # because 73 >= 72, and the logo came out horizontally misaligned.
    #
    # Requiring exactly two hex digits skips anything that is not an
    # attribute, however many blank or stray lines there are.
    attrs = []
    for line in lines[len(px):]:
        for tok in line.split():
            if len(tok) == 2 and all(c in '0123456789abcdefABCDEF'
                                     for c in tok):
                attrs.append(int(tok, 16))
    if len(attrs) != cw * ch:
        sys.exit('mklogo: expected exactly %d attribute cells, found %d -- '
                 'if that is 1 more than expected, something that is not an '
                 'attribute is being parsed as one'
                 % (cw * ch, len(attrs)))

    col = (32 - cw) // 2                    # centred
    pix = bytearray(THIRD)
    for y in range(h):
        crow = ROW_IN_THIRD + y // 8        # character row within the third
        line = y % 8                        # pixel line within it
        for bx in range(cw):
            b = 0
            for bit in range(8):
                if px[y][bx * 8 + bit] == '1':
                    b |= 0x80 >> bit
            pix[line * 256 + crow * 32 + col + bx] = b

    at = bytearray(ATTRS)                   # 0x00: black, like the loader
    for r in range(ch):
        for c in range(cw):
            at[(ROW_IN_THIRD + r) * 32 + col + c] = attrs[r * cw + c]

    open(out_pix, 'wb').write(bytes(pix))
    open(out_attr, 'wb').write(bytes(at))
    print('mklogo: %dx%d chars at row %d, col %d -> %d + %d bytes, raw'
          % (cw, ch, 16 + ROW_IN_THIRD, col, len(pix), len(at)))


main()
