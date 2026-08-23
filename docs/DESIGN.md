# ZX Strategy — Design

Living design document. This first pass covers **only the game states**: what
each one shows, what it does per frame, and how the player moves between them.
Game rules (turns, units, combat, economy) are deliberately not specified yet —
see [Open questions](#open-questions).


## Game overview

This is a single-player strategy game for the ZX Spectrum. The player controls a cursor that allows them to select units with the space bar. 

When selected, the unit's stats, movement range, and attack range are displayed. The player can move the unit by selecting any tiles hightlighted by the movement range.

When all the player's units have been moved or used their action, the player can press the enter key to end their turn.

The enemy units will then take their turn.

### Movement Range

Movement range is calculated using a simple pathfinding algorithm that calculates the distance from the unit to all reachable tiles.

### Attack Range

Attack range is calculated using a simple pathfinding algorithm that calculates the distance from the unit to all reachable enemy units. The player can cycle through the enemy units in attack range using the left and right arrow keys and select the target with the space bar.

### Units
Units have an attack range and damage value. They have a health value and can be killed. They also havea movement range

#### Infantry

Range : 3
Damage : 10
Health : 100
Movement : 3

#### Tank 

Range : 2
Damage : 20
Health : 150
Movement : 2

#### Cannon

Range : 4
Damage : 30
Health : 200
Movement : 0

#### Base : 

Health : 500
Movement : 0

### Enemy movement

Implementation: `src/game.c`, state ids in `include/game.h`.

## The loop

One iteration of `game_run()` is one 50 Hz frame:

```
vsync_wait()          sync to the beam (floating bus; ~28 000 T free)
border RED
  update_state()      per-frame work for the active state
border BLACK
poll_input()          one debounced sample of keyboard + Kempston
handle_input()        state transitions
[enter_state()]       only if a transition was requested
```

Consequences the design has to respect:

- **Everything visible is drawn in the vblank window.** A state may repaint at
  most ~1 KB of screen per frame; anything larger must be spread across frames
  (see `ST_PLAY`) or done in `enter_*`, which runs outside the window.
- **One state is active at a time.** There is no state stack; a state that
  interrupts another records where to return in `back_state`.
- **Input is edge-triggered and debounced**: an action must appear in two
  consecutive frames, then fires once on its rising edge. Held directions
  repeat every `NAV_DELAY` frames. Keyboard and Kempston fold into one action
  byte, so both debounce identically.

## States

| State | Screen | Purpose |
|-------|--------|---------|
| `ST_TITLE` | "ZX STRATEGY" + hardware report | Front end; entry to a game |
| `ST_PLAY` | "THE FIELD" — 8x4 page of terrain | The game proper |
| `ST_MAP` | "CAMPAIGN MAP" — whole world | Read-only overview, opened from play |
| `ST_PAUSE` | play screen, hint line replaced | Freeze play |
| `ST_GALLERY` | full-screen artwork | Show a decompressed graphic |
| `ST_MUSIC` | "PLAYING" banner | Play a tune (blocks) |

```
                  ┌─────────────────────── X ────────────────────────┐
                  ▼                                                  │
            ┌──────────┐  ENTER/FIRE   ┌──────────┐  SPACE   ┌────────┴──┐
            │ ST_TITLE │ ────────────▶ │ ST_PLAY  │ ───────▶ │ ST_PAUSE  │
            └────┬─────┘               └──┬────┬──┘ ◀─SPACE─ └───────────┘
                 │                        │    │
            G/M  │                    M   │    │  G
                 │                        ▼    │
                 │                  ┌──────────┐
                 │                  │  ST_MAP  │ ──── SPACE / X / FIRE ──▶ ST_PLAY
                 │                  └──────────┘
                 ▼
        ┌──────────────┐   ┌───────────┐
        │  ST_GALLERY  │   │ ST_MUSIC  │   both return to whichever state
        └──────────────┘   └───────────┘   opened them (`back_state`)
```

### ST_TITLE

- **Shows**: detected machine (48K/128K), Kempston presence, which of the three
  vsync modes is active, and the key legend. The hardware report doubles as a
  smoke test — if vsync fell back to HALT, the player sees it.
- **Per frame**: nothing.
- **Exits**: `SELECT` (ENTER / Z / fire 1) starts a game — resets the turn
  counter, loads the map and enters `ST_PLAY`. `G` → gallery, `M` → music.

### ST_PLAY

- **Shows**: an **8x4 page** of the world in 4x4-character tiles (rows 1-16,
  full screen width), the party as `@` on its terrain tile, and a status panel:
  turn number, party position, terrain under the party.
- **Per frame**: move the party (held directions repeat), repaint the two cells
  a step changed, advance a page flip if one is in progress, refresh the status
  panel when dirty.
- **Paging, not scrolling**: a full page repaint is ~4 KB of screen writes —
  several frames' work — so the party walks inside a fixed page and the page
  flips only when it steps off the edge. A flip repaints `PAGE_CELLS` tiles per
  frame and freezes movement until it completes (~0.3 s), which reads as a
  screen transition.
- **Movement**: one cell per step, blocked by terrain whose Tiled tile carries
  `impassable` (currently water). Diagonals fall out of two directions being
  held at once.
- **Exits**: `SELECT` ends the turn (turn counter only, for now), `SPACE` →
  `ST_PAUSE`, `M` → `ST_MAP`, `G` → gallery, `X` → title.

### ST_MAP

- **Shows**: the **whole world** in 2x2-character tiles, the party's cell
  highlighted (yellow + `@`), and a free cursor with the same status panel
  reporting the cursor's cell instead of the party's.
- **Per frame**: move the cursor, repaint the two cells it left and entered,
  refresh the status panel.
- **Read-only by design**: the overview exists to plan, not to act. Issuing
  orders from here is a candidate for the first rules pass.
- **Exits**: `SPACE` (also `X` or fire) returns to `ST_PLAY`. The cursor is
  seeded at the party's cell each time it opens.

### ST_PAUSE

- **Shows**: the play screen untouched, with the hint line replaced by
  "PAUSED — SPACE TO RESUME". Entering does **not** repaint the field, so
  resuming is instant and the frozen frame is exactly what the player left.
- **Per frame**: nothing — this is also the state to hang a menu off.
- **Exits**: `SPACE` / `SELECT` resumes, `X` abandons to the title.

### ST_GALLERY

- **Shows**: a ZX0-compressed graphic decompressed into a staging buffer and
  blitted to its authored position (currently the Great Old One).
- **Per frame**: sample the keyboard once, waiting for a release-then-press so
  the key that opened it cannot immediately close it.
- **Exits**: any key returns to `back_state` (title or play).

### ST_MUSIC

- **Shows**: a "PLAYING — PRESS A KEY" banner on a cleared screen.
- **Per frame**: calls the Tritone player, which **blocks** with interrupts off
  until a key is pressed; it owns the speaker and cannot coexist with the frame
  loop. This is why it is a state rather than a background feature.
- **Exits**: returns to `back_state` when the player stops it.

## Adding a state

1. Add the `ST_` id to `include/game.h`.
2. Add an `enter_*` function that paints the screen once, and its case in
   `enter_state()`.
3. Add per-frame work to `update_state()` only if the state animates.
4. Add its transitions to `handle_input()`; set `back_state` if it interrupts
   another state and must return to it.

Screen budget to respect: row 0 is the title bar, rows 18-21 the status panel
and hint line, **row 22 must stay pixel-blank** (the floating bus sync marker
lives in its attributes), and no attribute anywhere may be `0x03`.

## Open questions

Not designed yet — each is a rules layer on top of the state machine above:

- **Turns**: what "end turn" resolves. Currently it only increments a counter.
- **Units**: is the `@` a party, an army, or a cursor for selecting units?
- **Orders**: does `ST_MAP` become interactive (issue moves), or does a new
  `ST_ORDERS` state own that?
- **Combat**: resolved on the field page, on the overview, or in its own state?
- **Economy / objectives**: what the player is accumulating and what ends a game.
- **Save/load**: 48K tape realities probably mean a password/seed, not a save.
