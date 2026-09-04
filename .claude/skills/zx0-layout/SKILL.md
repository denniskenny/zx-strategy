---
name: zx0-layout
description: Decide where code, graphics, text and audio live in a ZX Spectrum project and what to compress with ZX0 — the four regions and what each can hold, why one shared decompressor makes compression nearly free, and the order of operations that stops a 128K silently losing everything you unpacked.
when_to_use: "starting a new ZX project" or "where should this go" or "should I compress this" or "zx0" or "memory layout" or "greenfield" or "laying out a new game" or "running out of room" or "what goes in a bank"
allowed-tools: Bash Read Write Edit
effort: medium
---

# Laying out a ZX Spectrum project, and what to ZX0

Decide this early. Moving a buffer later is cheap; discovering that the
thing you need to grow is the one region that cannot grow is not.

Every number here is measured from a shipped 48K/128K/+3 game.

## The one fact that drives everything

**`0xC000-0xFFFF` is a paged bank on a 128K-class machine.** Anything the
linker puts there vanishes the moment something pages, and the failure
looks like random corruption rather than a crash.

So the address space splits into regions that are NOT interchangeable:

| region | size | holds | notes |
|---|---|---|---|
| `0x4000-0x5AFF` | 6.9K | the screen | |
| `0x5B00-0x7FFF` | ~9K | **cold code**, buffers | CONTENDED: ~40% slower |
| `0x8000-0xBFFF` | 16K | **all hot code**, rodata, bss | the scarce one |
| `0xC000-0xFFFF` | 16K | **data only, never code** | a bank on a 128K |
| banks 1,3,4,6 | 64K | bulk storage | 128K/+3 ONLY, and see below |

**`0x8000-0xBFFF` is the budget.** Everything else is comparatively
plentiful, and the whole game of laying out a Spectrum project is moving
things out of those 16K.

## Where each kind of thing goes

```
CODE, hot          0x8000-0xBFFF   no choice
CODE, cold         0x5B00-0x7FFF   whole modules only; contended, so
                                   nothing per-frame
GRAPHICS           ZX0 low, unpacked above 0xC000
TEXT               ZX0 low, unpacked above 0xC000
MUSIC              ZX0 low, unpacked above 0xC000
big static art     a bank, 128K only, with a 48K fallback
```

"ZX0 low, unpacked above `0xC000`" is the pattern for all three kinds of
data, and it is worth stating why.

## Why compression is nearly free after the first use

ZX0's decompressor is ~70 bytes of Z80 (z88dk's `dzx0_standard`), and it
is the SAME routine for every kind of data. Once anything in the program
is compressed, every further use costs only the compressed bytes.

That changes what is worth doing:

| | raw | ZX0 | decoder |
|---|---|---|---|
| tile and sprite sheets | ~1.1K | ~500 | shared |
| ten tilemaps | 980 | 331 | shared |
| two tunes | 777 | 313 | shared |
| 46 strings | 628 | 356 | shared |

**Before inventing a format, check whether a decoder you already carry
will do.** A word-dictionary text packer was written for that last row:
it packed 628 to 500 and needed 116 bytes of bespoke expander, so it came
out TWELVE BYTES WORSE than storing the text raw. ZX0 packed it to 356
with no decoder at all.

## What NOT to compress

* **Anything under ~200 bytes.** ZX0 has per-stream overhead; short
  inputs come out bigger.
* **Anything you need random access to** while compressed. A ZX0 stream
  decompresses from the start, so you unpack the WHOLE thing or nothing.
* **Many small streams.** One block always beats several:

  ```
  one stream per string   2576 bytes   WORSE than raw
  groups of 8             1806
  ONE block               1352
  ```

  Compression finds redundancy ACROSS items. Splitting per level or per
  screen throws away the repetition that makes the data compressible in
  the first place.

## What a bank can and cannot hold

