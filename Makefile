# --- Toolchain ---
Z88DK ?= $(HOME)/z88dk
ZCC ?= $(Z88DK)/bin/zcc
ZCCCFG ?= $(Z88DK)/lib/config
PYTHON ?= python3
UNAME_S := $(shell uname -s)

# --- Build target -----------------------------------------------------
# ONE tap for every machine.
#
#   make            build it
#   make run        run it in Fuse
#   make map        build with a symbol map, then check the 0xC000 ceiling
#   make memmap     the full memory picture, linker-placed and hand-placed
#
# All code lives below 0xC000 so a 128K or +3 can keep page 7 banked in
# there as a shadow screen.  A 48K takes the shadow_ok=0 path — written
# for 128Ks whose paging is locked — and draws in place.
#
# There was a second target once, giving the 48K code up to 0xFFFF.  It
# bought 32 KB the program did not use (167 bytes over the ceiling, 16 KB
# clear) and cost the thing that matters: the test suite drove one build
# while a 128K owner was handed the other.  Do not reintroduce it.
#
# So the 16 KB ceiling at 0x8000-0xBFFF is universal and checkmem
# enforces it.  Above 0xC000 goes data, or a bank — never code.
# The BASIC loader must CLEAR below the lowest thing it loads, or BASIC's
# own workspace sits on top of the asset block at 0x6000.
CLEAR_ADDR = 24575
APP        = zxstrategy
ORG_DEF    = -zorg=32768
USR_ADDR   = 32768

DEBUG_KEYS ?= 0
TARGET_DEF = -DDEBUG_STATE_WALK=$(DEBUG_KEYS)

# The state-walk debug keys cost 99 bytes and the shipping tap has 5
# spare, so they are off by default and tests/p0_state_walk.py asks for
# them explicitly:
#     make DEBUG_KEYS=1 map
#
# That tap is 94 bytes over the ceiling and is therefore **48K only** —
# on a 128K page 7 would be banked in over its tail.  The ceiling is
# relaxed here rather than silently broken, and p0_state_walk.py is a
# 48K test.  Anything that has to run on a 128K must be built without
# DEBUG_KEYS; render_paths.py drives both machines and needs no keys.
ifeq ($(DEBUG_KEYS),0)
MEM_LIMIT  = 0xC000
else
MEM_LIMIT  = 0x10000
endif

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

# -zorg=32768 keeps code out of contended RAM.  0x6000 was tried: it buys
# 8 KB of code space and costs about 50% speed (p0_state_walk 20s -> 30s),
# because half the program then sits in 0x6000-0x7FFF where the ULA steals
# a cycle from every fetch.  The floating bus timed loops survive it fine
# -- that part of the old comment here was wrong -- but the speed does not.
# See docs/PLAN.md P9.  Compressed assets go down there instead: read once
# at boot, so contention costs them nothing.
CFLAGS=+zx -vn -SO3 $(ORG_DEF) -startup=31 --opt-code-speed -compiler=sdcc -mz80 -pragma-define:CRT_ENABLE_STDIO=0 $(TARGET_DEF) \
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
# (the unit table in docs/DESIGN.md).  Built with --attr-mode bright: a unit
# is cyan or red according to whose it is, so its ink is not the artist's to
# choose, but which of its cells are BRIGHT is — that is the sprite's shading,
# and it is all that survives from the sheet.
UNIT_COUNT = 4
# The view sheet carries the units PLUS an explosion effect, and now comes
# as a 2-frame grid: one column per frame, one row per sprite.  The map
# sheet is still four units in a strip -- an explosion is a moment, and the
# map view is a schematic.
VIEW_SPRITES = 5

include/units_map.h: assets/units_map.zxp tools/zxp_tiles_zx0.py
	$(ZXP_TILES_ZX0) $< $@ --name units_map --tiles $(UNIT_COUNT) \
	    --attr-mode bright --zx0 $(ZX0)

# --mask: unit sprites are drawn OVER terrain, so they need one.  The
# terrain sheets do not -- they ARE the background.
#
# The mask is a separate ZX0 blob and crushes far harder than the pixels
# do: 512 bytes of mostly solid runs go to 172, a 67%% saving, where the
# sprites themselves only manage 576 -> 301.  Appending it to the pixel
# stream would have buried those runs in sprite detail and lost most of
# that.  mkassets.py picks it up from the header automatically, so it
# lands in the contended block at 0x6000 with the other blobs.
include/units_view.h: assets/units_view_animated.zxp tools/zxp_tiles_zx0.py
	$(ZXP_TILES_ZX0) $< $@ --name units_view --tiles $(VIEW_SPRITES) \
	    --frames 2 --mask --attr-mode bright --zx0 $(ZX0)

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
SRCS = src/main.c src/game.c src/logic.c src/render.c src/gfx.c src/input.c src/hw_detect.c \
       src/vsync.c src/prng.c src/dzx0.c src/no_font64.asm src/assets_low_syms.asm src/logic_org.asm

