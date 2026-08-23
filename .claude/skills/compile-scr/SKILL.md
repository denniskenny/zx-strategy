---
name: compile-scr
description: Compile ZX Spectrum graphics (.scr screens, .zxp sprites) into ZX0-compressed or raw C headers for inclusion in the app, and decompress them at runtime with dzx0_decompress().
when_to_use: "compile scr" or "convert scr" or "compress screen" or "add a sprite" or "zx0" or "compress asset" or "new graphic"
allowed-tools: Bash Read Write Edit
effort: low
---

# Compile Graphics to C Headers (ZX0)

Convert screens and sprites in `assets/` into C headers in `include/`, compressed with ZX0 where it pays off, and decompress them at runtime with `dzx0_decompress()` from `src/dzx0.c`.

## Toolchain

| Piece | Location |
|-------|----------|
| ZX0 compressor | `$(ZX0)` — defaults to `$Z88DK/bin/z88dk-zx0`, falls back to `/tmp/ZX0/src/zx0` |
| Runtime decompressor | `src/dzx0.c` / `include/dzx0.h` — `void dzx0_decompress(const uint8_t *src, uint8_t *dst)` |
| `.zx0` → C header | `tools/zx0_to_header.py OUT.h name:file.zx0 [name2:file2.zx0 ...]` |
| `.scr` → raw C header | `tools/scr2header.py` |
| `.scr` → cropped + ZX0 | `tools/scr_crop_zx0.py` (crops to a bounding box before compressing) |
| `.scr` → dithered reveal frames | `tools/scr_dither_reveal.py` |
| `.zxp` (ZX-Paintbrush) → sprite header | `tools/zxp2header.py` (`--frames N --horizontal --downscale --name X`) |
| `.zxp` → screen-layout pixels + ZX0 | `tools/zxp2zx0.py` |

ZX0 refuses to overwrite an existing output file — always `rm -f` the `.zx0` first (or pass `-f`).

## CRITICAL: match the ZX0 format version

The ZX0 **stream format changed between v1 and v2**, and the decompressor must
match the compressor. z88dk ships ZX0 **v1.5** plus the matching `dzx0_standard`
in its library, which is why `src/dzx0.c` is a thin wrapper over
`<compress/zx0.h>` instead of a vendored copy of the routine.

The 68-byte "standard" ZX0 decompressor widely copied into projects is the **v2**
decoder. Feeding v1 data to it does not fail cleanly: it runs away, fills RAM
with garbage and crashes into the ROM. If you point `ZX0=` at a v2 compressor
(e.g. a GitHub checkout), you must supply a v2 decoder too.

Verify with `make dzx0check` (see below) after changing either side.

## Makefile rules

Two generic pattern rules already exist:

```make
# assets/NAME.scr → include/NAME.h   (full 6912-byte screen, ZX0, array NAME_zx0[])
include/%.h: assets/%.scr tools/zx0_to_header.py
	rm -f /tmp/$*.zx0
	$(ZX0) $< /tmp/$*.zx0
	$(PYTHON) tools/zx0_to_header.py $@ $*_zx0:/tmp/$*.zx0

# assets/NAME.zxp → include/NAME.h   (row-major sprite, uncompressed)
include/%.h: assets/%.zxp tools/zxp2header.py
	$(ZXP2HEADER) $< $@ --name $*
```

### The Great Old One (worked example)

```make
GOO_SRC = assets/goo.scr

include/goo_data.h: $(GOO_SRC) tools/scr_crop_zx0.py
	$(SCR_CROP_ZX0) $@ $(ZX0) goo_final:$(GOO_SRC)
```

`scr_crop_zx0.py` finds the art's bounding box, stores only that region
(row-major) and emits placement constants alongside the data:

```c
#define GOO_CROP_COL 3      /* byte column of the left edge  */
#define GOO_CROP_ROW 11     /* pixel row of the top edge     */
#define GOO_CROP_W   24     /* width in bytes                */
#define GOO_CROP_H   157    /* height in pixel rows          */
#define GOO_CROP_SIZE 3768
```

6144 raw → 3768 cropped → 2010 bytes ZX0. `src/game.c` decompresses it to a
low-RAM staging buffer and blits it back at its original position:

```c
dzx0_decompress(goo_final, SCRATCH_BUF);
write_blit(GOO_CROP_COL, GOO_CROP_ROW, SCRATCH_BUF, GOO_CROP_W, GOO_CROP_H);
set_attr_rect(GOO_CROP_COL, GOO_CROP_ROW >> 3, GOO_CROP_W,
              (GOO_CROP_H + 7) >> 3, GOO_ATTR);
```

Pass `--mirror` to store only the left half of a symmetric image (halves the
data); the runtime must then bit-reverse each byte to rebuild the right half,
and the header gains `GOO_MIRROR_COL`.

Note `scr_crop_zx0.py` handles **pixels only**. This .scr's attributes are a
flat 0x07, so the gallery state just paints a solid attribute rect; for coloured art,
compress the trailing 768 bytes separately with `zx0_to_header.py`.

To add an asset:

1. Drop the file in `assets/`.
2. Append the generated header to `GENERATED_HEADERS` in the Makefile so `make assets` builds it and `make clean` removes it.
3. If the defaults don't fit (multi-frame sprites, downscaled copies, cropping), write an explicit rule instead of relying on the pattern rule, e.g.:

```make
include/shark.h: assets/shark.zxp tools/zxp2header.py
	$(ZXP2HEADER) $< $@ --frames 2 --horizontal --name shark --downscale
```

Then `make assets && make`.

## Manual one-off

```bash
rm -f /tmp/vignette.zx0
$HOME/z88dk/bin/z88dk-zx0 assets/vignette.scr /tmp/vignette.zx0
python3 tools/zx0_to_header.py include/vignette.h vignette_zx0:/tmp/vignette.zx0
```

`zx0_to_header.py` accepts several `name:file` pairs and emits one header containing all of them.

## Using it in C

```c
#include "../include/dzx0.h"
#include "../include/vignette.h"

dzx0_decompress(vignette_zx0, SCREEN);   /* SCREEN = 0x4000 */
```

Notes:

- Compress the **full 6912 bytes** of a `.scr` when you want its attributes too; the decompressed block then covers 0x4000–0x5AFF.
- Decompressing a full screen takes a few thousand T-states — do it on a static screen, not inside a synced frame loop.
- **Attribute row 22 is the floating bus sync marker.** Anything decompressed over the attribute area will wipe it; `vsync_wait()` rewrites the marker each call, so at most one frame is lost. Just make sure no asset introduces the marker attribute value (0x03) elsewhere on screen — see `.claude/skills/floating-bus-vsync`.
- Headers are not tracked as dependencies of individual objects; after regenerating one, `touch` a `.c` that includes it (or `make clean`) to force a rebuild.

## Regression harness

`tests/dzx0check.c` (`make dzx0check`) decompresses the goo blob into the same
staging buffer the gallery state uses and writes a result block at 0xF000:

| Address | Meaning |
|---------|---------|
| 0xF000 | 0x5A once the run completed (anything else = crash) |
| 0xF001-2 | number of bytes written (LE) |
| 0xF003+ | first 16 decompressed bytes |

Read it back with `read-memory 61440 32` and compare against the host reference:

```bash
z88dk-dzx0 /tmp/goo_final.zx0 /tmp/goo_final.bin   # host-side ground truth
```
