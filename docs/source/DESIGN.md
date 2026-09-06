# ZX Strategy — Design

This document is the **specification**: what the game is, and why each rule is
the way it is. `docs/PLAN.md` is how it gets built, and tracks progress.

Most of it describes a game that runs today. These parts do not yet:

| Section | Status |
|---------|--------|
| § Cover, § Attack Range | **Built.** Cover reduces damage, rounding up; reach comes from `attack_reach()`. |
| § Win Conditions | **Built.** `check_win()` fires on every death, including a death caused by a counter-attack. |
| § Enemy turn | **Built.** The enemy moves and attacks, scoring exchanges rather than blows. |
| § Actions — the attack half | **Built**, including move-into-contact-and-strike as one action. |
| § Adjacency, counter-attack and wounded damage | **Built.** All three rules, the resolution order, and the AI's exchange scoring. Balance untested in a full campaign. |
| § Selection and highlighting | **Built**, including enemy-selection mode, best-cover approach, and magenta for enemy reach. `O`/`P` cycling is gone. |
| § Action and Cancel | **Built.** SPACE/fire 1 acts, ENTER/fire 2 cancels, on every screen. |
| § Walking a unit to its destination | **Not built.** Needs a path and a beat of its own; two-character vertical and four-character horizontal steps keep the sprite inside one cell throughout. |
| § Sprite masks and animation | **Not built.** Masks and a 128K-only second frame. The dirty-cell budget is the open problem, not the data. |
| § Stalemate | **Not built** as such — `X` already quits to the title, which is the whole mechanism, but nothing detects the stalemate. |

Everything else — the board, terrain, unit placement, selection, movement,
turns, the campaign loop and the two views — is implemented and verified.

## Game overview

This is a single-player strategy game for the ZX Spectrum. The player controls a cursor that allows them to select units with the space bar. 

When selected, the unit's stats, movement range, and attack range are displayed. The player can move the unit by selecting any tiles highlighted by the movement range.

When all the player's units have been moved or used their action, the player can press the enter key to end their turn.

The enemy units will then take their turn. Enemy units are red, player units are green.

### Game Init

The game starts with level_1.tmx. 

The global starting values for each unit type are defined in `config/game_config.h`.

A populate_map() function creates a friendly base and an enemy base at opposite corners of the map. It takes the level as a parameter, reads that level's roster from `config/game_config.h`, and places the created units within N tiles of the base (N = `UNITS_PLACE_RADIUS`, currently 4) but not on impassable tiles.

That block does not always hold enough land: level 8's enemy corner is most of a lake, leaving 10 free cells for a 15-unit roster. The overflow is then placed on the nearest free land outward from the base rather than dropped, because **both sides always field the same army** — the map is meant to decide the advantage, not the roster. A corner with no land at all is impossible, since the converter already requires both corners passable.

Because the bases sit in opposite corners, every map has to keep those corners passable and joined by a land path — otherwise a base is unreachable and the level cannot be won. The map converter checks both.

The initial local view will be centered on the player's base.

Each subsequent odd-numbered level will have an additional unit of each type.

### Action and Cancel

**Two keys run the whole game, and every screen obeys them.**

| | keyboard | Kempston |
|---|---|---|
| **ACTION** — yes, pick up, confirm, advance | `SPACE` | fire 1 |
| **CANCEL** — no, put down, back out | `ENTER` | fire 2 *if present* |

**A standard Kempston has ONE button.** Bits 0-4 of port 0x1F are
right/left/down/up/fire; bit 5 is a non-standard extension that most
hardware does not provide and ZEsarUX's emulated Kempston does not either.
src/input.c reads it, so a two-button stick gets Cancel for free -- but
**nothing may depend on it**, and `ENTER` is the Cancel a joystick player
is actually guaranteed.

tests/p0_state_walk.py found this: the joystick pass could pick a unit up
and never put it down, because Cancel was unreachable. A one-button stick
can move, select and confirm, and needs the keyboard to say no.

Nothing else is a general control. `QAOP` or the stick moves the cursor,
`M` opens the overview from the board, `X` leaves a level -- and that is
the entire scheme. It fits in three lines on the title screen, which is
the test it has to pass: a control that will not fit there is a control
the player has to be told about somewhere else.

#### The Ladder

Cancel is not a single action, it is a **ladder**. It backs out of the
innermost open context and only reaches the next rung when there is
nothing left to back out of:

```
enemy-selection open   ->  close it, keep the unit held
unit held              ->  put it down
nothing held           ->  end the turn
```

This ordering matters more than it looks. Reaching for "end turn" while
looking at a unit's movement range should not throw the turn away --
ending a turn cannot be undone, whereas putting a unit down costs
nothing. The safe rung comes first, always.

**`X` is deliberately NOT on the ladder.** Leaving a level and backing
out of an order are different intentions, and one should never be
reachable by repeating the other. (A confirmation for `X`, and for ending
a turn, is still to come.)

#### There is no "press any key"

Every prompt takes ACTION, including the campaign-complete screen, which
used to accept any key at all with a two-sample debounce to distinguish
the press that won from a fresh one. `enter_state()` flushes the keyboard
on every transition, so the edge detector already does that work -- and
"press any key" was the single place the game asked for something it
never asked for anywhere else.

#### What this replaced, and why

Three habits had accumulated, each defensible alone:

- **`ENTER` ended the turn outright.** The most destructive action in the
  game sat on a bare keypress, one rung from where a player's hand
  already was.
- **`ENTER` and `X` ran SEPARATE ladders** ending in different places --
  end turn for `ENTER`, quit for `X`. The same gesture meant two
  different things depending on which device you were holding, and the
  joystick could not end a turn at all.
- **`Z` and `X` shadowed fire 1 and fire 2** on the keyboard. Harmless
  until `X` needed to mean something else, at which point one key was
  Cancel and Quit at the same time.

The rule is worth more than any of them: **one gesture, one meaning,
everywhere.** A player who learns two keys on the title screen has
learned the whole game.

### Cursor and movement

The cursor is not a unit and terrain does not stop it. It crosses water and
every other `impassable` tile freely, and it costs nothing to move — it is
where the player is looking, not something standing on the board. Passability
and movement cost constrain *units*, and they are checked when an order is
issued, not when the cursor is moved. The one thing the cursor cannot do is
leave the map.

The cursor inverts the highlighted tile and uses white on black attributes.

#### Local view

The cursor does not move on the local view; the keys or joystick directions cause the local view to move in the opposite direction, keeping the cursor in the same position relative to the screen.

Each movement causes a push scroll, implemented in `render.c` using either the vertical or horizontal assembly scroll routine. User input is locked during scrolling and a buffer is passed to the scroll routine with the current tiles and the next row or column.

The buffer contains the tiles from the map, but where units are positioned on the map, it will include them instead of the tile.

The routine either scrolls vertically or horizontally, one character at time (to avoid attribute issues). When the scrolling is finished, the user input is unlocked and the local view is updated.

