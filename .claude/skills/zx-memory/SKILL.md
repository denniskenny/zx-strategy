---
name: zx-memory
description: Place code, graphics and buffers correctly on the ZX Spectrum — the 48K/128K/+3 memory map, contended vs uncontended RAM, bank switching and the ROM-select trap, and the linking rules that stop generated asset headers being duplicated into every translation unit.
when_to_use: "out of memory" or "checkmem failed" or "where should this buffer go" or "add a graphic" or "new asset" or "banking" or "paging" or "contended memory" or "0x7FFD" or "duplicate symbol" or "undefined symbol" or "it works on 48K but not 128K" or "crashes on the +3"
allowed-tools: Bash Read Write Edit
effort: medium
---

# ZX Memory: where things go, and what breaks if they go elsewhere

Run **`make memmap`** first. It prints both halves of the layout together —
the linker's and the hand-placed one — and neither is visible from the other:

```
  linker-placed (code, rodata, data, bss)
    8000 .. BB64   15204 bytes   top symbol _ppx_src
    BB64 .. C000    1180 bytes   FREE before the 0xC000 limit

  hand-placed (include/memmap.h)
    C000 .. D000    4096 bytes   MEM_VBUF
    ...
    DC48 .. FFFF    9144 bytes   FREE to the top of RAM
```

`make map` runs `tools/checkmem.py`, which **fails the build** if the
linker-placed part reaches 0xC000. That is not a style rule; see § Banking.

## The map, and who owns each part

| Range | What | Ours? |
|-------|------|-------|
| `0000-3FFF` | ROM | No — and *which* ROM is switchable, see § The ROM trap |
| `4000-57FF` | Screen pixels | Yes, via `gfx.c` |
| `5800-5AFF` | Screen attributes | Yes |
| `5B00-5BFF` | Printer buffer | Scratch, usable |
| `5C00-5CBF` | BASIC system variables | **No** |
| `5CC0-RAMTOP` | BASIC program, variables, calculator stack | **No** |
| ~`7FA0` | The machine stack, growing down from RAMTOP | **No** |
| `8000-BFFF` | Our code, rodata, data, bss (`-zorg=32768`) | Yes |
| `C000-FFFF` | Our hand-placed buffers — **and a paged bank on 128K** | Carefully |

**`0x6000-0x7FFF` looks free and is not.** The tap's loader does
`CLEAR 32767`, so RAMTOP is `0x7FFF` and everything from `~0x5CC0` up to the
stack belongs to BASIC. Putting 7 KB of buffers there survived on a 48K and a
128K by luck and was a prime suspect in a +3 crash. If you want that region,
lower the loader's `CLEAR` so BASIC never claims it — do not just move in.

## Contended vs uncontended

The ULA steals cycles from `4000-7FFF` while it is drawing the display.
Everywhere else runs full speed.

- **Code must live at `0x8000+`.** `-zorg=32768` is chosen for this, and the
  Makefile says so: the floating bus sync loops in `src/vsync.c` are timed and
  drift if the code fetching them is contended.
- **Data can be contended if it is only touched in the vblank window**, when
  the ULA is not drawing and contention does not apply. That is the argument
  that made `0x6000` look attractive — it was right about contention and wrong
  about ownership.
- On a 128K, contention follows the *bank*, not the address: banks 1, 3, 5, 7
  are contended wherever they are paged. Bank 0 at `0xC000` is not.

## Banking

`0xC000-0xFFFF` is a window onto one of eight RAM banks on a 128K-class
machine, selected by bits 0-2 of port **`0x7FFD`**. Whatever the linker puts
there vanishes when something pages, and the symptom is silent corruption
rather than a crash — which is why `checkmem` refuses to let it happen.

Our buffers live up there deliberately and are safe **only because bank 0 is
selected and then left alone**: `hw_detect()` ends by selecting it, and
`main()` locks paging (bit 5) on machines that do not need the +2A/+3 floating
bus. If anything ever pages again, those buffers move first.

### The ROM trap — read this before writing 0x7FFD

Port `0x7FFD` is write-only and does four things at once:

| Bits | Meaning |
|------|---------|
| 0-2 | RAM bank at `0xC000` |
| 3 | Display file: 0 = page 5, 1 = page 7 (the shadow screen) |
| **4** | **ROM select** |
| 5 | **Lock** — once set, every later write is ignored, silently |

Mirror whatever you write into BANKM at `0x5B5C`; see below.

**Update BANKM every time you write this port.** `0x7FFD` is write-only, so
the ROM keeps its own copy of the last value at the system variable **BANKM
(`0x5B5C`)** and writes that copy back whenever it touches paging — the
interrupt handler included. Leave it stale and the ROM undoes you, typically
within a frame:

```asm
    ld  bc, #0x7FFD
    ld  a, (_page_reg)
    out (c), a
    ld  (0x5B5C), a     ; BANKM — not optional
```

Skipping it made the shadow screen appear never to display on a +2A/+3: bit 3
was set on the port, the ROM restored its own value before the ULA read the
new screen, and every state composed into page 7 came up blank while the ones
composed into page 5 looked fine. **A 128K tolerated it**, so the test suite
was green throughout.

**Preserve bit 4 unless you mean to change the ROM.** On a 128K it picks the
128 editor (0) or 48K BASIC (1). On a +2A/+3 the ROM number is *two* bits —
`0x1FFD` bit 2 above `0x7FFD` bit 4 — and a 48K-format tap loads from 48
BASIC, which is **ROM 3**. Clearing bit 4 drops it to **ROM 2: +3DOS**.

Two things then break, and they look unrelated:

- `print_at()` reads the character set from `0x3D00`, which only 48K BASIC
  has. Text renders as noise, correctly positioned and coloured.
- IM 1 interrupts vector to `0x0038` in whatever ROM is paged. In +3DOS that
  is not a BASIC interrupt handler, and the machine ends up back in BASIC with
  **"Nonsense in BASIC"**.

So: **`di` around any paging sequence**, and keep bit 4 set. `hw_detect()` did
neither and crashed every +3 while passing every test on 48K and 128K. Its
`0x11 / 0x12 / 0x10` constants are that shape on purpose. Fixing it also made
128K floating-bus detection start working, because interrupts had been landing
inside `vsync_detect()`'s timed probe.

Port `0x1FFD` is decoded on a +2A/+3 only, and **partially decodes onto
`0x7FFD` on a plain 128K** — writing it there repages RAM. Guard any `0x1FFD`
write behind a genuine +2A/+3 test.

## The +2A/+3, specifically

Three faults this project hit are +3-only, and every one passed a full 48K and
128K test run first. If you change anything about paging, the ROM, or where
buffers live, **that machine is the one that decides**.

- **Do not keep anything above `0xC000`.** It is a paged bank, and on a +3 the
  ROM pages it for the RAM disk and +3DOS workspace whenever it likes.
  Buffers there are not corrupted at once — they rot between writes, which
  looks like a rendering bug, not a memory bug. The buffers moved to `0x6000`
  for exactly this reason, and that freed page 7 for the shadow screen as a
  bonus.
- **Page 7 is not spare RAM.** It is 16 KB and the shadow screen only uses
  6 912 bytes of it. Putting buffers in the remainder is arithmetically sound,
  works on a 128K, and gives a +3 part-garbage tiles and no title screen.
- **The ROM number is two bits** — `0x1FFD` bit 2 above `0x7FFD` bit 4 — so a
  write that is ROM-neutral on a 128K may not be on a +3. A 48K-format tap
  loads from 48 BASIC, ROM 3; clearing bit 4 lands on ROM 2, +3DOS.
- **A +3 is not reliably detectable.** `vsync_mode == VSYNC_MODE_128K` finds
  one only when the mode-2 floating bus was detected; a +3 that falls back to
  HALT is indistinguishable from a 128K by anything this program knows. Port
  `0x1FFD` is decoded there and not on a 128K, so probing it is the obvious
  route if a real test is ever needed.

**A snapshot will not test any of this.** `.sna`/`.z80` restore a machine
mid-flight and skip the boot path entirely — the ROM state the loader leaves,
`hw_detect()`, the first paging write. All three faults above lived there.
Testing a +3 means driving its boot menu and loading the tape for real.

## Reclaiming space you did not know you were spending

**Ask who *references* the thing, not which compiler switch removes it.**
z88dk's zx console keeps a pointer to a 4x8 font, and `console_vars.asm` does

```asm
    EXTERN  CRT_FONT_64
```

purely to store that address in a two-byte variable. The EXTERN alone makes
the linker load **768 bytes** of glyph data into a program that never prints
through the console. No `-startup=` value and no `CRT_ENABLE_STDIO=0` removes
it; hours went into looking for a switch that does not exist.

The fix is to satisfy the reference yourself. `src/no_font64.asm`:

```asm
    MODULE  no_font64
    PUBLIC  CRT_FONT_64
    defc    CRT_FONT_64 = 0x3D00    ; the ROM's own character set
```

Nothing is unresolved, the module is never loaded, and the pointer still aims
somewhere valid. **768 bytes, zero cost.** The same trick applies to anything
the crt drags in by name.

