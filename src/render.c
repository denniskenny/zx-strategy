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
#define LEVEL_1_DEFINE_DATA

#include <string.h>

#include "../config/app_config.h"
#include "../config/game_config.h"
#include "../include/board.h"
#include "../include/dzx0.h"
#include "../include/gfx.h"
#include "../include/hw.h"
#include "../include/prng.h"
#include "../include/cutscenes.h"
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
/* The MAP sheet is one sprite per unit type.  The VIEW sheet carries those
   PLUS the explosion effect, which is a sprite and not a type -- hence two
   counts, and VIEW_SPRITES is the one that must match this sheet. */
#if UNITS_MAP_TILES != UNIT_TYPES
#error "units_map.zxp and game_config.h disagree on the unit type count"
#endif
#if UNITS_VIEW_TILES != VIEW_SPRITES
#error "units_view.zxp and game_config.h disagree on the view sprite count"
#endif
#if (UNITS_MAP_TILE_W != TILES_MAP_TILE_W) \
 || (UNITS_MAP_TILE_H != TILES_MAP_TILE_H)
#error "units_map.zxp tiles are not the size of a campaign-map cell"
#endif
/* Unit sprites are HALF the cell height and sit in its lower half: a unit
   stands on the ground, and the top half of the cell stays terrain, which
   is both less to store and less to cover up.  Full width, exactly half
   height, and a whole number of character rows -- anything else and the
   attribute split below has nowhere to land. */
#if UNITS_VIEW_TILE_W != TILES_VIEW_TILE_W
#error "units_view.zxp tiles are not the width of a play-view cell"
#endif
#if UNITS_VIEW_TILE_H * 2 != TILES_VIEW_TILE_H
#error "units_view.zxp tiles must be exactly half the height of a cell"
#endif
#if (UNITS_VIEW_TILE_ROWS * 2) != TILES_VIEW_TILE_ROWS
#error "units_view.zxp tiles must be a whole number of character rows"
#endif
#define UNIT_ROW_OFF   UNITS_VIEW_TILE_ROWS   /* char rows of terrain above */
#define UNIT_PIX_OFF   UNITS_VIEW_TILE_H      /* pixel rows of terrain above */

/* A WALKING unit sits in the UPPER half of its cell for half of every
   vertical step (docs/DESIGN.md § Walking a unit to its destination).
   Vertical steps are two characters, which is half a cell, so a unit is
   always in one half or the other of exactly one cell -- never across a
   boundary, which is what keeps the blit shift-free.
   
   `walk_cell` is that cell and NO_CELL the rest of the time; `walk_high`
   says which half.  Two bytes, consulted by every path that has to know
   where in a cell the sprite goes -- and there are four of them, which is
   why this is one answer and not four constants. */
static uint8_t walk_cell = NO_CELL;
static uint8_t walk_high;

static uint8_t cell_row_off(uint8_t vx, uint8_t vy);

/* Character rows of terrain ABOVE the sprite in this world cell. */
static uint8_t row_off_of(uint8_t cell)
{
    return (uint8_t)((cell == walk_cell && walk_high) ? 0 : UNIT_ROW_OFF);
}

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

/* Public because render_screens.c's cutscene pages a bank and has to put
   this back exactly as the renderer expects it.  0x7FFD is write-only, so
   this copy IS the truth about what is mapped. */
uint8_t page_reg;           /* 0x7FFD is write-only; remember it       */
static uint8_t back;        /* 1 = the back buffer is SCREEN_1         */
/* HAZARD, now fixed, and fixed for free.
 *
 * This is written only from the inline asm in screens_init(), which SDCC
 * cannot see -- so while it was `static` it reasoned from the
 * `shadow_ok = 0` there and folded `if (!shadow_ok)` to always-true.
 * That is SDCC's "warning 126: unreachable code", and the line it
 * discarded was the one arming the vsync marker.  It was harmless only
 * because render_show() sets vsync_marker_addr again before anything
 * waits on it.
 *
 * The old note here said `volatile` was the correct fix at 18 bytes, and
 * that is_128k needed nothing because it is read from other translation
 * units so SDCC cannot fold it.  render_screens.c now reads shadow_ok, so
 * the same protection applies here, at no cost: it is no longer static
 * and no longer foldable.  Splitting the file paid for itself twice. */
uint8_t shadow_ok;          /* 0 = the 128K path is not usable yet     */

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
uint8_t anim_frame;             /* 0 or 1: which sprite frame is showing */

