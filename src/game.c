/*
 * game.c — the frame loop and the state machine
 *
 * One iteration of game_run() is one 50 Hz frame:
 *
 *     vsync_wait()          sync to the beam (floating bus)
 *     border RED
 *       update_state()      what the active state owes the screen
 *     border BLACK
 *     poll_input()          one debounced sample of keyboard + Kempston
 *     handle_input()        orders and state transitions
 *     [enter_state()]       only if a transition was requested
 *
 * This file owns the loop, the states, the keyboard and nothing else.
 * The work is split three ways, and the seam is the point
 * (docs/DESIGN.md § Logic and rendering):
 *
 *   src/logic.c   the board and the rules.  No deadline: the game is
 *                 turn-based, so a routine there may overrun the frame
 *                 and the loop just misses a vsync.
 *   src/render.c  everything that writes to the screen.  Hard deadline:
 *                 ~256 bytes between vsync_wait() returning and the
 *                 raster catching up, so big repaints are paid off
 *                 across frames.
 *   game.c        decides *when* each of those runs.
 *
 * States (see include/game.h):
 *
 *   ST_TITLE    front end; machine/vsync report, waits for START
 *   ST_PLAY     the game proper: an 8x4 page of the world, flipped when
 *               the cursor walks off its edge
 *   ST_MAP      campaign overview, opened with M from ST_PLAY and
 *               dismissed with SPACE; free cursor over the terrain grid
 *   ST_OVER     a level ended: a win loads the next one, a loss quits
 *   ST_WON      the last level was won; the campaign is over
 *
 * Controls:  Q/A/O/P or Kempston  move the cursor
 *            SPACE                the one "go on" key: starts a game,
 *                                 picks a unit up and puts it down,
 *                                 orders a move, closes the overview,
 *                                 and takes the level-end screen on.
 *                                 Fire 1 does the same on the screens a
 *                                 joystick has to get through.
 *            Z / fire 1           the same as SPACE: act
 *            ENTER                end the turn (ST_PLAY only, because
 *                                 SPACE is busy giving orders there).
 *                                 The one thing a joystick cannot do.
 *            X / fire 2           back (drops the held unit first)
 *            M                    campaign overview (ST_PLAY only)
 *
 * The tune plays itself whenever the title screen is entered, and any
 * key stops it; there is no key that starts it.
 *
 * The border flashes GREEN while a state repaints its screen — the work
 * SPACE and ENTER cause.  It is deliberately quiet during cursor
 * movement, which is a multi-frame scroll and would strobe.
 */

#include "../config/app_config.h"
#include "../config/game_config.h"
#include "../include/board.h"
#include "../include/game.h"
#include "../include/hw.h"
#include "../include/gfx.h"
#include "../include/input.h"
#include "../include/music.h"
#include "../include/render.h"
#include "../include/vsync.h"

/* Action bits — keyboard and Kempston are folded into one byte so a
   single edge test debounces both. */
#define ACT_UP      0x01
#define ACT_DOWN    0x02
#define ACT_LEFT    0x04
#define ACT_RIGHT   0x08
#define ACT_CANCEL  0x20
#define ACT_SPACE   0x40
#define ACT_M       0x80    /* the campaign overview, in ST_PLAY only */
#define ACT_QUIT    0x10    /* X: leave the level.  Its own key, not a
                               rung on the Cancel ladder -- backing out
                               of an order and abandoning the level are
                               different intentions. */

#define ACT_DIRS    (ACT_UP | ACT_DOWN | ACT_LEFT | ACT_RIGHT)

/* ACTION, everywhere.  SPACE and fire 1 and nothing else: one key means
   yes on every screen, so a joystick can play the whole game and the
   hint line never has to list alternatives.  See docs/DESIGN.md
   § Action and Cancel. */
#define ACT_GO      ACT_SPACE

/* Keyboard half-rows not covered by input.h. */
#define KEY_ENTER_ROW 0xBFFE    /* ENTER = bit 0            */
#define KEY_SPACE_ROW 0x7FFE    /* SPACE = bit 0, M = bit 2 */
#define KEY_ZX_ROW    0xFEFE    /* CAPS Z X C V: X = bit 2  */