Other things measured on this project, so nobody re-derives them:

| | bytes |
|---|---|
| ZX0 decompressor (one copy — there is no duplication) | **75** |
| Every string literal in the program, padding included | **~716** |
| `DEBUG_STATE_WALK` debug keys | **99** |
| The unused 4x8 font | **768** |

**Pad at the point of use, not in the literal.** Every hint message here was
written out to 31 characters by hand so the next would erase the last. One
`hint_row()` that pads to width recovered ~112 bytes across nine strings.

### Measuring: the trap

Sizes derived from a link map are *gap to the next symbol*, which **wildly
overstates the last symbol before any unmapped library code**. On this project
that made the ZX0 decompressor look like 1 414 bytes (it is 75) and the string
pool look like 2 324 (it is 716). Both were reported as findings before being
checked. **Per-module and per-section totals are trustworthy; individual symbol
sizes are an upper bound.** `__*_size` symbols in the map hold real section
sizes as their *value* — use those.

## zcc will not evaluate `#if` inside a function containing `__asm`

Preprocessor directives in the body of a function that has an inline asm
block are **passed through to the back end verbatim, unevaluated**. This is
not a warning; it is silent.

```c
int main(void) {
    __asm
    ld bc, #0x7FFD
    ...
    __endasm;

#if !BUILD_SHADOW          /* NOT evaluated.  Reaches the back end as text. */
    probe();               /* stays live in every build */
#endif
}
```

That cost a session. A function was excluded from `SRCS` for one target and
its only call site gated with `#if` in `main()` — which has an asm block — so
the call survived and the link failed with `undefined symbol` from source that
plainly excluded it. Every re-read of the source confirmed it was correct.
`zcc -E` output is what settles it: the `#if` is still there in the `.i` file.

The same mechanism explains **`syntax error: token -> '/'`** on an ordinary
comment following `__endasm;`, and a spurious `syntax error: token ->
'__endasm'` when a second asm block appears later in the same function.

**The breakage is per-function, not per-file.** `#if` works normally in an
asm-free function in a file whose other functions use asm — which is the
escape route: put the conditional in an asm-free function, or select the
implementation in the **Makefile** via a source-list variable and leave the
call site unconditional.

**Also: an empty function is not free.** Where headroom is measured in bytes,
an empty gated stub still costs its `call` and `ret`. Putting the gated call
inside a branch that is *already* compiled out for that target costs zero,
which is the only version that fits a build with five bytes spare.

## The address-space ceiling, and how to get it back

On a 128K-class machine the shadow screen lives in page 7, which can only be
addressed at `0xC000`. Bank it in at startup and leave it there — the obvious
implementation — and **all code must fit below 0xC000: a hard 16 KB ceiling**,
regardless of how much RAM the machine has. That is an *address space* limit,
not a memory one, and it is the single biggest constraint on this program.

**Page 7 only has to be mapped while you copy into it.** With interrupts off,
page it in, copy from a buffer that lives below 0xC000, page bank 0 back. Code
above 0xC000 is invisible for the duration and intact either side, so it can
run to 0xFFFF *and* the machine keeps its shadow screen. Requirements: the copy
routine and its source below 0xC000, the stack below 0xC000, `di`/`ei` around
the window, and nothing above 0xC000 touched inside it. See `docs/PLAN.md` P8.

### This is measured, not assumed

`src/pageprobe.c` writes a sentinel at 0xFF00, banks page 7 in at 0xC000,
writes to 0xC000 and 0xDAFF, banks bank 0 back, and rereads the sentinel. It
reports on the title screen as `PAGEWIN`, because a +3 can only be asked that
way.

| | |
|---|---|
| **48K** | `N/A` — nothing pages |
| **128K** (ZEsarUX) | **SURVIVES** |
| **+3** (Fuse, real load) | **SURVIVES** |

The +3 result is the one that counts: it has a second paging port at 0x1FFD, a
different memory controller, and every fault this project has had in that
window came from it. Two of those survived a green test suite.

The probe is also **its own witness** — `page_probe` links at 0xC0A7, inside
the window being paged away, and still holds the right value afterwards. That
is stronger than the sentinel alone, and it was luck rather than design; if you
write a probe like this, put a variable up there deliberately.

Two details that are not optional:

- **Store the result after banking back**, not during the window. Writing to a
  variable that lives above 0xC000 while page 7 is mapped puts it in the
  screen, not in your bss.
- **BANKM (0x5B5C) on every write, both directions.** Verified as 0x10 after
  the probe. Leave it stale and the ROM's interrupt handler writes its copy
  back over you — that cost most of a session once already.