/* World cells whose PICTURE is stale, redrawn on the next frame: a unit
   sprite that left one cell and arrived in another.  Orders are given
   outside the vblank window, so nothing there may draw its own result.

   FOUR, because that is the worst case one player action can now produce
   and mark_dirty() drops silently past the limit:

     1. the sprite leaves the cell it moved from
     2. it arrives in the cell it moved to
     3. the defender dies and its sprite goes
     4. the counter kills the attacker and its sprite goes too

   It was 2, sized for "a move needs two", and closing with an enemy
   quietly lost the last two marks.  The symptom was a tile drawn with
   the wrong bitmap while its ATTRIBUTES were correct -- recolour_page()
   repaints every attribute, so only the pixels stayed stale -- and it
   cleared as soon as the cell was scrolled off and back, because that
   redraws rather than repairs.  Nothing in either test suite looks at
   pixels this way; it was found by playing.

   Four draw_view_cell() calls a frame is twice what a page flip spends,
   which is affordable because it only happens on the frame after an
   attack, not every frame. */
#define DIRTY_MAX   4
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
/* Masks for the VIEW unit sprites only -- see docs/DESIGN.md.  Terrain is
   the background and needs none; the map-view sprites are 16x16 markers on
   a schematic and are drawn flat. */
#define MEM_UVIEW_MASK  (MEM_UVIEW_TILES + UNITS_VIEW_RAW_SIZE)
/* The second animation frame, pixels only -- it shares frame 1's mask and
   attributes.  Up here with the sheets rather than in a RAM bank: this
   region is plain RAM on a 48K and page 7 on a 128K, so one copy serves
   both machines and neither needs paging to read it. */
#define MEM_UVIEW_F2    (MEM_UVIEW_MASK + UNITS_VIEW_MASK_RAW_SIZE)

#define map_tiles       ((uint8_t *)MEM_MAP_TILES)
#define view_tiles      ((uint8_t *)MEM_VIEW_TILES)
#define unit_map_tiles  ((uint8_t *)MEM_UMAP_TILES)
#define unit_view_tiles ((uint8_t *)MEM_UVIEW_TILES)
#define unit_view_mask  ((uint8_t *)MEM_UVIEW_MASK)
#define unit_view_f2    ((uint8_t *)MEM_UVIEW_F2)

#if (TILES_MAP_RAW_SIZE + TILES_VIEW_RAW_SIZE + UNITS_MAP_RAW_SIZE \
     + UNITS_VIEW_RAW_SIZE + UNITS_VIEW_MASK_RAW_SIZE \
     + UNITS_VIEW_F2_RAW_SIZE) > MEM_TILES_SIZE
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
    dzx0_decompress(units_view_mask_zx0, unit_view_mask);
    dzx0_decompress(units_view_f2_zx0, unit_view_f2);
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
/* Bring the window to a world cell and repaint, before something there
   starts moving.
 *
 * The enemy turn used to centre the view AFTER enemy_step() returned --
 * which was fine when a move was instant, and wrong the moment moves
 * became animated: the unit walked while the camera was still on the
 * previous one, often off-screen entirely, and the view then jumped to
 * where it had finished.  The walk is the only evidence the player gets
 * of what the enemy did, so it has to be watched, not inferred. */
void render_view_to(uint8_t wx, uint8_t wy)
{
    cursor_x = wx;
    cursor_y = wy;
    set_page();
    render_play();
    render_hint("      ENEMY TURN");
}

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

/* A sprite over what is already in the buffer.

   The mask is stored INVERTED by tools/zxp_tiles_zx0.py -- set where the
   background should survive, clear over the sprite and its one-pixel
   outline -- so this is AND then OR with no complement per byte.

   Three memory accesses and two ALU ops per byte where compose_tile()
   manages one store, which is why only the view unit sprites get it:
   terrain is the background and has nothing to show through to. */
static void compose_masked(uint8_t vx, uint8_t vy,
                           const uint8_t *src, const uint8_t *mask)
{
    uint8_t *d = VBUF + (uint16_t)(vy * (VIEW_CH * 8)
                                   + cell_row_off(vx, vy) * 8) * 32
                      + vx * VIEW_CW;
    uint8_t r = UNITS_VIEW_TILE_H;

    while (r--) {
        d[0] = (uint8_t)((d[0] & mask[0]) | src[0]);
        d[1] = (uint8_t)((d[1] & mask[1]) | src[1]);
        d[2] = (uint8_t)((d[2] & mask[2]) | src[2]);
        d[3] = (uint8_t)((d[3] & mask[3]) | src[3]);
        src += VIEW_CW;
        mask += VIEW_CW;
        d += 32;
    }
}

#if VIEW_CW != 4
#error "compose_tile() is unrolled for 4-character-wide tiles"
#endif

/* One cell's attributes, either flooded with a single colour or copied
   from a sheet block with the side's ink ORed over it. */
