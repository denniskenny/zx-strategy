/*
 * render.c — everything that writes to the screen
 *
 * This is the half of the game with a deadline.  After vsync_wait()
 * returns the beam is off the display and roughly 256 bytes can be
 * written before it catches up; past that the write tears.  So every
 * routine here is sized against that budget, and anything bigger than
 * a couple of cells is paid off across frames by render_tick()
 * (docs/DESIGN.md § Logic and rendering).
 *
 * It reads the board through include/board.h and never changes it.
 * That is what makes it replaceable: these are the routines that will
 * become hand-written Z80, and a rewrite has to preserve only what
 * include/render.h declares.
 *
 * The one rule for anything added here: know what it costs in bytes
 * written, and if that is more than a page flip's PAGE_CELLS worth,
 * spread it.
 */

/* This file OWNS the tile sheets and level 1's terrain tables: the
   generated headers declare their data extern unless the including unit
   claims it, so exactly one copy reaches the binary.  These must come
   before ANY include — board.h reaches level_1.h, and a claim made
   after that arrives too late to have any effect. */
#define TILES_MAP_DEFINE_DATA
#define TILES_VIEW_DEFINE_DATA
#define UNITS_MAP_DEFINE_DATA
#define UNITS_VIEW_DEFINE_DATA
#define LEVEL_1_DEFINE_DATA

#include <string.h>

#include "../config/app_config.h"
#include "../config/game_config.h"
#include "../include/board.h"
#include "../include/dzx0.h"
#include "../include/gfx.h"
#include "../include/hw.h"
#include "../include/level_1.h"
#include "../include/memmap.h"
#include "../include/render.h"
#include "../include/tiles_map.h"
#include "../include/tiles_view.h"
#include "../include/units_map.h"
#include "../include/units_view.h"

/* Cell geometry, taken from the sheets themselves.  Here rather than in
   render.h so the sheets are included by ONE translation unit: their
   blobs are `static const` and were otherwise duplicated per includer. */
#define CELL_W      TILES_MAP_TILE_W
#define CELL_ROWS   TILES_MAP_TILE_ROWS
#define VIEW_CW     TILES_VIEW_TILE_W
#define VIEW_CH     TILES_VIEW_TILE_ROWS
#define VIEW_COL    ((32 - VIEW_COLS * VIEW_CW) / 2)

/* Both renderers must fit above the status panel. */
#if (MAP_COL + GRID_COLS * CELL_W > 32) \
 || (MAP_ROW + GRID_ROWS * CELL_ROWS > PANEL_TOP)
#error "level_1.tmx is too large for the campaign overview"
#endif
#if (VIEW_COL < 0) || (VIEW_ROW + VIEW_ROWS * VIEW_CH > PANEL_TOP)
#error "the play view does not fit on screen; shrink the page or the tiles"
#endif

#include "../include/vsync.h"

/* The sheets and the tileset have to agree on how many tiles there are
   and how big a cell is, or every blit after the first is offset. */
#if (TILES_MAP_TILES != TER_COUNT) || (TILES_VIEW_TILES != TER_COUNT)
#error "tile sheets and the .tmx tileset disagree on the terrain count"
#endif
#if (UNITS_MAP_TILES != UNIT_TYPES) || (UNITS_VIEW_TILES != UNIT_TYPES)
#error "unit sheets and game_config.h disagree on the unit type count"
#endif
#if (UNITS_MAP_TILE_W != TILES_MAP_TILE_W) \
 || (UNITS_MAP_TILE_H != TILES_MAP_TILE_H)
#error "units_map.zxp tiles are not the size of a campaign-map cell"
#endif
#if (UNITS_VIEW_TILE_W != TILES_VIEW_TILE_W) \
 || (UNITS_VIEW_TILE_H != TILES_VIEW_TILE_H)
#error "units_view.zxp tiles are not the size of a play-view cell"
#endif

/* ------------------------------------------------- 48K / 128K paths --
   The only place the two machines are told apart.  `hw_detect()` sets
   is_128k; everything else in this file is written once and works on
   both (docs/DESIGN.md § Two machines, two render paths).

   A 128K has a SECOND display file — RAM page 7 — and bit 3 of port
   0x7FFD chooses which of the two the ULA shows.  That buys a whole
   screen composed where nobody can see it, then shown in the time it
   takes to write one byte to a port.  A 48K has no such thing and
   never will: there, composing off-display would mean copying 6 912
   bytes back over the display afterwards, which is the very tearing it
   was meant to avoid.

   So the paths are:

     48K    draw straight to the displayed screen.  A full repaint is
            visible as it happens, and is spread over frames to keep
            each frame inside the vblank budget.
     128K   compose the full repaint into whichever screen is NOT being
            shown, then flip.  No budget applies — nobody is looking at
            it — and the change appears complete, between frames.

   Page 7 is banked in at 0xC000 once at startup and left there.  That
   is only safe because nothing of ours lives above 0xC000: the link map
   tops out well below it and the stack sits around 0x7FA0.  If the
   binary ever grows past 0xC000 this breaks, loudly and strangely, so
   the build checks it — see tools/checkmem.py. */

#define SCREEN_0    ((uint8_t *)0x4000)     /* page 5, always mapped   */
#define SCREEN_1    ((uint8_t *)0xC000)     /* page 7, banked in below */

#define PAGE_7FFD   0x17    /* page 7 at 0xC000, 48K ROM, screen 0     */
#define PAGE_SCREEN 0x08    /* bit 3: show the shadow screen           */
#define MARKER_ROW  22      /* the floating bus sync marker's row      */

static void hint_row(const char *s, uint8_t attr);

static uint8_t page_reg;    /* 0x7FFD is write-only; remember it       */
static uint8_t back;        /* 1 = the back buffer is SCREEN_1         */
/* HAZARD, currently benign, do not make it worse.

   This is written only from the inline asm in screens_init(), which SDCC
   cannot see, so it reasons from the `shadow_ok = 0` there and folds
   `if (!shadow_ok)` to always-true: that is SDCC's "warning 126:
   unreachable code", and the line it discards is the one arming the
   vsync marker.  It is harmless ONLY because render_show() sets
   vsync_marker_addr again before anything waits on it, which the 128K
   render_paths run confirms.  Add a second thing that depends on
   shadow_ok inside screens_init() and it will silently not happen.

   `volatile` is the correct fix and costs 18 bytes; the tap has 5.  The
   free fix is to move the sentinel test into its own __naked helper that
   RETURNS the result, so `shadow_ok = paging_works();` is an assignment
   SDCC can see.  Do that when there is room, or as part of the banked
   data work.  is_128k needs neither: it is read from other translation
   units, so SDCC cannot fold it. */