/* Frames between cursor steps while a direction is held. */
#define NAV_DELAY   5

uint8_t game_state;

static uint8_t next_state;
static uint8_t acts, last_acts, prev_stable, edge;
static uint8_t nav_delay;
static uint8_t redraw_status;

/* An irreversible action waiting on a yes.  Both of these throw work
   away and neither can be undone, which is the whole test for whether
   something needs asking about (docs/DESIGN.md § Action and Cancel). */
#define CONFIRM_NONE 0
#define CONFIRM_TURN 1
#define CONFIRM_QUIT 2
static uint8_t confirm;

/* Y= and N= rather than YES= and NO=: eleven bytes, which is what the
   title tune needed to fit under the 0xC000 ceiling.  The keys are named
   in full, which is the part a player cannot guess. */
#define HINT_TURN "END TURN? Y=SPACE N=ENTER"
#define HINT_QUIT "QUIT? Y=SPACE N=ENTER"

/* The eight keyboard half-rows, in the usual port order. */
static const uint16_t key_rows[8] = {
    0xFEFE, 0xFDFE, 0xFBFE, 0xF7FE, 0xEFFE, 0xDFFE, 0xBFFE, 0x7FFE
};

/* --------------------------------------------------------------- input */

static uint8_t scan_actions(void)
{
    uint8_t k = scan_input();
    uint8_t a = 0;

    if (k & INPUT_UP)    a |= ACT_UP;
    if (k & INPUT_DOWN)  a |= ACT_DOWN;
    if (k & INPUT_LEFT)  a |= ACT_LEFT;
    if (k & INPUT_RIGHT) a |= ACT_RIGHT;
    /* Fire 1 (and Z) mean ACT — the same as SPACE — not "end turn".
       They used to land in ACT_SELECT, which on the board is the
       end-turn key, so a joystick could never pick a unit up and any
       setup that maps fire onto the space bar selected a unit and ended
       the turn in the same frame.  ENTER is now the only thing that
       ends a turn. */
    if (k & INPUT_FIRE1) a |= ACT_SPACE;    /* fire 1 = Action */
    if (k & INPUT_FIRE2) a |= ACT_CANCEL;   /* fire 2 = Cancel */

    if (!(read_keys(KEY_SPACE_ROW) & 0x01)) a |= ACT_SPACE;   /* Action */
    if (!(read_keys(KEY_ENTER_ROW) & 0x01)) a |= ACT_CANCEL;  /* Cancel */
    if (!(read_keys(KEY_SPACE_ROW) & 0x04)) a |= ACT_M;
    if (!(read_keys(KEY_ZX_ROW) & 0x04))    a |= ACT_QUIT;    /* X */

    return a;
}

#if DEBUG_STATE_WALK
/* P0 state walk: W wins the level, L loses it.  Kept out of the action
   byte — every bit of it is taken, and this is temporary scaffolding
   that comes out with the real win check in P4. */
#define DBG_WIN     0x01
#define DBG_LOSE    0x02

static uint8_t dbg_last, dbg_prev, dbg_edge;

static void poll_debug(void)
{
    uint8_t a = 0, stable;

    if (!(read_keys(KEY_QWERT) & 0x02))     a |= DBG_WIN;    /* W */
    if (!(read_keys(KEY_ENTER_ROW) & 0x02)) a |= DBG_LOSE;   /* L */

    stable = (uint8_t)(a & dbg_last);
    dbg_last = a;
    dbg_edge = (uint8_t)(stable & ~dbg_prev);
    dbg_prev = stable;
}
#endif

/* An action counts only when the same bit is seen in two consecutive
   frames, then fires on its rising edge: held keys act once, and a
   single noisy read is ignored. */
static void poll_input(void)
{
    uint8_t stable;

    acts = scan_actions();
    stable = (uint8_t)(acts & last_acts);
    last_acts = acts;
    edge = (uint8_t)(stable & ~prev_stable);
    prev_stable = stable;
}

/* Forget the current key state so the key that caused a transition is
   not read again by the state we land in. */