static void compose_attr_rows(uint8_t vx, uint8_t vy, uint8_t row0,
                              uint8_t rows, uint8_t flat,
                              const uint8_t *src, uint8_t or_mask)
{
    uint8_t *d = VATTR + (uint16_t)(vy * VIEW_CH + row0) * 32 + vx * VIEW_CW;
    uint8_t r = rows;

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

static void compose_attr(uint8_t vx, uint8_t vy, uint8_t flat,
                         const uint8_t *src, uint8_t or_mask)
{
    compose_attr_rows(vx, vy, 0, VIEW_CH, flat, src, or_mask);
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
/* present_pixels() bakes these addresses into its assembly, and a #if
   INSIDE a function containing __asm is not evaluated by zcc -- see
   .claude/skills/zx-memory.  So the whole function is duplicated at file
   scope instead, where #if behaves, and the guard below catches any
   third layout before it can link with wrong constants. */
#if MEM_VBUF != 0x6000 && MEM_VBUF != 0xDB00
#error "present_pixels() has no baked constants for this MEM_VBUF"
#endif

static uint8_t ppx_rows;
static const uint8_t *ppx_src;

static void present_pixels(void) __naked
{
    __asm
        ld  a, #128
        ld  (_ppx_rows), a
#if MEM_VBUF == 0x6000
        ld  hl, #0x6000
        ld  de, #0x7200
#else
        ld  hl, #0xDB00
        ld  de, #0xED00
#endif
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
/* `whole` comes back 1 when the colour is a HIGHLIGHT rather than a unit's
   own: those wash the entire cell, because a half-height wash reads as a
   half-height thing rather than as "this cell is selected".  A unit's own
   colours keep the split -- that is the point of the split. */
static uint8_t attr_whole;

static const uint8_t *cell_attr(uint8_t vx, uint8_t vy,
                                uint8_t *flat, uint8_t *mask)
{
    attr_whole = 0;
    int8_t wx = (int8_t)(page_x + vx);
    int8_t wy = (int8_t)(page_y + vy);
    uint8_t cell, u;

    *mask = 0;
    if (wx < 0 || wx >= GRID_COLS || wy < 0 || wy >= GRID_ROWS)
        return view_attr_of(TER_SEA);

    cell = cell_of((uint8_t)wx, (uint8_t)wy);
    u = occupancy[cell];

    if (u != NO_UNIT) {
        if (u == selected)            { *flat = ATTR_HINT; attr_whole = 1; }
        else if (is_target(cell))     { *flat = ATTR_TARGET; attr_whole = 1; }
        else if (u_flags[u] & U_SIDE) {
            /* The enemy gets the sheet's per-cell BRIGHT bit too, which it
               never had: it was a flat colour, so every cell of an enemy
               sprite was the same shade.  Green can do this and red cannot
               -- see ATTR_UNIT_P -- so the two sides are asymmetric here
               out of necessity rather than choice. */
            *mask = ATTR_UNIT_E;
            return unit_view_attr_of(u_type[u]);
        }
        else if (u_flags[u] & U_ACTED) *flat = ATTR_UNIT_P_DONE;
        else {
            *mask = ATTR_UNIT_P;
            return unit_view_attr_of(u_type[u]);
        }
        return 0;
    }
    if (in_blue(cell)) {
        *flat = inspecting ? ATTR_RANGE_E : ATTR_RANGE;
        return 0;
    }
    return view_attr_of(terrain[cell]);
}

/* Colour, into the buffer. */
static void cell_layers(uint8_t vx, uint8_t vy, const uint8_t **bg,
                        const uint8_t **sp, const uint8_t **mask);

/* Where the sprite sits in this VIEW cell, in character rows. */
static uint8_t cell_row_off(uint8_t vx, uint8_t vy)
{
    int8_t wx = (int8_t)(page_x + vx);
    int8_t wy = (int8_t)(page_y + vy);

    if (wx < 0 || wx >= GRID_COLS || wy < 0 || wy >= GRID_ROWS)
        return UNIT_ROW_OFF;
    return row_off_of(cell_of((uint8_t)wx, (uint8_t)wy));
}

/* The terrain's own attribute block for a view cell, off-board included --
   the same answer cell_layers() gives for the background picture, so the
   colours above a unit's head always match the ground it is standing on. */
static const uint8_t *tile_attr_of(uint8_t vx, uint8_t vy)
{
    int8_t wx = (int8_t)(page_x + vx);
    int8_t wy = (int8_t)(page_y + vy);

    if (wx < 0 || wx >= GRID_COLS || wy < 0 || wy >= GRID_ROWS)
        return view_attr_of(TER_SEA);
    return view_attr_of(terrain[cell_of((uint8_t)wx, (uint8_t)wy)]);
}

/* A cell's colour.  With half-height sprites an occupied cell has TWO
   colourings: terrain in the top character rows, the unit's own block in
   the bottom ones where the sprite actually is.  Colouring the whole cell
   for the unit would paint terrain the unit's colour above its head,
   which is most of what covering the cell used to cost. */
static void compose_view_attr(uint8_t vx, uint8_t vy)
{
    uint8_t flat = 0, mask = 0;
    const uint8_t *blk = cell_attr(vx, vy, &flat, &mask);
    const uint8_t *bg, *sp, *m;

    cell_layers(vx, vy, &bg, &sp, &m);
    if (!sp || attr_whole) {
        compose_attr(vx, vy, flat, blk, mask);
        return;
    }

    /* A cell with a unit in it: the TOP rows are the tile's own colours,
       always.  Whatever cell_attr() decided -- the unit's block, a flat
       enemy red, a target wash -- applies only to the bottom rows where
       the sprite is.

       `flat` used to short-circuit this and colour the cell entire, which
       covered the terrain above an ENEMY unit (flat red) and above any
       unit that had already acted.  Those are the common cases, so most
       of the point of half-height sprites was being lost. */
    {
        uint8_t off = cell_row_off(vx, vy);

        /* Terrain fills whichever half the unit is NOT in. */
        if (off)
            compose_attr_rows(vx, vy, 0, off, 0, tile_attr_of(vx, vy), 0);
        compose_attr_rows(vx, vy, off, UNITS_VIEW_TILE_ROWS,
                          flat, blk, mask);
        if (!off)
            compose_attr_rows(vx, vy, UNITS_VIEW_TILE_ROWS,
                              (uint8_t)(VIEW_CH - UNITS_VIEW_TILE_ROWS), 0,
                              tile_attr_of(vx, vy) + UNITS_VIEW_TILE_ROWS
                                                     * VIEW_CW, 0);
    }
}

/* What a cell is made of: always a background tile, and for an occupied
   cell a sprite and its mask on top.  `*sp` is 0 when there is no unit.

   ONE answer, shared by every path that draws — the whole-cell compose
   and both scroll-edge slices.  They had drifted: the slices called
   cell_art() and copied it straight, so a sprite entering the view during
   a scroll arrived unmasked and stayed that way until something redrew
   its cell.  A second routine that draws the same thing differently is
   how that happens, so now there is not one. */
static void cell_layers(uint8_t vx, uint8_t vy, const uint8_t **bg,
                        const uint8_t **sp, const uint8_t **mask)
{
    int8_t wx = (int8_t)(page_x + vx);
    int8_t wy = (int8_t)(page_y + vy);
    uint8_t cell, u;

    *sp = 0;
    *mask = 0;
    if (wx < 0 || wx >= GRID_COLS || wy < 0 || wy >= GRID_ROWS) {
        *bg = view_tiles + (uint16_t)TER_SEA * TILES_VIEW_TILE_SIZE;
        return;
    }
    cell = cell_of((uint8_t)wx, (uint8_t)wy);
    *bg = view_tiles + (uint16_t)terrain[cell] * TILES_VIEW_TILE_SIZE;

    u = occupancy[cell];
    if (u != NO_UNIT) {
        uint16_t off = (uint16_t)u_type[u] * UNITS_VIEW_TILE_SIZE;
        /* One frame index for the whole board, so every unit is in step.
           The mask is shared, being the union of both frames. */
        *sp = (anim_frame ? unit_view_f2 : unit_view_tiles) + off;
        *mask = unit_view_mask + off;
    }
}

static void compose_view_cell(uint8_t vx, uint8_t vy)
{
    const uint8_t *bg, *sp, *mask;

    cell_layers(vx, vy, &bg, &sp, &mask);
    compose_tile(vx, vy, bg);
    if (sp) compose_masked(vx, vy, sp, mask);
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
        const uint8_t *bg, *art, *mask;
        uint8_t *d = VBUF + (uint16_t)vy * (VIEW_CH * 8) * 32 + dcol;
        uint8_t r = VIEW_CH * 8;

        uint8_t row = 0;

        cell_layers(vx, vy, &bg, &art, &mask);
        bg += sub;
        if (art) { art += sub; mask += sub; }

        while (r--) {
            /* The sprite is half-height and sits low, so the top of the
               cell is terrain even where a unit stands. */
            if (art && row >= UNIT_PIX_OFF) {
                *d = (uint8_t)((*bg & *mask) | *art);
                art += VIEW_CW;
                mask += VIEW_CW;
            } else {
                *d = *bg;
            }
            bg += VIEW_CW;
            d += 32;
            row++;
        }
    }
}

/* The character row arriving at the edge: sub-row `sub` of the cells in
   view row `vy`.  Contiguous in the tile, unlike a column. */
static void slice_row(uint8_t vy, uint8_t sub, uint8_t drow)
{
    uint8_t vx;

    for (vx = 0; vx < VIEW_COLS; vx++) {
        const uint8_t *bg, *art, *mask;
        uint8_t *d = VBUF + (uint16_t)drow * 32 + vx * VIEW_CW;
        uint16_t skip = (uint16_t)sub * 8 * VIEW_CW;
        uint8_t r = 8;

        cell_layers(vx, vy, &bg, &art, &mask);
        bg += skip;
        /* The sprite covers only the lower half of the cell, so a slice
           from the top half has terrain and nothing else. */
        if (art) {
            if (skip + 8 * VIEW_CW <= (uint16_t)UNIT_PIX_OFF * VIEW_CW) {
                art = 0;
            } else {
                uint16_t s2 = skip - (uint16_t)UNIT_PIX_OFF * VIEW_CW;
                art += s2;
                mask += s2;
            }
        }

        while (r--) {
            if (art) {
                d[0] = (uint8_t)((bg[0] & mask[0]) | art[0]);
                d[1] = (uint8_t)((bg[1] & mask[1]) | art[1]);
                d[2] = (uint8_t)((bg[2] & mask[2]) | art[2]);
                d[3] = (uint8_t)((bg[3] & mask[3]) | art[3]);
                art += VIEW_CW;
                mask += VIEW_CW;
            } else {
                d[0] = bg[0]; d[1] = bg[1]; d[2] = bg[2]; d[3] = bg[3];
            }
            bg += VIEW_CW;
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
        const uint8_t *bg, *sp, *m;
        const uint8_t *ter = tile_attr_of(vx, vy);
        uint8_t *d = VATTR + (uint16_t)vy * VIEW_CH * 32 + dcol;
        uint8_t r;

        /* Same split as compose_view_attr(): terrain above, unit below.
           Not merely for consistency -- a unit's attribute block is
           UNITS_VIEW_TILE_ROWS tall, which is HALF the cell, so indexing
           it by the cell's row read off the end of the block for the top
           rows and wrote whatever followed it into the screen.  That is
           the ragged colour at a scrolling edge. */
        uint8_t off = cell_row_off(vx, vy);

        cell_layers(vx, vy, &bg, &sp, &m);
        for (r = 0; r < VIEW_CH; r++) {
            if (sp && r >= off && r < (uint8_t)(off + UNITS_VIEW_TILE_ROWS)) {
                *d = (uint8_t)(blk ? (blk[(r - off) * VIEW_CW + sub] | mask)
                                   : flat);
            } else if (sp) {
                *d = ter[r * VIEW_CW + sub];
            } else {
                *d = blk ? (uint8_t)(blk[r * VIEW_CW + sub] | mask) : flat;
            }
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
        const uint8_t *bg, *sp, *m;
        uint8_t *d = VATTR + (uint16_t)drow * 32 + vx * VIEW_CW;
        uint8_t c;

        /* As slice_attr_col(): a unit's block is half the cell tall, so a
           sub-row above the sprite must take the terrain's colours and
           must NOT index the unit block, which does not reach that far. */
        uint8_t off = cell_row_off(vx, vy);

        cell_layers(vx, vy, &bg, &sp, &m);
        if (sp && (sub < off
                   || sub >= (uint8_t)(off + UNITS_VIEW_TILE_ROWS))) {
            const uint8_t *ter = tile_attr_of(vx, vy);

            for (c = 0; c < VIEW_CW; c++)
                d[c] = ter[sub * VIEW_CW + c];
        } else {
            uint8_t br = (uint8_t)(sp ? sub - off : sub);

            for (c = 0; c < VIEW_CW; c++)
                d[c] = blk ? (uint8_t)(blk[br * VIEW_CW + c] | mask) : flat;
        }
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

    /* Pay off the dirty list BEFORE pushing the buffer.

       A scroll shifts what is already in VBUF and composes only the
       incoming edge -- it never revisits the cells it slides along.  So a
       cell still waiting to be redrawn gets pushed WITH ITS STALE PIXELS
       and presented that way, and a held arrow key keeps pushing it: the
       old picture travels across the screen until render_tick() next gets
       a frame in.

       That is the artefact seen when selecting or moving a unit, which is
       exactly when the list is non-empty -- the sprite leaving one cell
       and arriving in another are the two marks, and moving the cursor
       immediately afterwards is the natural thing to do.

       Draining here rather than gating input: the marks are world
       coordinates and survive the scroll perfectly well, they simply have
       to be applied before the pixels move. */
    while (dirty_n) {
        uint8_t i = --dirty_n;
        int8_t vx = (int8_t)(dirty_x[i] - page_x);
        int8_t vy = (int8_t)(dirty_y[i] - page_y);

        if (vx >= 0 && vx < VIEW_COLS && vy >= 0 && vy < VIEW_ROWS)
            compose_view_cell((uint8_t)vx, (uint8_t)vy);
    }

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
/* Complain when a mark is dropped, in the debug build only.

   Dropping one is not a small thing: the cell keeps its old bitmap while
   its attributes are repainted, so the board shows a unit that is not
   there, or terrain where a unit is -- and it lasts until something
   scrolls the cell off and back.  That cost a playtest to find once
   already, because it is silent and neither test suite reads pixels.

   The border goes RED and STAYS red: a one-frame flash is exactly what
   gets missed, and nothing else in ST_PLAY writes the border, so it
   latches until the next state change repaints it.  Zero bytes in the
   shipping tap, where the whole function compiles away.

   The asm lives in its own __naked function on purpose: zcc does not
   evaluate preprocessor directives inside a function body that contains
   an __asm block (.claude/skills/zx-memory), so the #if has to be out
   here and mark_dirty() has to stay asm-free. */
#if DEBUG_STATE_WALK
static void dirty_dropped(void) __naked
{
    __asm
    ld  a, #2                   ; red
    out (#0xFE), a
    ret
    __endasm;
}
#else
#define dirty_dropped() ((void)0)
#endif

void mark_dirty(uint8_t x, uint8_t y)
{
    if (dirty_n < DIRTY_MAX) {
        dirty_x[dirty_n] = x;
        dirty_y[dirty_n] = y;
        dirty_n++;
        return;
    }
    dirty_dropped();
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
/* --- Destruction --------------------------------------------------- */

/* One speaker cycle: up for `noise_hold` counts, down for the same.
 *
 * Globals rather than arguments on purpose.  A __naked function has to dig
 * its parameters off the stack by hand, and the first version of this got
 * the offsets subtly wrong -- it worked, at the wrong pitch, which is the
 * hardest kind of wrong to see.  Two bytes of static costs less than that
 * risk.
 *
 * The hold is 16-bit and the loop is `dec de / ld a,d / or e`, ~26 T per
 * iteration.  With holds of 300-1800 that is roughly 8 000-47 000 T a half
 * cycle: about 40-220 Hz, which is low and crunchy.  The old version used
 * an 8-bit hold of 8-71 in a 13 T loop -- 2-16 kHz, which is why it
 * whistled.
 */
static uint16_t noise_hold;
static uint8_t  noise_port;

static void noise_cycle(void) __naked
{
    __asm
        ld  a, (_noise_port)
        or  #0x10               ; speaker up
        out (0xFE), a
        ld  de, (_noise_hold)
    _nc_up:
        dec de
        ld  a, d
        or  e
        jr  nz, _nc_up

        ld  a, (_noise_port)
        and #0x07               ; speaker down, border kept
        out (0xFE), a
        ld  de, (_noise_hold)
    _nc_dn:
        dec de
        ld  a, d
        or  e
        jr  nz, _nc_dn
        ret
    __endasm;
}

/* A burst of white noise: `len` cycles, each with its own random period.
 *
 * The width of the period is what stops it settling into a tone, and its
 * lowness is what makes it a crunch rather than a hiss.  Blocking, which
 * is fine -- a unit dies between turns, not inside a frame budget. */
static void noise(uint8_t len, uint8_t border, uint16_t base, uint16_t mask)
{
    static prng_t np = { 0xACE1u, 0x1234u };

    noise_port = border;
    while (len--) {
        noise_hold = (uint16_t)((prng_next(&np) & mask) + base);
        noise_cycle();
    }
}

/* One of the voices in config/game_config.h.  Border left alone -- only
   the explosion flickers it, and that is part of the explosion. */
void sfx(uint8_t voice)
{
    noise(sfx_len[voice], 7, sfx_base[voice], sfx_mask[voice]);
}

/* Draw the explosion over the terrain at a view cell, in `attr`. */
static void boom_cell(uint8_t vx, uint8_t vy, uint8_t frame, uint8_t attr)
{
    int8_t wx = (int8_t)(page_x + vx);
    int8_t wy = (int8_t)(page_y + vy);
    uint16_t off = (uint16_t)SPRITE_EXPLOSION * UNITS_VIEW_TILE_SIZE;
    const uint8_t *bg;

    if (wx < 0 || wx >= GRID_COLS || wy < 0 || wy >= GRID_ROWS) return;
    bg = view_tiles + (uint16_t)terrain[cell_of((uint8_t)wx, (uint8_t)wy)]
                      * TILES_VIEW_TILE_SIZE;

    compose_tile(vx, vy, bg);
    compose_masked(vx, vy,
                   (frame ? unit_view_f2 : unit_view_tiles) + off,
                   unit_view_mask + off);
    compose_attr(vx, vy, attr, 0, 0);
    present_cell(vx, vy);
}

/* Two frames, twice round, in FLASHing red and white, with a bang.

   The explosion is a sprite and not a unit type (SPRITE_EXPLOSION), so
   nothing on the board owns it -- by the time this runs the cell is empty
   and it is drawn straight over the terrain.

   Attributes alternate white-on-red and red-on-white, both with FLASH set,
   so the hardware flip runs on top of the frame swap: two rates of
   flicker at once, which is more violent than either alone and costs
   nothing to do. */
void render_boom(uint8_t wx, uint8_t wy)
{
    static const uint8_t boom_attr[2] = { ATTR_BOOM_A, ATTR_BOOM_B };
    int8_t vx = (int8_t)(wx - page_x);
    int8_t vy = (int8_t)(wy - page_y);
    uint8_t step;

    if (vx < 0 || vx >= VIEW_COLS || vy < 0 || vy >= VIEW_ROWS) return;

    for (step = 0; step < 4; step++) {           /* 2 frames x 2 cycles */
        boom_cell((uint8_t)vx, (uint8_t)vy, (uint8_t)(step & 1),
                  boom_attr[step & 1]);
        /* Shortening bursts, so the bang decays rather than droning. */
        noise((uint8_t)(sfx_len[SFX_BOOM] - step * 4),
              (uint8_t)(step & 1 ? 2 : 7),
              sfx_base[SFX_BOOM], sfx_mask[SFX_BOOM]);
    }

    /* Put the cell back to whatever it should be now -- empty ground, or
       the unit that survived if this was only a near miss. */
    mark_dirty(wx, wy);
}

/* One visual step of a walk: put the sprite in `cell` at `high` or low,
   repaint what changed, show it, and hold for a beat.

   `prev` is where the sprite was, so both cells get repainted -- the one
   being left back to bare terrain, the one being entered with the unit in
   it.  Two cells a step, which is inside DIRTY_MAX, but this does not use
   the dirty list: a walk is a SEQUENCE and that list is an unordered set
   of cells owed a repaint (docs/DESIGN.md § Walking a unit).

   Blocking, like scroll_view(): a unit moves between turns, not inside a
   frame budget. */
void render_walk_step(uint8_t prev, uint8_t cell, uint8_t high)
{
    int8_t vx, vy;
    uint8_t i;

    walk_cell = cell;
    walk_high = high;

    /* Compose into the buffer, then present the WHOLE view and flip --
       the same shape scroll_view() uses, and for the same reason.
     *
     * draw_view_cell() presents one cell to the screen currently being
     * drawn into.  With a shadow screen there are two, so a cell touched
     * that way is right on one and stale on the other, and the next flip
     * shows the stale one: a frozen sprite where the unit used to be.
     * That is the ghosting, and it is why it turned up on a +3 -- a 48K
     * has one screen and cannot show it.
     *
     * copy_chrome() already solves this for the chrome.  The view area
     * needs the same treatment, and present_all() gives it: every cell,
     * every time, so the two screens cannot drift. */
    for (i = 0; i < 2; i++) {
        uint8_t c = i ? cell : prev;

        vx = (int8_t)(col_of[c] - page_x);
        vy = (int8_t)((c / GRID_COLS) - page_y);
        if (vx >= 0 && vx < VIEW_COLS && vy >= 0 && vy < VIEW_ROWS)
            compose_view_cell((uint8_t)vx, (uint8_t)vy);
        if (c == cell) break;           /* same cell: one compose is enough */
    }
    if (shadow_ok) {
        copy_chrome();
        render_compose();
        present_all();
        render_show();
    } else {
        present_all();
    }
    /* One tick per character step, not one per move: the step is the thing
       the player sees, so it is the thing that should be heard.  SFX_MOVE
       is the shortest voice for exactly this reason. */
    sfx(SFX_MOVE);
    for (i = 0; i < WALK_BEAT; i++) vsync_wait();
}

/* Take every highlight off the board NOW, not over the next N frames.

   recolour_page() only queues the work for render_tick(), and a walk
   blocks -- so the movement wash would sit under the unit for the whole
   animation.  It reads as the unit still choosing where to go. */
void render_clear_highlights(void)
{
    uint8_t i;

    for (i = 0; i < VIEW_CELLS; i++)
        attr_view_cell((uint8_t)(i % VIEW_COLS), (uint8_t)(i / VIEW_COLS));
    attrs_left = 0;
}

/* The walk is over: the sprite rests in the lower half again, and the
   view is repainted whole.
 *
 * The repaint is a REMEDY, not a nicety, and it is worth being honest
 * about which: ghost sprites were appearing a cell away from a unit and
 * staying frozen while the real ones animated.  Repainting every cell on
 * the animation beat made them vanish, which proves they are stale buffer
 * content -- some path fails to repaint a cell it has invalidated -- but
 * not WHICH path.  This clears them at the one moment a full repaint is
 * affordable: the walk already blocks, so one more pass costs nothing the
 * player can feel.
 *
 * It is a symptom fix.  The path that leaves the cell dirty is still in
 * here, and will show up again anywhere a unit changes cell without a
 * walk -- so if ghosts return, look there rather than adding a second
 * repaint. */
void render_walk_end(void)
{
    uint8_t i;

    walk_cell = NO_CELL;
    walk_high = 0;

    /* One clean pass over both screens, so nothing is left half-updated
       whichever one is on show when the walk ends. */
    for (i = 0; i < VIEW_CELLS; i++)
        compose_view_cell((uint8_t)(i % VIEW_COLS), (uint8_t)(i / VIEW_COLS));
    if (shadow_ok) {
        copy_chrome();
        render_compose();
        present_all();
        render_show();
    }
    present_all();
}

/* Frames between animation steps.  Slow on purpose: the sprites are two
   poses, not a walk cycle, and a fast flip reads as a flicker. */
#define ANIM_BEAT   18

static uint8_t anim_beat;

/* Swap every unit to the other frame.

   Runs ONLY when the screen is at rest (docs/DESIGN.md § Animate only
   when the screen is at rest): nothing stale to repaint, no recolour in
   progress, and the view where it was.  That inverts the cost -- the
   expensive frames are exactly the ones this is skipped on, so it never
   competes for the budget it would otherwise blow, and a resting frame
   has the whole vblank window going spare anyway.

   Marking every occupied cell dirty would overrun DIRTY_MAX four times
   over, so this repaints them itself, in place, and leaves the dirty list
   alone. */
static void animate(void)
{
    uint8_t i;

    anim_frame = (uint8_t)!anim_frame;

#if DEBUG_STATE_WALK
    /* DIAGNOSTIC: repaint the WHOLE view, not just the occupied cells.
     *
     * A ghost sprite is a cell holding a picture nothing believes is
     * there, so nothing repaints it -- which is why it sits still while
     * the real units animate.  If it vanishes on the next animation beat
     * in this build, it is stale buffer content and the fault is a path
     * that fails to repaint.  If it survives, something is actively
     * drawing it and the fault is upstream in what a cell contains.
     *
     * Debug build only: this is VIEW_CELLS composes a beat, far past what
     * a resting frame can afford. */
    for (i = 0; i < VIEW_CELLS; i++)
        draw_view_cell((uint8_t)(i % VIEW_COLS), (uint8_t)(i / VIEW_COLS));
    return;
#endif
    for (i = 0; i < unit_count; i++) {
        int8_t vx, vy;

        if (u_type[i] == NO_UNIT) continue;
        vx = (int8_t)(col_of[u_cell[i]] - page_x);
        vy = (int8_t)((u_cell[i] / GRID_COLS) - page_y);
        if (vx < 0 || vx >= VIEW_COLS || vy < 0 || vy >= VIEW_ROWS)
            continue;
        draw_view_cell((uint8_t)vx, (uint8_t)vy);
    }
}

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
        return;
    }

    /* At rest: nothing else wanted this frame, so the board may breathe. */
    if (++anim_beat >= ANIM_BEAT) {
        anim_beat = 0;
        animate();
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
    render_hint("QAOP LOOK AROUND  ENTER BACK");
    render_show();
}

void render_won(void)
{
    render_compose();
    draw_header("CAMPAIGN COMPLETE");

    print_at(4, 9,  "EVERY LEVEL TAKEN");
    print_at(4, 11, "LEVELS WON     :");
    print_num(22, 11, LEVEL_COUNT, 2);
    print_at(4, 12, "FINAL SCORE    :");
    print_num(21, 12,
              (uint8_t)(campaign_score > 999 ? 999 : campaign_score), 3);
    set_attr_rect(0, 9, 32, 4, ATTR_TEXT);

    render_hint("SPACE / FIRE 1");
    render_show();
}

/* render_title(), render_over() and render_cutscene() live in
   src/render_screens.c, in the contended window: they paint once per state
   change and never inside a frame budget.

   The other three stayed, for three different reasons.  render_play() is
   not once-per-state -- the walk and the enemy turn call it mid-sequence.
   render_map()'s helpers all live here.  And render_won() simply did not
   fit: the contended window is full, which is the real limit on this
   technique. */