static uint8_t shadow_ok;   /* 0 = the 128K path is not usable yet     */

/* --- Why the 128K path is currently disarmed --------------------------
   It works, and it is verified: page 7 banks in, a whole screen composes
   off-display and the flip shows it.  It cannot be switched on yet
   because it collides with how this program finds the vertical blank.

   vsync_wait() writes the floating bus sync marker to VSYNC_MARKER_ADDR
   — 0x5AC0, attribute row 22 of the screen in page 5 — and then spins
   until it sees that byte come back off the bus.  The bus carries what
   the ULA is FETCHING, so the moment the shadow screen is the one being
   displayed, the ULA is reading page 7 and the marker in page 5 is
   never fetched.  vsync_wait() waits for ever: the game hangs on the
   title with the input dead, which is exactly what it looked like.

   Fixing it means teaching vsync.c which screen is live and writing the
   marker to 0xDAC0 instead of 0x5AC0 when it is page 7 — a change to
   the assembly in src/vsync.c and to the sync it has been tuned
   against.  That belongs with P7, where the shadow screen finally earns
   its keep (docs/PLAN.md).  Until then both machines take the 48K path
   and this costs nothing but the branch. */

/* Bank page 7 in for good, and prove it worked before relying on it.

   is_128k is not enough on its own: main() may have locked paging (bit
   5 of 0x7FFD), after which every write to that port is ignored in
   SILENCE — the page-in does nothing, the flip does nothing, and the
   flag would still be set.  The result is a whole screen composed into
   a bank the ULA never displays: a title screen that simply does not
   appear, with no other symptom.  That happened twice before this
   check existed.

   So prove it.  Sentinel into page 7, page something else in, write a
   different value, page 7 back, see which survived — the same trick
   hw_detect() uses on the machine itself, because a write-only port
   lies by omission.

   Bit 4 is set in every value written here.  It is the ROM select, and
   clearing it on a +2A/+3 pages in +3DOS underneath the running
   program; see .claude/skills/zx-memory. */
static void screens_init(void)
{
    back = 0;
    shadow_ok = 0;

    /* A 48K falls through here with shadow_ok 0 and draws in place —
       the same path a 128K with locked paging takes.  There is no
       separate 48k build any more; see the Makefile. */
    if (!is_128k) return;

    /* A +2A/+3 gets this too.  It did not, briefly, because the
       buffers were living inside page 7 and sharing it with whatever
       the +3's ROM keeps there; they are at 0x6000 now and page 7 is
       ours alone. */
    page_reg = PAGE_7FFD;
    __asm
        ld  bc, #0x7FFD
        ld  a, #0x17            ; page 7 at 0xC000, ROM bit kept
        out (c), a
        ld  a, #0xA5
        ld  (0xC000), a         ; sentinel into page 7

        ld  a, #0x10            ; page 0, same ROM
        out (c), a
        ld  a, #0x5A
        ld  (0xC000), a

        ld  a, #0x17            ; page 7 back
        out (c), a
        ld  a, (0xC000)
        cp  #0xA5               ; did the sentinel survive?
        ld  a, #0x00
        jr  nz, _si_done        ; no: paging is locked, stay on 48K path
        inc a
    _si_done:
        ld  (_shadow_ok), a
        ld  a, #0x17            ; leave BANKM agreeing with the port
        ld  (0x5B5C), a
    __endasm;

    if (!shadow_ok) {
        /* Locked. Whatever bank is at 0xC000 is the one we get, and the
           buffers at 0xDB00 live in it.  Draw where a 48K draws. */
        gfx_target(SCREEN_0);
        return;
    }
    vsync_marker_addr = gfx_attr + MARKER_ROW * 32;
}

/* Start a full-screen repaint.  On a 128K that means aiming at the
   screen the ULA is not showing; on a 48K it means what it always
   meant, and render_show() has nothing to do afterwards. */
void render_compose(void)
{
    if (!shadow_ok) return;
    gfx_target(back ? SCREEN_0 : SCREEN_1);
}

/* Show what render_compose() built.  One port write on a 128K — plus a
   copy of it into BANKM, which is not optional.

   0x7FFD is write-only, so the ROM keeps its own record of the last
   value at the system variable BANKM (0x5B5C) and writes that copy back
   whenever it touches paging — the interrupt handler included.  Set bit
   3 without updating BANKM and the ROM restores its own value within a
   frame: the flip is undone before the ULA ever shows the new screen.

   On a +2A/+3 that is exactly what happened.  Page 7 appeared never to
   display at all, so every state composed into the shadow screen came
   up blank or stale while the ones composed into page 5 looked fine.
   A 128K tolerated it, which is why it took a +3 to find.

   State changes flip, and so does every sub-step of a scroll — see
   copy_chrome() for what makes the second one safe. */
void render_show(void)
{
    if (!shadow_ok) return;

    back = (uint8_t)!back;
    page_reg = (uint8_t)(back ? (PAGE_7FFD | PAGE_SCREEN) : PAGE_7FFD);
    __asm
        ld  bc, #0x7FFD
        ld  a, (_page_reg)
        out (c), a
        ld  (0x5B5C), a         ; BANKM — see below
    __endasm;

    /* Incremental drawing after this goes to whatever is now on show —
       a cursor step or a dirty cell is two cells, not a screen, and
       tears no worse than it does on a 48K. */
    gfx_target(back ? SCREEN_1 : SCREEN_0);

    /* And the floating bus sync has to follow the display, or it waits
       for a marker the ULA is no longer fetching and never returns. */
    vsync_marker_addr = gfx_attr + MARKER_ROW * 32;
}


/* ------------------------------------------------------------- state */

int8_t page_x, page_y;
uint8_t attrs_left;
uint8_t dirty_n;

/* World cells whose PICTURE is stale, redrawn on the next frame: a unit
   sprite that left one cell and arrived in another.  Orders are given
   outside the vblank window, so nothing there may draw its own result.
   Two is all a move needs, and two draw_view_cell() calls is exactly
   what a page flip already spends per frame. */
