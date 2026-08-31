---
name: zesarux-test
description: Launch ZEsarUX headless, inspect screen and variables, and simulate input via ZRCP for the ZX Strategy project. Use when testing, profiling, debugging rendering, or checking screen output.
when_to_use: "test in emulator" or "check the screen" or "inspect attributes" or "profile the frame" or "screenshot" or "read memory in the emulator" or "simulate a keypress" or "ZRCP"
allowed-tools: Bash Read Write Edit
effort: high
---

# ZEsarUX Testing & Screen Inspection — ZX Strategy

ZEsarUX with ZRCP (remote control protocol) is used for automated inspection of the built `.tap`.

## Environment

- **ZEsarUX binary**: `/usr/local/bin/zesarux` — a symlink into the build tree at `~/projects/zesarux/src/zesarux`, currently **13.1-SN** (post-13.0 `main`)
- **48K ROM**: `~/projects/zesarux/src/48.rom`
- **ZRCP port**: TCP 10000 (localhost)
- **Tap**: `/Users/Kennyd/projects/zx-strategy/zxstrategy.tap`

To update the emulator (no install step needed — the symlink points at the build):

```bash
cp ~/projects/zesarux/src/zesarux /tmp/zesarux-backup   # keep a known-good copy
git -C ~/projects/zesarux pull --ff-only
cd ~/projects/zesarux/src && ./configure && make -j$(sysctl -n hw.ncpu)
```

On macOS it configures for cocoa + coreaudio with ZRCP and contended memory enabled; the `aalib.h`/`caca.h` errors in `config.log` are expected (optional drivers).

**Floating bus fidelity improved noticeably in 13.1**: the demo syncs at ~25 fps where 13.0 managed roughly 1 fps. It is still slower than real hardware, so **Fuse (`make run`) remains the reference for visual/timing checks**.

## 1. Launching headless

```bash
zesarux --vo null --ao null --enable-remoteprotocol --machine 48k \
  --noconfigfile --quickexit \
  --joystickemulated Kempston \
  --romfile ~/projects/zesarux/src/48.rom \
  /Users/Kennyd/projects/zx-strategy/zxstrategy.tap &
```

**`--joystickemulated Kempston` if the test drives the joystick.** The
ninth byte of `set-ui-io-ports` is the Kempston port, but the game only
reads it when `hw_detect()` found a joystick -- and headless ZEsarUX has
none by default. Without the flag a joystick test passes while pressing
nothing.

Use **absolute paths**. Wait **6 seconds** before connecting. Add `--realvideo` for accurate per-scanline ULA emulation. Clean up with `pkill -f zesarux`.

## 2. ZRCP connection

```python
import socket

def connect():
    s = socket.socket(); s.settimeout(20); s.connect(('localhost', 10000))
    b = b''
    while b'command>' not in b: b += s.recv(4096)
    return s

def cmd(s, c):
    s.sendall((c + '\n').encode()); r = b''
    while b'command>' not in r: r += s.recv(4096)
    t = r.decode('latin-1')
    return t[:t.rfind('command>')].strip()

def rd(s, addr_decimal, n):
    x = cmd(s, f'read-memory {addr_decimal} {n}')
    x = x.replace(' ', '').replace('\n', '').replace('\r', '')
    return bytes.fromhex(x[:n * 2])
```

After connecting, `smartload /abs/path/zxstrategy.tap` then wait **8–10 seconds** for the tape to autoload.

### Key commands

| Command | Purpose |
|---------|---------|
| `get-registers` | Dump Z80 registers |
| `get-tstates-partial` / `reset-tstates-partial` | T-state profiling |
| `read-memory <dec addr> <dec len>` | Read memory as hex |
| `write-memory <dec addr> <dec byte>...` | Write bytes |
| `hexdump <hex addr> <dec len>` | Hex + ASCII dump |
| `set-breakpoint <idx> PC=<addr>h` / `enable-breakpoints` / `run` | Breakpoints |
| `set-ui-io-ports <18 hex chars>` | Keyboard/joystick state |
| `smartload <path>` | Load a .tap |
| `save-screen <path>` | Save a .scr |

