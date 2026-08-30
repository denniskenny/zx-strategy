#!/usr/bin/env python3
"""Build a multi-block .tap: a BASIC loader plus one CODE block per binary.

z88dk's -create-app emits ONE contiguous CODE block from CRT_ORG_CODE and
a 30-byte loader that does a single LOAD ""CODE.  Anything outside that
range is either dropped silently (a section with `org`) or shipped
headerless and never loaded (a bank section).  Both failures look the
same at runtime -- the data reads as zeros -- so this builds the tap
explicitly instead.

    python3 tools/mktap.py out.tap --clear 32767 --usr 32768 \\
        --code 0x6000 assets_low.bin \\
        --code 0x8000 zxstrategy_CODE.bin

Blocks load in the order given.  Every CODE block gets a real header, so
a plain `LOAD ""CODE` reads each in turn -- which works on a 48K with no
paging, unlike anything bank-based.
"""

import argparse
import struct
import sys

# `CLEAR 24575` leaves the stack just under 0x8000, growing down.  Blocks
# loaded below the program grow up towards it.
STACK_FLOOR = 0x7FA0

# BASIC tokens
CLEAR, LOAD, CODE, RANDOMIZE, USR, OUT, POKE = \
    0xFD, 0xEF, 0xAF, 0xF9, 0xC0, 0xDF, 0xF4
FOR, TO, NEXT = 0xEB, 0xCC, 0xF3
BORDER, PAPER, INK, CLS = 0xE7, 0xDA, 0xD9, 0xFB


def number(n):
    """A BASIC numeric literal: the digits, then the 5-byte binary form.

       The ROM reads the binary form and ignores the digits, but LIST
       shows the digits, and a missing 0x0E marker makes the line
       unparseable rather than merely odd."""
    out = str(n).encode()
    # small-integer form: 0x00, sign, low, high, 0x00
    return out + bytes([0x0E, 0x00, 0x00, n & 0xFF, (n >> 8) & 0xFF, 0x00])


def basic_line(num, body):
    return struct.pack('>H', num) + struct.pack('<H', len(body) + 1) + body + b'\x0D'


# src/bankcopy.asm, assembled standalone into the PRINTER BUFFER.  Fixed
# addresses so the BASIC loader can name them without reading a link map
# that does not exist until after the link.
#
# 0x5F00, just above the loader's CLEAR.  Everything below RAMTOP belongs
# to the ROM or BASIC: 0x5AFA is the attribute file, and 0x5B00 is the
# 128K's system variables (BANKM itself is at 0x5B5C).  Both crashed.
BC_ENTRY  = 0x5F05          # call once per block; it advances itself
BC_TABLE  = 0x80            # offset of the table WITHIN the copier block
BC_ENTRY_LEN = 5            # bank, dest word, length word
STAGE     = 0x4000          # the screen: the only free 6912 bytes there is


