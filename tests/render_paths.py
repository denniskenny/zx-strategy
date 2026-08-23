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


def settle(s, tries=25):
    """Wait until the screen stops changing, then return its attributes.

       Necessary because `enter_state()` sets game_state BEFORE it paints,
       and because render_tick() finishes page flips over several frames.
       Sampling on the state change alone catches a half-drawn screen and
       reports it as a rendering fault — which is what the first draft of
       this test did, on both machines at once."""
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
         '--romfile', rompath, TAP],
        # NO --accelerate-loading.  It speeds the tape up on a 48K, but on
        # a 128K the load silently never completes and the machine sits in
        # the ROM with a blank screen — indistinguishable from here from
        # "this machine cannot render", which is what it got blamed for.
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
        up = wait(titled, 30)
        if not up:
            cmd(s, 'smartload ' + TAP)
            up = wait(titled, 30)
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
        check(press_until(s, 'SPACE', lambda: byte(s, 'game_state') == PLAY),
              'SPACE starts a game')

        a = settle(s)
        view = [v for r in range(1, 17) for v in row_attr(a, r)]
        check(len(set(view)) > 3,
              f'ST_PLAY: the board is painted ({len(set(view))} colours in '
              'rows 1-16)')
        check(all(v == ATTR_HINT for v in row_attr(a, 21)),
              'ST_PLAY: the key legend is painted')
        n = ink(s)
        check(n > 500, f'ST_PLAY: {n} ink bytes on the displayed screen')

        check(press_until(s, 'M', lambda: byte(s, 'game_state') == MAP),
              'M opens the overview')
        a = settle(s)
        omap = [v for r in range(3, 17) for v in row_attr(a, r)]
        check(len(set(omap)) > 3,
              f'ST_MAP: the overview is painted ({len(set(omap))} colours)')
        n = ink(s)
        check(n > 500, f'ST_MAP: {n} ink bytes on the displayed screen')

        check(press_until(s, 'SPACE', lambda: byte(s, 'game_state') == PLAY),
              'SPACE returns to the board')

        # The 128K path moves the screen out from under the floating bus
        # sync.  If the marker is gone or duplicated, vsync_wait() either
        # hangs or locks to the wrong row.
        a = settle(s)
        check(all(v == VSYNC_MARKER for v in row_attr(a, MARKER_ROW)),
              'the floating bus sync marker survives on row 22')
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