`read-memory`/`write-memory` use **decimal**; `hexdump` uses a **hex** address with a decimal length.

## 3. Symbols

Build with a map file, then look up addresses (hex → decimal for ZRCP):

```bash
make map                          # = make clean && make USER_CFLAGS="-m"
grep -E '^_(vsync_mode|frame|bar_col|sync_on|is_128k)\b' zxstrategy.map
```

Useful checks:
- `_vsync_mode` → 0 HALT, 1 floating bus 0x40FF, 2 floating bus 0x0FFD
- `_frame` (uint16) → sample twice a second apart to estimate frame rate

## 4. Screen memory

- **Pixels**: 0x4000–0x57FF → `read-memory 16384 6144`
- **Attributes**: 0x5800–0x5AFF → `read-memory 22528 768`

### On a 128K, 0x4000 is not what you are looking at

The 128K render path composes full screens into the shadow display file (RAM
page 7, banked in at `0xC000`) and shows it by setting bit 3 of port `0x7FFD`.
So after any state entry, **`0x4000` holds the back buffer** and reading it
gives you the previous screen — which looks exactly like a rendering bug.

Check `_page_reg` (render.c keeps a copy, since `0x7FFD` is write-only):

| `page_reg` | Displayed | Read screen from |
|---|---|---|
| bit 3 clear (e.g. `0x17`) | page 5 | `0x4000` / attrs `0x5800` |
| bit 3 set (e.g. `0x1F`) | page 7 | `0xC000` / attrs `0xD800` |

```python
pr = sym(s, 'page_reg', 1)[0]
base = 0xC000 if pr & 0x08 else 0x4000
pix, attr = rd(s, base, 6144), rd(s, base + 6144, 768)
```

**Do not use `--accelerate-loading` on a 128K.** It is a genuine speed win
on a 48K — the p0 walk uses it and the tape stops being most of the wall
clock — but on a 128K the load silently never completes: the machine sits in
the ROM with a blank screen, `is_128k` reads 0 because the program never ran,
and it looks exactly like the program crashing or failing to render. It cost
most of an afternoon and several wrong diagnoses. `tests/render_paths.py`
deliberately omits it; `tests/p0_state_walk.py` is 48K-only and keeps it.

**Do not retry `smartload` in a loop either.** Each one *resets* the machine,
so a retry loop guarantees the load is interrupted before it can finish. The
emulator autoloads whatever tap is named on its command line, so the normal
case needs no `smartload` at all — just wait for the screen.

To run 128K at all you must give ZEsarUX the right ROM — `--machine 128k`
alone fails with *"Unable to open rom file 128.rom"* and leaves a black screen
with the tap unloaded, which reads as the program crashing:

```bash
zesarux --vo null --ao null --enable-remoteprotocol --machine 128k \
  --noconfigfile --quickexit \
  --romfile ~/projects/zesarux/src/128.rom  $PWD/zxstrategy.tap
```

Sanity check after loading: `is_128k` should be 1. If it is 0 on a 128K, the
tap did not load — check the screen is not blank before believing anything
else.

Address for column `col` (0-31) and pixel row `y` (0-191):
```
addr = 0x4000 | ((y >> 6) << 11) | ((y & 7) << 8) | (((y >> 3) & 7) << 5) | col
```

Attribute byte: bit7 flash, bit6 bright, bits5-3 paper, bits2-0 ink.
Colours: 0 Black 1 Blue 2 Red 3 Magenta 4 Green 5 Cyan 6 Yellow 7 White.

Expected on the **title screen**: row 0 = 0x45 (the banner), rows 3-5 = 0x47 (the hardware report), rows 10-11 and 20-21 = 0x46 (legend and hint, 0x42 while the tune plays), row 22 = 0x03 (the sync marker), everything else 0x07.