def loader(clear_addr, usr_addr, n_blocks, banks, n_splash=0):
    """CLEAR, the bank phase, then the program.

       The bank phase is ONE LINE however many blocks it carries:

           FOR I=1 TO n: LOAD ""CODE: RANDOMIZE USR e: NEXT I

       src/bankcopy.asm walks a table appended to its own block, so BASIC
       does not have to name each destination.  It used to: a LOAD, five
       POKEs and a USR per block, which at ten cutscene screens made a
       1472-byte BASIC program -- past RAMTOP at 0x5EFF, over the stub at
       0x5F00 and into the assets at 0x6000.  Every block loaded and the
       game never started.

       A loader whose size grows with the content is the bug.  This one
       does not.

       Bank blocks come FIRST, before any code block: they stage through
       the SCREEN, the only free 6912 bytes on the machine, so doing them
       first means there is nothing else in memory to land on."""
    line = 10
    prog = basic_line(line, bytes([CLEAR]) + number(clear_addr))

    # BLACK ON BLACK, before a single byte is loaded.
    #
    # The bank blocks are staged THROUGH THE DISPLAY FILE -- it is the only
    # free 6912 bytes on the machine -- so ten cutscene screens land in the
    # top two thirds of the screen in turn, as noise.  The ROM's own
    # "Bytes:" messages print over them.  With INK 0 PAPER 0 and a CLS,
    # every attribute cell is 0x00 and none of it can be seen: the bitmap
    # still changes, but black on black shows nothing.
    #
    # BORDER 0 too, so the tape's edge flicker is the only motion.
    line += 10
    prog += basic_line(line,
                       bytes([BORDER]) + number(0) + b':'
                       + bytes([PAPER]) + number(0) + b':'
                       + bytes([INK]) + number(0) + b':'
                       + bytes([CLS]))

    # SILENCE THE LOADER'S MESSAGES, and this one is load-bearing.
    #
    # 23739 is the low byte of the OUTPUT ROUTINE ADDRESS for channel "S"
    # (the main screen) in the channel information block at CHANS, 23734.
    # It normally points at PRINT-OUT; 111 points it at a RET, so anything
    # the ROM prints to the screen is thrown away instead.
    #
    # Without it the ROM announces every block -- "Bytes: " x 14 -- and once
    # the print position passes the bottom of the screen it SCROLLS.  That
    # is what was taking the boot logo away: not the bank staging, which
    # never touches the bottom third, but the ROM tidily scrolling the
    # picture off the top.  Black on black hid the messages; it could not
    # stop them scrolling.
    #
    # The cost: a tape ERROR is now silent too.  If loading ever fails
    # mysteriously on real hardware, comment this line out first.
    line += 10
    prog += basic_line(line,
                       bytes([POKE]) + number(23739) + b',' + number(111))

    # SPLASH FIRST, before anything else on the tape.
    #
    # The logo is the first thing loaded so the screen stops being blank as
    # early as possible -- everything after it (14 blocks, ~50 KB) loads
    # with the picture already up.  Safe because the bank blobs stage
    # through 0x4000-0x49D7 and the logo lives in the bottom third at
    # 0x5000, which nothing else touches.
    for _ in range(n_splash):
        line += 10
        prog += basic_line(line, bytes([LOAD, ord('"'), ord('"'), CODE]))

    if banks:
        line += 10                          # the copier, and its table
        prog += basic_line(line, bytes([LOAD, ord('"'), ord('"'), CODE]))
        line += 10
        prog += basic_line(line,
                           bytes([FOR, ord('I'), ord('=')]) + number(1)
                           + bytes([TO]) + number(len(banks)) + b':'
                           + bytes([LOAD, ord('"'), ord('"'), CODE]) + b':'
                           + bytes([RANDOMIZE, USR]) + number(BC_ENTRY) + b':'
                           + bytes([NEXT, ord('I')]))

    for i in range(n_blocks):
        line += 10
        prog += basic_line(line, bytes([LOAD, ord('"'), ord('"'), CODE]))
    line += 10
    prog += basic_line(line, bytes([RANDOMIZE, USR]) + number(usr_addr))
    return prog


def block(data):
    """One tap block: length, then the flagged payload with its checksum."""
    chk = 0
    for b in data:
        chk ^= b
    payload = data + bytes([chk])
    return struct.pack('<H', len(payload)) + payload


