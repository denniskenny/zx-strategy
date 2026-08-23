---
name: floating-bus-vsync
description: Implement or modify floating bus vsync detection on the ZX Spectrum for both 48K and 128K models. Covers timed attribute-matching loops, marker setup, model detection, and +2A/+3 differences.
when_to_use: "floating bus" or "vsync" or "frame timing" or "sync to beam" or "free frame time" or "screen tearing"
allowed-tools: Bash Read Write Edit
effort: medium
---

# Floating Bus Vsync — ZX Spectrum

The floating bus trick exploits a hardware quirk where the CPU can read the same data the ULA is fetching from screen RAM. By timing reads precisely, we can detect which screen row the beam is scanning and sync our drawing to it.

**Reference**: Ast A. Moore, "The Definitive Programmer's Guide to Using the Floating Bus Trick on the ZX Spectrum" (sky.relative-path.com/zx/floating_bus.html)

## How It Works

During active display, the ULA fetches bitmap+attribute bytes in a repeating pattern with 4 T-state idle gaps. Reading an unattached port during this time returns whatever the ULA last put on the bus. During border/vblank, the bus floats high → reads 0xFF.

By placing a **unique marker attribute** at a known screen row and running a **precisely timed loop**, we detect when the beam reaches that row. This gives a sync point near the bottom of the display, maximising safe drawing time.

## Key Constants

| Item | Value |
|------|-------|
| 48K frame | 69 888 T-states, 312 lines × 224 T/line |
| Active display | 192 lines |
| Safe time (marker at row 22) | ~28 000 T-states (bottom border + vblank + top border) |
| Safe time (HALT fallback) | ~14 000 T-states (top border only) |

## Two Floating Bus Techniques

### 48K / 128K / +2 — Port 0xFF, 35 T-state loop

```z80
    ld  d, MARKER        ; expected attribute
    ld  e, 0x40          ; port MSB (port = 0x40FF)
loop:
    dec hl               ; [6]  padding
    ld  a, e             ; [4]
    in  a, (0xFF)        ; [11] read floating bus
    cp  d                ; [4]  match marker?
    jp  nz, loop         ; [10] total = 35 T
    ret
```

The 35 T-state timing ensures each `IN` lands on an **attribute fetch**, never a bitmap byte or idle interval. Only the port LSB matters for ULA decoding.

**Requirements:**
- Code must be in **non-contended memory** (≥ 0x8000) — this project builds with `-zorg=32768`
- Marker attribute must be **unique** on screen and must not be 0xFF

### +2A / +3 — Port 0x0FFD, 42 T-state loop

```z80
    ld  d, MARKER        ; expected attribute (bit 0 must be set)
    ld  e, 0x0F          ; port MSB (port = 0x0FFD)
loop:
    ld  a, (PRELOAD)     ; [13] contended read — preloads bus for idle
    ld  a, e             ; [4]
    in  a, (0xFD)        ; [11] read floating bus
    cp  d                ; [4]  match marker?
    jp  nz, loop         ; [10] total = 42 T
    ret
```

**Key differences from 48K:**
1. Only works on ports matching `1 + 4n` where n < 4096 (e.g. 0x0FFD = 4093)
2. **Paging must be enabled** (bit 5 of port 0x7FFD = 0). If paging is locked, the bus always returns 0xFF — `src/main.c` therefore skips the paging lock when mode 2 is active
3. The returned value is **ORed with 1** — the marker must have bit 0 set
4. During idle intervals the bus returns the last **contended memory** value, so `ld a,(VSYNC_PRELOAD_ADDR)` preloads a known non-marker byte
5. Padding changes from `dec hl` (6T) to `ld a,(nnnn)` (13T) to hit 42 T-states

## Marker Attribute Selection

The marker must:
1. Have **bit 0 set** (so `marker | 1 == marker` on +2A/+3)
2. **Not appear** anywhere else on the display as an attribute byte (nor any other attribute ORed with 1)
3. **Not be 0xFF**

This project uses **0x03** (black paper, magenta ink — invisible on the blank marker row), configured in `config/app_config.h`:

```c
#define VSYNC_MARKER       0x03
#define VSYNC_MARKER_ADDR  0x5AC0   /* attr row 22, col 0 */
#define VSYNC_MARKER_CELLS 32       /* whole row          */
#define VSYNC_PRELOAD_ADDR 0x5AE0   /* attr row 23, col 0 */
```