In `ST_PLAY` rows 1-16 are tile art and carry whatever the sheets author per character cell, so there is no single expected value there — see `.claude/skills/floating-bus-vsync` for the colour inventory and how to audit it.

### Render to PNG

```python
from PIL import Image
PAL = [(0,0,0),(0,0,0xCD),(0xCD,0,0),(0xCD,0,0xCD),(0,0xCD,0),(0,0xCD,0xCD),(0xCD,0xCD,0),(0xCD,0xCD,0xCD),
       (0,0,0),(0,0,0xFF),(0xFF,0,0),(0xFF,0,0xFF),(0,0xFF,0),(0,0xFF,0xFF),(0xFF,0xFF,0),(0xFF,0xFF,0xFF)]

def render(pix, attrs, path):
    img = Image.new('RGB', (256, 192))
    for py in range(192):
        t, cr, pr = py >> 6, (py >> 3) & 7, py & 7
        for col in range(32):
            byte = pix[(t << 11) | (pr << 8) | (cr << 5) | col]
            attr = attrs[(py >> 3) * 32 + col]
            ink, paper = attr & 7, (attr >> 3) & 7
            if attr & 0x40: ink += 8; paper += 8
            for bit in range(8):
                img.putpixel((col*8+bit, py), PAL[ink] if byte & (0x80 >> bit) else PAL[paper])
    img.save(path)
```

(`pip3 install pillow` if PIL is missing.)

## 5. Simulating input

Prefer Kempston — `set-ui-io-ports` takes 8 keyboard half-rows (`ff` = nothing pressed) plus one Kempston byte (active-high: bit0 right, bit1 left, bit2 down, bit3 up, bit4 fire1, bit5 fire2):

```
set-ui-io-ports ffffffffffffffff01    # right → move the cursor right
set-ui-io-ports ffffffffffffffff10    # fire1 → go on / end turn
set-ui-io-ports ffffffffffffffff20    # fire2 → back
set-ui-io-ports ffffffffffffffff00    # release
```

`game_run()` acts on *edges* of a combined keyboard+Kempston action byte, so always release between presses. Kempston is only polled when `has_kempston` is set by `hw_detect()`.

Keyboard equivalents (half-rows, active low): Q up on `0xFBFE` bit 0, A down on `0xFDFE` bit 0, O/P on `0xDFFE` bits 1/0, **SPACE on `0x7FFE` bit 0**, ENTER on `0xBFFE` bit 0, M on `0x7FFE` bit 2 (map while playing, tune on the title screen). Z/X sit on the CAPS SHIFT row (bits 1/2) only because `scan_input()` reads them.

**SPACE is what drives the app between screens** — starting a game, taking the level-end screen on, closing the overview — so a state walk presses `fffffffffffffffe00`. ENTER ends a turn inside `ST_PLAY`, and is the only thing it does.

The byte order for `set-ui-io-ports` is `0xFEFE, 0xFDFE, 0xFBFE, 0xF7FE, 0xEFFE, 0xDFFE, 0xBFFE, 0x7FFE` then the joystick; each row uses only bits 0-4. So holding M is `set-ui-io-ports fffffffffffffffb00`.

### The app boots into the tune — send a key first

**The title screen plays the Tritone tune the moment it is entered**, on boot and on every return to the title. The player runs with interrupts off and does not return until a key is pressed, so a freshly loaded tap sits there ignoring everything: `game_state` reads 0, the screen is painted, and nothing you send has any effect except stopping the tune.

Every harness therefore has to **spend one keypress waking it up**:

```python
press(SPACE)          # stops the tune; flushed, so it does NOT start a game
press(SPACE)          # this one starts the game
```

The keypress that stops the tune is deliberately flushed (`busy_off()` calls `flush_input()`), so it never doubles as the one that acts. A `press_until()` helper that retries until the state changes — as `tests/p0_state_walk.py` has — absorbs this without special-casing; a fixed single press does not.