def header(typ, name, length, p1, p2):
    h = bytes([0x00, typ]) + name.encode()[:10].ljust(10)
    h += struct.pack('<HHH', length, p1, p2)
    return block(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('output')
    ap.add_argument('--clear', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--usr', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--name', default='zxstrategy')
    ap.add_argument('--code', nargs=2, action='append', metavar=('ADDR', 'FILE'),
                    required=True, help='load address and binary, repeatable')
    ap.add_argument('--splash', nargs=2, action='append', default=[],
                    metavar=('ADDR', 'FILE'),
                    help='loaded FIRST, before the bank phase and the '
                         'program; for a picture the user looks at while '
                         'the rest of the tape runs')
    ap.add_argument('--bankcopy', metavar='FILE',
                    help='src/bankcopy.asm assembled; required with --bank')
    ap.add_argument('--bank', nargs=3, action='append', default=[],
                    metavar=('BANK', 'DEST', 'FILE'),
                    help='RAM bank number and binary, loaded at 0xC000 with '
                         'that bank paged in; 128K only in effect, harmless '
                         'on a 48K')
    a = ap.parse_args()

    codes = [(int(addr, 0), open(f, 'rb').read()) for addr, f in a.code]
    splash = [(int(addr, 0), open(f, 'rb').read()) for addr, f in a.splash]
    # (bank, offset-within-bank, bytes).  The offset is per-entry because
    # several blobs share a bank -- tools/mkcutscenes.py computes the
    # layout and passes it here.
    banks = [(int(b, 0), int(d, 0), open(f, 'rb').read())
             for b, d, f in a.bank]
    if banks and not a.bankcopy:
        sys.exit('mktap: --bank needs --bankcopy: BASIC cannot page safely, '
                 'so the copier has to be on the tape too')
    for bank, dest, data in banks:
        if len(data) > 0x1B00:
            sys.exit('mktap: %d bytes will not stage through the screen '
                     '(6912 max)' % len(data))
        if dest + len(data) > 0x4000:
            sys.exit('mktap: bank %d: %d bytes at 0x%04X overruns the bank'
                     % (bank, len(data), dest))
    for bank, dest, data in banks:
        if not 0 <= bank <= 7:
            sys.exit('mktap: bank %d is not 0-7' % bank)


    for i, (addr, data) in enumerate(codes):
        end = addr + len(data)
        if end > 0x10000:
            sys.exit('mktap: %d bytes at 0x%04X runs off the top of RAM' % (len(data), addr))
        # The DISPLAY FILE is exempt.  It is below CLEAR, but BASIC does not
        # keep variables there -- it is the screen -- so a block aimed at it
        # is a picture, not a mistake.  That is how the boot logo gets on
        # screen without costing the program a byte: the ROM's own LOAD puts
        # it there and nothing ever refers to it again.
        if addr <= a.clear and not (0x4000 <= addr and end <= 0x5B00):
            sys.exit('mktap: block at 0x%04X is at or below CLEAR %d -- BASIC '
                     'would overwrite it' % (addr, a.clear))
        # A block below the program grows UP towards the stack, which grows
        # DOWN from STACK_FLOOR.  Nothing else notices them meeting:
        # checkmem watches 0xC000 and knows nothing about these blocks, and
        # the tape happily loads over the stack's future home.  The symptom
        # is a return address eaten mid-call, arbitrarily far from the
        # module that grew.
        if addr < 0x8000 and end > STACK_FLOOR:
            sys.exit('mktap: block at 0x%04X..0x%04X runs into the stack at '
                     '0x%04X -- %d bytes too big.  Shrink it, or move it '
                     'above 0x8000 and pay the 0xC000 ceiling instead'
                     % (addr, end, STACK_FLOOR, end - STACK_FLOOR))
        # Blocks are placed by hand in the Makefile while their SIZES come
        # from the build, so one growing into the next is a question of
        # when, not whether.  The tape loads them in order and the second
        # simply lands on the first: no error, just a program built out of
        # two half-overwritten pieces.
        for j, (other, odata) in enumerate(codes):
            if j <= i:
                continue
            if addr < other + len(odata) and other < end:
                sys.exit('mktap: block at 0x%04X..0x%04X overlaps the one at '
                         '0x%04X..0x%04X -- the later load would land on top '
                         'of the earlier one'
                         % (addr, end, other, other + len(odata)))

    prog = loader(a.clear, a.usr, len(codes), banks, len(splash))
    tap = header(0, a.name[:10], len(prog), 10, len(prog))   # p1=autostart line
    tap += block(bytes([0xFF]) + prog)
    # The splash comes before everything, matching the loader.
    for addr, data in splash:
        tap += header(3, a.name[:10], len(data), addr, 0x8000)
        tap += block(bytes([0xFF]) + data)

    # Bank phase next: the copier, then each blob staged through the
    # screen.  The loader above expects exactly this order.
    if banks:
        # The copier and its table ship as ONE block: pad the code out to
        # BC_TABLE and append five bytes per bank block.  They cannot get
        # out of step because they are the same block.
        cp = open(a.bankcopy, 'rb').read()
        if len(cp) > BC_TABLE:
            sys.exit('mktap: bankcopy is %d bytes, past the table at 0x%02X'
                     % (len(cp), BC_TABLE))
        cp = cp.ljust(BC_TABLE, b'\x00')
        for bank, dest, data in banks:
            cp += bytes([bank, dest & 0xFF, dest >> 8,
                         len(data) & 0xFF, len(data) >> 8])
        if 0x5F00 + len(cp) > 0x6000:
            sys.exit('mktap: copier + %d table entries reaches 0x%04X, '
                     'into the assets at 0x6000'
                     % (len(banks), 0x5F00 + len(cp)))
        tap += header(3, a.name[:10], len(cp), 0x5F00, 0x8000)
        tap += block(bytes([0xFF]) + cp)
    for bank, dest, data in banks:
        tap += header(3, a.name[:10], len(data), STAGE, 0x8000)
        tap += block(bytes([0xFF]) + data)

    for addr, data in codes:
        tap += header(3, a.name[:10], len(data), addr, 0x8000)
        tap += block(bytes([0xFF]) + data)

    open(a.output, 'wb').write(tap)
    print('mktap: %s  loader + %d CODE blocks%s'
          % (a.output, len(codes),
             (' + %d bank' % len(banks)) if banks else ''))
    for bank, dest, data in banks:
        print('       bank %d @0x%04X  %6d bytes'
              % (bank, 0xC000 + dest, len(data)))
    for addr, data in sorted(codes):
        print('       0x%04X .. 0x%04X  %6d bytes' % (addr, addr + len(data), len(data)))
    # What is left in the contended window, which is the budget that
    # decides whether the next asset or module fits down there.
    low = [(addr, data) for addr, data in codes if addr < 0x8000]
    if low:
        top = max(addr + len(data) for addr, data in low)
        print('       0x%04X .. 0x%04X  %6d bytes FREE below the stack'
              % (top, STACK_FLOOR, STACK_FLOOR - top))


main()