**Beyond the edge of the map is sea.** A pinned cursor has to be able to reach
the corners — both bases are in one — so the view must be allowed to run off
the board, and what it shows there is the water tile rather than black. The
board is an island. This is what makes the pinned cursor workable at all: an
8x4 view on a 14x7 map with the cursor fixed near the middle is partly off the
board for most of the positions it can take.

The sea is scenery, not terrain. It is drawn, but it has no cell index, it is
not in `terrain[]`, and no unit can be ordered onto it — the movement fill
never sees it.

##### Map view

This renders the entire map and units in one view. The keys or joystick directions move the cursor around the map, restricted only by the map edges.

`ST_MAP` is therefore the quick way across the board: the local view costs a
scroll per cell, the overview costs nothing and shows everything at once. That
is a better reason for it to exist than it had before.

### Movement Range

Movement range is the set of tiles a unit can reach on its movement budget,
counting each tile's own cost to enter. It is recomputed the moment a unit is
picked up, never cached — units block each other, so the answer goes stale as
soon as anything moves. (`docs/PLAN.md` § Movement range has the algorithm.)

**Movement is 4-way**: north, south, east and west only. There are no diagonal
steps, so a tile's movement cost is the whole cost of entering it and no step
needs a different price from any other. This applies to the cursor as well as
to units.

The cursor is constrained by none of this — see § Cursor and movement.

### Sprite masks and animation

**Not built.** Two related additions to the graphics pipeline: a mask per
sprite, generated at build time, and a second frame of animation that only
a 128K gets.

#### Masks

**Only the VIEW unit sprites are masked.** Nothing else needs it and
nothing else gets it:

- **Terrain is the background.** There is nothing behind it to show
  through to, so a mask would cost three memory accesses a byte to
  composite against blank buffer.
- **The map-view sprites are not masked either.** They are 16x16 markers
  on a schematic overview, drawn flat, where a one-pixel outline would be
  a quarter of the sprite.
- **Only `units_view.zxp` is built with `--mask`**, and it is the only
  sheet that needs the one-pixel margin the tool insists on.

A unit sprite otherwise occupies its whole 32x32 cell, so terrain does not
show through it. The mask fixes that: one bit per pixel marking what is
transparent, and the blit becomes

```
screen = (screen AND mask) OR sprite
```

**Generated at build time, never by hand.** `tools/zxp_tiles_zx0.py`
already reads the .zxp sheets, so it derives the mask from the artwork and
emits it alongside the pixels -- the artist draws one thing and the tool
produces both. Nothing about a mask is a decision anybody should be making
twice.

**Nothing is keyed and nothing extra is drawn: the mask is derived from
the sprite.** White is solid, black is transparent, and the white region is
the sprite **dilated by one pixel** -- one pixel wider all round, diagonals
included.

That one pixel is the whole point. It puts a black rim between the sprite
and whatever it stands on, so a unit stays legible over busy terrain
instead of dissolving into it. The artist draws the sprite and gets the
outline for free; there is no second image to keep in step, which is the
failure mode a hand-drawn mask always ends in.

**Store the mask INVERTED.** The blit wants

```
screen = (screen AND NOT mask) OR sprite
```

and inverting at build time turns the inner loop into AND then OR with no
complement per byte. It is free to do in the tool and saves an instruction
in the one loop that cannot afford them.

**The artwork must leave a one-pixel margin inside its cell.** Dilation of
a sprite that already touches the edge of its 32x32 cell would spill
outside it, and the blit cannot reach there -- the outline would simply be
missing on that side, which looks like a drawing mistake rather than a
clipping one. `tools/zxp_tiles_zx0.py` should **fail the build** if a
sprite has ink on its outer edge, rather than silently clipping: a tool
that quietly produces a slightly wrong mask is worse than one that stops.

Costs, with today's numbers:

| | bytes |
|---|---|
| `units_view` pixels, decompressed | 512 |
| masks for the same | **+512** |
| free above MEM_END (0xF7AA-0xFFFF) | 2 134 |

So the masks fit where the sheets already live, with room to spare. The
cycle cost is the part to watch: a masked byte is load, AND, OR, store
where an unmasked one is a store, so **roughly three times the work per
byte**. That lands inside the vblank window, which is the one budget this
program has never had slack in.

#### Two frames, on every machine

**The sheet is a grid, not a strip**: column 1 is the units, column 2 each
unit's second frame, so unit *n* is row *n* and frame *f* is column *f*.
Both frames **share one mask, built from the two frames COMBINED**: union
the frames, then dilate the combined outline.

Deriving it from frame 1 alone does not work, and the explosion sprite is
why. A shared mask silently assumes frame 2 is a variation of frame 1 -- a
limb shifting, a turret turning -- and an explosion **expands**, so its
second frame is legitimately larger. Any pixel outside frame 1's outline
would draw with no black rim, on that frame only, which reads as an edge
that flickers.

Unioning first removes the assumption rather than constraining the art:
every frame is inside the mask by construction, however different they are.
The cost is a rim sized to the LARGER frame, so the smaller one carries a
slightly thicker black edge -- invisible at this resolution, and cheaper
than a second mask or a rule the artist has to remember.

Still one mask, still 512 bytes. See docs/PLAN.md P11.

A second frame adds 640 bytes of pixels and no mask, the mask being shared.
**Both machines get it.** It was going to be 128K-only, in a RAM bank,
until the free space above MEM_END was measured: 1334 bytes, against 640
needed. It sits with the decompressed sheets, which is plain RAM on a 48K
and page 7 on a 128K -- same addresses, one code path, no paging.

The second frame belongs in a **bank block**: banks are 128K/+3 only, so a
48K never loads it and pays nothing for it, and `tools/mktap.py` already
takes repeatable `--code` blocks. `is_128k` picks at runtime, which is the
same switch the shadow screen uses. Putting it in the contended asset
block at 0x6000 instead would make a 48K carry data it cannot use, on a
machine with less to spare.

#### The problem to solve first

**The renderer is built around two to four stale cells per frame.**
`DIRTY_MAX` is 4, sized for one unit moving and up to two dying, and
`draw_view_cell()` is expensive enough that a page flip spends its budget
on two of them.

Animation is the opposite shape: **every cell containing a unit goes stale
on the same frame**, every few frames, forever. A dozen units would need a
dozen redraws in one frame and the dirty list would drop most of them --
silently, which is the bug that already cost a playtest once and is why
`mark_dirty()` now complains in the debug build.

#### Animate only when the screen is at rest

So animation does not use the dirty list at all. **It runs only in the
frames where the renderer has nothing else to do**, and stops the moment it
has.

That inverts the problem. The expensive frames -- a scroll, a move, a
death, a recolour -- are exactly the frames animation is skipped, so it
never competes for the budget it would otherwise blow. And a frame at rest
is almost entirely spare: the whole vblank window is going unused, which is
where the redraws have to happen anyway.

**At rest** is a state the renderer can already answer for itself, from
counters that exist:

```
dirty_n == 0        nothing stale to repaint
attrs_left == 0     no recolour in progress
not scrolling       the view is where it was last frame
```

No new bookkeeping, and nothing to keep in step.

Consequences, all of which seem right rather than merely tolerable:

- **Units freeze while the board is busy.** During a scroll, a move or an
  attack, everything holds still. That reads as the board's attention
  being elsewhere, and it is what makes the animation affordable.
- **A held arrow key stops the animation**, because the view is scrolling.
  Movement stops the idling and idling resumes when movement does -- there
  is no state where both are trying to draw.
- **The enemy turn animates in its gaps.** ENEMY_BEAT leaves frames at rest
  between actions, so the board stays alive while the player watches.
- **The cost is bounded by what a resting frame can afford**, not by how
  many units are on the board. If a full sweep will not fit, it spreads
  over several resting frames; there is no worst case where it has to.

This is what makes the feature affordable, and it is worth building in this
order: masks first, since they are useful on their own and their cost is
per-blit rather than per-frame, then the resting check, then the second
frame behind it.

### Walking a unit to its destination

**Not built.** A move currently teleports: `move_selected_to()` updates
`occupancy[]`, marks two cells dirty, and the unit is simply somewhere
else on the next frame. Instead it should **step one character cell at a
time** -- 8 pixels, so four steps per tile -- from where it stands to
where it is going.

This is a bigger change than it sounds, for three reasons.

#### It needs a PATH, which nothing currently keeps

`movement_range()` flood-fills `cost[]` and that is all that survives: the
renderer knows which tiles are reachable and at what price, not how to get
to one. Walking needs the route.

The cheap answer is to **walk downhill through `cost[]`** from the
destination back to the unit, since each step of the fill is one cheaper
than the last -- no extra storage, and the route is recomputed rather than
remembered. Downhill is not unique -- several neighbours may tie -- so the tie-break
is **a fixed order, always the same one**. Not random.

The route is cosmetic: `cost[]` already fixes the destination and the price,
and nothing depends on which way the unit went -- no ambush, no
interruption, no facing. So a fixed order cannot be *wrong*, only
characteristic, and after two moves the player has learnt it. It looks like a rule.

**The order is VERTICAL FIRST**, and for a reason rather than a toss-up:
the vertical step is two characters and the horizontal one is four, so
vertical is the half-cell step and the only one with an intermediate
position. Leading with it means a move opens with the smooth part -- the
unit visibly steps rather than appearing somewhere new -- and the hops
follow once movement has already been established as motion. Horizontal
first would start every move with a jump, which is the thing the whole
feature exists to get away from.

The consequence to accept: a unit moving mostly sideways takes its one
vertical step and then a run of hops, so the smoothness is front-loaded
rather than spread. That is the right way round -- the beginning of a move
is where the player is looking.

Random would be the same cost mechanically and worse in two ways. The same
move would take a different route each time, so nothing is learnable --
which is the problem the tie-break exists to solve. And it would make odd
behaviour unreportable: most of the bugs found in this renderer were found
by someone playing and describing what they saw, and "the tank went a
strange way" stops being a reproducible observation the moment the route is
a coin toss.

#### A unit is always inside exactly one cell

**Steps are two characters vertically and four horizontally**, and that
choice makes the hard part disappear.

A cell is 4 characters square; the sprite is 4 wide and 2 tall, sitting in
the lower half. So:

- **Horizontally, four characters is a whole cell.** The unit moves cell to
  cell with nothing in between, always exactly aligned.
- **Vertically, two characters is half a cell.** From the lower half of a
  cell a step up lands in the UPPER half of the SAME cell; another lands in
  the lower half of the one above. Downwards works the same in reverse.

At every point the sprite occupies **two whole character rows of one
cell** -- either its top half or its bottom half. It never spans a cell
boundary, in either axis, at any moment of a move.

That removes the whole difficulty:

- **No boundary blit.** No sprite written across two cells, no second cell
  to compose, and nothing to present in pairs.
- **No pixel shifting.** Whole-character offsets mean bytes copy straight;
  a sub-character offset would need a shift-and-or per byte per row, which
  is the most expensive thing a Z80 blit can do.
- **No partial attribute cells.** The unit's colours land on whole
  character cells, as they already do.

What it needs instead is small: the unit's row offset within its cell
becomes a VARIABLE where `UNIT_ROW_OFF` is currently a constant -- 0 when
the sprite is in the upper half, 2 when it is in the lower. That single
number threads through `cell_layers()`, `compose_view_attr()` and both
attribute slices, and the terrain fills whichever half the unit is not in.

Note what is given up: **horizontal movement has no intermediate
position**, so a unit crossing three tiles sideways is seen at three
places rather than gliding. The route is still legible, which was the
point -- particularly on the enemy turn -- and a smoother horizontal step
would cost the shifting this avoids.

#### Timing, and what it must not fight

- **The at-rest rule already covers the frame animation**: during a walk
  the screen is not at rest, so the two-frame idle stops by itself. No
  interaction to manage.
- **The dirty list must not be used for it.** Two cells per step, four
  steps a tile, is well inside DIRTY_MAX, but the walk is a sequence over
  TIME and the dirty list is a set of cells owed a repaint -- it has no
  notion of order. Drive the walk like `scroll_view()` does its sub-steps,
  or like the enemy turn does its beats.
- **The view has to follow** if the destination is off-screen, which means
  a scroll interleaved with the walk. Both push VBUF; doing them in the
  same frame needs care, and the simplest correct answer is to finish the
  walk within the current view and scroll only when it leaves it.
- **The move sound fires once per move today** (SFX_MOVE). Per step it
  would need rate limiting, or it becomes a machine-gun; per tile is
  probably the right granularity.

#### What it buys, and what it costs

It makes movement legible -- the player sees which way a unit went, which
matters most for the ENEMY turn, where a unit appearing somewhere new is
currently the only evidence that anything happened.

The cost is a blocking sequence on every move, of every unit, on both
turns. At four steps a tile and three tiles of movement that is twelve
draws of two cells, and the enemy moves several units a turn. If it drags,
the dial is steps per tile (drop to two, 16 pixels) before it is anything
structural.

### Tiles

Tiles have an impassable flag and a movement cost. They also offer cover benefits to units standing on them. `impassable` is the name of the Tiled tile property the build actually reads, so the flag here and the flag in the tileset say the same thing the same way round.

The order below is load-bearing: it is the tileset order in
`assets/maps/level_1.tmx` and the column order in both terrain sheets
(`assets/tiles_map.zxp`, `assets/tiles_view.zxp`), because terrain id =
`GID - firstgid` = sheet column. New types are therefore **appended**, never
inserted — renumbering an existing tile silently reinterprets every map that
uses it.

A terrain tile's **colour is authored per character cell** and travels with its
art: a 32x32 play tile carries a 4x4 block of attributes, so a tile can be
several colours at once — a city with a lit window, a hill with a pale crown.
The runtime copies that block straight to the screen for bare ground. It is
overridden wholesale, not blended, when something is standing on the cell or
the cursor or a movement range is over it.

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

Attack range is calculated using a simple algorithm that calculates the distance from the unit to every enemy unit, and takes those within range. Terrain does not block it, so no path is needed. The player *did* cycle through enemy units in attack range with `O` and `P`, selecting with `SPACE`. **§ Selection and highlighting replaces that**: targets are highlighted on the board and clicked directly.

