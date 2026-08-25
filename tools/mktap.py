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


def loader(clear_addr, usr_addr, n_blocks, banks):
    """CLEAR, then one LOAD ""CODE per block, then RANDOMIZE USR.

       A BANK block is loaded with the bank paged in at 0xC000 and paged
       out again afterwards, which BASIC can do with `OUT 32765`.

       A 48K runs the same two OUTs harmlessly -- it has no paging -- and
       then loads the block into 0xC000, which on a 48K is spare RAM
       (docs/PLAN.md P11).  It costs the tape time and nothing else, which
       is far simpler than branching on machine type in BASIC, and means
       one loader for every machine."""
    line = 10
    prog = basic_line(line, bytes([CLEAR]) + number(clear_addr))
    for i in range(n_blocks):
        line += 10
        prog += basic_line(line, bytes([LOAD, ord('"'), ord('"'), CODE]))
    for bank in banks:
        line += 10
        # POKE BANKM as well as OUT, and in that order.
        #
        # 0x7FFD is write-only, so the ROM keeps its own copy at BANKM
        # (23388 / 0x5B5C) and writes it back whenever it touches paging
        # -- the interrupt handler included.  BASIC's OUT sets the port
        # and not the variable, so the ROM undoes the switch part way
        # through the LOAD and the bytes land in whatever bank BANKM
        # still names.  That is bank 0, and bank_probe read 2 ("readable,
        # wrong contents") every time.
        #
        # Bit 4 stays CLEAR: it is the ROM select, and this runs while
        # the 128K editor is executing the LOAD.  Setting it swaps the 48K
        # ROM in underneath the editor mid-load.
        line += 10
        prog += basic_line(line, bytes([LOAD, ord('"'), ord('"'), CODE]))
        line += 10
        prog += basic_line(line, bytes([POKE]) + number(0x5B5C)
                           + b"," + number(0))
        line += 10
        prog += basic_line(line, bytes([OUT]) + number(0x7FFD)
                           + b"," + number(0))
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
    ap.add_argument('--bank', nargs=2, action='append', default=[],
                    metavar=('BANK', 'FILE'),
                    help='RAM bank number and binary, loaded at 0xC000 with '
                         'that bank paged in; 128K only in effect, harmless '
                         'on a 48K')
    a = ap.parse_args()

    codes = [(int(addr, 0), open(f, 'rb').read()) for addr, f in a.code]
    banks = [(int(b, 0), open(f, 'rb').read()) for b, f in a.bank]
    for bank, data in banks:
        if not 0 <= bank <= 7:
            sys.exit('mktap: bank %d is not 0-7' % bank)
        if 0xC000 + len(data) > 0x10000:
            sys.exit('mktap: %d bytes at 0xC000 overruns the bank'
                     % len(data))

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

    prog = loader(a.clear, a.usr, len(codes), [b for b, _ in banks])
    tap = header(0, a.name[:10], len(prog), 10, len(prog))   # p1=autostart line
    tap += block(bytes([0xFF]) + prog)
    for addr, data in codes:
        tap += header(3, a.name[:10], len(data), addr, 0x8000)
        tap += block(bytes([0xFF]) + data)
    # Bank blocks come last, in the order the loader pages them.
    for bank, data in banks:
        tap += header(3, a.name[:10], len(data), 0xC000, 0x8000)
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
