"""P0 acceptance: walk the whole campaign, verify every level loads.

Drives the built .tap through title -> play -> ST_OVER -> next level ->
... -> ST_WON -> title using the DEBUG_STATE_WALK keys (W wins a level,
L loses it), and checks terrain[] against the .tmx at levels 1, 5 and 10.

    make map                      # symbols are read from zxstrategy.map
    zesarux --vo null --ao null --enable-remoteprotocol --machine 48k \
        --noconfigfile --quickexit --accelerate-loading \
        --romfile ~/projects/zesarux/src/48.rom \
        $PWD/zxstrategy.tap &
    python3 tests/p0_state_walk.py

`--accelerate-loading` is worth the flag: the tape is most of the wall
clock otherwise.  No sleep is needed before starting — the script waits
for the title screen to appear by itself.

Retire this with DEBUG_STATE_WALK in P4.  See docs/PLAN.md.

--- On being fast ---------------------------------------------------

Headless ZEsarUX runs well under 50 fps and its speed varies with the
host, so every wait here is a POLL, never a sleep.  Two habits do most
of the work:

  * A key is HELD until the app reacts, then released — rather than
    held for a guessed interval and hoped about.  The app acts on the
    rising edge of a bit seen in two consecutive frames, so holding
    longer than that is free and holding too briefly loses the press.
  * State is read in as few round trips as possible.  `level`, `turn`
    and `player_won` are adjacent in RAM, so one read fetches all three.

The one unavoidable pause is the title tune, which blocks with
interrupts off until a key is pressed: the first press stops it and is
then flushed, so starting a game always costs two presses.
"""
import re
import socket
import subprocess
import sys
import time

POLL = 0.01          # between ZRCP samples while waiting
PRESS_WAIT = 0.6     # how long to hold a key before re-pressing
TRIES = 10           # re-presses before giving up on a transition


def sym(name):
    out = subprocess.run(['grep', '-E', f'^_{name}\\b', 'zxstrategy.map'],
                         capture_output=True, text=True).stdout.split()
    if not out:
        sys.exit(f"symbol _{name} not found — run `make map` first")
    return int(out[2].lstrip('$'), 16)          # addresses from the fresh map


GS, TER = sym('game_state'), sym('terrain')
LVL, WON = sym('level'), sym('player_won')

# level / turn / player_won are contiguous, so one read covers all three.
BLK = min(LVL, WON)
BLK_LEN = max(LVL, WON) - BLK + 1

# Keep these in step with include/game.h.  Named, not inlined: the ids
# shift whenever a state is added or removed, and bare numbers in the
# assertions below survive that silently.
TITLE, PLAY, MAP, OVER, WON_ST = 0, 1, 2, 3, 4
ST = {TITLE: "TITLE", PLAY: "PLAY", MAP: "MAP", OVER: "OVER", WON_ST: "WON"}


def connect():
    s = socket.socket(); s.settimeout(25); s.connect(('localhost', 10000))
    b = b''
    while b'command>' not in b:
        b += s.recv(4096)
    return s


def cmd(s, c):
    s.sendall((c + '\n').encode()); r = b''
    while b'command>' not in r:
        r += s.recv(4096)
    t = r.decode('latin-1')
    return t[:t.rfind('command>')].strip()


def rd(s, a, n):
    x = cmd(s, f'read-memory {a} {n}').replace(' ', '').replace('\n', '') \
                                      .replace('\r', '')
    return bytes.fromhex(x[:n * 2])


KEY = {'W': (2, 1), 'L': (6, 1), 'SPACE': (7, 0)}
NONE = 'ffffffffffffffff00'


def io(s, k=None):
    """Assert one key, or release everything when k is None."""
    if k is None:
        cmd(s, 'set-ui-io-ports ' + NONE)
        return
    i, b = KEY[k]
    rows = [0xFF] * 8
    rows[i] &= ~(1 << b) & 0xFF
    cmd(s, 'set-ui-io-ports ' + ''.join(f'{x:02x}' for x in rows) + '00')


def frames(s):
    """The ROM's FRAMES counter at 0x5C78, ticked by the 50 Hz interrupt.
       A real frame clock, which matters because headless ZEsarUX does
       not run at 50 fps and no sleep here can guess its rate.  It stops
       while the title tune plays — that runs with interrupts off — so
       every wait on it is bounded."""
    return int.from_bytes(rd(s, 0x5C78, 2), 'little')


def wait_frames(s, n, timeout=0.5):
    was = frames(s)
    end = time.time() + timeout
    while ((frames(s) - was) & 0xFFFF) < n and time.time() < end:
        time.sleep(POLL)


def wait(cond, timeout):
    """Poll cond() until true or the clock runs out."""
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(POLL)
    return False