### Adjacency, counter-attack and wounded damage

Three rules that only make sense together. **Built** — the resolution
order and the AI's scoring at the end of this section are part of the
specification, not notes, and are implemented as written.

**1. Mobile units must be adjacent to attack.** Anything with a movement
allowance — Infantry, Tank — can only strike a unit orthogonally next to
it. Their attack range is 1 by definition, which overrides the Range
figure listed under each unit.

Cannon and Base do not move, so this does not apply to them: the Cannon
keeps its range of 4. That makes the Cannon the only unit that can strike
without being struck back, which is its whole purpose and the reason it
cannot move.

**2. A defender struck from an adjacent square hits back for half.** The
counter is an ordinary attack with the roles reversed — the attacker's
cover applies to it — then halved.

The counter is **free**: it does not consume the defender's action, and it
happens on the attacker's turn.

Consequences worth being deliberate about:

- **A Cannon firing at range 2-4 is never countered.** Adjacency is the
  trigger, so reaching out costs the Cannon nothing.
- **A Cannon attacked from an adjacent square does counter**, at half of
  30. Closing with a Cannon is meant to hurt.
- **The Base never counters**, because its damage is 0. A unit with no
  attack does not acquire one by being hit.

**3. Damage scales with the attacker's health.** A unit at half health
deals half damage. Maximum health comes from the unit type table, so
nothing extra is stored per unit.

```
scaled  = (damage * hp + max_hp - 1) / max_hp       health, rounds UP
landed  = (scaled * (100 - cover) + 99) / 100       cover,  rounds UP
counter = landed / 2, but never less than 1         halved, rounds DOWN
```

Health is applied before cover, and both round up, for the same reason
cover already does: an attack that lands always takes at least one point,
so no unit becomes unkillable by being nearly dead.

The counter is the one place that rounds **down**, with a floor of 1. So a
badly wounded defender still bites, but only just — and the floor keeps it
consistent with every other damage path, where a landed attack is never
free. A unit whose damage is 0 does not counter at all, so the floor never
gives the Base an attack.

**This makes fights snowball, on purpose.** A wounded unit hits softer, so
it loses the next exchange harder. Combined with rule 2 it means attacking
a healthy unit with a damaged one is a bad trade — the counter may take
more than the attack deals. Committing fresh units, and withdrawing hurt
ones, becomes the substance of the tactics.

It also makes the Cannon stronger than its stat line suggests: it never
takes a counter at range, so it never enters the spiral at all. Watch that
in play — if the Cannon dominates, its damage is the number to cut.

#### Resolution order

An attack resolves in this order, and the order is the rule:

1. The attacker's damage lands on the defender.
2. **If the defender dies, there is no counter.** A killing blow is
   therefore free, which is the strongest incentive in the system: finish
   what you start.
3. Otherwise, if the attack came from an adjacent square and the defender
   has an attack at all, the counter lands on the attacker.
4. **A counter can kill the attacker**, and runs the same win check as any
   other death. It has to: otherwise a Base destroyed by a counter would
   go unnoticed and the game would carry on unwinnable.

So a single player action can destroy two units and end the level, and the
win check must be able to fire from either death.

#### What the AI has to weigh

`pick_target()` currently scores by `damage_at()` alone, which under these
rules would have the enemy walk into trades it should refuse — and rule 3
would then grind its wounded units down for the rest of the level. **The
enemy must score the whole exchange before committing**, not just what it
deals:

```
gain  = damage it would deal
cost  = counter it would take back     (0 if the target dies, or the
                                        target has no attack, or the
                                        attack is from range)
score = gain - cost
```

Three things fall out of that, and are worth stating so the AI is not
later "fixed" back into ignoring them:

- **A kill is worth more than its damage number**, because it cancels the
  counter. `gain - 0` beats a larger `gain` that costs a counter.
- **A wounded attacker holds back on its own.** Its `gain` is scaled down
  by rule 3 while the defender's counter is not, so the arithmetic argues
  for withdrawal without needing a separate rule.
- **A Cannon shooting from range 2-4 always has `cost` 0**, so the AI will
  keep it back and shoot. That is the intended behaviour, not something to
  tune out.

This is the largest of the three pieces of work, and the one that decides
whether the rules read as tactics or as an enemy behaving stupidly.

##### The knobs, in config/game_config.h

| | default | what it does |
|---|---|---|
| `AI_W_COUNTER` | 1 | how heavily the counter weighs against the damage dealt |
| `AI_KILL` | 200 | score for a kill, which takes no counter |
| `AI_BASE_BONUS` | 150 | added, so a base is worth a bad trade |
| `AI_MIN_TRADE` | 0 | refuse exchanges scoring at or below this and move instead |

**`AI_W_COUNTER` is the difficulty dial**, and the most useful one here:

- **0** — counters are ignored entirely. This is exactly how the enemy
  behaved before these rules existed, so it is the control case: if the
  new AI feels wrong, set this to 0 and see whether the rules or the
  scoring are at fault. It plays recklessly, trading units away, and
  wounded-damage then compounds every bad trade it makes.
- **1** — an even trade. A point taken back is worth a point dealt, so
  the enemy attacks when it comes out ahead and holds when it does not.
- **2 and up** — cautious to timid. It will decline exchanges a human
  would take, hang back, and let the player dictate the tempo. Useful for
  an easier level rather than a harder one, which is the opposite of what
  the number looks like it should do.

Raising `AI_MIN_TRADE` has a similar effect from the other side: it makes
the enemy hold out for good trades rather than acceptable ones. Both dials
make the enemy *passive* when raised, so an easier game is a bigger
number, and neither makes it cleverer.

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
order below. Both sides share a sprite; the runtime picks the ink per side.

**What the sheet contributes to a unit's colour is its BRIGHT flags, and
nothing else.** Ink and paper are not the artist's to choose — a unit is green
or red according to whose it is — but *which character cells are lit* is, and
that is the sprite's shading. The build strips ink and paper at conversion
(`--attr-mode bright`) and the runtime ORs the side's colour over what is left.

One asymmetry falls out of the hardware: **enemy units are always flat.**
Non-bright red on black is `0x02`, and `0x02 | 1` is the floating bus sync
marker, so the enemy's ink has to carry BRIGHT already and the sheet cannot dim
it. Shading therefore reads on the player's units only — which is no loss,
since it is the player's units the player has to tell apart. A spent player
unit is flattened to dim for the same reason it always was.

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

### Selection and highlighting

The interaction model. **Built**, and it replaces the `O`/`P` cycling
described under § Attack Range.

"Clicking" here means putting the cursor on a cell and pressing `SPACE`;
there is no mouse.

**Selecting one of your units highlights two things at once:**

- its **movement area in blue** — the tiles it can reach, as § Movement
  Range computes them;
- every enemy it could **attack this turn in red**.

