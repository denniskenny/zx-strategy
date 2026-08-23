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
| `.zxp` strip → ZX0 blob + per-cell attributes | `tools/zxp_tiles_zx0.py` (`--tiles N`) — terrain tiles and unit sprites, see `.claude/skills/zx-tiles` |

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

### Cropped full-screen art

**Nothing in the game uses this today** — the art is all `.zxp` tile and sprite
strips — so there is no rule for it in the Makefile. This is how to add one.

```make
include/splash_data.h: assets/splash.scr tools/scr_crop_zx0.py
	$(SCR_CROP_ZX0) $@ $(ZX0) splash_final:assets/splash.scr
```

`scr_crop_zx0.py` finds the art's bounding box, stores only that region
(row-major) and emits placement constants alongside the data. The prefix comes
from the output header's name (`splash_data.h` → `SPLASH_`), or from `--name`:

```c
#define SPLASH_CROP_COL  3     /* byte column of the left edge  */
#define SPLASH_CROP_ROW  11    /* pixel row of the top edge     */
#define SPLASH_CROP_W    24    /* width in bytes                */
#define SPLASH_CROP_H    157   /* height in pixel rows          */
#define SPLASH_CROP_SIZE 3768
```

A 24x157 region is 3768 bytes cropped against 6144 raw, and ZX0 took that to
about 2 KB in practice. Decompress to a low-RAM staging buffer and blit it back
at its original position:

```c
dzx0_decompress(splash_final, SCRATCH_BUF);
write_blit(SPLASH_CROP_COL, SPLASH_CROP_ROW, SCRATCH_BUF,
           SPLASH_CROP_W, SPLASH_CROP_H);
set_attr_rect(SPLASH_CROP_COL, SPLASH_CROP_ROW >> 3, SPLASH_CROP_W,
              (SPLASH_CROP_H + 7) >> 3, attr);
```

Pass `--mirror` to store only the left half of a symmetric image (halves the
data); the runtime must then bit-reverse each byte to rebuild the right half,
and the header gains `SPLASH_MIRROR_COL`.

Note `scr_crop_zx0.py` handles **pixels only**. If the source has flat
attributes, paint a solid attribute rect as above; for coloured art, compress
the trailing 768 bytes separately with `zx0_to_header.py`.

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

`tests/dzx0check.c` (`make dzx0check`) decompresses the `units_view` sprite
sheet — a blob the game itself unpacks at startup — into a low-RAM staging
buffer and writes a result block at 0xF000:

| Address | Meaning |
|---------|---------|
| 0xF000 | 0x5A once the run completed (anything else = crash) |
| 0xF001-2 | 16-bit sum of the decompressed block (LE) |
| 0xF003+ | first 16 decompressed bytes |

A sum, not a count of bytes that changed from the 0xAA fill: counting
under-reports every byte the data itself sets to 0xAA, while a sum catches both
a short write and corruption in the middle.

Read it back with `read-memory 61440 32` and compare against the host reference:

```bash
# host-side ground truth; zxp_tiles_zx0.py leaves the blob in /tmp
z88dk-dzx0 /tmp/units_view_tiles.zx0 /tmp/units_view_tiles.bin
```

The byte count should equal `UNITS_VIEW_RAW_SIZE` and the first 16 bytes should
match the start of that file.