The full inventory on screen is 0x00, 0x04, 0x05, 0x07, 0x42, 0x44, 0x45, 0x46, 0x47, 0x4F and 0x78 — `src/game.c`'s `ATTR_*` defines plus the terrain sheets' authored cells. None of them (nor `| 1`) equals 0x03. `make assets` prints each sheet's colours, which is the quickest way to re-check after editing art.

The art sheets are the other source. Terrain tiles carry a **per-character-cell** attribute block authored in ZX-Paintbrush, and `tools/zxp_tiles_zx0.py` rejects any cell that is 0x02 or 0x03, naming the tile and cell. Unit sheets are converted with `--attr-mode bright`, which keeps only bit 6, so they cannot introduce a bad value at all.

This constraint is also why **enemy units cannot be dimmed**: non-bright red on black is 0x02. The enemy's ink carries BRIGHT permanently and `src/game.c` says so where `ATTR_UNIT_E` is defined. **Re-verify this whenever you add new attribute values.**

## Marker Placement — use a full row

The marker is written across **all 32 cells of attribute row 22** (0x5AC0–0x5ADF), refreshed at the start of every `vsync_wait()` (~1 000 T-states with contention).

Why the whole row and not 3 cells: the ULA only puts a byte on the bus during its fetch slots, so a narrow marker is hit rarely. On real hardware a 3-cell marker locks in within a frame or two; under emulators with a narrower floating bus window (ZEsarUX) it can take dozens of frames. A full row keeps the same sync precision (the beam is at row 22 either way) and matches almost immediately.

**Keep row 22 pixel-blank and row 23 free of the marker value** — row 23 is the +2A/+3 preload source.

## Auto-Detection (vsync_detect)

Called once at startup after `hw_detect()` and **before** locking paging:

1. Write the marker to attr row 22
2. Read port 0xFF up to 10 000 times. Any non-0xFF → **mode 1** (48K floating bus)
3. If timeout and `is_128k` → read port 0x0FFD up to 10 000 times. Any non-0xFF → **mode 2** (+2A/+3)
4. Both timeout → **mode 0** (HALT fallback)

The detection loop does not need the 35/42 T-state timing — it only checks whether non-0xFF values ever appear.

**Critical startup order** (`src/main.c`):
```c
hw_detect();       // needs paging enabled for the 128K bank test
vsync_detect();    // needs paging enabled for the +2A/+3 probe
if (vsync_mode != VSYNC_MODE_128K) { /* ld a,0x30 / out (0x7FFD),a */ }
```

## Emulator Support

- **Fuse** — 48K floating bus works well; this is the reference emulator for this project (`make run`).
- **ZEsarUX** — the bus is emulated but valid (non-0xFF) samples are sparser than on hardware: under 13.0 a probe of 65 536 untimed reads saw only ~77 attribute values and the demo synced at roughly 1 fps. **13.1 is much better (~25 fps)**, but still not hardware-accurate — use it for memory/ZRCP inspection, not for judging frame rate.
- The +2A/+3 technique is only supported by SpecEmu, Spectramine, SpecIde and ZXDS.

Emulators without floating bus support time out during detection and fall back to HALT.

## Diagnostic Harness

`tests/fbprobe.c` (`make probe`) fills the screen with a known attribute, writes the marker, samples port 0x40FF 65 536 times and builds a 256-entry histogram at 0xF000. Read it back over ZRCP with `read-memory 61440 256`:

- Only 0xFF non-zero → no floating bus on this machine/emulator
- Attribute values present → bus works; a hang in `vsync_wait()` is a marker/timing problem

## Implementation Files

| File | Role |
|------|------|
| `src/vsync.c` | `vsync_detect()` and `vsync_wait()` — all-assembly, `__naked` |
| `include/vsync.h` | Declarations, `vsync_mode`, `VSYNC_MODE_*` |
| `config/app_config.h` | Marker value, marker address/width, preload address |
| `src/main.c` | Detection order and the conditional paging lock |
| `src/hw_detect.c` | Sets `is_128k` (gates the +2A/+3 probe) |
| `tests/fbprobe.c` | Floating bus histogram probe |

## Troubleshooting

- **Hangs on the first `vsync_wait()`**: marker never matched. Widen the marker row, check the marker is actually in attr RAM, and confirm no other screen attribute equals it.
- **Very low frame rate under an emulator**: sparse floating bus emulation (see above) — verify in Fuse.
- **Flicker after a screen clear**: the clear wipes the marker; `vsync_wait()` rewrites it each call, so at most one frame is lost.
- **+2A/+3 always falls back to HALT**: paging was locked before detection.
- **Sync jitter**: code is in contended memory (< 0x8000). Build with `-zorg=32768`.