Red is not simply "within attack range". For a mobile unit, adjacency is
required to strike (§ Adjacency), so an enemy is red when there is a
reachable tile in the blue area orthogonally next to it. For a Cannon,
which cannot move, red means within its range of 4. **If no adjacent tile
is reachable the enemy is not highlighted**, so red always means "I can
hit that, now".

**Clicking a red enemy moves into contact and opens enemy-selection mode**
with that enemy highlighted — the § Actions exception, chosen rather than
assembled by hand. `SPACE` confirms; see below. If the unit is already
adjacent, nothing moves and the mode opens where it stands.

**Clicking a blue tile** moves there, as now, ending the action unless the
move lands adjacent to an enemy.

**Clicking the selected unit again, or any cell outside the highlights,
cancels**: the selection drops and both highlights come off. Nothing is
spent — cancelling is always free, so a player can look at a unit's reach
and change their mind.

#### Enemy-selection mode

**When a unit's move ends adjacent to one or more enemies it enters
enemy-selection mode**, rather than attacking anything automatically. The
first enemy is highlighted; the rest are one keypress away.

| input | |
|---|---|
| arrow keys | step through the adjacent enemies, as a **looped** list — off the end wraps to the start |
| `SPACE` / fire 1 | attack the highlighted enemy |
| fire 2 / `ENTER` | cancel the attack and deselect the unit |

**The arrows are modal here.** They cycle the target list, not the cursor.
That is the whole reason the mode exists as a mode: the same keys mean
something different while a target is being chosen, so the mode has to be
visible enough that the player knows which meaning is live. The
highlighted enemy is what makes it visible.

**Cancelling does not undo the move.** The unit has moved; the movement is
spent and the unit stays where it is. Only the attack is forfeited. That
follows from § Actions — the move-then-strike exception is one action, and
declining the strike does not buy the move back.

Cancelling out of this mode follows § The Ladder below.

##### Clicking a red enemy still goes through this mode

Clicking a red enemy names the target, but it does **not** skip
enemy-selection mode: the unit moves into contact and the mode opens with
that enemy highlighted. `SPACE` then confirms the attack.

It costs a keypress for a choice the player has already made, and buys two
things worth more:

- **One flow for every attack.** There is no second path into combat that
  behaves differently — which matters for the code as much as for the
  player, since the attack is entered from one place.
- **A mis-aimed click stays recoverable.** Without the mode, clicking
  would be the only irreversible keypress in the design: § The Ladder
  cannot back out of a decision that has already been executed. With it,
  `ENTER` or fire 2 does.

The arrows still cycle from there, so a player who clicked the wrong one
of two adjacent enemies can switch target without cancelling and starting
again.

#### Which square does it move to?

**The reachable adjacent tile with the best cover.** Ties break by fewest
movement points, so the choice is always deterministic — the player must be
able to learn what the game will do, and "best cover" alone does not settle
it when two tiles match.

The counter-attack rule is why: the square decides how much the unit takes
back (§ Adjacency), so approaching over open ground when a tile one step
further has 50% cover is a real cost. The player is not choosing the
square, so the game owes them the good one.

Two consequences to keep in mind:

- **A unit already adjacent does not move**, even if a neighbouring tile
  has better cover. It attacks where it stands. Shuffling a unit sideways
  before it strikes would spend movement the player did not ask to spend,
  and would move a unit they had deliberately placed.
- **The approach may look indirect.** Best cover is not nearest, so a unit
  will sometimes walk around to arrive from a wood or a city rather than
  straight in. That is the rule working; the blue highlight already shows
  the whole reachable area, so the route is never a surprise about *where*
  it could go, only about which square it chose.

The movement search already produces distances for the tie-break, and
`terrain_cover[]` is a lookup, so this costs a scan of at most four tiles
per highlighted enemy.

#### The colours already work — do not redesign them

Highlighting is attribute-only, one byte per 8x8 cell, so a highlight
repaints a whole cell. That sounds like it would swamp the art, and it does
not, because the three attributes already in use are chosen to avoid it:

```
ATTR_RANGE   0x4F   bright white ink, BLUE paper    movement area
ATTR_TARGET  0x57   bright white ink, RED paper     attackable enemy
ATTR_CURSOR  0x78   black ink,        WHITE paper   cursor
```

**Only the paper changes; the ink stays white.** So the sprite or terrain
pixels in a highlighted cell stay legible instead of vanishing into their
background, and the cursor stays visible on top of either highlight
because it inverts to black-on-white rather than competing for a hue.

This is proven in play and adequate. § Selection and highlighting uses
more of it at once — a whole movement area rather than a cell — but it
introduces no new colour problem and needs no new attributes.

The one thing to preserve is the property that makes it work: **paper
carries the meaning, ink stays bright.** A future highlight that changes
ink instead will disappear against the art it lands on.

### The Ladder

`ENTER` and fire 2 (`X`) both mean **back out of whatever you are in**.
They resolve against the innermost open context first, and only reach the
outermost one when nothing is open.

| context | `ENTER` / fire 2 does |
|---|---|
| choosing a target in enemy-selection mode | cancel the attack, deselect the unit |
| a unit is selected, no target being chosen | deselect it |
| nothing selected | `ENTER` ends the turn; `X` quits to the title |

So `ENTER` still ends the turn — but only when there is nothing to back
out of first. A player reaching for "end turn" with a target highlighted
cancels the attack instead, and has to press it again. That is the right
way round: cancelling costs nothing, whereas ending a turn with an attack
half-given cannot be taken back.

**This is the existing rule generalised, not a new one.** `src/game.c`
already does exactly this for `X` — it drops a held unit on the first
press and quits only on the second — and the comment there says why: "so
the exit cannot be hit while giving an order". Enemy-selection mode is one
more rung on the same ladder.

Two properties to preserve when adding any future context:

- **The two keys stay identical.** One rule to learn, not two exceptions.
  A joystick player has fire 2; a keyboard player has both.
- **Every rung is free.** Nothing on this ladder spends an action, a move
  or a turn. That is what makes it safe to press when unsure, and it is
  why the destructive things — ending a turn, quitting — sit at the bottom
  where they can only be reached with nothing else open.

### Actions

**One action per unit per turn.** A unit either moves or attacks; taking either
one uses the unit up for that turn.

**The turn ends when the player says so**, not when the last unit is spent.
There is no auto-end: a player who has moved everything still presses `ENTER`.
That is deliberate — the alternative snatches the turn away mid-thought, and
the board is worth looking at once all the moves are in. It also means
"forfeit" below has something to forfeit.

The exception is a move that closes with the enemy: **a unit whose movement
ends adjacent to an enemy unit may attack it in the same turn**. Adjacent means
one of the four cardinal neighbours, as movement is 4-way. So a unit can move
*or* attack at range, or move into contact and strike — but never move, attack,
and still have an action left.

### Enemy turn

**Turn order is strictly sided: the player moves first, then the enemy.** Every
player unit that is going to act does so before any enemy unit acts, and ending
the turn with `ENTER` forfeits the actions of any player units that have not
yet been used. The enemy turn is likewise indivisible — control returns to the
player only when every enemy unit has been processed — and the turn counter
increments once per full player-then-enemy round.

