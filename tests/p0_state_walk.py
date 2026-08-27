"""P0 acceptance: walk the whole campaign, verify every level loads.

Drives the built .tap through title -> play -> ST_OVER -> next level ->
... -> ST_WON -> title using the DEBUG_STATE_WALK keys (W wins a level,
L loses it), and checks terrain[] against the .tmx at levels 1, 5 and 10.

    make DEBUG_KEYS=1 map         # the W/L keys, and the symbol map

That tap is 94 bytes over the 0xC000 ceiling and so is 48K-only; a 128K
would bank page 7 over its tail.  This is a 48K test, so that is fine —
but do not hand that tap to a 128K.  See the Makefile.
    zesarux --vo null --ao null --enable-remoteprotocol --machine 48k \
        --noconfigfile --quickexit --accelerate-loading \
        --joystickemulated Kempston \
        --romfile ~/projects/zesarux/src/48.rom \
        $PWD/zxstrategy.tap &
    python3 tests/p0_state_walk.py

--joystickemulated Kempston IS REQUIRED.  The walk runs twice, once on
the keyboard and once on the joystick, and without it hw_detect() finds
no Kempston -- so scan_input() never reads the port and the joystick pass
silently tests nothing at all.  It does not fail; it just never presses
anything.

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
These were measured, so do not re-tune them on a hunch:

  | change | total |
  |---|---|
  | as shipped (POLL 0.01, PRESS_WAIT 0.6) | 19.7-21.1s |
  | POLL 0.003 | 21.0s |
  | PRESS_WAIT 0.25 | 20.0s |

  All within run-to-run noise. **The harness is not the bottleneck** —
  the ~20s is the emulated application: ten levels loading and
  rendering at roughly 50 fps.  Neither knob is timing out, which also
  means presses are landing on the first try.

  ZEsarUX offers no way out either: `get-cpu-turbo-speed` is read-only
  over ZRCP, there is no `set-`, and `TopSpeed` exists only as an F-key
  action.  A real speed-up has to come from the walk visiting fewer
  levels, not from tuning waits.

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


def _mapaddr(name):
    out = subprocess.run(['grep', '-E', f'^_{name}\\b', 'zxstrategy.map'],
                         capture_output=True, text=True).stdout.split()
    return int(out[2].lstrip('$'), 16) if out else None


def sym(name):
    """Address of a symbol, following an `at_` locator if there is one.

       Arrays placed by hand in the RAM below the program (see
       include/memmap.h) have no symbol of their own, because the linker
       never saw them.  src/logic.c exports `at_<name>` pointers for
       exactly this, so a lookup that misses falls through to reading the
       pointer out of the running machine."""
    a = _mapaddr(name)
    if a is not None:
        return a
    a = _mapaddr('at_' + name)
    if a is None:
        sys.exit(f"symbol _{name} not found — run `make map` first")
    return int.from_bytes(rd(_S[0], a, 2), 'little')


_S = [None]



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


# Keyboard: (half-row index, bit), active LOW.
KEY = {'W': (2, 1), 'L': (6, 1), 'SPACE': (7, 0),
       'ENTER': (6, 0), 'X': (0, 2)}

# Kempston: the ninth byte of set-ui-io-ports, active HIGH.
KEMP = {'FIRE1': 0x10, 'FIRE2': 0x20}

NONE = 'ffffffffffffffff00'


def io(s, k=None):
    """Assert one key or one joystick button, or release everything.

       Both paths go through the same call because the game folds them
       into one action byte -- so the test can drive ACTION as SPACE or
       as fire 1 and the code under test cannot tell which it was.  That
       is the property worth testing: docs/DESIGN.md § Action and Cancel
       promises a joystick can play the whole game."""
    if k is None:
        cmd(s, 'set-ui-io-ports ' + NONE)
        return
    rows = [0xFF] * 8
    kemp = 0
    if k in KEMP:
        kemp = KEMP[k]
    else:
        i, b = KEY[k]
        rows[i] &= ~(1 << b) & 0xFF
    cmd(s, 'set-ui-io-ports '
        + ''.join(f'{x:02x}' for x in rows) + f'{kemp:02x}')


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


def confirming(s):
    """game.c's `confirm`: 0 none, 1 end turn, 2 quit.

       Read from the symbol rather than by matching pixels on the hint
       row -- the row holds a bitmap, not text, and a test that decodes
       the font would break every time the font moved.  What matters is
       that the game is ASKING; the wording is a rendering detail."""
    return rd(s, CONFIRM, 1)[0]


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
_S[0] = s

# No smartload: the emulator autoloads the tap named on its command line,
# and smartload RESETS the machine.  Issuing one and then polling for the
# title matches the pre-reset screen still sitting in memory, declares
# success in a fraction of a second and then drives a machine that is
# busy rebooting — every check after that fails for no visible reason.
#
# `level` is the "the program is running" signal: game_run() sets it to 1
# before painting anything, and unlike the screen it cannot be left over
# from a previous run.
#
# NOT vsync_mode, which this used to watch — VSYNC_MODE_HALT is 0, so a
# machine that legitimately falls back to HALT sync never satisfies it and
# the test hangs for a minute before claiming the tap did not load.
LV = sym('level')
if not wait(lambda: rd(s, LV, 1)[0] != 0, 60):
    sys.exit("timed out waiting for the program to start — did the tap load?")
print(f"booted in {time.time() - t0:.1f}s")

GS, TER = sym('game_state'), sym('terrain')
LVL, WON = sym('level'), sym('player_won')
CONFIRM = sym('confirm')     # 0 none, 1 end turn, 2 quit
SELECTED = sym('selected')   # NO_UNIT when nothing is held

# level / turn / player_won are contiguous, so one read covers all three.
BLK = min(LVL, WON)
BLK_LEN = max(LVL, WON) - BLK + 1

# The device this pass is using.  docs/DESIGN.md § Action and Cancel says
# SPACE/fire 1 ACT and ENTER/fire 2 CANCEL on every screen, so the entire
# walk has to pass on either -- not merely "the joystick is wired up".
# CANCEL is ENTER on both, and that is not laziness.  A standard Kempston
# has ONE button: bits 0-4 are right/left/down/up/fire, and bit 5 is a
# non-standard extension that most hardware -- and ZEsarUX's emulated
# Kempston -- does not provide.  src/input.c reads it, so a two-button
# stick works, but nothing may DEPEND on it.
#
# Discovered by this test: the joystick pass could select a unit and never
# put it down, because Cancel was unreachable.  See docs/DESIGN.md
# § Action and Cancel.
DEVICES = {'keyboard': ('SPACE', 'ENTER'), 'kempston': ('FIRE1', 'ENTER')}

fails = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


# One pass, ALTERNATING device per level.
#
# Running the whole walk twice doubled the wall clock to prove a property
# that does not depend on which level it is tested on: the game folds
# keyboard and joystick into one action byte long before any state sees
# them, so if a level advances on fire 1 it would have advanced on SPACE.
# Alternating covers both paths across the ten levels for the price of one
# pass, and if one device breaks, every level using it fails -- five loud
# failures, not a silent gap.
def device(lvl):
    return DEVICES['keyboard' if lvl % 2 else 'kempston']


ACTION, CANCEL = device(1)

check(st(s) == TITLE, f"boots to TITLE (got {ST.get(st(s))})")
stop_tune(s)
press_until(s, ACTION, lambda: st(s) == PLAY)
for lvl in range(1, 11):
    ACTION, CANCEL = device(lvl)
    dev = 'keyboard' if lvl % 2 else 'kempston'
    level, _ = blk(s)
    check(st(s) == PLAY and level == lvl,
          f"level {lvl}: PLAY, level counter = {level}  [{dev}]")
    if lvl in (1, 5, 10):
        check(list(rd(s, TER, 98)) == expected(lvl),
              f"level {lvl}: terrain[] matches assets/maps/level_{lvl}.tmx")

    # The Ladder on this level's device: CANCEL with nothing held ASKS,
    # and the answer is the ordinary ACTION/CANCEL pair.  Both answers are
    # exercised: NO first, so a bug that ends the turn regardless shows up
    # as the level advancing when it should not have.
    # CANCEL is a LADDER: it may have a unit to put down before it
    # reaches the turn, so press until the question appears rather than
    # assuming one press gets there.  Three is the ladder's full depth.
    #
    # ANSWERED NO, every level.  Saying yes would hand over to the enemy
    # and the rest of this level's checks would race its turn -- so the
    # destructive answer is tested once, below, where there is nothing
    # left to disturb.
    for _ in range(3):
        if confirming(s) == 1:
            break
        io(s, CANCEL)
        wait_frames(s, 3)
        io(s, None)
        wait_frames(s, 3)
    check(confirming(s) == 1,
          f"level {lvl}: CANCEL reaches the end-turn question  [{dev}]"
          f" (confirm={confirming(s)} sel={rd(s, SELECTED, 1)[0]})")

    io(s, CANCEL)                       # ...NO
    wait_frames(s, 3)
    io(s, None)
    wait_frames(s, 3)
    check(confirming(s) == 0 and st(s) == PLAY and blk(s)[0] == lvl,
          f"level {lvl}: NO leaves the turn alone  [{dev}]")

    press_until(s, 'W', lambda: st(s) == OVER)
    check(st(s) == OVER and blk(s)[1] == 1,
          f"level {lvl}: W -> ST_OVER, player_won=1")
    want = WON_ST if lvl == 10 else PLAY
    press_until(s, ACTION, lambda: st(s) == want)
check(st(s) == WON_ST, f"winning level 10 -> ST_WON (got {ST.get(st(s))})")
press_until(s, ACTION, lambda: st(s) == TITLE)
check(st(s) == TITLE, "ACTION from ST_WON -> ST_TITLE")

# X leaves a level, and is NOT a rung on the ladder.  It asks too, so a
# stray X cannot throw a game away.
stop_tune(s)
press_until(s, 'SPACE', lambda: st(s) == PLAY)
io(s, 'X')
wait_frames(s, 3)
io(s, None)
wait_frames(s, 3)
check(confirming(s) == 2 and st(s) == PLAY, "X asks before quitting")
press_until(s, 'SPACE', lambda: st(s) == TITLE)
check(st(s) == TITLE, "YES to the quit question leaves the level")

stop_tune(s)
press_until(s, 'SPACE', lambda: st(s) == PLAY)
press_until(s, 'W', lambda: st(s) == OVER)
stop_tune(s)
press_until(s, 'FIRE1', lambda: st(s) == PLAY)
check(st(s) == PLAY and blk(s)[0] == 2, f"restart, win once -> level {blk(s)[0]}")
press_until(s, 'L', lambda: st(s) == OVER)
check(st(s) == OVER and blk(s)[1] == 0, "L -> ST_OVER with player_won=0")
press_until(s, 'FIRE1', lambda: st(s) == TITLE)
check(st(s) == TITLE, "a loss returns to ST_TITLE")
stop_tune(s)
press_until(s, 'SPACE', lambda: st(s) == PLAY)
check(st(s) == PLAY and blk(s)[0] == 1, f"a new game restarts at level {blk(s)[0]}")

# YES to the end-turn question, once: it is the destructive answer, so it
# is tested where there is nothing after it to disturb.
for _ in range(3):
    if confirming(s) == 1:
        break
    io(s, 'ENTER')
    wait_frames(s, 3)
    io(s, None)
    wait_frames(s, 3)
check(confirming(s) == 1, "the end-turn question comes up")
press_until(s, 'SPACE', lambda: confirming(s) == 0)
check(confirming(s) == 0 and st(s) == PLAY,
      "YES ends the turn and hands over to the enemy")

print(f"\nP0 ACCEPTANCE: {'PASS' if not fails else f'FAIL ({len(fails)})'}"
      f"   [{time.time() - t0:.1f}s]")
sys.exit(1 if fails else 0)