static void flush_input(void)
{
    last_acts = prev_stable = 0xFF;
    edge = 0;
    nav_delay = NAV_DELAY;
#if DEBUG_STATE_WALK
    dbg_last = dbg_prev = 0xFF;
    dbg_edge = 0;
#endif
}

/* True if any key on the whole keyboard, or the joystick, is down.
   Each row is masked to its 5 key bits — the upper bits carry EAR and
   the floating bus, not keyboard state.

   Port 0x00FE selects EVERY half-row at once, so one read answers for the
   whole keyboard rather than eight.

   This exists for ONE job: waiting for a press to be let go of before
   something else starts listening. edge/prev_stable cannot do it — they
   describe what the input layer has decided, not what the player's finger
   is doing, and a Tritone tune reads the hardware directly. */
static uint8_t any_key(void)
{
    if ((read_keys(0x00FE) & 0x1F) != 0x1F) return 1;
    return (uint8_t)(scan_input() != 0);
}

/* --- Long operations -------------------------------------------------
   The game is turn-based, so nothing animates and nothing waits on a
   clock: work that overruns a frame costs a pause and nothing else, and
   game logic is never chopped up to fit the frame (docs/DESIGN.md
   § Long operations).

   What the player must not get is a game that looks dead, or one that
   acts on a key pressed half a second ago.  So an operation long enough
   to see says what it is doing, and throws away whatever was pressed
   while it ran.

   Pass a message padded to the full 31 columns; it overwrites the hint
   line in place. */
static void busy_on(const char *msg)
{
    render_busy(msg);
}

/* Put the legend back and forget the keyboard.  Only needed when the
   work does NOT end in a state change: enter_state() repaints the whole
   screen and flushes input already. */
static void busy_off(const char *hint)
{
    render_hint(hint);
    flush_input();
}

/* Play the tune.  The Tritone player blocks with interrupts off until a
   key is pressed — it owns the speaker and cannot share the frame loop —
   which used to make it a state of its own.  It does not need to be: it
   is a long operation like any other, so it borrows the hint row to say
   so and flushes the key that stopped it.  Nothing else on the title
   screen is disturbed, so nothing has to be repainted afterwards. */
/* play_music() is gone.  The tune was a thing you ASKED FOR on the title
   screen, bracketed by a busy banner; it now plays over the boot logo as
   soon as loading finishes (splash()), so there is nothing to bracket and
   nowhere to call it from. */

/* ------------------------------------------------------------- states */

/* Paint the state's screen, then do whatever entering it means beyond
   painting.  The split is deliberate: render.c knows how to draw each
   screen and nothing else, and this is where the consequences live. */
/* How long a cutscene is guaranteed to stay on screen, in FRAMES, before
   any press can dismiss it.  ONE SECOND: long enough that the picture
   registers, short enough that a deliberate player is not held up.
 *
 * Past the hold, pressing DOES dismiss it -- five fast taps still get
 * through, and should: mashing the key means "skip this".  What the hold
 * guarantees is that the cutscene is never invisible, which is what the
 * bug was.
 *
 * A MINIMUM TIME, not a demand for quiet.  The first attempt required the
 * keyboard to be silent before the tune started, which fixed the skip and
 * introduced something worse: every press restarted the count, so a
 * player mashing the key -- exactly what someone impatient with a
 * cutscene does -- could never get past it.  render_paths caught that
 * immediately by hammering SPACE and hanging at the title.
 *
 * Also NOT counted in loop iterations, which is what the first version
 * did: 200 of them sounded like a fifth of a second and was nearer six
 * milliseconds.  A counter whose unit is "however fast this CPU spins"
 * cannot express a human timescale. */
#define CUTSCENE_HOLD   50

