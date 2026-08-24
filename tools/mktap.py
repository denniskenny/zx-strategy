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
CLEAR, LOAD, CODE, RANDOMIZE, USR = 0xFD, 0xEF, 0xAF, 0xF9, 0xC0


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


def loader(clear_addr, usr_addr, n_blocks):
    prog = basic_line(10, bytes([CLEAR]) + number(clear_addr))
    for i in range(n_blocks):
        prog += basic_line(20 + i, bytes([LOAD, ord('"'), ord('"'), CODE]))
    prog += basic_line(20 + n_blocks,
                       bytes([RANDOMIZE, USR]) + number(usr_addr))
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
    a = ap.parse_args()

    codes = [(int(addr, 0), open(f, 'rb').read()) for addr, f in a.code]

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

    prog = loader(a.clear, a.usr, len(codes))
    tap = header(0, a.name[:10], len(prog), 10, len(prog))   # p1=autostart line
    tap += block(bytes([0xFF]) + prog)
    for addr, data in codes:
        tap += header(3, a.name[:10], len(data), addr, 0x8000)
        tap += block(bytes([0xFF]) + data)

    open(a.output, 'wb').write(tap)
    print('mktap: %s  loader + %d CODE blocks' % (a.output, len(codes)))
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
