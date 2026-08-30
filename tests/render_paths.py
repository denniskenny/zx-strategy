"""Render-path acceptance: prove BOTH machines actually put pixels up.

`src/render.c` is the one file that behaves differently on a 48K and a
128K (docs/DESIGN.md § Two machines, two render paths), so it is the one
thing that needs testing twice.  This walks title -> play -> overview ->
play on each machine in turn — three screens, not ten levels.  The
campaign loop is p0_state_walk.py's job and there is no reason to pay
for it twice.

    make map
    python3 tests/render_paths.py          # launches both emulators itself

--- Why this test exists ----------------------------------------------

A bug shipped that p0_state_walk.py could not have caught: on a 128K,
every full screen was composed into a RAM bank the ULA was not
displaying, so the game rendered nothing and then hung.  Two blind spots
let it through, and this file is built around closing them:

  1. **p0 only reads `game_state`.** A program can march through every
     state in the campaign with the screen blank.  So every check here
     is against SCREEN MEMORY — is there ink, is it the right colour, is
     it in the right place.

  2. **On a 128K, 0x4000 is not necessarily what you are looking at.**
     The original "verification" read back the buffer it had just
     written and declared success, which proved only that the write
     happened.  `displayed()` below picks the bank from bit 3 of
     `page_reg`, so it reads what the ULA reads.

It also asserts the floating bus sync marker survives on both, because
the 128K path moves the screen out from under it — that is precisely how
the hang happened.
"""
import os
import re
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAP = os.path.join(ROOT, 'zxstrategy.tap')
MAPFILE = os.path.join(ROOT, 'zxstrategy.map')
ROMDIR = os.path.expanduser('~/projects/zesarux/src')

# id, rom, expect is_128k, ZRCP port.
#
# A port each, rather than sharing 10000.  Sharing cost an afternoon: the
# emulators run back to back, killing one does not free its port at once,
# and the second one then fails to bind while `connect()` cheerfully
# attaches to the corpse of the first.  Every check after that interrogates
# the previous machine, so the 128K "failed to render" when it had in fact
# never been launched at.  Separate ports make that unrepresentable.
MACHINES = [
    ('48k',  '48.rom',  0, 10000),
    ('128k', '128.rom', 1, 10001),
]

POLL = 0.01
ATTR_TITLE, ATTR_TEXT, ATTR_HINT, ATTR_BUSY = 0x45, 0x47, 0x46, 0x42
VSYNC_HALT, VSYNC_48K, VSYNC_128K = 0, 1, 2
VSYNC_NAME = {0: 'HALT fallback', 1: 'floating bus 0x40FF',
              2: 'floating bus 0x0FFD'}
VSYNC_MARKER = 0x03
MARKER_ROW = 22
TITLE, PLAY, MAP = 0, 1, 2

SYM = {}
for line in open(MAPFILE):
    m = re.match(r'(\S+)\s+=\s+\$([0-9A-Fa-f]+)\s*;', line)
    if m:
        SYM.setdefault(m.group(1), int(m.group(2), 16))


def sym(n):
    if '_' + n not in SYM:
        sys.exit(f"symbol _{n} not in {MAPFILE} — run `make map`")
    return SYM['_' + n]


# ----------------------------------------------------------------- ZRCP

def connect(port, tries=40):
    for _ in range(tries):
        try:
            s = socket.socket(); s.settimeout(25)
            s.connect(('localhost', port))
            b = b''
            while b'command>' not in b:
                b += s.recv(4096)
            return s
        except OSError:
            time.sleep(0.25)
    sys.exit(f'could not reach ZEsarUX on port {port}')


def cmd(s, c):
    s.sendall((c + '\n').encode()); r = b''
    while b'command>' not in r:
        r += s.recv(4096)
    t = r.decode('latin-1')
    return t[:t.rfind('command>')].strip()