Because the turn is indivisible and the player has nothing to do while it runs,
it is a **long operation** (§ Long operations): the banner goes up, the enemy
army is processed, and any key pressed during it is discarded. The AI is under
no obligation to fit a frame — it steps unit by unit so the player can follow
what happened, not because the loop needs servicing.

The enemy cycles through its units and performs one action for each:

- Attack if a target is in range.
- Otherwise pick a target, and move towards it.
- Never path through impassable tiles, and prefer cells outside the player's
  attack ranges.

Once every enemy unit has been processed, the turn returns to the player.

`docs/PLAN.md` § Enemy decisions works this into something implementable — a
threat map built once per turn, then a scored choice per unit — and P5 builds
it. The scoring weights are the part still open, and they belong in
`config/game_config.h` so tuning is a rebuild rather than a code change.


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

Implementation: `src/game.c`, state ids in `include/game.h`. What it calls is
split between `src/logic.c` and `src/render.c` — see § Logic and rendering.

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

- **Drawing belongs in the vblank window. Thinking does not.** The window is a
  hardware fact: after `vsync_wait()` returns, roughly 256 bytes can be written
  to the screen before the raster catches up and the write tears. Anything
  larger is either spread across frames or accepted as a tear — a scroll step
  is 4 608 bytes and takes the second option. **Computation has no such
  limit** — this is the whole reason for the file split; see § Logic and
  rendering.
- **The border flashes green only when a state repaints its screen**, which is
  what SPACE and ENTER cause. It used to wrap `update_state()` as a CPU-budget
  meter, from when a frame's work was one or two cells. A cursor step is now a
  five-frame scroll, so the meter strobed green on every step: an honest
  reading of the budget and an unusable thing to look at.
- **One state is active at a time.** There is no state stack, and every state
  has a single caller, so each just names its destination — `ST_MAP` returns to
  `ST_PLAY` and says so. Nothing needs to remember where it came from. If a
  state ever gains two callers it will need a `back_state` to return through;
  there is deliberately no such variable until then.
- **Input is edge-triggered and debounced**: an action must appear in two
  consecutive frames, then fires once on its rising edge. Held directions
  repeat every `NAV_DELAY` frames. Keyboard and Kempston fold into one action
  byte, so both debounce identically.

## Logic and rendering

The single most useful thing to know about this program is that **its two
halves are paid in different currencies**, and the code is split three ways to
keep them apart.

| | Deadline | Because |
|---|---|---|
| **Logic** — `src/logic.c` | **None.** Take as long as you like. | The game is turn-based. Nothing animates, nothing is on a timer, nothing is waiting on the player. A routine that overruns costs a pause and nothing else. |
| **Rendering** — `src/render.c` | **~256 bytes of screen writes per frame.** | The raster does not wait. After `vsync_wait()` returns, that is roughly what can be written before the beam catches up and the write tears. |
| **The loop** — `src/game.c` | — | Owns the frame, the states and the keyboard, and decides *when* each of the other two runs. |

This is why the two are allowed such different shapes. Logic can be
**heuristic**: flood the whole board, score every candidate cell, scan all 98
cells three times if that is the clearest way to express the rule. The
movement-range fill takes about six tenths of a frame and nobody minds; the
enemy AI will take a fifth of a second for a full army and nobody will mind
that either, because it happens behind a banner (§ Long operations).

Rendering gets no such licence. Every routine in it is sized against the
budget, and anything bigger is spread across frames: a page flip goes out
`PAGE_CELLS` (2) full cells at a time, a movement highlight `RANGE_CELLS` (8)
attribute-only cells at a time, and `render_tick()` pays off at most one of
those debts per frame. **The cost of a routine there is measured in bytes
written, not in what it computes.**

### The seam

`include/board.h` is the whole contract. Logic owns the board — terrain, the
armies, occupancy, the movement costs — and rendering reads it. The rule that
keeps the split honest runs both ways:

- **Logic never draws.** It changes the board and says what is now stale:
  `mark_dirty()`, `recolour_page()`, `start_page_flip()`. It does not know or
  care when that gets painted.
- **Rendering never changes the game.** `attr_view_cell()` decides a cell's
  colour by reading `occupancy`, `selected` and `cost`; it writes none of them.

That second rule is what makes rendering **replaceable**. These routines are
the ones that will be hand-written Z80 — they are the only ones with a
deadline, so they are the only ones where it would buy anything — and a rewrite
has to preserve nothing except the function signatures in `include/render.h`.
Nothing else in the program can tell the difference.

The split is also the answer to "where does this belong?". If it computes
something, it goes in `logic.c` and may take as long as it likes. If it puts
bytes on the screen, it goes in `render.c` and has to say what it costs. If it
decides *when*, it stays in `game.c`.

## Two machines, two render paths

`hw_detect()` sets `is_128k` before anything is drawn, and `src/render.c` is
the only file that reads it. The difference is worth having because a 128K has
something a 48K cannot be given: **a second display file**.

| | 48K | 128K |
|---|---|---|
| Full-screen repaint | drawn straight onto the displayed screen, spread over frames to stay inside the vblank budget | composed into the display file the ULA is *not* showing, then shown |
| Cost to show it | — | one write to port `0x7FFD` |
| What the player sees | the screen being painted | the finished screen, appearing between frames |
| Incremental repaint (a cursor step, a dirty cell) | straight to the screen | the same — two cells is not worth a flip |

The 128K's screens are RAM page 5 at `0x4000` and RAM page 7, and bit 3 of port
`0x7FFD` picks which the ULA shows. `render_compose()` aims drawing at whichever
is not on show; `render_show()` flips. On a 48K both are no-ops and the code
below them never learns which machine it is on — the whole difference is two
functions and a flag.

**The 128K path arms itself only if it can prove paging works.**
`is_128k` is not sufficient: `main()` locks port `0x7FFD` (bit 5) whenever the
+2A/+3 floating bus is not in use, and after that every write to it is ignored
*silently*. The page-in does nothing, the flip does nothing, and the flag is
still set — so a whole screen gets composed into a bank the ULA never shows and
the title screen simply fails to appear, with no other symptom. That has
happened twice. `screens_init()` now writes a sentinel into page 7, pages
something else in, writes a different value, pages back and checks which
survived; only then does it arm. If paging is locked it falls back to the 48K
path and the game renders correctly, just without the shadow screen.

**The shadow screen is therefore off on every machine today**, and the reason
is a +3, not a 128K. `main()` locks paging (`0x7FFD` bit 5), which makes
`screens_init()`'s page-in a no-op, which the sentinel test detects, so the
48K path is used everywhere.

Clearing that lock does give a 128K its shadow screen — verified — but it also
lets those same writes reach a **+2A/+3**, where banking page 7 in behind the
loader's back drops the machine into BASIC with *"Nonsense in BASIC"*. The
lock had been protecting those machines silently all along; removing it looked
free precisely because nothing here tests a +3.