#define DIRTY_MAX   2
static uint8_t dirty_x[DIRTY_MAX], dirty_y[DIRTY_MAX];

/* Tile pixels AND their attribute blocks, decompressed once from the
   ZX0 blobs in the headers.  Each blob is pixels for every tile
   followed by one attribute block per tile, so a sheet costs a single
   decompression and tile t's colours live at a fixed offset. */
/* Laid out by hand in the 0x6000-0x7FFF region rather than left to the
   linker, for the same reason as the view buffer: everything the linker
   places sits above 0x8000, and the 128K path needs the program to stay
   under 0xC000 (tools/checkmem.py enforces it).  1 620 bytes of unpacked
   sheets is the difference between fitting and not.

   Contended memory, which costs nothing here — the sheets are read
   while composing, and composing happens in the vblank window. */
#define MEM_MAP_TILES   (MEM_TILES)
#define MEM_VIEW_TILES  (MEM_MAP_TILES  + TILES_MAP_RAW_SIZE)
#define MEM_UMAP_TILES  (MEM_VIEW_TILES + TILES_VIEW_RAW_SIZE)
#define MEM_UVIEW_TILES (MEM_UMAP_TILES + UNITS_MAP_RAW_SIZE)

#define map_tiles       ((uint8_t *)MEM_MAP_TILES)
#define view_tiles      ((uint8_t *)MEM_VIEW_TILES)
#define unit_map_tiles  ((uint8_t *)MEM_UMAP_TILES)
#define unit_view_tiles ((uint8_t *)MEM_UVIEW_TILES)

#if (TILES_MAP_RAW_SIZE + TILES_VIEW_RAW_SIZE + UNITS_MAP_RAW_SIZE \
     + UNITS_VIEW_RAW_SIZE) > MEM_TILES_SIZE
#error "the unpacked sheets have outgrown MEM_TILES_SIZE in memmap.h"
#endif

/* Tile t's attribute block within each sheet. */
#define map_attr_of(t) \
    (map_tiles + TILES_MAP_ATTR_OFF + (uint16_t)(t) * TILES_MAP_ATTR_SIZE)
#define view_attr_of(t) \
    (view_tiles + TILES_VIEW_ATTR_OFF + (uint16_t)(t) * TILES_VIEW_ATTR_SIZE)
#define unit_map_attr_of(t) \
    (unit_map_tiles + UNITS_MAP_ATTR_OFF + (uint16_t)(t) * UNITS_MAP_ATTR_SIZE)
#define unit_view_attr_of(t) \
    (unit_view_tiles + UNITS_VIEW_ATTR_OFF + (uint16_t)(t) * UNITS_VIEW_ATTR_SIZE)

/* --- The view buffer -------------------------------------------------
   The play view is composed here and then presented in one pass, which
   is both what makes a scroll possible and, on its own, most of the
   speed (docs/PLAN.md § P7).

   Composing straight to the screen cost 1 024 scr_off() calls for a full
   repaint — one per pixel row per cell — because the ZX display is
   interleaved and every row needs its address worked out.  The buffer is
   LINEAR: row r starts at r * 32, so composing needs no address
   arithmetic whatsoever, and presenting needs 128 table lookups instead
   of 1 024 computations.

   VIEW_COL is 0 and a tile is a whole number of characters wide, so
   every copy is byte-aligned and nothing is ever shifted.  The #error
   below is what keeps that true.

   These live at 0x6000, in the free RAM below the program: 4 608 bytes
   would not fit under the 0xC000 ceiling the 128K path needs (see
   tools/checkmem.py).  It is contended memory, but contention only bites
   while the ULA is drawing and this all happens in the vblank window. */
#define VIEW_PX_ROWS  (VIEW_ROWS * VIEW_CH * 8)     /* 128 */
#define VIEW_AT_ROWS  (VIEW_ROWS * VIEW_CH)         /*  16 */

#define VBUF     ((uint8_t *)MEM_VBUF)
#define VATTR    ((uint8_t *)MEM_VATTR)
#define VIEW_OFF ((uint16_t *)MEM_VIEW_OFF)

#if VIEW_COL != 0 || (VIEW_COLS * VIEW_CW) != 32
#error "the view buffer assumes a full-width, byte-aligned play view"
#endif

/* Status-panel labels, padded to 8 characters like the terrain names
   the map converter generates. */
static const char *const unit_names[UNIT_TYPES] = {
    "INFANTRY",
    "TANK    ",
    "CANNON  ",
    "BASE    "
};

/* ------------------------------------------------------------ drawing */

void print_num(uint8_t col, uint8_t row, uint16_t v, uint8_t digits)
{
    char buf[6];
    uint8_t i;

    buf[digits] = 0;
    for (i = digits; i > 0; i--) {
        buf[i - 1] = (char)('0' + (v % 10));
        v /= 10;
    }
    print_at(col, row, buf);
}

void print_char(uint8_t col, uint8_t row, char c)
{
    char buf[2];

    buf[0] = c;
    buf[1] = 0;
    print_at(col, row, buf);
}

void draw_header(const char *title)
{
    screen_clear(ATTR_BG);
    print_at(1, 0, title);
    set_attr_rect(0, 0, 32, 1, ATTR_TITLE);
}

/* Unpack the tile and unit strips into RAM.  ~1.1 KB of pixels cost
   ~500 bytes of ZX0 in the binary and one decompression at startup. */
void load_tiles(void)
{
    uint8_t r;

    screens_init();

    /* Screen offset of each buffer row, worked out once.  This is the
       table that replaces 1 024 scr_off() calls per repaint with 128
       array reads. */
    for (r = 0; r < VIEW_PX_ROWS; r++)
        VIEW_OFF[r] = scr_off(0, (uint8_t)(VIEW_ROW * 8 + r));

    dzx0_decompress(tiles_map_zx0, map_tiles);
    dzx0_decompress(tiles_view_zx0, view_tiles);
    dzx0_decompress(units_map_zx0, unit_map_tiles);
    dzx0_decompress(units_view_zx0, unit_view_tiles);
}

/* The status panel's colour for a unit: side, dimmed once spent.  One
   flat byte, because that line is text — the sheet's per-cell shading
   belongs to the sprite, not to a row of characters. */
static uint8_t unit_line_attr(uint8_t u)
{
    if (u_flags[u] & U_SIDE)   return ATTR_UNIT_E;
    return (u_flags[u] & U_ACTED) ? ATTR_UNIT_P_DONE
                                  : (uint8_t)(ATTR_UNIT_P | ATTR_BRIGHT);
}

