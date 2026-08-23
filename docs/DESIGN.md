# ZX Strategy — Design

## Game overview

This is a single-player strategy game for the ZX Spectrum. The player controls a cursor that allows them to select units with the space bar. 

When selected, the unit's stats, movement range, and attack range are displayed. The player can move the unit by selecting any tiles highlighted by the movement range.

When all the player's units have been moved or used their action, the player can press the enter key to end their turn.

The enemy units will then take their turn. Enemy units are red, player units are cyan.

### Game Init

The game starts with level_1.tmx. 
The global starting values for each unit type are defined in `config/game_config.h`.

A populate_map() function creates a friendly base and an enemy base at opposite corners of the map. It takes the level as a parameter, reads that level's roster from `config/game_config.h`, and places the created units within N tiles of the base (N = `UNITS_PLACE_RADIUS`, currently 4) but not on impassable tiles.

Because the bases sit in opposite corners, every map has to keep those corners passable and joined by a land path — otherwise a base is unreachable and the level cannot be won. The map converter checks both.

Each subsequent odd-numbered level will have an additional unit of each type.

### Movement Range

Movement range is calculated using a simple pathfinding algorithm that calculates the distance from the unit to all reachable tiles.

**Movement is 4-way**: north, south, east and west only. There are no diagonal
steps, so a tile's movement cost is the whole cost of entering it and no step
needs a different price from any other. This applies to the cursor as well as
to units.

### Tiles

Tiles have an impassable flag and a movement cost. They also offer cover benefits to units standing on them. `impassable` is the name of the Tiled tile property the build actually reads, so the flag here and the flag in the tileset say the same thing the same way round.

The order below is load-bearing: it is the tileset order in
`assets/maps/level_1.tmx` and the column order in both terrain sheets
(`assets/tiles_map.zxp`, `assets/tiles_view.zxp`), because terrain id =
`GID - firstgid` = sheet column. New types are therefore **appended**, never
inserted — renumbering an existing tile silently reinterprets every map that
uses it.

1. Plain
  impassable: false
  movement cost : 1
  cover : 0
2. Forest
  impassable: false
  movement cost : 2
  cover : 50
3. Water
  impassable: true
  movement cost : 0
  cover : 0
4. Hills
  impassable: false
  movement cost : 2
  cover : 25
5. City 
  impassable: false
  movement cost : 1
  cover : 75

### Cover 
Cover is a percentage of damage reduction that units receive when standing on tiles with cover.

Damage lands as whole points, and the reduction **rounds up** — the defender
never gains a fraction of a point from cover:

```
damage = (attack damage * (100 - cover) + 99) / 100      integer division
```

So a Cannon's 30 into a city's 75% cover is 7.5, dealt as **8**. Rounding up
also means any attack that would otherwise land still does at least 1 point
while cover is under 100%, so no unit is ever unkillable by position alone.

### Attack Range

Attack range is calculated using a simple algorithm that calculates the distance from the unit to every enemy unit, and takes those within range. Terrain does not block it, so no path is needed. The player can cycle through the enemy units in attack range using the O and P keys and select the target with the space bar.

### Units
Units have an attack range and damage value. They have a health value and can be killed. They also have a movement range

**Health fits in a byte**, so no unit may have more than 255 HP. Damage is
subtracted from it directly and 0 is death; nothing in the game needs a 16-bit
quantity per unit.

**One unit per tile.** A tile holding a unit is impassable to every other unit,
friendly or enemy alike, so units block movement exactly as water does — the
difference is that terrain is impassable for the whole level while a unit's
tile frees up when it moves or dies. Movement range therefore has to be
recalculated per unit, at the moment it is selected, not cached for the turn.

Art: `assets/units_view.zxp` (32x32 sprites, `ST_PLAY`) and
`assets/units_map.zxp` (16x16, `ST_MAP`), one sprite column per unit in the
order below. Both sides share a sprite; the runtime picks the attribute per
side.

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

#### Base 
Range : 0
Damage : 0
Health : 255
Movement : 0

### Actions

