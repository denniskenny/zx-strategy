#!/usr/bin/env python3
"""Golden-master PIXEL test for the blit paths.

    make FREEZE_ANIM=1 map
    python3 tests/pixel_hash.py            # compare against the baseline
    python3 tests/pixel_hash.py --bless    # record a new baseline

WHY THIS EXISTS
    Neither existing suite reads pixels.  render_paths.py checks attribute
    colours and p0_state_walk.py checks state, so a blit that writes the
    WRONG PIXELS to the RIGHT CELLS with the RIGHT COLOURS passes both of
    them.  Every assembly conversion in src/render.c is exactly that kind
    of change.

    A first attempt hashed the screen from a normal build and compared C
    against assembly.  The hashes differed -- and then the SAME build
    hashed differently twice, because the at-rest animation flips every
    sprite every ~18 frames and the capture lands wherever it lands.
    FREEZE_ANIM=1 removes that variable; without it this test is noise.

WHAT IT CAPTURES
    The play board only -- the top two thirds of the bitmap, which is the
    32x16-character view.  The bottom third is the status panel and the
    hint row, which carry text and the floating-bus marker and are not
    what a blit change touches.

WHEN IT FAILS
    Either the blit changed or the artwork did.  If the ART changed on
    purpose, re-bless.  If it did not, the blit is wrong -- and this is
    the only test that will tell you.
"""

import hashlib
import os
import re
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TAP = os.path.join(ROOT, 'zxstrategy.tap')
MAPFILE = os.path.join(ROOT, 'zxstrategy.map')
BASELINE = os.path.join(HERE, 'pixel_baseline.txt')
ROM = os.path.expanduser('~/projects/zesarux/src/48.rom')
PORT = 10000

BLESS = '--bless' in sys.argv


def sym(name):
    for line in open(MAPFILE):
        g = re.match(r'^_%s\s+=\s+\$([0-9A-F]+)' % name, line)
        if g:
            return int(g.group(1), 16)
    sys.exit('pixel_hash: no symbol _%s in the map -- build with `map`' % name)


def main():
    if not os.path.exists(TAP) or not os.path.exists(MAPFILE):
        sys.exit('pixel_hash: build first -- make FREEZE_ANIM=1 map')

    emu = subprocess.Popen(
        ['zesarux', '--vo', 'null', '--ao', 'null', '--enable-remoteprotocol',
         '--machine', '48k', '--noconfigfile', '--quickexit',
         '--accelerate-loading', '--romfile', ROM, TAP],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        preexec_fn=os.setsid)
    try:
        s = None
        for _ in range(90):
            try:
                s = socket.create_connection(('127.0.0.1', PORT), timeout=5)
                break
            except Exception:
                time.sleep(1)
        if s is None:
            sys.exit('pixel_hash: no ZRCP connection')

        def cmd(c):
            s.sendall((c + '\n').encode())
            time.sleep(0.25)
            out = b''
            s.settimeout(4)
            try:
                while True:
                    d = s.recv(200000)
                    if not d:
                        break
                    out += d
                    if out.rstrip().endswith(b'command>'):
                        break
            except socket.timeout:
                pass
            return out.decode('latin-1').replace('command>', '').strip()

        cmd('')

        def rd(addr, n):
            x = cmd('read-memory %d %d' % (addr, n))
            x = x.replace(' ', '').replace('\n', '').replace('\r', '')
            return bytes.fromhex(x[:n * 2])

        GS = sym('game_state')

        def io(key=None):
            rows = [0xFF] * 8
            if key is not None:
                i, b = key
                rows[i] &= ~(1 << b) & 0xFF
            cmd('set-ui-io-ports ' + ''.join('%02x' % x for x in rows) + '00')

        # Press and release until the state moves, never a fixed wait: the
        # ROM's frame counter stops during the title tune and every other
        # blocking operation, so "wait N frames" is unbounded.  Same lesson
        # as tests/p0_state_walk.py.
        for _ in range(90):
            if rd(GS, 1)[0] == 0:
                break
            time.sleep(0.4)
        for _ in range(60):
            if rd(GS, 1)[0] == 1:
                break
            io((7, 0))          # SPACE
            time.sleep(0.6)
            io(None)
            time.sleep(0.6)
        if rd(GS, 1)[0] != 1:
            sys.exit('pixel_hash: never reached ST_PLAY')

        time.sleep(1.0)         # let the first full repaint settle

        # SELECT A UNIT, and this is not optional.
        #
        # Entering ST_PLAY paints the whole board through present_all().
        # A dirty cell goes through present_cell() instead -- a different
        # blit, on a different path -- and the first version of this test
        # captured only the full repaint.  A deliberate one-byte bug in
        # present_cell() left the hash IDENTICAL, which is how that gap
        # was found.
        #
        # The cursor starts on the player's base, so one ACTION press
        # selects it, marks its cell dirty and sends it through
        # present_cell().  Both blits are now in the hash.
        io((7, 0))              # SPACE
        time.sleep(0.5)
        io(None)
        time.sleep(1.0)

        blob = rd(0x4000, 0x800) + rd(0x4800, 0x800)
        got = hashlib.sha1(blob).hexdigest()
        nz = sum(1 for b in blob if b)
    finally:
        os.killpg(os.getpgid(emu.pid), signal.SIGKILL)

    if nz < len(blob) // 8:
        sys.exit('pixel_hash: only %d/%d non-zero bytes -- the board looks '
                 'blank, so the capture is wrong, not the blit'
                 % (nz, len(blob)))

    if BLESS:
        open(BASELINE, 'w').write(got + '\n')
        print('pixel_hash: baseline recorded  %s  (%d non-zero bytes)'
              % (got, nz))
        return 0

    if not os.path.exists(BASELINE):
        sys.exit('pixel_hash: no baseline -- run with --bless first')
    want = open(BASELINE).read().strip()
    if got == want:
        print('PIXEL HASH: PASS  %s' % got)
        return 0
    print('PIXEL HASH: FAIL')
    print('  baseline %s' % want)
    print('  got      %s   (%d non-zero bytes)' % (got, nz))
    print('  The board draws different PIXELS than the baseline.  If the')
    print('  artwork changed on purpose, re-bless; otherwise a blit is wrong.')
    return 1


sys.exit(main())