/* Colour a unit's cell in its side's ink, keeping the shading the
   artist put in the sheet: `bright` is that tile's block of BRIGHT
   flags, ORed over the side colour.
   
   Two cases come out flat rather than shaded, both for reasons that
   are not about taste.  An ENEMY unit's ink already carries BRIGHT
   because non-bright red on black is 0x02, which the floating bus sync
   marker reserves — ORing the sheet's flag over it cannot dim
   anything.  A SPENT player unit is dimmed deliberately: dropping the
   shading is what makes "this one has moved" readable next to a
   sprite that is otherwise lit. */
static void attr_unit_cell(uint8_t col, uint8_t row, uint8_t w, uint8_t h,
                           uint8_t u, const uint8_t *bright)
{
    if (u_flags[u] & U_SIDE)
        set_attr_rect(col, row, w, h, ATTR_UNIT_E);
    else if (u_flags[u] & U_ACTED)
        set_attr_rect(col, row, w, h, ATTR_UNIT_P_DONE);
    else
        blit_attr_rect(col, row, w, h, bright, ATTR_UNIT_P);
}

/* Paint one campaign-overview cell's own colours: the selected unit's
   yellow, an occupant's side colour, or the terrain's authored
   attribute block straight from the sheet.  Attributes only — the
   pixels beneath are unchanged by any of it. */
void attr_map_cell(uint8_t cx, uint8_t cy)
{
    uint8_t col = (uint8_t)(MAP_COL + cx * CELL_W);
    uint8_t row = (uint8_t)(MAP_ROW + cy * CELL_ROWS);
    uint8_t cell = cell_of(cx, cy);
    uint8_t u = occupancy[cell];

    if (u == NO_UNIT)
        blit_attr_rect(col, row, CELL_W, CELL_ROWS,
                       map_attr_of(terrain[cell]), 0);
    else if (u == selected)
        set_attr_rect(col, row, CELL_W, CELL_ROWS, ATTR_HINT);
    else
        attr_unit_cell(col, row, CELL_W, CELL_ROWS, u,
                       unit_map_attr_of(u_type[u]));
}

/* Flood one overview cell with a single colour — the cursor, or the
   marker showing where the play cursor is.  Pixels untouched. */
void solid_map_cell(uint8_t cx, uint8_t cy, uint8_t attr)
{
    set_attr_rect((uint8_t)(MAP_COL + cx * CELL_W),
                  (uint8_t)(MAP_ROW + cy * CELL_ROWS),
                  CELL_W, CELL_ROWS, attr);
}

/* Terrain, then the unit standing on it, then the cell's colours.  The
   sprites are opaque, so a unit hides its tile rather than sitting over
   it — masked sprites are a later pass (docs/PLAN.md, Risks). */
void draw_cell(uint8_t cx, uint8_t cy)
{
    uint8_t col = (uint8_t)(MAP_COL + cx * CELL_W);
    uint8_t row = (uint8_t)(MAP_ROW + cy * CELL_ROWS);
    uint8_t cell = cell_of(cx, cy);
    uint8_t u = occupancy[cell];

    write_blit((int8_t)col, (uint8_t)(row << 3),
               map_tiles + (uint16_t)terrain[cell] * TILES_MAP_TILE_SIZE,
               TILES_MAP_TILE_W, TILES_MAP_TILE_H);
    if (u != NO_UNIT)
        write_blit((int8_t)col, (uint8_t)(row << 3),
                   unit_map_tiles + (uint16_t)u_type[u] * UNITS_MAP_TILE_SIZE,
                   UNITS_MAP_TILE_W, UNITS_MAP_TILE_H);
    attr_map_cell(cx, cy);
}

void draw_map(void)
{
    uint8_t x, y;

    for (y = 0; y < GRID_ROWS; y++)
        for (x = 0; x < GRID_COLS; x++)
            draw_cell(x, y);
}

/* Row 17: the unit under the cursor — type, HP against its type's
   maximum, attack range and movement.  Enemy units report the same
   numbers as friendly ones; knowing what is about to shoot you is not
   a privilege, and the line's colour says whose it is.  An empty cell
   blanks the whole field, so nothing is left over from the last unit
   the cursor crossed. */
static void draw_unit_line(uint8_t cell)
{
    uint8_t u = occupancy[cell];
    uint8_t t;

    print_at(1, ROW_UNIT, "UNIT   :");

    if (u == NO_UNIT) {
        print_at(10, ROW_UNIT, "-");
        set_attr_rect(0, ROW_UNIT, 32, 1, ATTR_TEXT);
        return;
    }

    t = u_type[u];
    print_at(10, ROW_UNIT, unit_names[t]);
    print_num(19, ROW_UNIT, u_hp[u], 3);
    print_char(22, ROW_UNIT, '/');
    print_num(23, ROW_UNIT, unit_health[t], 3);
    print_char(27, ROW_UNIT, 'R');
    print_num(28, ROW_UNIT, unit_range[t], 1);
    print_char(30, ROW_UNIT, 'M');
    print_num(31, ROW_UNIT, unit_movement[t], 1);

    /* Same side/spent rule as the sprite, but one flat colour: this is
       a row of text, and the sheet's per-cell shading means nothing
       here. */
    set_attr_rect(0, ROW_UNIT, 32, 1, unit_line_attr(u));
}

/* Unit / turn / coordinate / terrain panel, shared by the play and map
   states.  Both report whatever their own cursor is over. */
void draw_status(const char *label, uint8_t x, uint8_t y)
{
    uint8_t cell = cell_of(x, y);
    uint8_t t = terrain[cell];

    draw_unit_line(cell);

    print_at(1, ROW_TURN, "TURN   :");
    print_num(10, ROW_TURN, turn, 3);

    print_at(1, ROW_COORD, label);
    print_num(10, ROW_COORD, x, 2);
    print_char(12, ROW_COORD, ',');
    print_num(13, ROW_COORD, y, 2);

    print_at(1, ROW_TERRAIN, "TERRAIN:");
    print_at(10, ROW_TERRAIN, level_1_terrain_names[t]);
    print_at(19, ROW_TERRAIN, "COVER");
    print_num(25, ROW_TERRAIN, terrain_cover[t], 2);
    print_char(27, ROW_TERRAIN, '%');

    set_attr_rect(0, ROW_TURN, 32, PANEL_ROWS - 1, ATTR_TEXT);
}