static void enter_state(uint8_t s)
{
    game_state = s;
    confirm = CONFIRM_NONE;     /* no question survives a state change */

    switch (s) {
        case ST_TITLE:
            render_title();
            /* No tune here.  It plays over the boot logo the moment
               loading finishes -- see splash() -- so the march starts
               ~30 seconds earlier and covers the tape instead of being
               something the player goes and asks for. */
            break;
        case ST_PLAY:
            render_play();
            break;
        case ST_MAP:
            render_map();
            break;
        case ST_OVER:
            render_over();
            break;
        case ST_CUTSCENE:
            render_cutscene((uint8_t)(level - 1));
            /* lowlands_play() directly, NOT play_music().
             *
             * play_music() brackets the tune with busy_on()/busy_off(),
             * which draw chrome and PRESENT -- and presenting flips to
             * the shadow screen, which the cutscene never drew into.  The
             * picture vanished and the display was left on a screen with
             * nothing on it, which reads as a lock.
             *
             * The tune blocks either way; the picture just has to survive
             * it.
             *
             * WAIT FOR THE RELEASE FIRST.  The press that chose the level
             * is still physically down at this point, and lowlands_play()
             * reads the hardware -- so without this it returns instantly
             * and the cutscene flashes past unseen.  flush_input() cannot
             * help: it runs at the end of enter_state(), and it clears the
             * input layer's idea of the keyboard, not the keyboard. */
            /* Hold the picture on screen before anything can dismiss it.
             *
             * A release check alone is not enough.  render_cutscene()
             * takes a visible moment -- a bank page, a decompress and two
             * 6912-byte copies -- so a player whose keypress appears to
             * do nothing taps again, which is the normal human response.
             * The second tap lands after the release check has passed and
             * stops the tune the instant it starts, so the picture is
             * gone before it is seen.
             *
             * Reproduced by tapping twice 0.12 s apart; a single press,
             * or a 1.2 s hold, never showed it, which is why this
             * survived several rounds of "cannot reproduce".
             *
             * See CUTSCENE_HOLD for why this is a fixed time rather than
             * a wait for the keyboard to fall quiet. */
            {
                uint8_t f;

                for (f = 0; f < CUTSCENE_HOLD; f++)
                    vsync_wait();       /* the picture, guaranteed seen */
            }
            while (any_key()) { }       /* ...then let go of the burst */
            lowlands_play();

            /* The tune's RETURN is the dismissal.  It only returns on a
               key or fire, so that press has happened and been spent;
               asking the input loop for another made the player press
               twice to get past one picture.  Same shape as splash():
               where a tune blocks until input, the tune IS the wait, and
               nothing after it may wait again. */
            next_state = ST_PLAY;
            break;
        case ST_WON:
            render_won();
            break;
    }
    flush_input();
}

static void set_state(uint8_t s)
{
    next_state = s;
}

/* Held directions repeat every NAV_DELAY frames.  Returns 1 when a step
   is due and writes the target cell to *nx / *ny. */
static uint8_t nav_step(uint8_t x, uint8_t y, uint8_t *nx, uint8_t *ny)
{
    *nx = x;
    *ny = y;

    if (!(acts & ACT_DIRS)) {
        nav_delay = 0;
        return 0;
    }
    if (nav_delay) {
        nav_delay--;
        return 0;
    }

    if ((acts & ACT_LEFT)  && x > 0)              (*nx)--;
    if ((acts & ACT_RIGHT) && x < GRID_COLS - 1)  (*nx)++;
    if ((acts & ACT_UP)    && y > 0)              (*ny)--;
    if ((acts & ACT_DOWN)  && y < GRID_ROWS - 1)  (*ny)++;

    nav_delay = NAV_DELAY;
    return (uint8_t)(*nx != x || *ny != y);
}

static void move_cursor(void)
{
    uint8_t nx, ny;

    if (!nav_step(cur_x, cur_y, &nx, &ny)) return;

    /* Only colours change as the cursor moves, so the tiles underneath
       are left alone: put the vacated cell's own attributes back, then
       flood the new one. */
    if (cur_x == cursor_x && cur_y == cursor_y)
        solid_map_cell(cur_x, cur_y, ATTR_HINT);    /* the play cursor */
    else
        attr_map_cell(cur_x, cur_y);
    cur_x = nx;
    cur_y = ny;
    solid_map_cell(cur_x, cur_y, ATTR_CURSOR);
    redraw_status = 1;
}

/* The cursor roams the grid, stopped only by its edges: it is where the
   player is looking, not something standing on the board, so water and
   occupied tiles do not block it (docs/DESIGN.md § Movement Range).
   Passability is a constraint on the unit being ordered, and is checked
   when the order is issued.  A step inside the page repaints just the
   two cells that changed; a step off it starts a page flip. */