**One action per unit per turn.** A unit either moves or attacks; taking either
one uses the unit up for that turn, and the turn ends when every unit has acted
or the player ends it.

The exception is a move that closes with the enemy: **a unit whose movement
ends adjacent to an enemy unit may attack it in the same turn**. Adjacent means
one of the four cardinal neighbours, as movement is 4-way. So a unit can move
*or* attack at range, or move into contact and strike — but never move, attack,
and still have an action left.

### Enemy turn

**Turn order is strictly sided: the player moves first, then the enemy.** Every
player unit that is going to act does so before any enemy unit acts, and ending
the turn with `SELECT` forfeits the actions of any player units that have not
yet been used. The enemy turn is likewise indivisible — control returns to the
player only when every enemy unit has been processed — and the turn counter
increments once per full player-then-enemy round.

The enemy will cycle through their units and perform an action for each unit. 

They will attack if in range.
If no attack is possible, they will choose a unit to attack and move to that unit.
They will not path through impassable tiles and will avoid player unit attack ranges.
Once all enemy units have been processed, the enemy will end their turn.

This logic will be elaborated in subsequent reviews.


### Win Conditions

The first side to destroy the enemy base or destroy all enemy units wins. This
is tested every time a unit is destroyed — a base counts as a unit, so the two
conditions are one check over the loser's remaining roster. Either outcome
transitions to `ST_OVER`, which displays a win or lose message and owns what
happens next:

- **Lose** → `ST_TITLE`. The campaign is over; starting again from the title
  resets the level to 1.
- **Win** → increment the level, decompress that level's map, and return to
  `ST_PLAY`. The turn counter resets, `populate_map()` rebuilds both armies
  from `config/game_config.h` at the new level, and play resumes on the new
  field.

The level counter lives with the game state, not the map: `ST_TITLE` sets it to
1 and `ST_OVER` increments it on a win. There are ten levels, so winning **past
the last level** is the campaign being finished rather than another map to
load, and `ST_OVER` sends the player to `ST_WON` instead of `ST_PLAY`.

### Stalemate

Cannon and Base cannot move, so a side reduced to immobile units with nothing
in range can neither win nor lose, and the turn counter would climb forever.
There is no turn cap: **the player quits to the title screen with `X`**, which
is the same exit `ST_PLAY` already offers. The campaign ends there and the next
game starts at level 1, exactly as a loss does — the difference is only that no
`ST_OVER` message is shown, because nothing was decided.

## The loop

Implementation: `src/game.c`, state ids in `include/game.h`.

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

- **Everything visible is drawn in the vblank window.** A state may repaint
  around 256 bytes of screen per frame — two 4x4-character tiles, which is what
  `PAGE_CELLS` is set to; anything larger must be spread across frames (see
  `ST_PLAY`) or done in `enter_*`, which runs outside the window.
- **One state is active at a time.** There is no state stack; a state that can
  be opened from more than one place records where to return in `back_state`
  (`ST_GALLERY` and `ST_MUSIC`). States with a single caller — `ST_MAP` — just
  name their destination.
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
| `ST_OVER` | win / lose message | Ends a level; advances or abandons the campaign |
| `ST_WON` | "CAMPAIGN COMPLETE" | The last level was won; the campaign is over |
| `ST_GALLERY` | full-screen artwork | Show a decompressed graphic |
| `ST_MUSIC` | "PLAYING" banner | Play a tune (blocks) |

```
            ┌──────────┐  ENTER/FIRE   ┌──────────┐   M    ┌──────────┐
            │ ST_TITLE │ ────────────▶ │ ST_PLAY  │ ─────▶ │  ST_MAP  │
            │          │ ◀─────── X ── │          │ ◀───── │          │
            └───┬───┬──┘               └──┬────┬──┘ SPACE  └──────────┘
              M │   │ G                 G │    │ a base or an army is lost
                │   └──────────┬──────────┘    ▼
                ▼              ▼          ┌──────────┐
        ┌───────────┐  ┌──────────────┐   │ ST_OVER  │
        │ ST_MUSIC  │  │  ST_GALLERY  │   └────┬─────┘
        └───────────┘  └──────────────┘        │
          both return to whichever             ├─ WIN, level < 10 ─▶ level++, load ─▶ ST_PLAY
          state opened them (`back_state`)     ├─ WIN, level 10 ─▶ ST_WON ─ any key ─▶ ST_TITLE
                                               └─ LOSE ───────────────────────────────▶ ST_TITLE
```

