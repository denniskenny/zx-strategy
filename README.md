# ZX Map

A ZX Spectrum 48K/128K application written in C (z88dk + SDCC), built around a
**floating bus vsync** so screen updates happen while the beam is off the
display.

It doubles as a **project template**: floating bus vsync, hardware detection,
graphics/input/PRNG helpers, a ZX0 asset pipeline with a runtime decompressor,
the Tritone (Beepola) beeper music pipeline, and the Claude skills that document
all of it.

The current program is a demo/scaffold: a status panel reporting the detected
machine and vsync mode, a fast moving bar that visibly tears when the sync is
switched off, and a Tritone tune on the M key.

## Requirements

- [z88dk](https://z88dk.org) (`Z88DK` defaults to `$HOME/z88dk`)
- Fuse (`make run`) — reference emulator
- Optional: ZEsarUX + Python 3 for headless inspection (see `.claude/skills/zesarux-test`)

## Build & run

```bash
make            # → zxmap.tap (builds assets + music as needed)
make assets     # generated headers + music modules only
make run        # build + launch Fuse (48K)
make map        # rebuild with zxmap.map symbol map (for ZRCP debugging)
make probe      # build tests/fbprobe.tap, the floating bus histogram probe
make dzx0check  # build tests/dzx0check.tap, the ZX0 decompression harness
make clean
```

Override the toolchain or add compiler flags:

```bash
make Z88DK=/path/to/z88dk
make USER_CFLAGS="-m"
```

## Controls

| Key | Kempston | Action |
|-----|----------|--------|
| O / P | left / right | Bar speed (1–8) |
| S | fire 1 | Toggle floating bus sync on/off |
| R | fire 2 | Reset the frame counter |
| G | — | Show the ZX0-compressed Great Old One (any key returns) |
| M | — | Play the Tritone tune (blocks; any key returns) |

Z and X also work as fire 1 / fire 2 (that's what `scan_input()` reads), but the
demo's own bindings are S/R/M so nothing sits on the CAPS SHIFT row.

With sync **on** the bar is redrawn entirely during the border/vblank window and
stays solid; with sync **off** the redraw races the beam and the bar tears. The
border turns **red** while the frame's work runs and black while waiting, so the
red band shows how much of the frame budget is actually used.

## Layout

```
Makefile                 build + asset/music pipelines (-zorg=32768)
config/app_config.h      screen/keyboard constants + floating bus marker config
config/basic_config.mk   build config included by the Makefile
include/                 public headers
src/main.c               startup order: hw_detect → vsync_detect → paging lock
src/vsync.c              floating bus vsync (all assembly, __naked)
src/hw_detect.c          128K + Kempston detection
src/gfx.c                screen address maths, blits, XOR sprites, ROM-font text
src/input.c              keyboard half-rows + Kempston
src/prng.c               16-bit LFSR/Weyl PRNG
src/dzx0.c               ZX0 decompression (wraps z88dk's dzx0_standard)
src/demo.c               the demo loop
assets/goo.scr           the Great Old One (example graphic)
assets/music/            Tritone template/engine + one example tune
tools/                   asset + music converters, ZRCP profiler
tests/fbprobe.c          floating bus diagnostic histogram
tests/dzx0check.c        ZX0 decompression regression harness
.claude/skills/          floating-bus-vsync, compile-scr, tritone-music,
                         zesarux-test
```

## Assets (ZX0)

Drop a file in `assets/`, add the generated header to `GENERATED_HEADERS`, and
the Makefile pattern rules do the rest:

- `assets/NAME.scr` → `include/NAME.h` — whole screen, ZX0-compressed as
  `NAME_zx0[]`
- `assets/NAME.zxp` → `include/NAME.h` — ZX-Paintbrush sprite, row-major

At runtime: `dzx0_decompress(NAME_zx0, SCREEN);`. The compressor is
`$Z88DK/bin/z88dk-zx0` by default (`make ZX0=/path/to/zx0` to override).
Converters live in `tools/` (`zx0_to_header.py`, `zxp2header.py`, `zxp2zx0.py`,
`scr2header.py`, `scr_crop_zx0.py`, `scr_dither_reveal.py`).

The worked example is **the Great Old One**, `assets/goo.scr`:
`tools/scr_crop_zx0.py` crops it to its bounding box and ZX0-compresses it
(6144 raw → 3768 cropped → 2010 bytes), emitting `include/goo_data.h` with the
data plus `GOO_CROP_*` placement constants. Press **G** in the demo: it is
decompressed into a low-RAM staging buffer and blitted back at its original
screen position.

**ZX0 v1 vs v2**: the stream format differs and the decompressor must match the
compressor. z88dk ships ZX0 v1.5 and the matching `dzx0_standard`, so
`src/dzx0.c` wraps the library routine rather than vendoring the widely copied
68-byte v2 decoder — which silently corrupts RAM when fed v1 data. `make
dzx0check` verifies the pair. See `.claude/skills/compile-scr`.

## Music (Tritone / Beepola)

```
assets/music/NAME.txt  →  NAME.asm  →  NAME_linkable.asm  →  NAME_play()
         txt2tritone.py      gen_tritone_module.py --name NAME
```

The ~300-line Tritone player is factored into one shared module,
`assets/music/tritone_engine.asm`, extracted with `--engine` from
`assets/music/tritone_template.asm` — a Beepola "Tritone v2" export with its
song data stripped, which also supplies the engine/drums that `txt2tritone.py`
splices each new tune onto. Swap that file for another Beepola export to change
the engine, drum kit or volume mode for every tune at once.

Per-tune modules hold song data only. Declare tunes in `include/music.h`, add
the module to `MUSIC_LINKABLE`, and call `NAME_play()` from a **static screen**
— it blocks until a key is pressed. `assets/music/lowlands.txt` is the single
example melody. See `.claude/skills/tritone-music`.

## Floating bus notes

`vsync_wait()` supports three modes, chosen once at boot by `vsync_detect()`:

| Mode | Machine | Port | Loop |
|------|---------|------|------|
| 1 | 48K / 128K / +2 | 0x40FF | 35 T-states |
| 2 | +2A / +3 | 0x0FFD | 42 T-states |
| 0 | anything else | — | `ei / halt / di` fallback |

A unique attribute marker (`0x03`, black paper on magenta ink) is written across
attribute row 22 every frame; when the timed loop reads it back off the bus the
beam has just reached that row, leaving ~28 000 T-states of border and vblank
for tear-free drawing.

Constraints worth remembering when extending the app:

- Keep code in non-contended RAM (`-zorg=32768`) or the timed loop jitters.
- Never use attribute `0x03` (or any value that becomes `0x03` when ORed with 1)
  anywhere on screen.
- Keep attribute row 22 pixel-blank, and keep row 23 col 0 (the +2A/+3 preload
  byte) different from the marker.
- Run `hw_detect()` and `vsync_detect()` before locking 128K paging.

Full details, including the emulator support matrix, are in
`.claude/skills/floating-bus-vsync/SKILL.md`.

Reference: Ast A. Moore, *The Definitive Programmer's Guide to Using the
Floating Bus Trick on the ZX Spectrum*.