/* Page the view onto whichever VIEW_COLS x VIEW_ROWS block of the world
   holds the cursor. */
/* Put the window where the pinned cursor lands on CURSOR_VX/VY.  No
   clamping to the board: that is the whole point of the sea, and without
   it the cursor could never reach a corner — which is where both bases
   are. */
void set_page(void)
{
    page_x = (int8_t)(cursor_x - CURSOR_VX);
    page_y = (int8_t)(cursor_y - CURSOR_VY);
}

/* --- Composing into the buffer ---------------------------------------
   Everything below writes to VBUF/VATTR, never to the screen.  Nothing
   here computes a screen address: a tile row is four bytes at a fixed
   offset, which is the whole reason this is faster than drawing
   directly. */

/* One 4x4-character tile's pixels.  Unrolled to four byte moves per
   row: the width is a compile-time 4, so a loop would spend more on the
   counter than on the copy. */
static void compose_tile(uint8_t vx, uint8_t vy, const uint8_t *src)
{
    uint8_t *d = VBUF + (uint16_t)vy * (VIEW_CH * 8) * 32 + vx * VIEW_CW;
    uint8_t r = VIEW_CH * 8;

    while (r--) {
        d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
        src += VIEW_CW;
        d += 32;
    }
}

#if VIEW_CW != 4
#error "compose_tile() is unrolled for 4-character-wide tiles"
#endif

/* One cell's attributes, either flooded with a single colour or copied
   from a sheet block with the side's ink ORed over it. */
static void compose_attr(uint8_t vx, uint8_t vy, uint8_t flat,
                         const uint8_t *src, uint8_t or_mask)
{
    uint8_t *d = VATTR + (uint16_t)vy * VIEW_CH * 32 + vx * VIEW_CW;
    uint8_t r = VIEW_CH;

    while (r--) {
        if (src) {
            d[0] = (uint8_t)(src[0] | or_mask);
            d[1] = (uint8_t)(src[1] | or_mask);
            d[2] = (uint8_t)(src[2] | or_mask);
            d[3] = (uint8_t)(src[3] | or_mask);
            src += VIEW_CW;
        } else {
            d[0] = d[1] = d[2] = d[3] = flat;
        }
        d += 32;
    }
}

/* --- Presenting ------------------------------------------------------
   The only place the play view touches screen memory.  One table read
   per row, then a straight 32-byte run, because VIEW_COL is 0 and the
   view spans the full width. */

/* The cursor, straight onto the screen, over whatever the buffer says.

   It belongs here rather than in the buffer because it is anchored to
   the SCREEN, not to the world: it sits on view cell CURSOR_VX/VY and
   stays there while the board slides underneath.  Composing it into the
   buffer meant every push dragged it along with the art, so it could
   only be put right after the last sub-step of a scroll — it smeared
   for four frames and then snapped back.  Sixteen bytes at present time
   instead, and it holds still for every character of the scroll. */
static void stamp_cursor(void)
{
    uint8_t *d = gfx_attr + (uint16_t)(VIEW_ROW + CURSOR_VY * VIEW_CH) * 32
                 + CURSOR_VX * VIEW_CW;
    uint8_t r = VIEW_CH;

    while (r--) {
        d[0] = d[1] = d[2] = d[3] = ATTR_CURSOR;
        d += 32;
    }
}

/* The pixel half of a present, in Z80.

   4 096 bytes, four times a cursor step, so it is the hottest thing the
   program does.  Two costs come out against the C version: a memcpy
   call per row, and LDIR's 21 T-states a byte.  Unrolled LDI is 16,
   which is 5 x 4 096 = ~20 000 T a present — about a third of a frame.

   BC cannot hold the row counter: LDI decrements it.  Hence the counter
   in memory, ~40 T a row against the 160 the unrolling saves.

   Addresses are baked in because inline assembly cannot see C
   expressions.  The #error fails the build if memmap.h moves them,
   rather than letting this write somewhere else in silence. */
#if MEM_VBUF != 0x6000 || MEM_VIEW_OFF != 0x7200
#error "present_pixels() has MEM_VBUF/MEM_VIEW_OFF baked into its assembly"
#endif

static uint8_t ppx_rows;
static const uint8_t *ppx_src;

static void present_pixels(void) __naked
{
    __asm
        ld  a, #128
        ld  (_ppx_rows), a
        ld  hl, #0x6000
        ld  de, #0x7200
    ppx_row:
        push de
        ex  de, hl
        ld  e, (hl)
        inc hl
        ld  d, (hl)
        ex  de, hl
        ld  de, (_gfx_pix)
        add hl, de
        ex  de, hl
        pop hl
        inc hl
        inc hl
        push hl
        ld  hl, (_ppx_src)
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ldi
        ld  (_ppx_src), hl
        pop de
        ld  a, (_ppx_rows)
        dec a
        ld  (_ppx_rows), a
        jr  nz, ppx_row
        ret
    __endasm;
}

static void present_all(void)
{
    ppx_src = VBUF;
    present_pixels();

    /* Attributes are NOT interleaved — they are 768 flat bytes — and
       VATTR's rows are contiguous too.  So the whole block is one copy,
       not sixteen.  This was sixteen memcpy calls for no reason but
       symmetry with the loop above. */
    memcpy(gfx_attr + VIEW_ROW * 32, VATTR, VIEW_AT_ROWS * 32);

    stamp_cursor();
}

/* Just one cell, for the two ends of a move.  Four bytes a row rather
   than thirty-two: a dirty cell must not cost a whole view. */
static void present_cell(uint8_t vx, uint8_t vy)
{
    uint8_t col = (uint8_t)(vx * VIEW_CW);
    uint16_t r = (uint16_t)vy * (VIEW_CH * 8);
    uint8_t n = VIEW_CH * 8;
    uint8_t ar;

    while (n--) {
        uint8_t *d = gfx_pix + VIEW_OFF[r] + col;
        const uint8_t *sp = VBUF + r * 32 + col;

        d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = sp[3];
        r++;
    }
    for (ar = 0; ar < VIEW_CH; ar++) {
        uint8_t *d = gfx_attr + (uint16_t)(VIEW_ROW + vy * VIEW_CH + ar) * 32
                     + col;
        const uint8_t *sa = VATTR + (uint16_t)(vy * VIEW_CH + ar) * 32 + col;

        d[0] = sa[0]; d[1] = sa[1]; d[2] = sa[2]; d[3] = sa[3];
    }
    if (vx == CURSOR_VX && vy == CURSOR_VY) stamp_cursor();
}