### ST_TITLE

- **Shows**: detected machine (48K/128K), Kempston presence, which of the three
  vsync modes is active, and the key legend. The hardware report doubles as a
  smoke test — if vsync fell back to HALT, the player sees it.
- **Per frame**: nothing.
- **Exits**: `SELECT` (ENTER / Z / fire 1) starts a game — sets the level to 1
  and the turn counter to 1, loads that level's map and enters `ST_PLAY`.
  `G` → gallery, `M` → music.

### ST_PLAY

- **Shows**: an **8x4 page** of the world in 4x4-character tiles (rows 1-16,
  full screen width), the party as `@` on its terrain tile, and a status panel:
  turn number, party position, terrain under the party. A page that runs off
  the edge of the world — the right and bottom pages of a 14x7 map — blanks the
  cells beyond it with `ATTR_VOID`.
- **Per frame**: move the party (held directions repeat), repaint the two cells
  a step changed, advance a page flip if one is in progress, refresh the status
  panel when dirty.
- **Paging, not scrolling**: a full page repaint is ~4 KB of screen writes —
  several frames' work — so the view holds a fixed page and flips only when it
  has to. A flip repaints `PAGE_CELLS` tiles per frame and freezes movement
  until it completes (~0.3 s), which reads as a screen transition.
- **The cursor flips the page**, not the unit: the page changes when the cursor
  steps off its edge. The player has to be able to look at the whole board —
  the enemy base is in the opposite corner and therefore always on another page
  — and the cursor is the only thing that moves freely. A unit ordered to move
  never leaves the page it was selected on, because its movement range is at
  most 3 tiles.
- **Movement**: one cell per step in the four cardinal directions, blocked by
  terrain whose Tiled tile carries `impassable` (currently water) and by any
  tile that already holds a unit.
- **Exits**: `SELECT` ends the turn (turn counter only, for now), `M` →
  `ST_MAP`, `G` → gallery, `X` → title.

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

### ST_OVER

- **Shows**: a win or lose message on a cleared screen, the level just played,
  and a prompt for the key that continues.
- **Per frame**: nothing.
- **Exits**: `SELECT`. On a **loss** that goes back to `ST_TITLE` — the campaign
  is over and the next game starts at level 1. On a **win** it increments the
  level, loads that level's map, repopulates both armies and enters `ST_PLAY`;
  past the last level — `++level > LEVEL_COUNT` — it goes to `ST_WON` instead.
- The load happens in `enter_*`, outside the vblank window: rebuilding
  `terrain[]` and placing an army is far more than a frame's work, and the
  screen is repainted by `enter_play()` afterwards anyway.
- **Loading another level** already works: the campaign is ten 14x7 maps,
  `assets/maps/level_1.tmx` .. `level_10.tmx`, ZX0'd into `include/level_N.h`
  by the build and reached through `level_maps[]` in `src/game.c`. `load_map()`
  decompresses whichever level `level` names into `terrain[]` and seeds the
  party from that level's `start` object. All ten cost ~330 bytes compressed.

### ST_WON

- **Shows**: "CAMPAIGN COMPLETE" on a cleared screen, with the number of levels
  won and a prompt.
- **Per frame**: sample the keyboard once, the same release-then-press wait the
  gallery uses.
- **Exits**: any key returns to `ST_TITLE`, which resets the level to 1 when
  the player starts again. It uses the gallery's release-then-press debounce so
  the keypress that finished the last level cannot dismiss the ending.
- Reached only from `ST_OVER`, when a win takes the level past `LEVEL_COUNT`.

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
and hint line (a state without a status panel may use them for its own text,
as `ST_TITLE` does with row 20), **row 22 must stay pixel-blank** (the floating bus sync marker
lives in its attributes), and no attribute anywhere may be `0x03`.

