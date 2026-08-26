---
name: zx-loader
description: Build a multi-block ZX Spectrum .tap by hand — one CODE block per region, laid out across contended RAM, the program, and RAM banks. Covers the zcc/z80asm directives that emit a block, the appmake behaviour that silently drops one, how to load into a bank at all, and how to prove every byte arrived where it was addressed.
when_to_use: "tap loader" or "multi-block tap" or "code block" or "load into a bank" or "banked data" or "mktap" or "-create-app" or "appmake" or "constseg" or "codeseg" or "SECTION CODE_1" or "BANK_1" or "the data is zeros" or "blob did not load" or "where do assets go"
allowed-tools: Bash Read Write Edit
effort: medium
---

# ZX Loader: getting bytes to the address you asked for

`-create-app` emits **one** contiguous CODE block from `CRT_ORG_CODE`, plus
a 30-byte BASIC loader that does a single `LOAD ""CODE`. Everything else
you place — an asset blob low in memory, a cold module, a bank — needs
building into the tap yourself.

**The failure mode is always the same and always silent:** the link map is
correct, the program boots, and the data reads as zeros. This skill is
mostly about not being fooled by that.

See `.claude/skills/zx-memory` for what belongs at which address; this is
about how it gets there.

## The regions, and what each is for

| region | holds | reachable by |
|---|---|---|
| `0x6000-0x7FA0` contended | compressed assets, COLD code | everything, always |
| `0x8000-0xBFFF` uncontended | the program | everything, always |
| `0xC000-0xFFFF` | buffers, decompressed sheets, the 128K shadow screen | everything, always |
| banks 1,3,4,6 | 64 KB of storage | **128K/+3 only**, and only when paged in |

Contended RAM costs a cycle per instruction fetch, so it is right for
things read once at boot (asset blobs) or run rarely (AI, pathfinding) and
wrong for the render path. Moving ALL code there cost ~50% speed on this
project; moving one cold module cost nothing measurable.

## Emitting a block

### A standalone binary — the simplest thing that works

Assemble it on its own, never linked into the C program, so its bytes
cannot count against the code budget:

```make
$(APP)_assets_low.bin: src/assets_low.asm
	z88dk-z80asm -b -O. -o$@ src/assets_low.asm
```

```asm
    org     0x6000
_my_blob:
    defb    0xA5, 0x5A, ...
```

The C side sees only addresses. Resolve them with a `defc` module that IS
linked and costs nothing:

```asm
    MODULE  assets_low_syms
    PUBLIC  _my_blob
    defc    _my_blob = 0x6000
```

That satisfies an `extern const uint8_t my_blob[]` at zero bytes — the
same trick that removes an unwanted library font.

### A C module in its own section

```c
#pragma codeseg LOGIC
```

plus an `.asm` stub that places the section:

```asm
    SECTION LOGIC
    org     0x6500
```

z88dk then emits `$(APP)_LOGIC.bin` automatically, and the main binary
shrinks by exactly that much. The `org` in a separate module DOES apply to
a section another module contributes to.

### A bank

```
zcc ... -o bank3.o bank3.c --codesegBANK_3 --constsegBANK_3 --datasegBANK_3 -c
```

Produces `$(APP)_BANK_3.bin` and a map entry at `$3C000` — the address
encoding is `(bank << 16) | addr`. This is the correct mechanism; hand-rolled
`SECTION CODE_1` also links but is not the documented route.

## Assembling the tap

`tools/mktap.py` writes the loader and one headed CODE block per binary:

```
python3 tools/mktap.py out.tap --clear 24575 --usr 32768 \
    --code 0x6000 assets_low.bin \
    --code 0x6500 logic.bin \
    --code 0x8000 program
```

Every block gets a **real header**, so a plain `LOAD ""CODE` reads each in
turn — which is what makes it work on a 48K, where nothing can page.

Blocks load in the order given. Addresses are written by hand while their
SIZES come from the build, so one growing into the next is a matter of
when, not whether: mktap refuses overlaps, blocks at or below `CLEAR`, and
anything that runs into the stack.

## What appmake will not do

**`org` in a user module: placed, then DROPPED.** z80asm honours it —
`__ORGPROBE_head = $7D00`, symbol at `$7D00` — and `-create-app` then
discards the bytes. The tap comes out byte-for-byte the size it was
without the section. C reading that array gets zeros.

**A bank section: shipped, but HEADERLESS.** `SECTION CODE_1` reaches the
tap (16471 -> 17499 bytes) as a block with no header. `LOAD ""CODE` cannot
read one, and the generated loader would not try. appmake emits banked
blocks for a program that loads its own banks at runtime; the upstream
z88dk example sidesteps this entirely by shipping a `.sna`.

**Parse the tap. Do not trust its size.** The size said the bytes shipped;
only dumping the block headers showed nothing would load them:

```python
d = open('out.tap','rb').read(); i = 0
while i < len(d):
    ln = int.from_bytes(d[i:i+2],'little'); i += 2
    b = d[i:i+ln]; i += ln
    if b[0] == 0 and len(b) >= 19:
        print('HEADER', {0:'BASIC',3:'CODE'}.get(b[1],'?'),
              'len', int.from_bytes(b[12:14],'little'),
              'addr 0x%04X' % int.from_bytes(b[14:16],'little'))
    else:
        print('  data', len(b)-2)          # <-- headerless: nothing loads it
```

