---
name: test-design
description: Write emulator and hardware tests that are capable of failing. Covers proving an instrument works before trusting it, avoiding comparisons between two paths that share the suspect code, reading state that exists yet, and the ZX-specific traps — BREAK during tape load, the frame counter stopping in blocking operations, and "the screen" being two things on a 128K.
when_to_use: "the test passes but the bug is still there" or "all the tests pass" or "cannot reproduce" or "the fix is unverified" or "control run" or "how do I know my test can fail" or "prove the test works" or "golden master" or "invariant test" or "the test hangs waiting for the title" or "the tap does not load in the test" or "does my test cover this" or writing a new test harness
allowed-tools: Bash Read Write Edit
effort: medium
---

# Test design: making a test that can fail

`.claude/skills/zesarux-test` covers driving the emulator. This is about the
harness being **wrong** while reporting success.

Every rule below came out of one session that made five defensible fixes,
reported "PASS" throughout, and did not touch the actual bug — which the
user found by playing the game and describing two symptoms.

## The rule everything else follows from

**An instrument that has never returned a failure has not been shown to
work.**

Before trusting a new check, break the thing it watches ON PURPOSE and
confirm it notices. Then put it back.

```bash
# the control, and it is not optional
cp src/render.c /tmp/ok.c
sed -i '' 's/ld  bc, #28/ld  bc, #27   ; DELIBERATE/' src/render.c
make FREEZE_ANIM=1 map && python3 tests/pixel_hash.py    # MUST fail
cp /tmp/ok.c src/render.c
```

This is cheap and it is the difference between a test and a decoration.
Three controls were run in that session: two proved a test worked (a
one-byte stride error was caught immediately), one proved a test was
useless — the "fix" and the "bug" produced identical output, which is how
the useless test was found.

## Do not compare two paths that share the suspect code

The most expensive mistake of the session. An invariant compared the
composed buffer against a full repaint:

```python
inc  = read(VBUF)            # built by compose_view_cell()
full = force_repaint(); read(VBUF)   # ...also built by compose_view_cell()
assert inc == full           # cannot see a fault in compose_view_cell()
```

Both sides go through the same function, so a bug there is identical on
both sides and the comparison passes for ever. It passed for a whole
session while the user was photographing the fault.

**Ask what the comparison would look like if the suspect code were
wrong.** If the answer is "the same", the test is not about that code.

## Confirm the scenario reaches the code

Two tests in that session measured nothing at all:

* A diagonal-scroll test held LEFT+UP — but **the cursor starts at x=0**,
  so LEFT was clamped, `dx` was 0 and no diagonal ever occurred. It
  reported PASS, and the bug was in the diagonal path.
* A dirty-cell test pressed select, waited, then scrolled — and
  `render_tick()` drains the dirty list within a frame or two, so the list
  was already empty. It exercised nothing.

Assert the precondition, do not assume it. Read the state back:

```python
print('cursor=(%d,%d) page=(%d,%d)' % (cx, cy, px, py))   # did it MOVE?
```

An instrumented counter is better still — but see the first rule, because
a counter that reads 0 because the code never ran looks exactly like a
counter that reads 0 because the bug is absent.

## Do not read state that does not exist yet

A boot check watched `vsync_mode` for a non-zero value. That address is in
the program's own BSS, so **until the last tape block lands it holds
uninitialised RAM** — which on the 48K read non-zero. The test decided the
game had booted and started pressing keys in the middle of the tape.

Ask something that cannot be stale. The **program counter** works: it is
in the ROM below `0x4000` while the tape runs, and in `0x8000-0xBFFF` once
the program has control.

```python
def booted():
    m = re.search(r'PC=([0-9a-fA-F]{4})', cmd('get-registers'))
    return bool(m) and 0x8000 <= int(m.group(1), 16) < 0xC000
```

## Never press a key while the tape loads

The ROM's loader watches for BREAK, so a harness pressing SPACE in a loop
to get past a splash screen **aborts the load**. The result is
indistinguishable from a tap that does not work, and it cost two separate
debugging detours in one session.

Wait passively for the load. Press only after the program has control.

## The frame counter stops

`FRAMES` (0x5C78) ticks on the ROM interrupt, which is off during every
blocking operation this project has: the tune, a walk, an explosion, a
level load, a cutscene. So `wait_frames(3)` is sometimes three frames and
sometimes however long a walk takes.

**Poll for the outcome and re-press.** Never a fixed wait:

```python
def press_until(key, want, tries=90):
    for _ in range(tries):
        if want(): return True
        io(key); sleep(0.4); io(None); sleep(0.4)
    return want()
```

Four checks in `p0_state_walk.py` used fixed waits and failed at random
levels run to run. Random failure that moves around IS a lost press.

## Freezing non-determinism cuts both ways

A screen hash is impossible while anything animates: the at-rest tick
flips every sprite every ~18 frames, and the SAME BUILD hashed differently
twice. `FREEZE_ANIM=1` fixed that and made `tests/pixel_hash.py` possible.

It also **hid `animate()` from every test in the file**, and the bug being
hunted was reported as "the ghosts are the ones that do not animate".

When freezing something to get determinism, write down what has just
become untestable, and test it another way.

## On a 128K, "the screen" is two things

Checking the displayed screen against the buffer passed while the OTHER
screen — the one the next flip reveals — was wrong in 2403 bytes. That
staleness was a real bug, and the check that found it had to name both:

```python
back  = read(sym('back'))
shown = 0xC000 if back else 0x4000
other = 0x4000 if back else 0xC000     # <-- the next flip shows THIS
```

Any partial present (one cell, one column) must write both screens or be
followed by a full present before a flip. Three separate code paths in
this project got that wrong.

## Timeouts grow with the tape

A tap going from 24 KB to 51 KB outgrew three waits, each failing as
"title screen never appears". When a block is added, bump the harness
waits — and check `--accelerate-loading` is on: it IS safe on a 128K,
despite an old comment in this project claiming otherwise, and it cut a
suite from 121 s to 22 s.

## Checklist for a new check

1. What would this look like if the code under test were broken? If
   "the same", start again.
2. Does the scenario actually reach that code? Print the state and see.
3. Break it on purpose. Watch the check fail. Put it back.
4. Is anything being read that may not exist yet — BSS before load?
5. Is anything pressed before the program has control?
6. Is any wait a fixed number of frames?
7. On a 128K, does it check both screens?

## When it still will not reproduce

Say so, plainly, and stop. Five hypotheses were eliminated with passing
invariants in that session and none was the bug; what closed it was the
user reporting two facts — "units rather than terrain" and "when I hold
two directions at once" — that no instrument had been pointed at.

A fix that cannot be demonstrated should be described as such. "Correct by
inspection, unverified against the symptom" is a useful thing to hand
over. "Fixed" is not, if it is not.

## Related

- `.claude/skills/zesarux-test` — driving the emulator: ZRCP, symbols,
  screen memory, input
- `.claude/skills/zx-loader` — tape and block layout, and the BREAK trap
- `tests/render_paths.py` — both machines, attributes; the PC boot check
- `tests/pixel_hash.py` — golden-master pixels; needs `FREEZE_ANIM=1`
- `tests/p0_state_walk.py` — state walk, keyboard and Kempston