A bank is paged in at `0xC000` -- **the same window as the shadow screen
and every buffer above it.** While a bank is in, none of that is
reachable.

```
GOOD in a bank    read at a moment you control:
                  a picture decompressed while nothing else happens,
                  a tune unpacked before it plays, a level between turns
BAD in a bank     anything a per-frame or per-character routine reads
```

A 768-byte font was put in a bank and the printer pointed at it. It works
on a 48K, which has no paging, and corrupts on a 128K, which draws to the
shadow screen the bank displaces:

```
48k    ' UNIT   :Red Base  025/025 R0 M0'
128k   ' ?NI?   :Red B?se   2?/ 2? R  ? '
```

**If a routine both pages and draws, it cannot use the bank window for
either.**

## Two traps that cost real days

### Data with internal pointers must be ASSEMBLED at its destination

A music order table is `DEFW PAT0, DEFW PAT1, ...` -- absolute addresses
resolved by the linker. Compress those bytes and unpack them anywhere
else and every pointer is wrong by the relocation distance. The symptom
is the right tempo, the wrong notes, and a crash a bar or two in.

Give the data `org <destination>`, assemble it, and compress THAT image.
Then the pointers are correct on arrival.

Applies to anything with internal offsets: sprite tables, level indices,
menu structures.

### Unpack AFTER the paging map is settled

On a 128K the buffers above `0xC000` belong to whichever RAM page is
mapped there. Unpack before the program has selected its page and the
data goes into the wrong bank; the real page then comes in over the top
and the data is simply gone.

This cost a debugging session: strings unpacked in `main()` before the
paging write, so the 128K title screen came up with its attributes
painted and **not one pixel of ink**, while a 48K -- which has no paging
-- was perfectly fine.

**Do all unpacking from one place, after the map is established**, next
to whatever else fills those buffers.

## Addresses of unpacked data are compile-time constants

The destination is fixed and the layout is known when the generator runs,
so a symbol can be a constant:

```c
#define TXT_HINT ((const char *)(MEM_TEXTPOOL + 123))
```

One `ld hl,nn` -- the same as the plain array it replaced, so **no call
site changes and no lookup table is needed.**

Routing it through `str_of(n)` instead cost about six bytes at each of 52
call sites, some 300 bytes against 180 saved. Generate constants.

## Verify the compression in the build

A compressor that quietly alters data will not announce itself, and the
symptom appears far from the cause. Three checks, all cheap:

1. **Round trip.** Decompress with `z88dk-dzx0` and compare with the
   input. Fail the build if they differ.
2. **Structure.** If the data has internal pointers, check every one
   lands inside the block -- not just the first. A bad pointer at the END
   is a crash rather than visibly wrong output.
3. **On the machine.** Read the unpacked buffer back over ZRCP and
   compare with the reference. That is the only check covering the whole
   chain.

This project shipped broken compressed data twice before those existed.
One extractor recovered 485 bytes from a 513-byte song -- the missing 28
were an order table it silently skipped, and the discrepancy was printed
on screen and read past. See `.claude/skills/test-design`.

## A layout to start from

```
0x5B00  cold modules: menus, level loading, anything not per-frame
0x8000  hot code, rodata, bss
0xC000  128K: shadow screen (page 7)      48K: spare
0xDB00  buffers: compose buffer, tile sheets, decompressed text, music
banks   cutscenes, per-level art, extra tunes  (128K only)
```

Then, in the build:

* a tool that FAILS if the linker crosses `0xC000`, printing free space
  per region -- the single most useful thing you can automate
* one generator per asset kind, each emitting ZX0 plus a header of
  constants
* every generator round-trips its own output

## Related

- `.claude/skills/zx-memory` -- the map in detail, banking, the ROM-select
  trap, the worked music and text cases
- `.claude/skills/zx-loader` -- getting the blocks onto tape and into
  banks
- `.claude/skills/test-design` -- why the round-trip checks above exist