The same applies after any return to the title: losing a level, or `X` out of play, lands you back in the tune.

### Phantom keypresses = a Kempston false positive

If the app appears to press its own keys (sync toggling, counters resetting with no input), check `has_kempston` first. With no joystick configured, port 0x1F is unattached and answers from the floating bus; a detector that accepts "any zero read" as proof of an interface will latch on and then feed random directions into `scan_input()`.

`hw_detect()` therefore requires **all 16 samples** of `in a,(0x1F) & 0x1F` to be zero. After that fix, ZEsarUX reports `has_kempston = 0` and the app runs for minutes with zero spurious actions.

`game.c` also debounces: `poll_input()` needs an action bit in two consecutive frames before acting, and `ST_WON`'s exit check samples once per frame with the same two-sample rule. Sampling once per frame matters — hammering the ULA port in a tight loop invites bus noise.

## 6. Profiling

`tools/profile_zrcp.py` breakpoints a list of symbols and measures T-states between them:

```bash
make map
python3 tools/profile_zrcp.py --frames 5 --mapfile zxstrategy.map
```

Edit `FRAME_START` (default `_vsync_wait`) and `WAYPOINTS` at the top of the script to match the frame loop you're profiling. Options: `--frames`, `--tap`, `--settle`, `--motion HH` (held Kempston byte), `--screenshot PATH`.

Audio (`*_play()` from `include/music.h`) cannot be verified here — ZEsarUX runs with `--ao null`. Use Fuse.

## 7. Floating bus probe

```bash
make probe
# load tests/fbprobe.tap, wait ~12s, then:
#   read-memory 61440 256     → 256-entry histogram of bytes seen on port 0x40FF
```

Only 0xFF non-zero means no floating bus support; attribute values present means the bus works.

### ZX0 decompression harness

```bash
make dzx0check
# load tests/dzx0check.tap, wait ~12s, then:
#   read-memory 61440 32      → 0xF000 = 0x5A on success, byte count, first 16 bytes
```

Compare those bytes against the host reference produced by `z88dk-dzx0`. A magic
byte other than 0x5A means the decompressor crashed — usually a ZX0 v1/v2
format mismatch (see `.claude/skills/compile-scr`).

### Verifying a blitted graphic

A graphic blitted back to the position it was cropped from must match its
source byte for byte. With `PREFIX_CROP_*` from the generated header:

```python
def off(col, y):
    return ((y >> 6) << 11) | ((y & 7) << 8) | (((y >> 3) & 7) << 5) | col

src = open('assets/splash.scr', 'rb').read()[:6144]
pix = read_bytes(s, 16384, 6144)
same = sum(pix[off(c, y)] == src[off(c, y)]
           for y in range(CROP_ROW, CROP_ROW + CROP_H)
           for c in range(CROP_COL, CROP_COL + CROP_W))
# expect CROP_W * CROP_H while the graphic is on screen
```

## 8. Diagnosing common issues

- **`_frame` never advances**: hung in `vsync_wait()` — see the `floating-bus-vsync` skill.
- **PC stuck at 0x0038**: the ROM interrupt handler; expected while the program spins with interrupts enabled.
- **Blank screen after smartload**: the tape hasn't finished loading; wait 10+ s and check `get-registers` (PC < 0x8000 means still in ROM).
- **Screen looks right but the frame rate is bad**: verify in Fuse before optimising — ZEsarUX's floating bus emulation is sparser than real hardware, so `vsync_wait()` takes longer to lock (much improved in 13.1, but still not hardware-accurate).
- **The app presses its own keys**: see the Kempston false positive above.
- **The test hangs waiting for the title**: the boot logo and title tune
  block until a key. Wait for the program to have control (PC in
  `0x8000-0xBFFF`), THEN press — never during the load, which the ROM
  reads as BREAK and aborts.
- **The test passes but the bug is still there**: that is a test-design
  problem, not a driving problem. See `.claude/skills/test-design`, which
  is about instruments that cannot fail.