The probe lives in the **48k build only**, which is the point rather than a
limitation: that build is the one with code and bss above 0xC000, so it is the
only one whose survival means anything. The 128k build has nothing up there to
lose.

## One binary for 48K and 128K

The obvious plan is to detect the machine and choose a layout. **You cannot:
code placement is fixed at link time**, long before any detection runs. A
binary with code above 0xC000 can never bank page 7 there.

The tempting escape is to map page 7 only while copying into it, so code can
live to 0xFFFF *and* a 128K keep its shadow screen. **The hardware permits
this** — measured, see above — **but a renderer that draws directly into the
shadow screen does not.** If `gfx_target()` aims at 0xC000 and then chrome,
text, cursor and dirty cells all write there, page 7 must stay mapped across
arbitrary game code, which is precisely what forbids code above 0xC000.
Confining it to a copy means composing a whole 6912-byte screen low first, and
that space does not exist.

**Check who writes to 0xC000 before designing around the window.** A session
went into a plan that did not survive contact with one `#define`.

### What works instead

**If all code is below 0xC000 anyway, page 7 can simply stay mapped, and no
window is needed.** The single binary is then the 128K build, running on
everything:

- The `shadow_ok = 0` fallback — written for 128Ks whose paging is locked —
  **is** the 48K path. Verified: the 128k tap runs correctly under ZEsarUX
  `--machine 48k`.
- Reserve `0xDB00-0xFFFF` as the data region on *both* machines: real RAM on a
  48K, page-7 RAM on a 128K, same addresses, no conditional accessors.
- `0xC000-0xDAFF` is the shadow screen on a 128K and spare on a 48K.

Measure before assuming the 48K build needs its extra 16 KB. On this project
it exceeded the 16 KB ceiling by **167 bytes** while leaving 16 217 clear — the
address space it paid the shadow screen for was almost entirely unused.

## Lowering -zorg: 8 KB of code space, at 50%% of the speed

Code at 0x8000 leaves 16 KB below the 0xC000 ceiling.  Moving the
hand-placed buffers up to 0xDB00 and setting `-zorg=24576` puts code at
0x6000 instead: **5 bytes clear became 8197**.

Two beliefs were tested and both turned out wrong in opposite directions.

**The floating bus does NOT need non-contended code.** The Makefile
asserted for a long time that -zorg=0x8000 was required for stable sync.
Measured: vsync still settles on 0x40FF and the marker still lands on the
displayed screen, on 48K and 128K alike.

**But contention is expensive.** 0x6000-0x7FFF is contended, the ULA
steals cycles from every fetch there, and half the program now lives in
it: the state walk went from 19.7-21.1s to **30.4s**.  Functionally
perfect, materially slower.

So the trade is 8 KB against roughly half the speed, and the interesting
version is selective: put only COLD code low — AI, pathfinding, anything
run once a turn rather than once a frame — and keep the render path at
0x8000.  Per-module placement, not a global -zorg.

The layout itself is safe on all three machines (`tests/lowmem.tap`,
border green): stack lands at 0x5FA4, below the new code base, and
0xDB00-0xFFFF round-trips every byte.  Worth confirming rather than
assuming, because a +3 had crashed on that region before.

## `org` in a user module places the section but DROPS the data

Tempting idea: put the ZX0 asset blobs in a hand-written `.asm` at a fixed
address in contended RAM, freeing RODATA from the 16 KB code budget at no
speed cost — the blobs are read once, at boot.

```asm
    SECTION ORGPROBE
    org     0x7D00
    PUBLIC  _org_probe_blob
_org_probe_blob:
    defb    0xA5, 0x5A, 0xC3, 0x3C
```

z80asm honours it perfectly:

```
__ORGPROBE_head = $7D00
__ORGPROBE_size = $0004
_org_probe_blob = $7D00
```

**And `-create-app` then throws the data away.** The tap came out
16 471 bytes — byte-for-byte the size it was without the section — and
0x7D00 read `00 00 00 00` on a real load. No warning, no error. C code
reading that array would get zeros, so every graphic would decompress
from nothing.

`-create-app` emits ONE contiguous block from CRT_ORG_CODE. A section
placed outside that range is addressed but never packaged.

To actually do this you need a **multi-block tap**: `-split-bin` to get the
section as its own binary, `appmake +zx` to wrap it as a second CODE
block, and a BASIC loader with two LOADs. That is a real piece of work,
not a pragma — budget for it accordingly.

**The general lesson: a correct link map does not mean the bytes ship.**
Check the tap size and read the address back on a real load. This one
would have failed silently and looked like a decompressor bug.

