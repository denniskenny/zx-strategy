#!/usr/bin/env python3
"""Convert .zx0 compressed binary files to C header arrays.

Usage: python3 tools/zx0_to_header.py output.h name1:file1.zx0 [name2:file2.zx0 ...]
"""

import sys


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} output.h name1:file1.zx0 [name2:file2.zx0 ...]")
        sys.exit(1)

    dst = sys.argv[1]
    entries = []
    for arg in sys.argv[2:]:
        name, path = arg.split(":", 1)
        with open(path, "rb") as f:
            data = f.read()
        entries.append((name, data))

    import os
    base = os.path.basename(dst).replace(".", "_").upper()
    guard = f"_{base}_"
    with open(dst, "w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        for name, data in entries:
            f.write(f"/* ZX0 compressed screen data ({len(data)} bytes) */\n")
            f.write(f"static const unsigned char {name}[] = {{\n")
            for i in range(0, len(data), 16):
                chunk = data[i : i + 16]
                f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
                if i + 16 < len(data):
                    f.write(",")
                f.write("\n")
            f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")

    total = sum(len(d) for _, d in entries)
    print(f"Written {dst}: {len(entries)} arrays, {total} bytes total")


if __name__ == "__main__":
    main()
