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
BC_PARAMS = 0x5F00          # bank, length, destination
BC_ENTRY  = 0x5F05
STAGE     = 0x4000          # the screen: the only free 6912 bytes there is


def loader(clear_addr, usr_addr, n_blocks, banks, dest=0):
    """CLEAR, the bank blocks, then the program, then RANDOMIZE USR.

       BANK BLOCKS COME FIRST, and before any code block, for two reasons.

       They stage through the SCREEN -- the only free 6912 bytes on the
       machine -- so doing them first means there is nothing else in
       memory for the staging to land on.  And the copier itself has to be
       resident before it can be called, so something has to be ordered
       anyway; putting the whole bank phase up front makes that explicit
       rather than incidental.

       The load shows as noise on the screen, which is honest: something
       is loading, and the next block along overwrites it.

       BASIC never writes 0x7FFD.  It LOADs, POKEs the parameters and
       calls the copier; the paging is machine code, with the ROM bit kept
       and BANKM updated.  BASIC doing the paging failed three times --
       see .claude/skills/zx-loader."""
    line = 10
    prog = basic_line(line, bytes([CLEAR]) + number(clear_addr))

    if banks:
        line += 10                          # the copier itself
        prog += basic_line(line, bytes([LOAD, ord('"'), ord('"'), CODE]))
    for bank, data in banks:
        line += 10                          # the blob, into the screen
        prog += basic_line(line, bytes([LOAD, ord('"'), ord('"'), CODE]))
        for off, val in ((0, bank),
                         (1, len(data) & 0xFF), (2, len(data) >> 8),
                         (3, dest & 0xFF), (4, dest >> 8)):
            line += 10
            prog += basic_line(line, bytes([POKE]) + number(BC_PARAMS + off)
                               + b"," + number(val))
        line += 10
        prog += basic_line(line, bytes([RANDOMIZE, USR]) + number(BC_ENTRY))

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
    ap.add_argument('--bank-dest', default='0',
                    help='offset within the bank (default 0; avoid 0, which '
                         'collides with hw_detect on this project)')
    ap.add_argument('--bankcopy', metavar='FILE',
                    help='src/bankcopy.asm assembled; required with --bank')
    ap.add_argument('--bank', nargs=2, action='append', default=[],
                    metavar=('BANK', 'FILE'),
                    help='RAM bank number and binary, loaded at 0xC000 with '
                         'that bank paged in; 128K only in effect, harmless '
                         'on a 48K')
    a = ap.parse_args()

    codes = [(int(addr, 0), open(f, 'rb').read()) for addr, f in a.code]
    banks = [(int(b, 0), open(f, 'rb').read()) for b, f in a.bank]
    if banks and not a.bankcopy:
        sys.exit('mktap: --bank needs --bankcopy: BASIC cannot page safely, '
                 'so the copier has to be on the tape too')
    for bank, data in banks:
        if len(data) > 0x1B00:
            sys.exit('mktap: %d bytes will not stage through the screen '
                     '(6912 max)' % len(data))
    for bank, data in banks:
        if not 0 <= bank <= 7:
            sys.exit('mktap: bank %d is not 0-7' % bank)


    for i, (addr, data) in enumerate(codes):
        end = addr + len(data)
        if end > 0x10000:
            sys.exit('mktap: %d bytes at 0x%04X runs off the top of RAM' % (len(data), addr))
        if addr <= a.clear:
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

    prog = loader(a.clear, a.usr, len(codes), banks, int(a.bank_dest, 0))
    tap = header(0, a.name[:10], len(prog), 10, len(prog))   # p1=autostart line
    tap += block(bytes([0xFF]) + prog)
    # Bank phase first: the copier, then each blob staged through the
    # screen.  The loader above expects exactly this order.
    if banks:
        cp = open(a.bankcopy, 'rb').read()
        tap += header(3, a.name[:10], len(cp), BC_PARAMS, 0x8000)
        tap += block(bytes([0xFF]) + cp)
    for bank, data in banks:
        tap += header(3, a.name[:10], len(data), STAGE, 0x8000)
        tap += block(bytes([0xFF]) + data)

    for addr, data in codes:
        tap += header(3, a.name[:10], len(data), addr, 0x8000)
        tap += block(bytes([0xFF]) + data)

    open(a.output, 'wb').write(tap)
    print('mktap: %s  loader + %d CODE blocks%s'
          % (a.output, len(codes),
             (' + %d bank' % len(banks)) if banks else ''))
    for bank, data in banks:
        print('       bank %d @0xC000  %6d bytes' % (bank, len(data)))
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