def rd(s, a, n):
    out = b''
    while n:
        k = min(n, 512)
        x = cmd(s, f'read-memory {a} {k}').replace(' ', '') \
                                          .replace('\n', '').replace('\r', '')
        out += bytes.fromhex(x[:k * 2]); a += k; n -= k
    return out


def byte(s, name):
    return rd(s, sym(name), 1)[0]


# ------------------------------------------------------------ the screen

def displayed(s):
    """Base address of the display file the ULA is actually showing.

       On a 128K with the shadow path armed this is page 7 at 0xC000,
       not 0x4000.  Reading the wrong one is how a blank screen gets
       mistaken for a working one."""
    if byte(s, 'is_128k') and (byte(s, 'page_reg') & 0x08):
        return 0xC000
    return 0x4000


def attrs(s):
    return rd(s, displayed(s) + 6144, 768)


def pixels(s):
    return rd(s, displayed(s), 6144)


def ink(s):
    return sum(1 for b in pixels(s) if b)


def row_attr(a, row):
    return a[row * 32:(row + 1) * 32]


# ------------------------------------------------------------ the keys

KEY = {'SPACE': (7, 0), 'M': (7, 2)}
NONE = 'ffffffffffffffff00'


def io(s, k=None):
    if k is None:
        cmd(s, 'set-ui-io-ports ' + NONE); return
    i, b = KEY[k]
    rows = [0xFF] * 8
    rows[i] &= ~(1 << b) & 0xFF
    cmd(s, 'set-ui-io-ports ' + ''.join(f'{x:02x}' for x in rows) + '00')