/* A direction moves the WORLD, not the cursor: the cursor is pinned to
   CURSOR_VX/VY and the window slides under it (docs/DESIGN.md § Cursor
   and movement).  Nothing on screen survives a step, so there is no
   two-cell shortcut left and no page flip to spread — the whole window
   is repainted in one go.

   That is ~4 608 bytes against a ~256-byte vblank window, so it tears.
   Deliberately, for now: the alternative is spreading it over 16 frames,
   which makes a single cursor step take a third of a second.  The double
   buffer is what removes the tear (docs/PLAN.md § P7). */
static void cursor_to(uint8_t cell);

static void move_play_cursor(void)
{
    uint8_t nx, ny;
    int8_t dx, dy;

    if (!nav_step(cursor_x, cursor_y, &nx, &ny)) return;

    /* The arrows are MODAL in enemy-selection mode: they walk the target
       list instead of the cursor (docs/DESIGN.md § Enemy-selection mode).
       The cursor then follows the chosen target, which is what makes the
       mode visible -- there is no separate highlight to maintain. */
    if (targeting) {
        int8_t d = (int8_t)((nx > cursor_x || ny > cursor_y) ? 1 : -1);
        targeting_step(d);
        cursor_to(target_now());
        return;
    }

    dx = (int8_t)(nx - cursor_x);
    dy = (int8_t)(ny - cursor_y);
    cursor_x = nx;
    cursor_y = ny;
    redraw_status = 1;

    set_page();
    scroll_view(dx, dy);
}

/* --- The enemy turn --------------------------------------------------
 * logic.c decides; this paces it and moves the view.  One unit every
 * ENEMY_BEAT frames, with the window travelling to whoever is acting —
 * the player has to see what happened, and a board that changes all at
 * once tells them nothing.
 *
 * Input is discarded throughout (poll_input() still runs, so nothing is
 * queued up to fire the moment control returns). */
#define ENEMY_BEAT  14          /* frames between enemy actions */

static uint8_t enemy_active;
static uint8_t enemy_beat;

/* Put the cursor on a cell and bring the window with it. */
static void cursor_to(uint8_t cell)
{
    if (cell == NO_CELL) return;
    cursor_x = col_of[cell];
    cursor_y = (uint8_t)(cell / GRID_COLS);
    set_page();
    render_play();
    redraw_status = 1;
}

/* Bring the window to a cell without a scroll.  The enemy can act
   anywhere on the board, including several cells away, and sliding
   there would take longer than the move itself. */
static void view_to(uint8_t cell)
{
    cursor_x = col_of[cell];        /* a table, not a divide */
    cursor_y = (uint8_t)(cell / GRID_COLS);
    render_play();          /* set_page, whole window, one clean reveal */
    render_hint("      ENEMY TURN");
    redraw_status = 1;
}

static void enemy_tick(void)
{
    uint8_t cell;

    if (enemy_beat) {
        enemy_beat--;
        return;
    }
    enemy_beat = ENEMY_BEAT;

    cell = enemy_step();
    if (cell == NO_CELL) {
        enemy_active = 0;
        render_hint(PLAY_HINT);
        return;
    }
    view_to(cell);
}

/* Per-frame work for the active state, run inside the vblank window. */
/* Everything the active state owes the screen this frame, inside the
   vblank window.  The renderer decides how much of its debt fits;
   this only says when. */
static void update_state(void)
{
    switch (game_state) {
        case ST_PLAY:
            if (enemy_active) {
                enemy_tick();
                render_tick();
                if (redraw_status) {
                    draw_status("ENEMY  :", cursor_x, cursor_y);
                    redraw_status = 0;
                }
                break;
            }
            move_play_cursor();
            render_tick();
            if (redraw_status) {
                draw_status("CURSOR :", cursor_x, cursor_y);
                redraw_status = 0;
            }
            break;

        case ST_MAP:
            move_cursor();
            if (redraw_status) {
                draw_status("CURSOR :", cur_x, cur_y);
                redraw_status = 0;
            }
            break;

        default:
            break;
    }
}

