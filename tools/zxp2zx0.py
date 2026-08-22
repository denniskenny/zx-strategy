#!/usr/bin/env python3
"""zxp2zx0.py — Convert a full-width .zxp banner into a ZX0-compressed C header.

For a title banner that spans the full 256px width and starts at the top of the
screen, the pixel bytes are emitted in ZX-screen-address order so the runtime
can decompress them straight to $4000 (no blit) — they land on pixel rows
0..height-1.  Attributes are emitted uncompressed (copy to $5800).

  assets/logo.zxp  ->  include/title_logo.h
      title_logo_zx0[]   ZX0-compressed pixels (screen layout)
      title_logo_attr[]  height/8 * 32 attribute cells (rows 0..)

Usage: zxp2zx0.py INPUT.zxp OUTPUT.h --name NAME --zx0 /path/to/zx0
Height must be a multiple of 8 and width must be 256.
"""
import argparse
import os
import subprocess
import sys


def parse_zxp(path):
    lines = [l.rstrip("\r\n") for l in open(path)]
    i = 2
    while i < len(lines) and lines[i].strip() == "":
        i += 1
    pixels = []
    while i < len(lines) and lines[i].strip() != "":
        if not all(c in "01" for c in lines[i]):
            sys.exit(f"bad pixel line {i+1}")
        pixels.append(lines[i])
        i += 1
    attrs = []
    for l in lines[i + 1:]:
        for tok in l.split():
            attrs.append(int(tok, 16))
    return pixels, attrs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--name", default="title_logo")
    ap.add_argument("--zx0", default=os.environ.get("ZX0", "/tmp/ZX0/src/zx0"))
    args = ap.parse_args()

    pixels, attrs = parse_zxp(args.input)
    h = len(pixels)
    w = len(pixels[0])
    if w != 256:
        sys.exit(f"width must be 256 (got {w})")
    if h % 8:
        sys.exit(f"height must be a multiple of 8 (got {h})")
    char_rows = h // 8

    # art[y][x] = 1 if ink
    art = [[1 if pixels[y][x] == "1" else 0 for x in range(w)] for y in range(h)]

    # Emit bytes in ZX screen-address order so they decompress straight to
    # $4000.  The screen interleaves by pixel-line, so a partial "third"
    # (2048 bytes / 64 rows) would leave gaps — round the byte layout UP to
    # whole thirds and pad rows >= h with black.  offset -> (col, y):
    #   third=o>>11, line=(o>>8)&7, char_row=(o>>5)&7, col=o&31
    #   y = third*64 + char_row*8 + line
    thirds = (h + 63) // 64
    nbytes = thirds * 2048
    raw = bytearray(nbytes)
    for o in range(nbytes):
        third = o >> 11
        line = (o >> 8) & 7
        crow = (o >> 5) & 7
        col = o & 31
        y = third * 64 + crow * 8 + line
        if y >= h:
            continue                       # black padding to the third boundary
        b = 0
        for bit in range(8):
            if art[y][col * 8 + bit]:
                b |= 0x80 >> bit
        raw[o] = b

    def zx0(data, tag):
        b = "/tmp/%s_%s.bin" % (args.name, tag)
        z = "/tmp/%s_%s.zx0" % (args.name, tag)
        open(b, "wb").write(bytes(data))
        if os.path.exists(z):
            os.remove(z)
        subprocess.run([args.zx0, "-f", b, z], check=True)
        return open(z, "rb").read()

    comp = zx0(raw, "pix")

    if len(attrs) < char_rows * 32:
        attrs = attrs + [0x07] * (char_rows * 32 - len(attrs))
    attrs = attrs[:char_rows * 32]
    comp_attr = zx0(attrs, "attr")            # runs of one/two colours -> tiny

    guard = "_%s_H_" % args.name.upper()

    def carr(name, data, comment):
        out = [f"/* {comment} */", f"static const unsigned char {name}[] = {{"]
        for i in range(0, len(data), 16):
            out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
        out.append("};")
        return out

    lines = [f"#ifndef {guard}", f"#define {guard}", ""]
    lines += carr(f"{args.name}_zx0", comp,
                  f"ZX0 {len(comp)} B <- {nbytes} B, 256x{h} banner, "
                  f"screen layout, decompress to $4000")
    lines.append("")
    lines += carr(f"{args.name}_attr_zx0", comp_attr,
                  f"ZX0 {len(comp_attr)} B <- {len(attrs)} B, "
                  f"{char_rows}x32 attribute cells, decompress to $5800")
    lines += ["", f"#endif /* {guard} */", ""]
    open(args.output, "w").write("\n".join(lines))
    print(f"wrote {args.output}: pixels {len(comp)} B (<- {nbytes} B), "
          f"attrs {len(comp_attr)} B (<- {len(attrs)} B)")


if __name__ == "__main__":
    main()
