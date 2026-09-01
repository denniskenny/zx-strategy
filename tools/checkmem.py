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

# EVERY section the linker places, not a list of four.
#
# This used to name code_compiler, rodata_compiler, data_compiler and
# bss_compiler -- the sections SDCC emits -- and was therefore blind to
# everything the LIBRARY contributes: bss_clib, rodata_clib, code_driver,
# code_clib.  It reported the top symbol as _cs_bank at 0xBBFC while
# __exit_atexit_funcs sat at 0xBC05, nine bytes higher, so the headroom
# figure the whole project plans against was nine bytes optimistic and
# would have grown wrong as more library code was pulled in.
#
# Matching on "; addr," instead catches anything with a real address,
# whatever section it came from.  Absolute constants are "; const," and
# are skipped, which is right: they are not placed anywhere.
ADDR_LINE = re.compile(r"(\S+)\s+=\s+\$([0-9A-Fa-f]+)\s*;\s*addr,")

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


# The stack sits here, growing down: `CLEAR 32767` in the BASIC loader
# leaves SP just under 0x8000.  Free space below it must stop short.
STACK_TOP = 0x7FA0


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
    # NOT "free to the top of RAM": 0x8000 upwards is the program, and
    # reporting it as free is how this tool once claimed 33 622 spare
    # bytes while the binary had five.  Two separate runs, one below the
    # stack and one above the linker's top symbol.
    if end < STACK_TOP:
        print("    %04X .. %04X   %5d bytes   FREE below the stack"
              % (end, STACK_TOP, STACK_TOP - end))
    print("    %04X .. 8000   %5d bytes   stack, grows down" % (STACK_TOP, 0x8000 - STACK_TOP))
    print()
    print("  above the linker's reach (data only — never code)")
    print("    C000 .. DB00    6912 bytes   128K: shadow screen (page 7)")
    print("                                 48K:  spare")
    print("    DB00 .. FFFF    %5d bytes   addressable on BOTH machines"
          % (0x10000 - 0xDB00))
    print("    banks 1,3,4,6   %5d bytes   128K/+3 ONLY, paged at 0xC000"
          % (4 * 16384))


def free_report(top_addr, limit, mapfile):
    """The four budgets, on four lines, in the units a feature is planned in.

       `make memmap` prints the whole layout; this prints only what is
       LEFT, because "can this feature fit?" was being answered by reading
       a 30-line dump and picking the wrong number out of it.  The
       contended figure in particular was read as the project's headroom
       when it is the one region that is deliberately full."""
    vals, _ = hand_placed()
    mem_end = vals.get("MEM_END", 0)

    # the contended window: whatever mktap reported, recomputed here so
    # this does not depend on a build step having just run
    logic_top = 0
    for line in open(mapfile):
        m = ADDR_LINE.match(line)
        if m:
            a = int(m.group(2), 16)
            if 0x6000 <= a < LOAD_ADDR and a > logic_top:
                logic_top = a
    contended = (STACK_TOP - logic_top) if logic_top else 0

    print()
    print("  FREE MEMORY")
    print("    %-30s %6d bytes   %s"
          % ("uncontended  0x8000-0xC000", limit - top_addr,
             "<-- NEW CODE goes here"))
    print("    %-30s %6d bytes   %s"
          % ("contended    0x6000-0x7FA0", contended,
             "cold whole modules only"))
    print("    %-30s %6d bytes   %s"
          % ("data         above MEM_END", 0x10000 - mem_end,
             "never code: a 128K pages it"))
    # the banks, less whatever the cutscene blobs already occupy
    used = 0
    d = os.path.join(HERE, "..", "build", "cutscenes")
    if os.path.isdir(d):
        used = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))
    print("    %-30s %6d bytes   %s"
          % ("banks        1,3,4,6", 4 * 16384 - used - 256,
             "128K/+3 only, storage not code"))


def main():
    args = sys.argv[1:]
    # --addr NAME: print one MEM_* constant and stop.  The Makefile needs
    # MEM_MUSIC as a number to assemble the song data at its destination,
    # and memmap.h is the only place that knows it -- a second copy in the
    # Makefile would be wrong within the hour.
    if "--addr" in args:
        i = args.index("--addr")
        vals, _ = hand_placed()
        name = args[i + 1]
        if name not in vals:
            print("checkmem: no %s in memmap.h" % name, file=sys.stderr)
            return 1
        print("0x%04X" % vals[name])
        return 0
    limit = DEFAULT_LIMIT
    if "--limit" in args:
        i = args.index("--limit")
        limit = int(args[i + 1], 0)
        del args[i:i + 2]
    layout = "--layout" in args
    if layout:
        args.remove("--layout")
    freeonly = "--free" in args
    if freeonly:
        args.remove("--free")
    if not args:
        print(__doc__)
        return 1

    # Only symbols at or above LOAD_ADDR: the contended sections (LOGIC,
    # MUSIC) are placed below 0x8000 and guarded by mktap's stack check,
    # which knows about the stack and this does not.
    top_addr, top_name = 0, None
    for line in open(args[0]):
        m = ADDR_LINE.match(line)
        if m:
            a = int(m.group(2), 16)
            if a >= LOAD_ADDR and a > top_addr:
                top_addr, top_name = a, m.group(1)

    if top_addr == 0:
        print("checkmem: no symbols found in %s — is it a -m map file?"
              % args[0], file=sys.stderr)
        return 1

    if layout:
        report(top_addr, top_name, limit)
    if layout or freeonly:
        free_report(top_addr, limit, args[0])

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

    if not layout and not freeonly:   # `make map` already said this
        print("checkmem: ok — top symbol %s at 0x%04X, %d bytes clear "
              "of 0x%04X" % (top_name, top_addr, limit - top_addr, limit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