/* --- What a cell looks like ------------------------------------------ */

/* What colour a view cell is.  Returns the sheet's 4x4 attribute block,
   or NULL when the cell is a flat wash — *flat then holds the colour.
   *mask is ORed over a block, which is how a unit's side ink combines
   with the shading the sheet authored.

   The cursor is NOT here — it is stamped at present time, see
   stamp_cursor().  A unit outranks the range highlight, but
   only nominally: the flood fill refuses occupied cells, so no cell is
   ever both.  Bare ground gets the terrain sheet's own block, which is
   the only path that paints authored per-cell colour rather than a flat
   wash.  Off the board is sea.

   Split out from the compose below so the scroll can ask the same
   question about a single character column without duplicating the
   rules. */
static const uint8_t *cell_attr(uint8_t vx, uint8_t vy,
                                uint8_t *flat, uint8_t *mask)
{
    int8_t wx = (int8_t)(page_x + vx);
    int8_t wy = (int8_t)(page_y + vy);
    uint8_t cell, u;

    *mask = 0;
    if (wx < 0 || wx >= GRID_COLS || wy < 0 || wy >= GRID_ROWS)
        return view_attr_of(TER_SEA);

    cell = cell_of((uint8_t)wx, (uint8_t)wy);
    u = occupancy[cell];

    if (u != NO_UNIT) {
        if (u == selected)            *flat = ATTR_HINT;
        else if (is_target(cell))     *flat = ATTR_TARGET;
        else if (u_flags[u] & U_SIDE) *flat = ATTR_UNIT_E;
        else if (u_flags[u] & U_ACTED) *flat = ATTR_UNIT_P_DONE;
        else {
            *mask = ATTR_UNIT_P;
            return unit_view_attr_of(u_type[u]);
        }
        return 0;
    }
    if (selected != NO_UNIT && cost[cell] != NO_COST) {
        *flat = ATTR_RANGE;
        return 0;
    }
    return view_attr_of(terrain[cell]);
}

/* Colour, into the buffer. */
static void compose_view_attr(uint8_t vx, uint8_t vy)
{
    uint8_t flat = 0, mask = 0;
    const uint8_t *blk = cell_attr(vx, vy, &flat, &mask);

    compose_attr(vx, vy, flat, blk, mask);
}

/* The art a view cell shows: sea off the board, the unit if one stands
   here, the terrain otherwise.

   Returning the unit sprite INSTEAD of the terrain, not on top of it.
   Sprites are opaque and cover the whole cell, so the terrain underneath
   was being composed and then completely overwritten — half the work for
   every occupied cell, and there are 14 of them on a starting board. */
static const uint8_t *cell_art(uint8_t vx, uint8_t vy)
{
    int8_t wx = (int8_t)(page_x + vx);
    int8_t wy = (int8_t)(page_y + vy);
    uint8_t cell, u;

    if (wx < 0 || wx >= GRID_COLS || wy < 0 || wy >= GRID_ROWS)
        return view_tiles + (uint16_t)TER_SEA * TILES_VIEW_TILE_SIZE;

    cell = cell_of((uint8_t)wx, (uint8_t)wy);
    u = occupancy[cell];
    if (u != NO_UNIT)
        return unit_view_tiles + (uint16_t)u_type[u] * UNITS_VIEW_TILE_SIZE;
    return view_tiles + (uint16_t)terrain[cell] * TILES_VIEW_TILE_SIZE;
}

/* Art, into the buffer. */
static void compose_view_cell(uint8_t vx, uint8_t vy)
{
    compose_tile(vx, vy, cell_art(vx, vy));
    compose_view_attr(vx, vy);
}

/* --- The public draw calls ------------------------------------------- */

void attr_view_cell(uint8_t vx, uint8_t vy)
{
    compose_view_attr(vx, vy);
    present_cell(vx, vy);
}

void draw_view_cell(uint8_t vx, uint8_t vy)
{
    compose_view_cell(vx, vy);
    present_cell(vx, vy);
}

void draw_view(void)
{
    uint8_t i;

    for (i = 0; i < VIEW_CELLS; i++)
        compose_view_cell((uint8_t)(i % VIEW_COLS), (uint8_t)(i / VIEW_COLS));
    present_all();
}

/* --- The push scroll -------------------------------------------------
   A cursor step moves the world one whole tile, but the buffer is pushed
   one CHARACTER at a time and presented between each — four sub-steps
   per step.  A character at a time because attributes have character
   granularity: shifting by pixels would slide the ink out from under its
   colour.

   Shifting a linear buffer is a memmove per row and nothing else, which
   is the second reason the buffer earns its keep.  A vertical push is
   better still: rows are contiguous, so the whole thing is one move. */

/* Push the buffer one character sideways.  d > 0 moves the world LEFT,
   which is what happens when the cursor goes right. */
static void push_h(int8_t d)
{
    uint8_t r;

    for (r = 0; r < VIEW_PX_ROWS; r++) {
        uint8_t *p = VBUF + (uint16_t)r * 32;

        if (d > 0) memmove(p, p + 1, 31);
        else       memmove(p + 1, p, 31);
    }
    for (r = 0; r < VIEW_AT_ROWS; r++) {
        uint8_t *p = VATTR + (uint16_t)r * 32;

        if (d > 0) memmove(p, p + 1, 31);
        else       memmove(p + 1, p, 31);
    }
}

/* Push the buffer one character row.  d > 0 moves the world UP. */
static void push_v(int8_t d)
{
    if (d > 0) {
        memmove(VBUF, VBUF + 8 * 32, (VIEW_PX_ROWS - 8) * 32);
        memmove(VATTR, VATTR + 32, (VIEW_AT_ROWS - 1) * 32);
    } else {
        memmove(VBUF + 8 * 32, VBUF, (VIEW_PX_ROWS - 8) * 32);
        memmove(VATTR + 32, VATTR, (VIEW_AT_ROWS - 1) * 32);
    }
}

/* The character column arriving at the edge: sub-column `sub` of the
   cells in view column `vx`, written to buffer column `dcol`.  A strided
   read — every fourth byte of the tile — which is why the incoming edge
   needs its own path and cannot reuse compose_tile(). */