HINT_ROW = 0x5800 + 21 * 32     # the row a long operation borrows
ATTR_BUSY = 0x42                # ...and the colour it borrows it in


def tune_playing(s):
    return rd(s, HINT_ROW, 1)[0] == ATTR_BUSY


def stop_tune(s):
    """The title plays its tune on entry, blocking with interrupts off
       until a key arrives — so the frame clock stops and the first
       press is swallowed stopping it.  Waiting that out generically
       costs seconds per title visit and there are three of them, so
       spend one press deliberately and watch the banner clear."""
    if not tune_playing(s):
        return
    io(s, 'SPACE')
    wait(lambda: not tune_playing(s), 5.0)
    io(s, None)
    wait_frames(s, 2)


def press_until(s, k, want, tries=TRIES):
    """Hold k until the app reacts, release, repeat if it did not.

       An edge fires once per press-release cycle, so a repeat is safe
       when nothing happened — which is exactly what the first press on
       the title screen does, where it only stops the tune."""
    for _ in range(tries):
        if want():
            return True
        io(s, k)
        ok = wait(want, PRESS_WAIT)
        io(s, None)
        # The edge detector needs the bit LOW again before it will fire
        # a second time.  Releasing and pressing straight away loses the
        # press, which is slow AND wrong: it retries until it happens to
        # land, and the retries are what a fixed-delay version was
        # accidentally paying for.
        wait_frames(s, 2)
        if ok or want():
            return True
    return want()


st = lambda s: rd(s, GS, 1)[0]


def blk(s):
    """level, turn, player_won in one round trip."""
    b = rd(s, BLK, BLK_LEN)
    return b[LVL - BLK], b[WON - BLK]


def expected(n):
    csv = re.search(r'<data encoding="csv">\s*(.*?)\s*</data>',
                    open(f'assets/maps/level_{n}.tmx').read(), re.S).group(1)
    return [int(v) - 1 for v in csv.replace('\n', '').split(',') if v.strip()]


t0 = time.time()
s = connect()
cmd(s, 'smartload /Users/Kennyd/projects/zx-strategy/zxstrategy.tap')

# The title screen paints ATTR_TITLE across row 0; nothing else does, so
# it is the cheapest "the program is up and running" signal there is.
if not wait(lambda: rd(s, 0x5800, 1)[0] == 0x45, 40):
    sys.exit("timed out waiting for the title screen — did the tap load?")
print(f"booted in {time.time() - t0:.1f}s")

fails = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


check(st(s) == TITLE, f"boots to TITLE (got {ST.get(st(s))})")
stop_tune(s)
press_until(s, 'SPACE', lambda: st(s) == PLAY)
for lvl in range(1, 11):
    level, _ = blk(s)
    check(st(s) == PLAY and level == lvl,
          f"level {lvl}: PLAY, level counter = {level}")
    if lvl in (1, 5, 10):
        check(list(rd(s, TER, 98)) == expected(lvl),
              f"level {lvl}: terrain[] matches assets/maps/level_{lvl}.tmx")
    press_until(s, 'W', lambda: st(s) == OVER)
    check(st(s) == OVER and blk(s)[1] == 1,
          f"level {lvl}: W -> ST_OVER, player_won=1")
    want = WON_ST if lvl == 10 else PLAY
    press_until(s, 'SPACE', lambda: st(s) == want)
check(st(s) == WON_ST, f"winning level 10 -> ST_WON (got {ST.get(st(s))})")
press_until(s, 'SPACE', lambda: st(s) == TITLE)
check(st(s) == TITLE, "any key from ST_WON -> ST_TITLE")

stop_tune(s)
press_until(s, 'SPACE', lambda: st(s) == PLAY)
press_until(s, 'W', lambda: st(s) == OVER)
stop_tune(s)
press_until(s, 'SPACE', lambda: st(s) == PLAY)
check(st(s) == PLAY and blk(s)[0] == 2, f"restart, win once -> level {blk(s)[0]}")
press_until(s, 'L', lambda: st(s) == OVER)
check(st(s) == OVER and blk(s)[1] == 0, "L -> ST_OVER with player_won=0")
press_until(s, 'SPACE', lambda: st(s) == TITLE)
check(st(s) == TITLE, "a loss returns to ST_TITLE")
stop_tune(s)
press_until(s, 'SPACE', lambda: st(s) == PLAY)
check(st(s) == PLAY and blk(s)[0] == 1, f"a new game restarts at level {blk(s)[0]}")

print(f"\nP0 ACCEPTANCE: {'PASS' if not fails else f'FAIL ({len(fails)})'}"
      f"   [{time.time() - t0:.1f}s]")
sys.exit(1 if fails else 0)