The missing piece is **detecting a +2A/+3 reliably**. `vsync_mode ==
VSYNC_MODE_128K` identifies one only if the mode-2 floating bus was detected;
a +3 that falls back to HALT is indistinguishable from a 128K by anything this
program currently knows. Until there is a real test for it — the +3 decodes
port `0x1FFD`, which is the obvious probe — the lock stays and the shadow
screen with it. A tear-free scroll on one model is not worth a crash on
another.

**Flips happen on state changes and on every scroll sub-step.** `render_compose()` and
`render_show()` bracket the whole-screen painters and nothing else, so the
buffers swap on the keys that move between screens and never while the cursor
is moving. A cursor step presents into the screen already on show — that
tears on both machines, but it does not strobe, which flipping would: the
header, status panel and key legend live only in the buffer they were painted
into, so the other one would come up with a board and no chrome. Making the
scroll itself tear-free means painting the chrome into both buffers, and that
is not done.

Two things had to be fixed before this could be switched on at all, and both
are worth remembering because each failed silently:

- `vsync_wait()` writes its floating bus sync marker to attribute row 22 and
  waits to see that byte come back off the bus. The bus carries what the ULA
  is *fetching*, so with page 7 on display a marker in page 5 is never fetched
  and the wait never ends — the game hung on the title with the input dead.
  `vsync_marker_addr` now follows the live screen.
- `ST_PLAY` used to draw incrementally after a flip, which diverged the two
  buffers and brought the board back unpainted. P7 composes the whole window
  per repaint, so that cannot happen any more.

Composing off-display is not merely tidier, it is **free of the frame budget**:
nobody is looking at the back buffer, so a full repaint there has no deadline
at all. That is the same argument as § Logic and rendering, one level further
down — and it is why the 128K path is the one that matters for P7's scrolling
view, where a whole window has to be rebuilt per scroll step.

### The constraint this puts on the binary

Page 7 is banked in at `0xC000` **once at startup and left there**, so the
shadow screen is addressable without paging around every repaint. That is only
safe while nothing of ours lives at `0xC000` or above — anything up there would
be swapped out of sight the moment page 7 arrives, and the symptom would be
random corruption rather than an honest crash.

So the whole program has to fit between `0x8000` and `0xC000`: **16 KB**, for
code, rodata, data and bss together. `make map` runs `tools/checkmem.py`, which
fails the build if the top symbol reaches `0xC000`. The stack is not part of
that check; z88dk leaves it near `0x7FA0`, in the page-5 RAM that is always
mapped.

`.claude/skills/zx-memory` is the working guide to all of this.

Run **`make memmap`** to see the whole picture. The linker knows where code,
rodata, data and bss went; `include/memmap.h` knows where the buffers went; and
neither view is complete on its own, which is how this has gone wrong before.

The hand-placed buffers sit *above* 0xC000, which on a 128K-class machine is a
paged bank. They survive because bank 0 is selected and then left alone —
`hw_detect()` ends by selecting it, and `main()` locks paging on every machine
that does not need the +2A/+3 floating bus. **That is a real dependency, not a
coincidence to rely on quietly**: if anything ever pages again, these move
first.

Two rules for that port, both learned the hard way. **Mirror every write into
BANKM at `0x5B5C`** — it is write-only, the ROM keeps its own copy there, and
it writes that copy back from the interrupt handler; a flip that skips BANKM is
undone within a frame. And **bit 4 is the ROM select, bit 5 locks paging.** Anything writing that
port must preserve bit 4 or it changes the ROM underneath the running program
— on a +2A/+3 that means +3DOS, whose `0x0038` is not a BASIC interrupt
handler. `hw_detect()` did exactly this and crashed every +3; see
`docs/PLAN.md` § The +3 problem.

## Long operations

This is a turn-based game. Between orders nothing animates, nothing moves on a
timer, and nothing is waiting on the player. A computation that takes longer
than a frame therefore costs a pause and nothing else — so **game logic is
never chopped up to fit the frame**. It runs to completion and the loop misses
however many vsyncs it misses.

What the player must not get is a game that looks dead, or one that acts on a
key pressed half a second ago. So any operation long enough to notice:

1. **Says what it is doing.** A banner replaces the key legend on row 21, in
   red rather than the legend's yellow, and is painted *before* the work
   starts. Every state repaints that row on entry, so nothing has to be saved.
2. **Discards whatever was pressed while it ran.** A held direction or an
   impatient `SPACE` must not fire late. Work that ends in a state change gets
   this free — `enter_state()` already flushes the keyboard — and work that
   stays put restores the legend and flushes explicitly.

"Long enough to notice" means more than a frame or two. A movement-range flood
fill is about six tenths of a frame and gets no banner: it would flash for a
single frame, which reads worse than the pause it was announcing. Loading a
level and placing two armies gets one, and so does the enemy turn.

This is the one place the design spends the player's time freely, and it is
worth being clear about why it is safe: a real-time game could not do this,
because the world would jump while the loop was away. Here the world only
changes when somebody takes a turn.

## States

| State | Screen | Purpose |
|-------|--------|---------|
| `ST_TITLE` | "ZX STRATEGY" + hardware report | Front end; entry to a game |
| `ST_PLAY` | "THE FIELD" — 8x4 window on the board | The game proper |
| `ST_MAP` | "CAMPAIGN MAP" — whole world | Read-only overview, opened from play |
| `ST_OVER` | win / lose message | Ends a level; advances or abandons the campaign |
| `ST_WON` | "CAMPAIGN COMPLETE" | The last level was won; the campaign is over |

Every state is part of the game. **Music is not a state**: the tune is a
blocking call `ST_TITLE` makes, which is what § Long operations is for.

**`SPACE` is the key that moves you on**, everywhere: it starts a game, closes
the overview and takes the level-end screen on, and inside `ST_PLAY` it is also
what picks a unit up and orders its move. **Fire 1 and `Z` mean the same
thing**, so a Kempston stick can play the game without reaching for the
keyboard.

The single exception is **ending a turn, which is `ENTER` and only `ENTER`**.
Fire 1 used to end the turn as well, which was wrong twice over: a joystick
could never pick a unit up, and any setup mapping fire onto the space bar
selected a unit and ended the turn in the same frame — `end_turn()` deselects,
so SPACE appeared to do nothing but advance the counter. Ending a turn is
therefore the one thing a joystick alone cannot do; it is also the one action
that throws away everything you have not used, so needing a deliberate reach
for it is no bad thing.

```
            ┌──────────┐  SPACE/FIRE   ┌──────────┐   M    ┌──────────┐
            │ ST_TITLE │ ────────────▶ │ ST_PLAY  │ ─────▶ │  ST_MAP  │
            │          │ ◀─────── X ── │          │ ◀───── │          │
            └──────────┘               └────┬─────┘ SPACE  └──────────┘
              the tune plays on              │ a base or an army is lost
              entry and blocks until          ▼
              a key stops it             ┌──────────┐
                                         │ ST_OVER  │
                                         └────┬─────┘
                                              │
                        ├─ WIN, level < 10 ─▶ level++, load ─▶ ST_PLAY
                        ├─ WIN, level 10 ─▶ ST_WON ─ any key ─▶ ST_TITLE
                        └─ LOSE ───────────────────────────────▶ ST_TITLE
```

