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
#define ACT_SELECT  0x10
#define ACT_BACK    0x20
#define ACT_SPACE   0x40
#define ACT_M       0x80    /* the campaign overview, in ST_PLAY only */

#define ACT_DIRS    (ACT_UP | ACT_DOWN | ACT_LEFT | ACT_RIGHT)

/* Moving between screens is SPACE, and fire 1 arrives as ACT_SPACE too,
   so a joystick can start a game and take the level-end screen on.
   ENTER is accepted here as well — it is only on the board that the two
   part company, where SPACE gives orders and ENTER ends the turn. */
#define ACT_GO      (ACT_SPACE | ACT_SELECT)

/* Keyboard half-rows not covered by input.h. */
#define KEY_ENTER_ROW 0xBFFE    /* ENTER = bit 0            */
#define KEY_SPACE_ROW 0x7FFE    /* SPACE = bit 0, M = bit 2 */

/* Frames between cursor steps while a direction is held. */
#define NAV_DELAY   5

uint8_t game_state;

static uint8_t next_state;
static uint8_t acts, last_acts, prev_stable, edge;
static uint8_t nav_delay;
static uint8_t redraw_status;
static uint8_t key_idle, key_down;

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
    if (k & INPUT_FIRE1) a |= ACT_SPACE;    /* Z / Kempston fire 1 */
    if (k & INPUT_FIRE2) a |= ACT_BACK;     /* X / Kempston fire 2 */

    if (!(read_keys(KEY_ENTER_ROW) & 0x01)) a |= ACT_SELECT;
    if (!(read_keys(KEY_SPACE_ROW) & 0x01)) a |= ACT_SPACE;
    if (!(read_keys(KEY_SPACE_ROW) & 0x04)) a |= ACT_M;

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
   the floating bus, not keyboard state. */
static uint8_t any_key(void)
{
    uint8_t i;

    if (scan_input()) return 1;
    for (i = 0; i < 8; i++)
        if ((read_keys(key_rows[i]) & 0x1F) != 0x1F) return 1;
    return 0;
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
static void play_music(void)
{
    busy_on("PLAYING - PRESS A KEY          ");
    lowlands_play();
    busy_off(TITLE_HINT);
}

/* ------------------------------------------------------------- states */

/* Paint the state's screen, then do whatever entering it means beyond
   painting.  The split is deliberate: render.c knows how to draw each
   screen and nothing else, and this is where the consequences live. */
static void enter_state(uint8_t s)
{
    game_state = s;

    switch (s) {
        case ST_TITLE:
            render_title();
            play_music();       /* blocks; the screen is already up */
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
        case ST_WON:
            render_won();
            key_idle = 0;       /* wait for a release, then a press */
            key_down = 0;
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
static void move_play_cursor(void)
{
    uint8_t nx, ny;
    int8_t dx, dy;

    if (!nav_step(cursor_x, cursor_y, &nx, &ny)) return;

    dx = (int8_t)(nx - cursor_x);
    dy = (int8_t)(ny - cursor_y);
    cursor_x = nx;
    cursor_y = ny;
    redraw_status = 1;

    set_page();
    scroll_view(dx, dy);
}

/* Per-frame work for the active state, run inside the vblank window. */
/* Everything the active state owes the screen this frame, inside the
   vblank window.  The renderer decides how much of its debt fits;
   this only says when. */
static void update_state(void)
{
    switch (game_state) {
        case ST_PLAY:
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
                /* Decompressing a map and placing two armies runs long.
                   No banner is put back: enter_play() repaints the
                   screen and enter_state() flushes the keyboard. */
                busy_on("DEPLOYING...                   ");
                load_map();
                set_state(ST_PLAY);
            }
            break;

        case ST_PLAY:
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
            if (edge & ACT_SPACE) {
                uint8_t cell = cell_of(cursor_x, cursor_y);
                uint8_t u = occupancy[cell];

                if (selected == NO_UNIT) {
                    if (u != NO_UNIT &&
                        (u_flags[u] & (U_SIDE | U_ACTED)) == SIDE_PLAYER)
                        select_unit(u);
                } else if (u == NO_UNIT && cost[cell] != NO_COST) {
                    move_selected();
                } else {
                    /* The unit's own cell, or ground it cannot reach. */
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
            if ((edge & ACT_SELECT) && !(edge & ACT_SPACE)) {
                end_turn();
                redraw_status = 1;
            }
            /* X drops the held unit first and only quits on a second
               press, so the exit cannot be hit while giving an order. */
            if (edge & ACT_BACK) {
                if (selected != NO_UNIT) deselect();
                else                     set_state(ST_TITLE);
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
            if (edge & (ACT_GO | ACT_BACK))
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
                    busy_on("DEPLOYING...                   ");
                    load_map();
                    set_state(ST_PLAY);
                }
            }
            break;

        case ST_WON:
            /* Wait for the key that won the level to be released, then
               for a fresh press: two consecutive samples in each state
               debounce both.  Sampling once per frame rather than in a
               tight loop keeps ULA bus noise out of the reads. */
            if (key_idle < 2) {
                key_idle = any_key() ? 0 : (uint8_t)(key_idle + 1);
            } else {
                key_down = any_key() ? (uint8_t)(key_down + 1) : 0;
                if (key_down >= 2) set_state(ST_TITLE);
            }
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

void game_run(void)
{
    uint8_t act;

    turn = 0;
    level = 1;
    load_tiles();
    load_map();

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
        if (act) border(4);

        handle_input();

        if (next_state != game_state) {
            /* Every state repaints its own screen on entry, so returning
               from a full-screen state needs no extra bookkeeping. */
            enter_state(next_state);
        }

        if (act) border(0);
    }
}
