# --- Toolchain ---
Z88DK ?= $(HOME)/z88dk
ZCC ?= $(Z88DK)/bin/zcc
ZCCCFG ?= $(Z88DK)/lib/config
PYTHON ?= python3
UNAME_S := $(shell uname -s)

APP = zxstrategy

ifeq ($(UNAME_S),Darwin)
FUSE ?= open -a Fuse
FUSE_RUN = defaults write FusePreferences machine -string "48" 2>/dev/null; $(FUSE) $(APP).tap
else
FUSE ?= fuse-sdl
FUSE_RUN = $(FUSE) --machine 48 $(APP).tap &
endif

# --- Config ---
.DEFAULT_GOAL := all

CONFIG_MK ?= config/basic_config.mk
include $(CONFIG_MK)

# -zorg=32768 keeps all code in NON-CONTENDED RAM, which the floating bus
# timed loops require for stable sync.
CFLAGS=+zx -vn -SO3 -zorg=32768 -startup=31 --opt-code-speed -compiler=sdcc -mz80 \
       --reserve-regs-iy --allow-unsafe-read -Cc--max-allocs-per-node=50000
USER_CFLAGS ?=
LDFLAGS=-lm -create-app

# --- Asset pipeline (ZX0) ----------------------------------------------------
# ZX0 by Einar Saukas.  Defaults to the copy z88dk ships, falling back to a
# checkout at /tmp/ZX0.  Override with `make ZX0=/path/to/zx0`.
# Runtime decompressor: src/dzx0.c → dzx0_decompress(src, dst).
ZX0 ?= $(firstword $(wildcard $(Z88DK)/bin/z88dk-zx0) /tmp/ZX0/src/zx0)
ZXP2HEADER = $(PYTHON) tools/zxp2header.py
SCR_CROP_ZX0 = $(PYTHON) tools/scr_crop_zx0.py

# assets/NAME.scr  → include/NAME.h   (full 6912-byte screen, ZX0, NAME_zx0[])
include/%.h: assets/%.scr tools/zx0_to_header.py
	rm -f /tmp/$*.zx0
	$(ZX0) $< /tmp/$*.zx0
	$(PYTHON) tools/zx0_to_header.py $@ $*_zx0:/tmp/$*.zx0

# assets/NAME.zxp  → include/NAME.h   (row-major sprite, uncompressed)
# Add --frames N / --horizontal / --downscale in a per-asset rule if needed.
include/%.h: assets/%.zxp tools/zxp2header.py
	$(ZXP2HEADER) $< $@ --name $*

# The Great Old One: full-screen .scr cropped to its bounding box, then ZX0'd.
# scr_crop_zx0.py emits the pixel data plus GOO_CROP_* placement constants, so
# only the ~24x157 byte area that actually contains art is stored (3768 bytes
# cropped -> ~2 KB compressed, vs 6144 raw).  Add --mirror to store just the
# left half of a symmetric image (see .claude/skills/compile-scr).
GOO_SRC = assets/goo.scr

include/goo_data.h: $(GOO_SRC) tools/scr_crop_zx0.py
	$(SCR_CROP_ZX0) $@ $(ZX0) goo_final:$(GOO_SRC)

# List generated headers here so `make assets` and `make clean` know them.
GENERATED_HEADERS = include/goo_data.h

assets: $(GENERATED_HEADERS) $(MUSIC_LINKABLE)

# --- Tritone / Beepola music -------------------------------------------------
# Pipeline: transcription (.txt) --txt2tritone.py--> Tritone (.asm)
#           --gen_tritone_module.py--> per-tune data module (_NAME_play).
# The ~300-line Tritone engine is factored into ONE shared module
# (tritone_engine.asm, PUBLIC TRI_PLAY); each tune module holds only its song
# data and CALLs it, so the engine is in the binary once regardless of tune
# count.  To add a tune NAME: author assets/music/NAME.txt, append
# assets/music/NAME_linkable.asm to MUSIC_LINKABLE, and call NAME_play().
# (See .claude/skills/tritone-music.)
MUSIC_ENGINE = assets/music/tritone_engine.asm
MUSIC_LINKABLE = $(MUSIC_ENGINE) \
                 assets/music/lowlands_linkable.asm