HEADERS = config/app_config.h config/game_config.h include/gfx.h include/input.h include/hw.h \
          include/vsync.h include/prng.h include/game.h include/board.h \
          include/render.h include/dzx0.h \
          include/music.h $(GENERATED_HEADERS)

# --- Top-level targets ---
all: $(APP).tap

.PHONY: all assets run map probe dzx0check clean

run: $(APP).tap
	$(FUSE_RUN)

# Build with a symbol map (needed by the ZEsarUX profiler and by the
# tests, which read addresses from it), then check the binary still
# clears 0xC000 — a 128K banks page 7 in there and anything of ours
# above it would vanish.  See tools/checkmem.py.
map:
	$(MAKE) clean
	$(MAKE) USER_CFLAGS="-m" DEBUG_KEYS=$(DEBUG_KEYS)
	$(PYTHON) tools/checkmem.py $(APP).map --limit $(MEM_LIMIT)

.PHONY: checkmem memmap
checkmem: map

# The whole memory picture: what the linker placed, what memmap.h placed
# by hand, and how much room is left in each.  Neither half is visible
# from the other, which is how the layout has gone wrong before.
memmap: map
	@$(PYTHON) tools/checkmem.py $(APP).map --layout --limit $(MEM_LIMIT)

# --- Compile, link & package ---
#
# TWO CODE blocks, built by tools/mktap.py rather than -create-app.
#
# -create-app emits one contiguous block from CRT_ORG_CODE and a loader
# that does a single LOAD ""CODE.  Assets placed outside that range are
# dropped silently (a section with `org`) or shipped headerless and never
# loaded (a bank section) -- both read as zeros at runtime and look like a
# decompressor bug.  So the tap is assembled explicitly.
#
# src/assets_low.asm is assembled STANDALONE and never linked into the C
# program, so its bytes do not count against the 16 KB ceiling.  It gets a
# real CODE header, which is what lets a 48K -- no paging at all -- load it
# exactly like a 128K does.
ASSETS_LOW_BIN = $(APP)_assets_low.bin

# tools/mkassets.py writes BOTH halves from the generated headers: the
# bytes (assembled standalone, never linked) and the `defc` symbols that
# resolve them (linked, zero bytes).  Regenerate whenever an asset does.
src/assets_low.asm src/assets_low_syms.asm src/logic_org.asm logic_org.addr: $(GENERATED_HEADERS) tools/mkassets.py
	$(PYTHON) tools/mkassets.py

$(ASSETS_LOW_BIN): src/assets_low.asm
	PATH=$(Z88DK)/bin:$$PATH $(Z88DK)/bin/z88dk-z80asm -b -O. -o$@ src/assets_low.asm

$(APP).tap: $(SRCS) $(HEADERS) $(MUSIC_LINKABLE) $(ASSETS_LOW_BIN) src/assets_low_syms.asm src/logic_org.asm logic_org.addr tools/mktap.py
	PATH=$(Z88DK)/bin:$$PATH Z88DK=$(Z88DK) ZCCCFG=$(ZCCCFG) $(ZCC) $(CFLAGS) $(USER_CFLAGS) -o $(APP) $(SRCS) $(MUSIC_LINKABLE) $(LDFLAGS)
	$(PYTHON) tools/mktap.py $(APP).tap --name $(APP) --clear $(CLEAR_ADDR) --usr $(USR_ADDR) \
	    --code 0x6000 $(ASSETS_LOW_BIN) \
	    --code $$(cat logic_org.addr) $(APP)_LOGIC.bin \
	    --code $(USR_ADDR) $(APP)

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
	rm -f zxstrategy zxstrategy.tap zxstrategy_CODE.bin zxstrategy_data_user.bin zxstrategy_code.tap
	rm -f zxstrategy.map *_assets_low.bin *_LOGIC.bin src/assets_low.asm src/assets_low_syms.asm src/logic_org.asm logic_org.addr
	rm -f tests/fbprobe tests/fbprobe.tap tests/fbprobe_CODE.bin tests/fbprobe_data_user.bin tests/fbprobe_code.tap
	rm -f tests/dzx0check tests/dzx0check.tap tests/dzx0check_CODE.bin tests/dzx0check_data_user.bin tests/dzx0check_code.tap
	rm -f *.o src/*.o tests/*.o *.map
	rm -f $(GENERATED_HEADERS)
	rm -f assets/music/*_linkable.asm assets/music/*.o assets/music/lowlands.asm
