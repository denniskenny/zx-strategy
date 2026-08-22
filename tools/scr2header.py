#!/usr/bin/env python3
"""Convert a .scr file's pixel data to an RLE-compressed C header.

RLE format: pairs of (count, value) bytes, terminated by count=0.
Only the 6144-byte pixel area is encoded; attributes are ignored.

Usage: python3 tools/scr2header.py input.scr output.h [array_name]
"""

import sys


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} input.scr output.h [array_name]")
        sys.exit(1)

    src = sys.argv[1]
    dst = sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else "scr_data"

    with open(src, "rb") as f:
        data = f.read()[:6144]

    # RLE encode
    runs = []
    i = 0
    while i < len(data):
        val = data[i]
        count = 1
        while i + count < len(data) and data[i + count] == val and count < 255:
            count += 1
        runs.append((count, val))
        i += count

    rle = []
    for count, val in runs:
        rle.append(count)
        rle.append(val)
    rle.append(0)  # terminator

    guard = f"_{name.upper()}_H_"
    with open(dst, "w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write(f"/* RLE-compressed screen pixel data ({len(rle)} bytes) */\n")
        f.write(f"/* Decoded size: 6144 bytes */\n")
        f.write(f"static const unsigned char {name}[] = {{\n")
        for i in range(0, len(rle), 16):
            chunk = rle[i : i + 16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
            if i + 16 < len(rle):
                f.write(",")
            f.write("\n")
        f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")

    print(f"Written {dst}: {len(rle)} bytes RLE ({len(runs)} runs)")


if __name__ == "__main__":
    main()