static void slice_col(uint8_t vx, uint8_t sub, uint8_t dcol)
{
    uint8_t vy;

    for (vy = 0; vy < VIEW_ROWS; vy++) {
        const uint8_t *sp = cell_art(vx, vy) + sub;
        uint8_t *d = VBUF + (uint16_t)vy * (VIEW_CH * 8) * 32 + dcol;
        uint8_t r = VIEW_CH * 8;

        while (r--) {
            *d = *sp;
            sp += VIEW_CW;
            d += 32;
        }
    }
}

/* The character row arriving at the edge: sub-row `sub` of the cells in
   view row `vy`.  Contiguous in the tile, unlike a column. */
static void slice_row(uint8_t vy, uint8_t sub, uint8_t drow)
{
    uint8_t vx;

    for (vx = 0; vx < VIEW_COLS; vx++) {
        const uint8_t *sp = cell_art(vx, vy) + (uint16_t)sub * 8 * VIEW_CW;
        uint8_t *d = VBUF + (uint16_t)drow * 32 + vx * VIEW_CW;
        uint8_t r = 8;

        while (r--) {
            d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = sp[3];
            sp += VIEW_CW;
            d += 32;
        }
    }
}

/* The attribute column arriving with slice_col()'s pixels: sub-column
   `sub` of the cells in view column `vx`.  Sixteen bytes.

   Without this the incoming column kept whatever push_h() smeared into
   it and only came right when the whole tile had arrived, so colour
   trailed the art by up to four characters and then snapped.  Doing it
   per character is not merely better looking, it is CHEAPER: it costs
   16 bytes a sub-step against recomposing all 32 cells at the end. */
static void slice_attr_col(uint8_t vx, uint8_t sub, uint8_t dcol)
{
    uint8_t vy;

    for (vy = 0; vy < VIEW_ROWS; vy++) {
        uint8_t flat = 0, mask = 0;
        const uint8_t *blk = cell_attr(vx, vy, &flat, &mask);
        uint8_t *d = VATTR + (uint16_t)vy * VIEW_CH * 32 + dcol;
        uint8_t r;

        for (r = 0; r < VIEW_CH; r++) {
            *d = blk ? (uint8_t)(blk[r * VIEW_CW + sub] | mask) : flat;
            d += 32;
        }
    }
}

/* The same for a row arriving top or bottom: sub-row `sub` of the cells
   in view row `vy`.  Thirty-two bytes, contiguous. */
static void slice_attr_row(uint8_t vy, uint8_t sub, uint8_t drow)
{
    uint8_t vx;

    for (vx = 0; vx < VIEW_COLS; vx++) {
        uint8_t flat = 0, mask = 0;
        const uint8_t *blk = cell_attr(vx, vy, &flat, &mask);
        uint8_t *d = VATTR + (uint16_t)drow * 32 + vx * VIEW_CW;
        uint8_t c;

        for (c = 0; c < VIEW_CW; c++)
            d[c] = blk ? (uint8_t)(blk[sub * VIEW_CW + c] | mask) : flat;
    }
}

/* Give the OTHER screen the same chrome as the one on show.

   A scroll flips on every sub-step, so both screens are displayed in
   turn.  The view area needs no help — present_all() writes all of it
   every time — but the header, status panel and key legend were painted
   into one screen only, by whichever render_*() last ran.  Without this
   the board would appear over the previous state's furniture on alternate
   frames, which is a strobe rather than a tear: worse than what it
   replaces.

   Once per scroll is enough, not once per sub-step.  Four flips means
   each screen is shown twice, and copying before the first one leaves
   both correct for the whole slide.

   Copied: character row 0, character rows 16-23 (the third third of the
   display file, contiguous), and the whole attribute area.  Row 16 is
   part of the view and gets overwritten immediately — including it costs
   nothing and saves picking rows out of an interleaved layout. */
static void copy_chrome(void)
{
    uint8_t *shown = (uint8_t *)(back ? 0xC000 : 0x4000);
    uint8_t *other = (uint8_t *)(back ? 0x4000 : 0xC000);
    uint8_t r;

    for (r = 0; r < 8; r++)                       /* row 0, interleaved */
        memcpy(other + (uint16_t)r * 256, shown + (uint16_t)r * 256, 32);
    memcpy(other + 0x1000, shown + 0x1000, 2048); /* rows 16-23 */
    memcpy(other + 6144, shown + 6144, 768);      /* attributes */
}

/* Scroll the window one cell, in VIEW_CW sub-steps, presenting each.

   Colour arrives with the art, a character at a time — the incoming
   attribute column is sliced in alongside the pixels rather than the
   whole view being recomposed once the tile has landed.  The cursor
   does not move at all: present_all() stamps it after every sub-step,
   so it stays put while the board slides under it. */
void scroll_view(int8_t dx, int8_t dy)
{
    uint8_t sub;

    /* Tear-free where there are two screens to swap between: each
       sub-step is composed off-display and revealed whole.  A 48K has
       nowhere to hide the work and presents into the live screen, which
       tears — that is the machine, not the design. */
    if (shadow_ok) copy_chrome();

    for (sub = 0; sub < VIEW_CW; sub++) {
        if (dx) {
            uint8_t vx   = dx > 0 ? (VIEW_COLS - 1) : 0;
            uint8_t src  = dx > 0 ? sub : (uint8_t)(VIEW_CW - 1 - sub);
            uint8_t dcol = dx > 0 ? 31 : 0;

            push_h(dx);
            slice_col(vx, src, dcol);
            slice_attr_col(vx, src, dcol);
        } else {
            uint8_t vy   = dy > 0 ? (VIEW_ROWS - 1) : 0;
            uint8_t src  = dy > 0 ? sub : (uint8_t)(VIEW_CH - 1 - sub);
            uint8_t drow = dy > 0 ? (VIEW_PX_ROWS - 8) : 0;
            uint8_t arow = dy > 0 ? (VIEW_AT_ROWS - 1) : 0;

            push_v(dy);
            slice_row(vy, src, drow);
            slice_attr_row(vy, src, arow);
        }
        if (shadow_ok) {
            render_compose();
            present_all();
            render_show();
        } else {
            present_all();
        }
    }
}

/* Queue a world cell for a full redraw next frame.  Orders are given
   from handle_input(), which runs outside the vblank window, so nothing
   there may draw its own result. */
