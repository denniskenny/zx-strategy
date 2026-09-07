#!/usr/bin/env python3
"""anim_paths.py — the at-rest animation beat, which nothing else tests.

    python3 tests/anim_paths.py

WHY THIS EXISTS

tests/pixel_hash.py is built on FREEZE_ANIM=1, because a screen hash is
impossible while anything animates.  That makes animate() invisible to
it: the one path that repaints cells every ~18 frames, for the whole
game, has had no coverage at all.  Three separate rendering bugs this
project has shipped were in cells being repainted; the beat is the most
frequent repainter there is.

WHAT IT ASSERTS

After a beat, for every cell of the view:

    the DISPLAYED screen  == VBUF
    the OTHER screen      == VBUF

VBUF is what the compose step built, and present copies it to the screen,
so a beat that composes a cell and fails to present it -- or presents to
one screen and not the other, which is exactly the ghosting that took a
session to find -- shows up as a mismatch.

Attributes are compared too, with the cursor's own cell excluded: it is
washed with ATTR_CURSOR after the present, so it is legitimately not
VATTR.

HOW IT AVOIDS RACING THE THING IT MEASURES

Two things are needed, and the first alone is not enough.

ZRCP reads are slow -- 4 KB is seconds -- and the beat fires every 18
frames, so a naive read would capture a screen from before a beat and a
VBUF from after.  `enter-cpu-step` FREEZES the CPU and everything is read
with the machine stopped.

But it freezes at whatever instruction happens to be next, INCLUDING
half way through a blit.  The first run of this test reported that 11 of
128 rows on the non-displayed screen disagreed with VBUF -- which is not
a bug, it is a present_cell() caught 21 rows in.  So the freeze is
retried until the PC is parked in vsync_wait(), where the program spends
most of its time and no repaint is in flight.

Without that second step this test reports its own timing as a rendering
fault, which is the failure mode it exists to rule out.

DETECTION IS NOT CERTAIN, and that is worth knowing before trusting a
pass.  Reintroducing the one-screen present -- the actual ghost bug --
was caught on ONE of three beats, not all three: any full present from
another path resyncs the screens and wipes the divergence before the next
beat is inspected.  So this is a net that catches a persistent fault
quickly and an intermittent one eventually, not a proof.

Raising BEATS improves the odds at about 23 seconds a beat.
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
MAP = os.path.join(ROOT, 'zxstrategy.map')
ROM = os.path.expanduser('~/projects/zesarux/src/128.rom')
ZESARUX = '/usr/local/bin/zesarux'

VBUF, VATTR = 0xDB00, 0xEB00        # from include/memmap.h
VIEW_ROW, VIEW_COLS, VIEW_CW, VIEW_CH = 1, 8, 4, 4
VIEW_PX_ROWS = 128
CURSOR_VX, CURSOR_VY = 3, 1

fails = []


def check(ok, what):
    print('  %-4s %s' % ('ok' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


def sym(name):
    for line in open(MAP):
        if line.startswith('_' + name + ' '):
            return int(line.split('$')[1][:4], 16)
    sys.exit('anim_paths: no symbol _%s -- run `make map` first' % name)


def scr_off(y):
    """Display-file offset of pixel row y, column 0."""
    third = y // 64
    line = (y // 8) % 8
    within = y % 8
    return third * 0x800 + within * 0x100 + line * 0x20


def main():
    if not os.path.exists(MAP):
        sys.exit('anim_paths: no %s -- run `make map`' % MAP)

    emu = subprocess.Popen(
        [ZESARUX, '--vo', 'null', '--ao', 'null', '--enable-remoteprotocol',
         '--machine', '128k', '--noconfigfile', '--quickexit',
         '--accelerate-loading', '--romfile', ROM, TAP],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        preexec_fn=os.setsid)
    t0 = time.time()
    try:
        s = None
        for _ in range(90):
            try:
                s = socket.create_connection(('127.0.0.1', 10000), timeout=5)
                break
            except Exception:
                time.sleep(1)
        if s is None:
            sys.exit('anim_paths: no ZRCP connection')

        def cmd(c):
            s.sendall((c + '\n').encode())
            time.sleep(0.06)
            out = b''
            s.settimeout(10)
            try:
                while True:
                    d = s.recv(400000)
                    if not d:
                        break
                    out += d
                    # BOTH prompts: `enter-cpu-step` changes it to
                    # "command@cpu-step>", which does not end in
                    # "command>" and hung the first version of this.
                    if out.rstrip().endswith(b'>'):
                        break
            except socket.timeout:
                pass
            return re.sub(r'command(@cpu-step)?>', '',
                          out.decode('latin-1')).strip()

        cmd('')

        def rd(addr, n):
            hexs = cmd('read-memory %d %d' % (addr, n))
            hexs = re.sub(r'[^0-9a-fA-F]', '', hexs)
            return bytes.fromhex(hexs[:n * 2])

        def io(*keys):
            rows = [0xFF] * 8
            for i, b in keys:
                rows[i] &= ~(1 << b) & 0xFF
            cmd('set-ui-io-ports ' + ''.join('%02x' % x for x in rows) + '00')

        def booted():
            m = re.search(r'PC=([0-9a-fA-F]{4})', cmd('get-registers'))
            return bool(m) and 0x8000 <= int(m.group(1), 16) < 0xC000

        for _ in range(120):
            if booted():
                break
            time.sleep(0.5)
        check(booted(), 'tap loaded and running')

        GS, BACK = sym('game_state'), sym('back')
        AF = sym('anim_frame')
        VW = sym('vsync_wait')

        def freeze_at_rest(tries=40):
            """Stop the CPU with no repaint in flight.

               vsync_wait() is a tight spin, so the program is nearly
               always in it; anything else means a blit may be part
               done."""
            for _ in range(tries):
                cmd('enter-cpu-step')
                m = re.search(r'PC=([0-9a-fA-F]{4})', cmd('get-registers'))
                pc = int(m.group(1), 16) if m else 0
                if VW <= pc < VW + 96:
                    return True
                cmd('exit-cpu-step')
                time.sleep(0.05)
            return False
        for _ in range(30):
            if rd(GS, 1)[0] == 1:
                break
            io((7, 0))
            time.sleep(0.5)
            io()
            time.sleep(1.4)
        check(rd(GS, 1)[0] == 1, 'reached ST_PLAY')

        # Several beats, each inspected with the CPU stopped.  More than
        # one because a miss may depend on which frame index is up, and
        # because a full present from elsewhere can resync the screens
        # between beats -- see DETECTION above.
        BEATS = 4
        for attempt in range(BEATS):
            time.sleep(1.2)                  # let a beat land
            if not freeze_at_rest():
                check(False, 'beat %d: could not stop the CPU at rest'
                             % (attempt + 1))
                continue
            try:
                frame = rd(AF, 1)[0]
                back = rd(BACK, 1)[0]
                shown = 0xC000 if back else 0x4000
                other = 0x4000 if back else 0xC000

                buf = rd(VBUF, VIEW_PX_ROWS * 32)
                att = rd(VATTR, VIEW_COLS * VIEW_CW * VIEW_CH)

                bad_s = bad_o = 0
                for r in range(VIEW_PX_ROWS):
                    off = scr_off(VIEW_ROW * 8 + r)
                    want = buf[r * 32:(r + 1) * 32]
                    if rd(shown + off, 32) != want:
                        bad_s += 1
                    if rd(other + off, 32) != want:
                        bad_o += 1

                bad_a = 0
                for ar in range(VIEW_CH):
                    for vx in range(VIEW_COLS):
                        if vx == CURSOR_VX and ar // VIEW_CH == CURSOR_VY:
                            continue
                        col = vx * VIEW_CW
                        a = rd(shown + 0x1800 + (VIEW_ROW + ar) * 32 + col, 4)
                        w = att[ar * 32 + col:ar * 32 + col + 4]
                        if a != w:
                            bad_a += 1
            finally:
                cmd('exit-cpu-step')         # always let it run again

            check(bad_s == 0, 'beat %d (anim_frame=%d): displayed screen '
                              'matches VBUF (%d/%d rows differ)'
                              % (attempt + 1, frame, bad_s, VIEW_PX_ROWS))
            check(bad_o == 0, 'beat %d: the OTHER screen matches VBUF too '
                              '(%d/%d rows differ)'
                              % (attempt + 1, bad_o, VIEW_PX_ROWS))
            check(bad_a == 0, 'beat %d: attributes match VATTR (%d groups '
                              'differ, cursor cell excluded)'
                              % (attempt + 1, bad_a))
    finally:
        os.killpg(os.getpgid(emu.pid), signal.SIGKILL)

    print()
    if fails:
        print('ANIM PATHS: FAIL (%d)   [%.1fs]' % (len(fails), time.time() - t0))
        return 1
    print('ANIM PATHS: PASS   [%.1fs]' % (time.time() - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