/* State transitions driven by the debounced action edges. */
static void handle_input(void)
{
    switch (game_state) {
        case ST_TITLE:
            if (edge & ACT_GO) {
                turn = 1;
                level = 1;
                campaign_score = 0;
                /* Decompressing a map and placing two armies runs long.
                   No banner is put back: enter_play() repaints the
                   screen and enter_state() flushes the keyboard. */
                busy_on("DEPLOYING...");
                load_map();
                /* Through the cutscene on a 128K; a 48K has no bank to
                   read it from and goes straight to the board. */
                set_state(is_128k ? ST_CUTSCENE : ST_PLAY);
            }
            break;

        case ST_PLAY:
            /* A question is on the hint line: answer it and nothing
               else.  ACTION is yes and CANCEL is no, the same two keys
               as everywhere -- a confirmation is not a special mode, it
               is the ordinary pair asked a narrower question.
               
               Short-circuits the rest of ST_PLAY so a yes cannot also
               pick a unit up in the same frame. */
            if (confirm) {
                if (edge & ACT_GO) {
                    uint8_t what = confirm;

                    confirm = CONFIRM_NONE;
                    if (what == CONFIRM_QUIT) {
                        set_state(ST_TITLE);
                    } else {
                        end_turn();
                        /* Hand straight over: the turn counter has
                           already moved on, so what follows is theirs. */
                        enemy_begin();
                        enemy_active = 1;
                        enemy_beat = ENEMY_BEAT;
                        render_hint("      ENEMY TURN");
                        redraw_status = 1;
                    }
                } else if (edge & ACT_CANCEL) {
                    confirm = CONFIRM_NONE;
                    render_hint(PLAY_HINT);
                }
                break;
            }

            /* SPACE is the one order key: with nothing held it picks
               the unit under the cursor up, and with a unit held it
               either sends it to the highlighted cell the cursor is on
               or puts it down again.  Only the player's own units can
               be held, and only ones that still have their action; an
               enemy under the cursor is reported in the panel but never
               selectable. */
            /* Orders wait for a page flip to finish, exactly as cursor
               movement does: the flip is part way through repainting
               the page, so a move made now could have its two cells
               drawn before the unit had left one and reached the
               other. */
            if (enemy_active) break;    /* their turn: input is ignored */

            if (edge & ACT_SPACE) {
                uint8_t cell = cell_of(cursor_x, cursor_y);
                uint8_t u = occupancy[cell];

                if (targeting) {
                    /* SPACE confirms the highlighted target.  Nothing else
                       on this screen can act while the mode is up. */
                    render_hint("");
                    sfx(SFX_ATTACK);
                    attack(target_now());
                    targeting = 0;
                } else if (selected == NO_UNIT) {
                    /* An own unit with an action left can be ordered; ANY
                       enemy can be looked at.  select_unit() decides which
                       from the side bit and sets `inspecting`. */
                    if (u != NO_UNIT &&
                        ((u_flags[u] & (U_SIDE | U_ACTED)) == SIDE_PLAYER
                         || (u_flags[u] & U_SIDE)))
                        select_unit(u);
                } else if (inspecting) {
                    /* Nothing can be ordered while looking at an enemy.
                       SPACE on another unit looks at that one instead,
                       which saves a cancel between two glances; anywhere
                       else puts the board back. */
                    if (u != NO_UNIT && u != selected) select_unit(u);
                    else                               deselect();
                } else if (is_target(cell)) {
                    /* An enemy washed red.  Checked before the move,
                       because an occupied cell is never in the movement
                       set anyway -- the two can never both be true.

                       Clicking does NOT attack.  Either the unit can
                       already hit it, in which case enemy-selection mode
                       opens where it stands, or it closes to the
                       best-cover adjacent tile first and the mode opens
                       there.  Both ways the player confirms, so a
                       mis-aimed click stays recoverable
                       (docs/DESIGN.md § Clicking a red enemy).

                       "Can I hit it from here" rather than "am I
                       adjacent": a Cannon shoots at range 4 and is never
                       adjacent to anything it fires at, and asking the
                       narrower question left it unable to fire at all. */
                    if (targeting_open(selected, cell)) {
                        cursor_to(target_now());
                        /* Say so.  The cursor is the "which target" cue,
                           but the player just put it there themselves, so
                           for a unit that does not move -- a Cannon -- the
                           first press changed nothing visible and looked
                           like a shot that did no damage. */
                        render_hint("QAOP PICK  SPACE FIRE  ENTER BACK");
                    } else {
                        uint8_t step = best_adjacent(cell);

                        if (step != NO_CELL) {
                            sel_x = col_of[u_cell[selected]];
                            sel_y = (uint8_t)(u_cell[selected] / GRID_COLS);
                            move_selected_to(step);
                            targeting_open(selected, cell);
                            cursor_to(target_now());
                            render_hint("QAOP PICK  SPACE FIRE  ENTER BACK");
                        }
                    }
                } else if (u == NO_UNIT && cost[cell] != NO_COST &&
                           !(u_flags[selected] & U_ACTED)) {
                    move_selected();
                } else {
                    /* Its own cell, ground it cannot reach, or a unit
                       that has already moved and has nothing adjacent. */
                    deselect();
                }
                redraw_status = 1;
            }
            /* ENTER, not SPACE: SPACE is giving orders on this screen,
               and it is the only screen where the two differ.

               Belt and braces: never when SPACE fired in the same
               frame.  Fire 1 used to land in ACT_SELECT, so a setup
               mapping fire onto the space bar picked a unit up and
               ended the turn in one press — end_turn() deselects, so
               SPACE looked like it did nothing but advance the turn.
               Fire 1 is ACT_SPACE now and the collision is gone at
               source, but one keypress should still mean one action. */
            /* THE LADDER (docs/DESIGN.md § Action and Cancel).
             *
             * Cancel backs out of the INNERMOST open context and only
             * reaches the turn when there is nothing left to back out
             * of.  With a unit held it deselects -- otherwise reaching
             * for "end turn" while looking at a unit's reach throws the
             * turn away, whereas putting the unit down costs nothing.
             * The safe rung comes first, always.
             *
             * The last rung ASKS.  Ending a turn cannot be undone, and
             * the ladder means a player can arrive at it by pressing
             * Cancel twice without meaning to. */
            if ((edge & ACT_CANCEL) && !(edge & ACT_SPACE)) {
                if (targeting)                targeting_cancel();
                else if (selected != NO_UNIT) deselect();
                else {
                    confirm = CONFIRM_TURN;
                    render_hint(HINT_TURN);
                }
            }

            /* X leaves the level, and is NOT a rung on the ladder:
               backing out of an order and abandoning a level are
               different intentions, and one should never be reachable by
               repeating the other.  It asks too. */
            if (edge & ACT_QUIT) {
                confirm = CONFIRM_QUIT;
                render_hint(HINT_QUIT);
            }

            /* Something died: play it before anything else changes, so
               the explosion lands where the unit was rather than where
               the board has since moved on to. */
            if (boom_cell_at != NO_CELL) {
                render_boom(col_of[boom_cell_at],
                            (uint8_t)(boom_cell_at / GRID_COLS));
                boom_cell_at = NO_CELL;
            }
            /* A base or an army was destroyed.  logic.c raised the flag
               and set player_won; turning it into a state change is the
               loop's job, not its. */
            if (outcome_ready) {
                outcome_ready = 0;
                /* Banked HERE, once, at the transition -- render_over()
                   runs every frame the screen is up, so adding it there
                   would count the level again on each one. */
                if (player_won) campaign_score += level_score();
                set_state(ST_OVER);
            }
#if DEBUG_STATE_WALK
            /* Stand in for the win check until P4 gives us one. */
            if (dbg_edge & (DBG_WIN | DBG_LOSE)) {
                player_won = (uint8_t)((dbg_edge & DBG_WIN) ? 1 : 0);
                set_state(ST_OVER);
            }
#endif
            break;

        case ST_MAP:
            /* Read-only overview: SPACE (or back) dismisses it. */
            if (edge & (ACT_GO | ACT_CANCEL))
                set_state(ST_PLAY);
            break;

        case ST_OVER:
            /* A loss ends the campaign.  A win advances: level 11 does
               not exist, so passing the last level is the campaign
               being complete rather than another map to load. */
            if (edge & ACT_GO) {
                if (!player_won) {
                    set_state(ST_TITLE);
                } else if (++level > LEVEL_COUNT) {
                    set_state(ST_WON);
                } else {
                    turn = 1;
                    busy_on("DEPLOYING...");
                    load_map();
                    set_state(is_128k ? ST_CUTSCENE : ST_PLAY);
                }
            }
            break;

        case ST_WON:
            /* ACTION, like every other screen.  This used to take ANY
               key, with a two-sample debounce to tell the press that won
               the campaign from a fresh one.  enter_state() flushes the
               keyboard, so the edge detector does that on its own -- and
               "press any key" was the one place the game asked for
               something it never asked for anywhere else. */
            if (edge & ACT_GO) set_state(ST_TITLE);
            break;

        case ST_CUTSCENE:
            /* Nothing to do.  The cutscene is dismissed by the tune in
               enter_state() returning, which happens on the press the
               player has already made; see the note there.  A second test
               here is what made it take two presses. */
            break;
    }

    /* M opens the overview.  It means nothing anywhere else — the tune
       plays itself when the title screen is entered. */
    if ((edge & ACT_M) && game_state == ST_PLAY) {
        cur_x = cursor_x;
        cur_y = cursor_y;
        set_state(ST_MAP);
    }
}