void mark_dirty(uint8_t x, uint8_t y)
{
    if (dirty_n < DIRTY_MAX) {
        dirty_x[dirty_n] = x;
        dirty_y[dirty_n] = y;
        dirty_n++;
    }
}

/* --------------------------------------------------------- repainting */

/* Recolour the whole page, RANGE_CELLS cells a frame: a movement range
   going on or coming off changes colour and not one pixel. */
void recolour_page(void)
{
    attrs_left = VIEW_CELLS;
}

/* Throw away everything outstanding.  For a caller that is about to
   repaint the screen from scratch, so paying the old debt would just
   draw the previous level's board. */
void render_discard(void)
{
    attrs_left = 0;
    dirty_n = 0;
}

/* One frame's worth of what the renderer owes the screen.

   The two are mutually exclusive per frame on purpose: a move's two
   dirty cells are already a frame's worth at 144 bytes each, so the
   cheap attribute pass waits for a frame of its own.

   Moving the cursor is NOT in here.  The window follows the cursor, so
   a step changes every cell and there is nothing to spread — it is
   repainted in one go and accepts the tear (docs/PLAN.md § P7). */
void render_tick(void)
{
    if (dirty_n) {
        while (dirty_n) {
            uint8_t i = --dirty_n;
            int8_t vx = (int8_t)(dirty_x[i] - page_x);
            int8_t vy = (int8_t)(dirty_y[i] - page_y);

            if (vx >= 0 && vx < VIEW_COLS && vy >= 0 && vy < VIEW_ROWS)
                draw_view_cell((uint8_t)vx, (uint8_t)vy);
        }
        return;
    }

    if (attrs_left) {
        uint8_t n = RANGE_CELLS;

        while (n-- && attrs_left) {
            uint8_t i = (uint8_t)(VIEW_CELLS - attrs_left);

            attr_view_cell((uint8_t)(i % VIEW_COLS),
                           (uint8_t)(i / VIEW_COLS));
            attrs_left--;
        }
    }
}

/* ------------------------------------------------------- whole screens */

void render_busy(const char *msg)
{
    hint_row(msg, ATTR_BUSY);
}

/* The hint row, padded to full width here rather than in the literal.

   Every message used to be written out to 31 characters by hand so the
   next one would erase all of the last — up to thirty bytes of spaces
   each, in a build that has to fit in sixteen kilobytes.  The banner and
   the legend differ only in the colour they wash the row with. */
static void hint_row(const char *s, uint8_t attr)
{
    uint8_t col = 1;

    while (*s && col < 32) print_char(col++, ROW_HINT, *s++);
    while (col < 32)       print_char(col++, ROW_HINT, ' ');
    set_attr_rect(0, ROW_HINT, 32, 1, attr);
}

void render_hint(const char *hint)
{
    hint_row(hint, ATTR_HINT);
}

void render_title(void)
{
    render_compose();
    draw_header("ZX STRATEGY");

    print_at(1, 3, "MACHINE :");
    print_at(11, 3, is_128k ? "128K" : "48K");

    print_at(1, 4, "KEMPSTON:");
    print_at(11, 4, has_kempston ? "YES" : "NO");

    print_at(1, 5, "VSYNC   :");
    switch (vsync_mode) {
        /* The two bus lines differ by four characters, so they share a
           prefix rather than carrying it twice. */
        case VSYNC_MODE_48K:
        case VSYNC_MODE_128K:
            print_at(11, 5, "FLOATING BUS 0X");
            print_at(26, 5, vsync_mode == VSYNC_MODE_48K ? "40FF" : "0FFD");
            break;
        default:
            print_at(11, 5, "HALT FALLBACK  ");
            break;
    }
    /* Whether the shadow screen is actually in use.  On the title
       screen because it is the only place a tester without a debugger
       can see it, and because "is the 128K path live?" has been the
       hardest question to answer all the way through this. */
    print_at(1, 6, "SCREEN  :");
    print_at(11, 6, shadow_ok ? "DOUBLE" : "SINGLE");

    set_attr_rect(0, 3, 32, 4, ATTR_TEXT);

    print_at(1, 10, "SPACE / FIRE   START");
    set_attr_rect(0, 10, 32, 1, ATTR_HINT);

    print_at(1, 20, "QAOP / KEMPSTON TO MOVE");
    set_attr_rect(0, 20, 32, 1, ATTR_TEXT);

    /* The hint row is where the tune's banner goes, and where it is
       cleared back to. */
    render_hint(TITLE_HINT);

    /* Row 22 is left blank on purpose — it holds the floating bus sync
       marker written by vsync_wait(). */
    render_show();
}

void render_play(void)
{
    render_compose();
    draw_header("THE FIELD");
    set_page();
    draw_view();
    render_discard();       /* draw_view() already used the real colours */
    draw_status("CURSOR :", cursor_x, cursor_y);
    render_hint(PLAY_HINT);
    render_show();
}

void render_map(void)
{
    render_compose();
    draw_header("CAMPAIGN MAP");
    draw_map();
    solid_map_cell(cursor_x, cursor_y, ATTR_HINT);  /* the play cursor */
    solid_map_cell(cur_x, cur_y, ATTR_CURSOR);
    draw_status("CURSOR :", cur_x, cur_y);
    render_hint("QAOP LOOK AROUND  SPACE CLOSE");
    render_show();
}

/* A level ended.  player_won says which message to show; the exit is
   handled in handle_input(), which is where the level advances. */
void render_over(void)
{
    render_compose();
    draw_header(player_won ? "VICTORY" : "DEFEAT");

    print_at(1, 10, player_won ? "LEVEL TAKEN    :" : "LEVEL LOST     :");
    print_num(18, 10, level, 2);
    set_attr_rect(0, 10, 32, 1, ATTR_TEXT);

    render_hint(player_won ? "SPACE FOR THE NEXT LEVEL"
                           : "SPACE TO RETURN TO THE TITLE");
    render_show();
}

void render_won(void)
{
    render_compose();
    draw_header("CAMPAIGN COMPLETE");

    print_at(4, 9,  "EVERY LEVEL TAKEN");
    print_at(4, 11, "LEVELS WON     :");
    print_num(21, 11, LEVEL_COUNT, 2);
    set_attr_rect(0, 9, 32, 3, ATTR_TEXT);

    render_hint("PRESS A KEY");
    render_show();
}
