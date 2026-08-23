#!/usr/bin/env python3
"""checkmem.py — fail the build if the binary would break 128K paging.

The 128K render path banks RAM page 7 in at 0xC000 and leaves it there,
so the shadow screen is addressable without paging around every
repaint.  That is only safe while nothing of ours lives at 0xC000 or
above: anything up there would be swapped out of sight the moment page 7
arrives, and the failure looks like random corruption rather than an
obvious crash.

So the rule is a hard one and worth enforcing mechanically: the whole
program — code, rodata, data and bss — has to fit between the load
address and 0xC000.  That is 16 KB at -zorg=32768.

The stack is not checked here; z88dk leaves it around 0x7FA0, in the
page-5 RAM that is always mapped.  If the startup ever moves it above
0xC000 this script will not notice and the 128K path will break, so
check `SP` in the debugger if paging starts misbehaving.

Usage:
    python3 tools/checkmem.py zxstrategy.map [--limit 0xC000]
"""

import re
import sys

DEFAULT_LIMIT = 0xC000

SECTIONS = ("code_compiler", "rodata_compiler",
            "data_compiler", "bss_compiler")


def main():
    args = sys.argv[1:]
    limit = DEFAULT_LIMIT
    if "--limit" in args:
        i = args.index("--limit")
        limit = int(args[i + 1], 0)
        del args[i:i + 2]
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

    if top_addr >= limit:
        print("checkmem: FAIL — %s is at 0x%04X, at or above the 0x%04X "
              "limit.\n"
              "  The 128K render path banks page 7 in at 0xC000, which "
              "would hide it.\n"
              "  Free some space, or move data into the 0x6000-0x7FFF "
              "region (contended,\n"
              "  but drawing happens in the vblank window where "
              "contention does not bite)."
              % (top_name, top_addr, limit), file=sys.stderr)
        return 1

    print("checkmem: ok — top symbol %s at 0x%04X, %d bytes clear of 0x%04X"
          % (top_name, top_addr, limit - top_addr, limit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