### The bank route gets further, but still needs a loader

`SECTION CODE_1` in a user module *does* reach the tap, unlike `org`:

```
_bank_probe_blob = $1C000       bank 1, offset 0xC000
tap 16471 -> 17499 bytes
```

But it arrives as a **headerless data block**, and the generated 30-byte
BASIC loader does a single `LOAD ""CODE`. Nothing loads it. appmake emits
banked blocks for a program that loads its own banks at runtime.

`LOAD ""CODE` cannot read a headerless block — it wants a header. A custom
loader has to either make appmake emit headers for the extra blocks, or
call the ROM's LD-BYTES with the headerless flag.

**Parse the tap, do not trust the size.** `16471 -> 17499` said the bytes
shipped; only dumping the block headers showed nothing would load them.

## Adding a graphic

The converters do the work; see `.claude/skills/zx-tiles`. What this skill
adds is where the bytes end up.

1. **Compressed source** goes in the binary, below `0xC000`, as rodata from a
   generated header.
2. **Unpacked destination** goes in `include/memmap.h`, above `0xC000`. Add a
   block to the end of the chain — never pick an address:

   ```c
   #define MEM_NEWTHING  (MEM_U_FLAGS + 40)
   #define MEM_END       (MEM_NEWTHING + 512)
   ```

   Every block is sized from the thing that lives in it and `MEM_END` is
   checked against `0x10000`.
3. `make memmap` to confirm it landed where you meant.

If `checkmem` fails, the fix is to move *data* into `memmap.h`, not to raise
the limit. But check what is actually big first — `make memmap` reports per
region, and the squeeze is usually code.

## Linking: generated headers must not be duplicated

A generated header that defines `static const uint8_t foo[] = {...}` puts a
**copy in every .c file that includes it**. Three copies of a 380-byte tile
sheet cost 760 wasted bytes and show up nowhere but the link map.

`tools/tmx2header.py` and `tools/zxp_tiles_zx0.py` emit this instead:

```c
#ifndef TILES_VIEW_DEFINE_DATA
extern const uint8_t tiles_view_zx0[380];
#else
const uint8_t tiles_view_zx0[380] = { ... };
#endif
```

Exactly one translation unit claims each header:

```c
#define TILES_VIEW_DEFINE_DATA      /* BEFORE every #include */
#include "../include/tiles_view.h"
```

- **The claim must precede every include**, not just the direct one.
  `board.h` reaches `level_1.h`; a claim made after that arrives too late and
  nobody defines the data.
- `extern const x[36] = {...}` is **still a definition** — the initialiser is
  what matters, not the keyword. The guard has to wrap the whole array.
- Failure modes are both link errors, which is the point: *undefined symbol*
  means nobody claimed it, *duplicate symbol* means two files did.

Current owners: `src/render.c` takes the tile sheets and level 1; `src/logic.c`
takes campaign maps 2-10.

The same trap applies to any header pulling in a generated one for its *size
macros* — the data comes along. `include/memmap.h` and `include/render.h` both
carry a note about it; `render.h` was carrying three copies of the tile sheets
purely to reach `TILES_VIEW_TILE_W`.

## Inline assembly and addresses

Inline assembly cannot see C expressions, so any `memmap.h` address used in
`__asm` has to be a literal. Guard it:

```c
#if MEM_VBUF != 0xC000
#error "present_pixels() has MEM_VBUF baked into its assembly"
#endif
```

SDCC's assembler rejects `.rept`/`.endm` and the `0 (iy)` indexed operand
form. Copy the shape of `border()` in `src/gfx.c` rather than re-deriving it.

## Verifying

```bash
make memmap                 # both halves of the layout, and the free space
make map                    # build + checkmem (fails if the linker crosses 0xC000)
python3 tools/checkmem.py zxstrategy.map --layout
```

Per-symbol sizes derived from a link map are *gap to the next symbol* and
overstate the last symbol before any library code. Per-module totals are
trustworthy; individual symbols are an upper bound.

**A green test run says nothing about a machine the tests cannot load.** The
+3 fault above survived a fully passing suite on 48K and 128K for a whole
session, and three confident diagnoses were wrong before the real one. If a
bug is model-specific, reproduce it on that model before theorising.

## Related

- `.claude/skills/zx-tiles` — the converters and the .zxp format
- `.claude/skills/floating-bus-vsync` — why code must be uncontended
- `.claude/skills/zesarux-test` — driving each model headlessly
- `docs/DESIGN.md` § Two machines, two render paths
- `docs/PLAN.md` § The +3 problem — the full post-mortem