def wait(cond, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(POLL)
    return False


def frames(s):
    return int.from_bytes(rd(s, 0x5C78, 2), 'little')


def wait_frames(s, n, timeout=0.5):
    was = frames(s); end = time.time() + timeout
    while ((frames(s) - was) & 0xFFFF) < n and time.time() < end:
        time.sleep(POLL)


def settle(s, was=None, tries=25):
    """Wait for the screen to stop changing, then return its attributes.

       Two separate hazards, both of which have bitten:

       `enter_state()` sets game_state BEFORE it paints, so sampling on
       the state change alone catches a half-drawn screen.

       And on a 128K the new screen appears ATOMICALLY, at the flip.
       Until then the old one is still up and perfectly stable, so "two
       equal samples" is satisfied by the screen we are trying to replace
       — the test then measures the previous state and cheerfully passes.
       That is why `was` exists: pass the attributes from before the
       action and this waits for them to change first.  Without it,
       ST_MAP was reporting ST_PLAY's pixels down to the byte."""
    if was is not None:
        for _ in range(tries):
            if attrs(s) != was:
                break
            wait_frames(s, 2)
    prev = attrs(s)
    for _ in range(tries):
        wait_frames(s, 3)
        cur = attrs(s)
        if cur == prev:
            return cur
        prev = cur
    return prev


def stop_tune(s):
    """The title tune blocks with interrupts off; one press ends it and
       is then flushed, so leaving the title always costs two."""
    hint = displayed(s) + 6144 + 21 * 32
    if rd(s, hint, 1)[0] != ATTR_BUSY:
        return
    io(s, 'SPACE')
    wait(lambda: rd(s, displayed(s) + 6144 + 21 * 32, 1)[0] != ATTR_BUSY, 5.0)
    io(s, None); wait_frames(s, 2)


def press_until(s, k, want, tries=10):
    for _ in range(tries):
        if want():
            return True
        io(s, k)
        ok = wait(want, 0.6)
        io(s, None)
        wait_frames(s, 2)       # the edge needs the bit low to re-arm
        if ok or want():
            return True
    return want()


# ------------------------------------------------------------- the test

def run(machine, rom, want_128k, port, results):
    rompath = os.path.join(ROMDIR, rom)
    if not os.path.isfile(rompath):
        results.append((machine, 'ROM missing: ' + rompath, False)); return

    emu = subprocess.Popen(
        ['zesarux', '--vo', 'null', '--ao', 'null', '--enable-remoteprotocol',
         '--remoteprotocol-port', str(port),
         '--machine', machine, '--noconfigfile', '--quickexit',
         '--accelerate-loading',
         '--romfile', rompath, TAP],
        # --accelerate-loading IS safe on a 128K.  This used to say it was
        # not -- that the load silently never completed and the machine sat
        # in the ROM with a blank screen.  Re-tested with the same tap on
        # the same machine, with and without: both reach the title screen
        # and both read page-7 attr 0x45.  Whatever was seen originally, it
        # was not the flag.
        #
        # "Blank screen" has many causes and this project has misattributed
        # it more than once -- to the flag here, and to the paging when a
        # 76-line BASIC loader was overrunning RAMTOP.  It is a symptom,
        # never a diagnosis.
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        preexec_fn=os.setsid)
    try:
        s = connect(port)

        def check(cond, msg):
            print(('  ok   ' if cond else '  FAIL ') + msg)
            results.append((machine, msg, bool(cond)))

        # The title paints ATTR_TITLE across row 0 and nothing else does,
        # so it is both "the tap loaded" and "rendering works at all".
        def titled():
            return row_attr(attrs(s), 0)[0] == ATTR_TITLE

        # The emulator autoloads the tap named on its command line, so
        # normally there is nothing to do but wait.  smartload is kept only
        # as a fallback, and is NOT retried in a loop: each one resets the
        # machine, so retrying guarantees the load never gets to finish.
        # 60 s covers a 50 KB tap even accelerated; the tape grew from
        # 24 KB when the cutscene screens arrived.
        # Two phases, and the ORDER MATTERS.
        #
        # 1. Wait passively for the tape.  NEVER press during a load: the
        #    ROM's loader watches for BREAK, so a harness that presses
        #    SPACE in a loop aborts the load, and every run then "fails to
        #    reach the title" with a perfectly good tap.  vsync_mode is
        #    zero until vsync_detect() runs, so it going non-zero means the
        #    program has control and the tape is done.
        #
        # 2. Then press, to get past the splash.  The boot logo is up with
        #    the title march over it, blocking until a key -- ST_TITLE does
        #    not paint on its own.
        # "Has the program started?" asked of the PROGRAM COUNTER, not of
        # memory.
        #
        # The first version read vsync_mode and waited for it to go
        # non-zero.  That address is inside the program's own BSS, so until
        # the last block loads it holds UNINITIALISED RAM -- which on the
        # 48K read as non-zero, so the test decided the game had booted and
        # started pressing SPACE in the middle of the tape.  The ROM's
        # loader treats that as BREAK and aborts, and the failure looks
        # exactly like "this machine cannot render".
        #
        # PC cannot lie the same way: while the tape runs it is in the ROM,
        # below 0x4000.  Once it is in 0x8000-0xBFFF the program has
        # control and pressing is safe.
        def booted():
            m = re.search(r'PC=([0-9a-fA-F]{4})', cmd(s, 'get-registers'))
            return bool(m) and 0x8000 <= int(m.group(1), 16) < 0xC000

        def to_title(budget):
            if not wait(booted, budget):
                return False
            end = time.time() + 45
            while time.time() < end:
                if titled():
                    return True
                io(s, 'SPACE')
                time.sleep(0.4)
                io(s, None)
                time.sleep(0.4)
            return titled()

        up = to_title(90)
        if not up:
            cmd(s, 'smartload ' + TAP)
            up = to_title(90)
        check(up, 'title screen appears (tap loaded and rendering)')
        if not up:
            return

        check(byte(s, 'is_128k') == want_128k,
              f'hw_detect reports is_128k={want_128k}')
        check(byte(s, 'game_state') == TITLE, 'boots to ST_TITLE')

        a = settle(s)
        check(all(v == ATTR_TITLE for v in row_attr(a, 0)),
              'ST_TITLE: row 0 is the title bar')
        check(all(v == ATTR_TEXT for v in row_attr(a, 3)),
              'ST_TITLE: the hardware report is painted')
        n = ink(s)
        check(n > 300, f'ST_TITLE: {n} ink bytes on the displayed screen')

        stop_tune(s)
        was = attrs(s)
        check(press_until(s, 'SPACE', lambda: byte(s, 'game_state') == PLAY),
              'SPACE starts a game')

        a = settle(s, was)
        view = [v for r in range(1, 17) for v in row_attr(a, r)]
        check(len(set(view)) > 3,
              f'ST_PLAY: the board is painted ({len(set(view))} colours in '
              'rows 1-16)')
        check(all(v == ATTR_HINT for v in row_attr(a, 21)),
              'ST_PLAY: the key legend is painted')
        n = ink(s)
        check(n > 500, f'ST_PLAY: {n} ink bytes on the displayed screen')

        was = attrs(s)
        check(press_until(s, 'M', lambda: byte(s, 'game_state') == MAP),
              'M opens the overview')
        a = settle(s, was)
        omap = [v for r in range(3, 17) for v in row_attr(a, r)]
        check(len(set(omap)) > 3,
              f'ST_MAP: the overview is painted ({len(set(omap))} colours)')
        n = ink(s)
        check(n > 500, f'ST_MAP: {n} ink bytes on the displayed screen')

        was = attrs(s)
        check(press_until(s, 'SPACE', lambda: byte(s, 'game_state') == PLAY),
              'SPACE returns to the board')
        settle(s, was)

        # --- vsync -------------------------------------------------
        # Which technique the machine settled on, and whether the marker
        # that technique depends on is actually on the screen the ULA is
        # reading.  The 128K path moves the display out from under the
        # sync, which is precisely how it hung the first time.
        vm = byte(s, 'vsync_mode')
        print(f'         (vsync: {VSYNC_NAME.get(vm, vm)})')
        check(vm in (VSYNC_HALT, VSYNC_48K, VSYNC_128K),
              f'vsync_detect settled on a known mode ({vm})')

        a = settle(s)
        if vm == VSYNC_HALT:
            # HALT does not use the marker and does not write it, so its
            # absence is correct rather than a fault.
            check(True, 'HALT sync: no marker expected')
        else:
            check(all(v == VSYNC_MARKER for v in row_attr(a, MARKER_ROW)),
                  'the floating bus sync marker survives on row 22')
            marker = int.from_bytes(rd(s, sym('vsync_marker_addr'), 2),
                                    'little')
            want = displayed(s) + 6144 + MARKER_ROW * 32
            check(marker == want,
                  'the marker is written to the screen being DISPLAYED '
                  f'(0x{marker:04X}, want 0x{want:04X})')
        stray = [i for i, v in enumerate(a)
                 if v in (VSYNC_MARKER, VSYNC_MARKER & 0xFE)
                 and i // 32 != MARKER_ROW]
        check(not stray,
              f'no stray 0x02/0x03 elsewhere on screen ({len(stray)} found)')

        if want_128k:
            print('         (shadow path: shadow_ok=%d page_reg=0x%02X)'
                  % (byte(s, 'shadow_ok'), byte(s, 'page_reg')))
    finally:
        try:
            os.killpg(os.getpgid(emu.pid), signal.SIGKILL)
        except Exception:
            pass
        try:
            emu.wait(timeout=10)
        except Exception:
            pass


def main():
    if not os.path.isfile(MAPFILE):
        sys.exit('no zxstrategy.map — run `make map` first')
    t0 = time.time()
    results = []
    for machine, rom, want, port in MACHINES:
        print(f'--- {machine} ---')
        run(machine, rom, want, port, results)
    bad = [r for r in results if not r[2]]
    print(f'\nRENDER PATHS: {"PASS" if not bad else f"FAIL ({len(bad)})"}'
          f'   [{time.time() - t0:.1f}s]')
    for m, msg, _ in bad:
        print(f'  {m}: {msg}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
