#!/usr/bin/env python3
"""checkmem.py — report the memory layout, and fail the build if it breaks.

This program's memory comes from two places that never see each other,
which is how it has gone wrong before:

  * the LINKER places code, rodata, data and bss from the load address
    upwards, and only zxstrategy.map knows where they ended up;
  * include/memmap.h places the big buffers BY HAND, and only the C
    preprocessor knows where those are.

Neither view is complete on its own, so this prints both together — run
`make memmap` — and enforces the one rule that keeps them apart:

    the linker-placed part must stay below 0xC000.

That is not arbitrary.  0xC000-0xFFFF is a paged bank on a 128K-class
machine, so anything the linker puts up there vanishes the moment
something pages, and the failure looks like random corruption rather
than a crash.  The hand-placed buffers live up there deliberately and
survive only because bank 0 is selected and left alone: hw_detect() ends
by selecting it, and main() locks paging on every machine that does not
need the +2A/+3 floating bus.  If anything ever pages again, those
buffers move first.

The stack is not checked.  z88dk leaves it near 0x7FA0, below the
program and above BASIC, in the page-5 RAM that is always mapped.

Usage:
    python3 tools/checkmem.py zxstrategy.map [--limit 0xC000]
    python3 tools/checkmem.py zxstrategy.map --layout
"""

import os
import re
import sys

DEFAULT_LIMIT = 0xC000
LOAD_ADDR = 0x8000
SECTIONS = ("code_compiler", "rodata_compiler",
            "data_compiler", "bss_compiler")

HERE = os.path.dirname(os.path.abspath(__file__))
MEMMAP_H = os.path.join(HERE, "..", "include", "memmap.h")


def hand_placed():
    """Resolve the MEM_* chain out of include/memmap.h.

       Parsed rather than duplicated: these constants have moved three
       times, and a copy here would have been wrong within the hour."""
    src = open(MEMMAP_H).read()
    vals, order = {}, []
    for name, expr in re.findall(r"#define\s+(MEM_\w+)\s+(.+)", src):
        expr = expr.split("/*")[0].strip()
        try:
            vals[name] = int(eval(expr, {"__builtins__": {}}, dict(vals)))
        except Exception:
            continue
        order.append(name)
    return vals, order


def report(top_addr, top_name, limit):
    vals, order = hand_placed()
    print("  linker-placed (code, rodata, data, bss)")
    print("    %04X .. %04X   %5d bytes   top symbol %s"
          % (LOAD_ADDR, top_addr, top_addr - LOAD_ADDR, top_name))
    print("    %04X .. %04X   %5d bytes   FREE before the 0x%04X limit"
          % (top_addr, limit, limit - top_addr, limit))

    if not vals:
        return
    print()
    print("  hand-placed (include/memmap.h)")
    placed = [(vals[n], n) for n in order
              if n not in ("MEM_END", "MEM_TILES_SIZE")]
    placed.sort()
    end = vals.get("MEM_END", 0)
    for i, (a, n) in enumerate(placed):
        nxt = placed[i + 1][0] if i + 1 < len(placed) else end
        if nxt == a:
            continue                    # an alias for the next block
        print("    %04X .. %04X   %5d bytes   %s" % (a, nxt, nxt - a, n))
    print("    %04X .. FFFF   %5d bytes   FREE to the top of RAM"
          % (end, 0x10000 - end))


def main():
    args = sys.argv[1:]
    limit = DEFAULT_LIMIT
    if "--limit" in args:
        i = args.index("--limit")
        limit = int(args[i + 1], 0)
        del args[i:i + 2]
    layout = "--layout" in args
    if layout:
        args.remove("--layout")
    if not args:
        print(__doc__)
        return 1

    pat = re.compile(r"(\S+)\s+=\s+\$([0-9A-Fa-f]+)\s*;.*?(" +
                     "|".join(SECTIONS) + r")")
    top_addr, top_name = 0, None
    for line in open(args[0]):
        m = pat.match(line)
        if m:
            a = int(m.group(2), 16)
            if a > top_addr:
                top_addr, top_name = a, m.group(1)

    if top_addr == 0:
        print("checkmem: no symbols found in %s — is it a -m map file?"
              % args[0], file=sys.stderr)
        return 1

    if layout:
        report(top_addr, top_name, limit)

    if top_addr >= limit:
        print("checkmem: FAIL — %s is at 0x%04X, at or above the 0x%04X "
              "limit.\n"
              "  0xC000+ is a paged bank on a 128K; anything the linker "
              "puts there\n"
              "  disappears when something pages, and it looks like "
              "corruption, not a crash.\n"
              "  Move data into include/memmap.h instead — run "
              "`make memmap` to see the room."
              % (top_name, top_addr, limit), file=sys.stderr)
        return 1

    if not layout:      # `make map` already said this; do not say it twice
        print("checkmem: ok — top symbol %s at 0x%04X, %d bytes clear "
              "of 0x%04X" % (top_name, top_addr, limit - top_addr, limit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