## Loading into a bank

**This works and ships.** `assets/action-force.scr` -- 6912 bytes, 2519
ZX0'd, which fits nowhere in addressable memory -- rides in bank 1 and is
decompressed to the screen when the cutscene state opens.

### BASIC loads, YOUR code pages

BASIC must never write `0x7FFD`. Three attempts to make it do so failed,
and the first failed while looking perfect: it booted, rendered, and had
the data in the wrong bank.

```
LOAD ""CODE            the copier, above RAMTOP
LOAD ""CODE            the blob, into the SCREEN at 0x4000
POKE  bank, length, destination
RANDOMIZE USR          the copier: pages, copies, pages back
LOAD ""CODE  x3        the program
RANDOMIZE USR          go
```

Bit 4 of `0x7FFD` is the ROM select and bit 5 the paging lock: BASIC's
`OUT` swaps a ROM under a running interpreter, and its failure to update
BANKM lets the ROM undo the switch mid-`LOAD`. Machine code with `di` and
BANKM handled properly has none of those problems.

### Stage through the screen, bank phase first

The display file is the only free 6912 bytes on the machine, and ordering
the bank phase before every other block means there is nothing in memory
for the staging to land on. The load shows as noise, which is honest.

### Where the stub lives: above RAMTOP, nowhere else

| address | what happens |
|---|---|
| `0x5AFA` | tail of the ATTRIBUTE FILE. The ROM writes to the screen while loading and wipes the parameters. Dead boot. |
| `0x5B00` | the printer buffer **on a 48K**. On a 128K it is the machine's own system variables -- BANKM is at `0x5B5C`. Crash. |
| `0x5F00` | above `CLEAR 24319`. **Correct.** Nothing else has a claim above RAMTOP; that is what CLEAR is for. |

"Traditional home for a loader stub" is 48K advice. Check it against the
128K map first.

### Preserve BANKM, do not force it

```asm
ld  a, (0x5B5C)         ; whatever the ROM had
ld  (_bc_save), a
and 0xF8                ; keep ROM select, screen, and the LOCK
or  b                   ; only the bank bits change
```

Forcing a value assumes the machine arrived with bank 0, the 48K ROM and
nothing else set. If the lock was on, the `OUT` is silently ignored and
the copy lands in bank 0 looking successful.

### Do NOT load at offset 0

`hw_detect()` proves a machine is a 128K by writing to **bank 1 at
0xC000** and reading it back through another bank. A payload at offset 0
is scratch by the time anything looks for it. Use the copier's
destination: this project loads at `BANK_DEST = 0x0100`.

### Reading it back

Paging evicts the whole `0xC000` window -- shadow screen, every buffer.
That is survivable only where nothing in that window is touched between
the two `OUT`s. The cutscene qualifies: no board is being drawn, the
decompressor's code is at `0x8000`, its workspace is on the stack below,
and the destination is the screen at `0x4000`. Interrupts off across it.

## The tests that lied, and what they cost

Nine rounds. **Three real bugs** (two stub addresses, the `hw_detect`
collision) and **three self-inflicted**, each of which looked exactly like
a banking failure:

**A hand-typed address.** Five consecutive taps failed with the game
returning instantly to BASIC because the invocation said `--code 0x6800`
for the LOGIC block where the build computes `0x6600` from
`logic_org.addr`. Nothing about banking was wrong in any of them. **Put
`--bank` in the Makefile next to the other blocks** -- generated
addresses must come from the generated file.

**A probe without `volatile`.** The scan wrote a bank number and read a
byte back around an `__asm` block, both plain globals. At `-SO3
--opt-code-speed` the compiler deferred the store and cached the load, so
the probe paged somewhere nobody asked for and compared a stale value. It
reported NOWHERE while the copy had been landing correctly the whole time.
**Anything an `__asm` block reads or writes must be `volatile`.**

**A probe hunting the wrong byte.** After the sentinel was replaced with
the real payload the check still looked for `0xA5`, so a perfectly good
bank reported NOWHERE.

The lesson is not "write better probes". It is that **a probe is code and
can be wrong**, and an instrument that has never returned a positive has
not been shown to work. Ours never had. Prefer a test whose success is
visible -- a picture that draws -- over one byte that has to survive the
whole boot to be believed.

## Checklist for a new block

1. Emit it — standalone `.asm`, `#pragma codeseg`, or `--constsegBANK_n`.
2. Add it to `mktap.py` with an explicit address.
3. **Parse the tap** and confirm a HEADER, not a bare data block.
4. Check `mktap`'s free-space line and `checkmem`'s ceiling.
5. Read the bytes back on a real load — 48K, 128K **and +3**.
6. Only then write code that depends on it.

## Related

- `.claude/skills/zx-memory` — what belongs at which address, and the ROM trap
- `.claude/skills/zesarux-test` — driving each model headlessly
- `tools/mktap.py`, `tools/mkassets.py` — the loader and the asset sweep
- `docs/PLAN.md` P10, P11 — the full record of what failed and why