# Shared engine, extracted once from the Beepola export template.
MUSIC_TEMPLATE = assets/music/tritone_template.asm

$(MUSIC_ENGINE): $(MUSIC_TEMPLATE) tools/gen_tritone_module.py
	$(PYTHON) tools/gen_tritone_module.py $< -o $@ --engine

# transcription -> Tritone assembly
assets/music/%.asm: assets/music/%.txt $(MUSIC_TEMPLATE) tools/txt2tritone.py
	$(PYTHON) tools/txt2tritone.py $< -o $@ --template $(MUSIC_TEMPLATE)

# Tritone assembly -> per-tune data module (symbol prefix = filename stem)
assets/music/%_linkable.asm: assets/music/%.asm tools/gen_tritone_module.py
	$(PYTHON) tools/gen_tritone_module.py $< -o $@ --name $*

# keep generated .asm intermediates from being auto-deleted
.SECONDARY:

# --- Source files ---
SRCS = src/main.c src/demo.c src/gfx.c src/input.c src/hw_detect.c \
       src/vsync.c src/prng.c src/dzx0.c

HEADERS = config/app_config.h include/gfx.h include/input.h include/hw.h \
          include/vsync.h include/prng.h include/demo.h include/dzx0.h \
          include/music.h $(GENERATED_HEADERS)

# --- Top-level targets ---
all: $(APP).tap

.PHONY: all assets run map probe dzx0check clean

run: $(APP).tap
	$(FUSE_RUN)

# Build with a symbol map (needed by the ZEsarUX profiler / ZRCP debugging)
map:
	$(MAKE) clean
	$(MAKE) USER_CFLAGS="-m"

# --- Compile, link & package ---
$(APP).tap: $(SRCS) $(HEADERS) $(MUSIC_LINKABLE)
	PATH=$(Z88DK)/bin:$$PATH Z88DK=$(Z88DK) ZCCCFG=$(ZCCCFG) $(ZCC) $(CFLAGS) $(USER_CFLAGS) -o $(APP) $(SRCS) $(MUSIC_LINKABLE) $(LDFLAGS)

# --- Floating bus probe (diagnostic harness, see tests/fbprobe.c) ---
probe: tests/fbprobe.tap

tests/fbprobe.tap: tests/fbprobe.c
	PATH=$(Z88DK)/bin:$$PATH Z88DK=$(Z88DK) ZCCCFG=$(ZCCCFG) $(ZCC) $(CFLAGS) -o tests/fbprobe tests/fbprobe.c -create-app

# --- ZX0 decompression harness (see tests/dzx0check.c) ---
dzx0check: tests/dzx0check.tap

tests/dzx0check.tap: tests/dzx0check.c src/dzx0.c include/goo_data.h
	PATH=$(Z88DK)/bin:$$PATH Z88DK=$(Z88DK) ZCCCFG=$(ZCCCFG) $(ZCC) $(CFLAGS) -m -o tests/dzx0check tests/dzx0check.c src/dzx0.c -create-app

# --- Clean ---
clean:
	rm -f $(APP) $(APP).tap $(APP)_CODE.bin $(APP)_data_user.bin $(APP)_code.tap
	rm -f tests/fbprobe tests/fbprobe.tap tests/fbprobe_CODE.bin tests/fbprobe_data_user.bin tests/fbprobe_code.tap
	rm -f tests/dzx0check tests/dzx0check.tap tests/dzx0check_CODE.bin tests/dzx0check_data_user.bin tests/dzx0check_code.tap
	rm -f *.o src/*.o tests/*.o *.map
	rm -f $(GENERATED_HEADERS)
	rm -f assets/music/*_linkable.asm assets/music/*.o assets/music/lowlands.asm