### ST_TITLE

- **Shows**: detected machine (48K/128K), Kempston presence, which of the three
  vsync modes is active, and the key legend. The hardware report doubles as a
  smoke test — if vsync fell back to HALT, the player sees it.
- **Per frame**: nothing.
- **Exits**: `SPACE` (or fire 1) starts a game — sets the level to 1
  and the turn counter to 1, loads that level's map and enters `ST_PLAY`.
- **Music**: the Tritone tune **plays itself** as the last thing entering this
  state does, on boot and on every return to the title. There is no key that
  starts it. The player blocks with interrupts off until a key is pressed — it
  owns the speaker and cannot share the frame loop — so it is a **long
  operation** (§ Long operations): a `PLAYING - PRESS A KEY` banner goes up on
  row 21, the tune runs, the banner clears and the key that stopped it is
  flushed. No state change, and nothing else on the screen is touched, so there
  is nothing to repaint.
- **The title is therefore unresponsive until the tune is stopped.** That is
  the cost of a beeper engine that owns the CPU, and the reason the banner
  exists: it is the difference between "the game is waiting for you" and "the
  game has hung". Starting a game takes two keypresses from cold — one to stop
  the tune, then `SPACE`. The screen is painted before the tune starts, so
  there is something to read in the meantime.

### ST_PLAY

- **Shows**: an **8x4 window** on the world in 4x4-character tiles (rows 1-16,
  full screen width), the units standing on it, the cursor, and a four-line
  status panel on rows 17-20: the unit under the cursor, the turn number, the
  cursor's cell, and its terrain with that terrain's cover. Where the window
  runs off the board it shows sea.
- **Per frame**: move the cursor (held directions repeat), repaint the two
  cells a step changed, advance a page flip if one is in progress, refresh the
  status panel when dirty.
- **Selection**: `SPACE` picks up the unit under the cursor and `SPACE` again
  (or `X`) puts it down. Only the player's own units can be selected, and only
  ones that still have their action; an enemy unit under the cursor still
  reports its stats, because knowing what is about to shoot you is not a
  privilege. A selected unit keeps its highlight while the cursor wanders,
  which is what lets the player look at the ground before committing to a move.
- **Ordering a move**: picking a unit up floods its movement range and washes
  the reachable ground blue. `SPACE` on one of those cells sends the unit
  there; `SPACE` anywhere else — its own cell, or ground out of reach — puts it
  down instead, so there is one key for the whole order and no way to
  half-issue it. Orders are ignored while a page flip is running, for the same
  reason cursor movement is.
- **Spent units go dim**: a unit that has used its action is drawn in
  non-bright green, in the board and in the status panel alike, so what is left
  to move is readable at a glance. The enemy has no dim form — non-bright red
  is `0x02`, which the floating bus sync marker reserves — and does not need
  one.
- **Ending the turn**: `ENTER` — and not fire 1, which acts — returns every unit's action and
  advances the turn counter. This is the one screen where the "go on" key is
  not `SPACE`, because `SPACE` is busy giving orders here.
- **The view moves, the cursor does not.** The cursor is pinned to a fixed
  screen cell and a direction pushes the world past it, one tile per step. Off
  the edge of the board is sea, which is what lets the cursor reach a corner —
  the window is never clamped to the map. See § Cursor and movement.

  The window is composed in a linear off-screen buffer and pushed one
  character at a time, four sub-steps to a tile. Composing there costs no
  screen-address arithmetic at all, which is most of why it is quick; see
  `docs/PLAN.md` § P7. It still tears on a 48K, because presenting is 4 608
  bytes against a ~256-byte window — the 128K shadow screen is what removes
  that, and now that the whole window is composed per repaint, nothing stands
  in its way but `shadow_ok`.
- **Exits**: `ENTER` ends the turn, `M` → `ST_MAP`, `X` → title.

### ST_MAP

- **Shows**: the **whole world** in 2x2-character tiles, every unit on it, the
  play cursor's cell highlighted in yellow, and a second free cursor with the
  same status panel reporting whatever it is over.
- **Per frame**: move the cursor, repaint the two cells it left and entered,
  refresh the status panel.
- **Read-only by design**: the overview exists to plan, not to act. Issuing
  orders from here is a candidate for the first rules pass.
- **Exits**: `SPACE` (also `X` or fire) returns to `ST_PLAY`. The overview
  cursor is seeded at the play cursor's cell each time it opens.

### ST_OVER

- **Shows**: a win or lose message on a cleared screen, the level just played,
  and a prompt for the key that continues.
- **Per frame**: nothing.
- **Exits**: `SPACE` (or fire 1). On a **loss** that goes back to `ST_TITLE` — the campaign
  is over and the next game starts at level 1. On a **win** it increments the
  level, loads that level's map, repopulates both armies and enters `ST_PLAY`;
  past the last level — `++level > LEVEL_COUNT` — it goes to `ST_WON` instead.
- The load is a **long operation**: rebuilding `terrain[]` and placing two
  armies is far more than a frame's work, so a `DEPLOYING...` banner goes up
  first. No banner has to be taken down — `enter_play()` repaints the screen
  and `enter_state()` flushes the keyboard.
- **Loading another level** already works: the campaign is ten 14x7 maps,
  `assets/maps/level_1.tmx` .. `level_10.tmx`, ZX0'd into `include/level_N.h`
  by the build and reached through `level_maps[]` in `src/game.c`. `load_map()`
  decompresses whichever level `level` names into `terrain[]`, flattens it into
  the per-cell entry costs the movement fill reads, parks the cursor on that
  level's `start` object and calls `populate_map()`. All ten maps cost ~330
  bytes compressed.

### ST_WON

- **Shows**: "CAMPAIGN COMPLETE" on a cleared screen, with the number of levels
  won and a prompt.
- **Per frame**: sample the keyboard once, waiting for a release then a press.
- **Exits**: any key returns to `ST_TITLE`, which resets the level to 1 when
  the player starts again. The release-then-press debounce means the keypress
  that finished the last level cannot dismiss the ending.
- Reached only from `ST_OVER`, when a win takes the level past `LEVEL_COUNT`.

## Adding a state

1. Add the `ST_` id to `include/game.h`.
2. Add an `enter_*` function that paints the screen once, and its case in
   `enter_state()`.
3. Add per-frame work to `update_state()` only if the state animates.
4. Add its transitions to `handle_input()`.

Before adding one, check it needs to be a state at all. Something that blocks
and then hands control straight back — the tune is the example — is a **long
operation**, not a state: it has no per-frame work, no transitions of its own
and nothing to repaint on the way out.

Screen budget to respect: row 0 is the title bar, rows 17-21 the status panel
and hint line (a state without a status panel may use them for its own text,
as `ST_TITLE` does with row 20), **row 22 must
stay pixel-blank** (the floating bus sync marker lives in its attributes), and
no attribute anywhere may be `0x03`.

Row 21 is also where a long operation puts its banner, so a state that runs one
should keep its legend to a single named string it can restore — `PLAY_HINT` in
`src/game.c` is the example.

