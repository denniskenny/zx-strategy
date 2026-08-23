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
ZXP_TILES_ZX0 = $(PYTHON) tools/zxp_tiles_zx0.py
# tools/scr_crop_zx0.py crops a .scr to its bounding box before compressing.
# Nothing in the game is a full-screen graphic today, so no rule uses it; see
# .claude/skills/compile-scr for the invocation if one is added.
SCR_CROP_ZX0 = $(PYTHON) tools/scr_crop_zx0.py

# assets/NAME.scr  → include/NAME.h   (full 6912-byte screen, ZX0, NAME_zx0[])
include/%.h: assets/%.scr tools/zx0_to_header.py
	rm -f /tmp/$*.zx0
	$(ZX0) $< /tmp/$*.zx0
	$(PYTHON) tools/zx0_to_header.py $@ $*_zx0:/tmp/$*.zx0

# --- Campaign levels ---------------------------------------------------------
# One .tmx per level, all the same size and all sharing level_1's tileset.
# The GIDs are ZX0'd (98 raw bytes each -> ~35), and load_map() decompresses
# the level it needs straight into terrain[] before converting GIDs in place.
# level_1 carries the terrain name/passability tables; the rest are built with
# --shared-terrain and borrow them, which is why every level's tileset has to
# match — src/game.c compares LEVEL_*_TERRAIN_SIG and fails the build if one
# has drifted.  To add level 11: author the .tmx, append 11 to LEVELS, and add
# it to the tables in src/game.c.  See .claude/skills/tiled-maps.
LEVELS = 1 2 3 4 5 6 7 8 9 10
LEVEL_HEADERS = $(foreach n,$(LEVELS),include/level_$(n).h)

include/level_1.h: assets/maps/level_1.tmx tools/tmx2header.py
	$(PYTHON) tools/tmx2header.py $< $@ --name level_1 --zx0 $(ZX0)

include/level_%.h: assets/maps/level_%.tmx tools/tmx2header.py
	$(PYTHON) tools/tmx2header.py $< $@ --name level_$* --zx0 $(ZX0) \
	    --shared-terrain

# assets/maps/NAME.tmx → include/NAME.h  (Tiled map: raw GIDs + constants)
# The runtime converts the GIDs into its own terrain ids in memory, so the
# header stays a faithful dump of what Tiled saved.
include/%.h: assets/maps/%.tmx tools/tmx2header.py
	$(PYTHON) tools/tmx2header.py $< $@ --name $*

# assets/NAME.zxp  → include/NAME.h   (row-major sprite, uncompressed)
# Add --frames N / --horizontal / --downscale in a per-asset rule if needed.
include/%.h: assets/%.zxp tools/zxp2header.py
	$(ZXP2HEADER) $< $@ --name $*

# Terrain tile sheets: N tiles side by side in one .zxp, ZX0-compressed into
# one blob per sheet plus a per-tile attribute table (colours are authored in
# ZX-Paintbrush).  The runtime decompresses each blob once and blits tiles out
# of it; tile column order must match the .tmx tileset (see
# .claude/skills/zx-tiles).
TILE_COUNT = 5

include/tiles_map.h: assets/tiles_map.zxp tools/zxp_tiles_zx0.py
	$(ZXP_TILES_ZX0) $< $@ --name tiles_map --tiles $(TILE_COUNT) --zx0 $(ZX0)

include/tiles_view.h: assets/tiles_view.zxp tools/zxp_tiles_zx0.py
	$(ZXP_TILES_ZX0) $< $@ --name tiles_view --tiles $(TILE_COUNT) --zx0 $(ZX0)

# Unit sprite sheets: same strip format and converter as the terrain tiles,
# one sheet per renderer, sprite column order = INFANTRY, TANK, CANNON, BASE
# (the unit table in docs/DESIGN.md).  Unlike the terrain sheets these are
# built with --attr: a unit is drawn cyan or red by side, so the runtime picks
# the colour and whatever ZX-Paintbrush saved with the art is ignored rather
# than being a build error every time a sprite is redrawn.
UNIT_COUNT = 4
UNIT_ATTR = 47

include/units_map.h: assets/units_map.zxp tools/zxp_tiles_zx0.py
	$(ZXP_TILES_ZX0) $< $@ --name units_map --tiles $(UNIT_COUNT) \
	    --attr $(UNIT_ATTR) --zx0 $(ZX0)

include/units_view.h: assets/units_view.zxp tools/zxp_tiles_zx0.py
	$(ZXP_TILES_ZX0) $< $@ --name units_view --tiles $(UNIT_COUNT) \
	    --attr $(UNIT_ATTR) --zx0 $(ZX0)

# List generated headers here so `make assets` and `make clean` know them.
GENERATED_HEADERS = $(LEVEL_HEADERS) \
                    include/tiles_map.h include/tiles_view.h \
                    include/units_map.h include/units_view.h

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
SRCS = src/main.c src/game.c src/gfx.c src/input.c src/hw_detect.c \
       src/vsync.c src/prng.c src/dzx0.c

HEADERS = config/app_config.h config/game_config.h include/gfx.h include/input.h include/hw.h \
          include/vsync.h include/prng.h include/game.h include/dzx0.h \
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

tests/dzx0check.tap: tests/dzx0check.c src/dzx0.c include/units_view.h
	PATH=$(Z88DK)/bin:$$PATH Z88DK=$(Z88DK) ZCCCFG=$(ZCCCFG) $(ZCC) $(CFLAGS) -m -o tests/dzx0check tests/dzx0check.c src/dzx0.c -create-app

# --- Clean ---
clean:
	rm -f $(APP) $(APP).tap $(APP)_CODE.bin $(APP)_data_user.bin $(APP)_code.tap
	rm -f tests/fbprobe tests/fbprobe.tap tests/fbprobe_CODE.bin tests/fbprobe_data_user.bin tests/fbprobe_code.tap
	rm -f tests/dzx0check tests/dzx0check.tap tests/dzx0check_CODE.bin tests/dzx0check_data_user.bin tests/dzx0check_code.tap
	rm -f *.o src/*.o tests/*.o *.map
	rm -f $(GENERATED_HEADERS)
	rm -f assets/music/*_linkable.asm assets/music/*.o assets/music/lowlands.asm