/* The boot logo, with the title march over it, until the player says go.
 *
 * Nothing here draws the logo.  It is the FIRST block on the tape, loaded
 * straight into the bottom third of the display file by the ROM, so it has
 * been on screen since a couple of seconds into loading -- and it is still
 * there now (tools/mklogo.py, tools/mktap.py --splash).
 *
 * The tune is also the wait.  Every Tritone tune blocks until a key or
 * fire and then returns, which is exactly what a splash screen wants, so
 * "wait for input" is not separate code -- it is the player call.
 *
 * THIS ONLY WORKS BECAUSE THE LOADER IS SILENT.  A first attempt found the
 * logo already gone by this point and the tune playing over a blank
 * screen.  The cause was not the bank staging, which never touches the
 * bottom third: it was the ROM announcing all fourteen blocks and
 * SCROLLING the picture off the top once the print position ran out of
 * screen.  POKE 23739,111 in the loader throws that output away, and the
 * logo now survives -- verified by reading rows 17-19 back as 0x07 after
 * the program has control.
 *
 * Runs after load_tiles() and load_map() so the logo covers the asset
 * decompression too, and before ST_TITLE so the first thing heard is the
 * march rather than silence. */
static void splash(void)
{
    grenadiers_play();
}

void game_run(void)
{
    uint8_t act;

    turn = 0;
    level = 1;
    load_tiles();
    load_map();

    splash();

    next_state = ST_TITLE;
    enter_state(ST_TITLE);

    for (;;) {
        /* ---- Sync to the beam --------------------------------------
           After this returns the beam is at attribute row 22, so the
           bottom border + vblank + top border (~28 000 T-states on a
           48K) are free for tear-free updates. */
        vsync_wait();

        update_state();

        poll_input();
#if DEBUG_STATE_WALK
        poll_debug();
#endif

        /* The border marks work the PLAYER asked for: selecting a unit,
           ordering a move, ending a turn, changing screen.  Gated on a
           non-direction edge, so cursor movement stays quiet — it is a
           multi-frame scroll and would strobe green on every step.

           Wrapping update_state() instead, as this used to, made the
           meter useless for the same reason.  Wrapping only
           enter_state() went too far the other way: SPACE that picks a
           unit up is not a state change, so the flash vanished from the
           one place it is most useful. */
        act = (uint8_t)(edge & ~ACT_DIRS);
        if (act) border(INK_PLAYER);    /* the player's own colour */

        handle_input();

        if (next_state != game_state) {
            /* Every state repaints its own screen on entry, so returning
               from a full-screen state needs no extra bookkeeping. */
            enter_state(next_state);
        }

        if (act) border(0);
    }
}
