#ifndef _RENDER_H_
#define _RENDER_H_

/* ================================================================== */
/* render.h — everything that writes to the screen                    */
/* ================================================================== */
/* This is the side of the program with a deadline.  After
 * vsync_wait() returns, roughly 256 bytes can be written before the
 * raster catches up and the write tears, so every routine here is
 * sized against that budget and anything larger is spread across
 * frames (docs/DESIGN.md § Logic and rendering).
 *
 * It reads the board through board.h and never changes it.  That is
 * what makes it replaceable: these functions are the candidates for
 * hand-written Z80, and a rewrite has to preserve only what is
 * declared here.
 */

#include <stdint.h>

#include "board.h"
#include "tiles_map.h"
#include "tiles_view.h"

/* --- Attributes (never 0x02 or 0x03: the vsync marker owns those) --- */
#define ATTR_TITLE  0x45    /* bright cyan ink, black paper   */
#define ATTR_TEXT   0x47    /* bright white ink, black paper  */
#define ATTR_HINT   0x46    /* bright yellow ink, black paper */
#define ATTR_BG     0x07    /* white ink, black paper         */
#define ATTR_CURSOR 0x78    /* black ink, white paper         */
#define ATTR_VOID   0x00    /* off-world cells: black on black */

/* Units share one sprite per type; the side is carried by the cell
   attribute, so these are the only thing telling the armies apart
   (docs/DESIGN.md: "Enemy units are red, player units are cyan").
   The sheets supply a BRIGHT flag per character cell — the sprite's
   shading — which is ORed over the side's ink. */
#define ATTR_UNIT_P 0x05    /* cyan ink, black paper; sheet adds BRIGHT */
#define ATTR_UNIT_E 0x42    /* bright red ink, black paper             */
#define ATTR_BRIGHT 0x40    /* the BRIGHT bit the unit sheets carry     */

/* The enemy is always bright, and cannot be otherwise: non-bright red
   on black is 0x02, and 0x02 | 1 is the floating bus sync marker.  Its
   ink therefore carries BRIGHT already, so ORing the sheet's flag over
   it is a no-op and enemy sprites are drawn flat.  Only the player's
   shading survives.  This is the same collision that stops a spent
   enemy unit from being dimmed. */

/* A player unit that has spent its action goes uniformly dim — the
   sheet's shading is dropped, which is what makes "used" legible at a
   glance against an otherwise shaded sprite. */
#define ATTR_UNIT_P_DONE 0x05   /* cyan ink, black paper, not bright */

/* Ground the selected unit can reach: the terrain art stays, its paper
   turns blue.  Distinct from the cursor (white paper) and from every
   terrain and unit colour, all of which use black paper. */
#define ATTR_RANGE  0x4F    /* bright white ink, blue paper   */

/* The "working" banner, deliberately not the hint line's yellow. */
#define ATTR_BUSY   0x42    /* bright red ink, black paper    */

/* --- Campaign overview: the whole world in tiles_map.zxp cells ------- */
#define CELL_W      TILES_MAP_TILE_W
#define CELL_ROWS   TILES_MAP_TILE_ROWS
#define MAP_COL     2
#define MAP_ROW     3

/* --- Play view: an 8x4 page of tiles_view.zxp cells.  A full page is
       32x16 characters — 4 KB of screen writes, far more than one vblank
       window allows — so the view PAGES rather than scrolls: the cursor
       moves inside a fixed page (2 cells redrawn per step) and the page
       flips only when it steps off the edge. --- */
#define VIEW_COLS   8
#define VIEW_ROWS   4
#define VIEW_CELLS  (VIEW_COLS * VIEW_ROWS)
#define VIEW_CW     TILES_VIEW_TILE_W
#define VIEW_CH     TILES_VIEW_TILE_ROWS
#define VIEW_COL    ((32 - VIEW_COLS * VIEW_CW) / 2)
#define VIEW_ROW    1

/* Cells repainted per frame during a page flip.  Two 4x4 tiles is ~256
   bytes of screen writes, which fits the post-vsync_wait() budget with
   room to spare; the whole page takes VIEW_CELLS / this many frames. */
#define PAGE_CELLS  2

/* Cells RECOLOURED per frame, when a movement range goes on or comes
   off.  An attribute-only cell is 16 bytes against a full repaint's
   144, so eight of them is 128 bytes — half the budget above, and the
   whole page in four frames. */
#define RANGE_CELLS 8

/* Status panel: four lines, ending one row above the key legend.  The
   play view stops at row 16 and the overview at row 17, so ROW_UNIT is
   the last row either renderer leaves free. */
#define ROW_UNIT    17
#define ROW_TURN    18
#define ROW_COORD   19
#define ROW_TERRAIN 20
#define PANEL_TOP   ROW_UNIT
#define PANEL_ROWS  4

/* The key legend, and the row a long operation borrows to say it is
   working.  Every state paints this row on entry, so a banner needs
   nothing saved: whoever comes next writes over it. */
#define ROW_HINT    21

/* The legends a banner has to be able to put back, each 31 columns so
   it overwrites whatever the banner left.  ST_TITLE keeps its key list
   up at rows 10-11 and leaves this row blank. */
#if DEBUG_STATE_WALK
#define PLAY_HINT   "SPC MOVE ENTER TURN MAP W/L END"
#else
#define PLAY_HINT   "SPACE MOVE  ENTER END TURN  MAP"
#endif
#define TITLE_HINT  "                               "

/* Both renderers must fit above the status panel. */
#if (MAP_COL + GRID_COLS * CELL_W > 32) \
 || (MAP_ROW + GRID_ROWS * CELL_ROWS > PANEL_TOP)
#error "level_1.tmx is too large for the campaign overview"
#endif
#if (VIEW_COL < 0) || (VIEW_ROW + VIEW_ROWS * VIEW_CH > PANEL_TOP)
#error "the play view does not fit on screen; shrink the page or the tiles"
#endif

/* --- Repaint queues --------------------------------------------------
 * What the renderer owes the screen, paid off a few cells per frame by
 * render_tick().  Ask for a repaint through the calls below rather than
 * setting these; they are exposed because the frame loop has to be able
 * to SEE the debt — `cells_left` non-zero means a page flip is mid-way
 * and both cursor movement and new orders hold until it finishes. */
extern uint8_t page_x, page_y;  /* top-left world cell of the page      */
extern uint8_t cells_left;      /* cells still to repaint on a flip     */
extern uint8_t attrs_left;      /* page cells still to recolour         */
extern uint8_t dirty_n;         /* cells whose picture is stale         */

/* --- Setup ----------------------------------------------------------- */
void load_tiles(void);          /* unpack the four sheets, once         */

/* --- Text and chrome -------------------------------------------------- */
void print_num(uint8_t col, uint8_t row, uint16_t v, uint8_t digits);
void print_char(uint8_t col, uint8_t row, char c);
void draw_header(const char *title);
void draw_status(const char *label, uint8_t x, uint8_t y);

/* --- The play view ---------------------------------------------------- */
void set_page(void);            /* page the view onto the cursor        */
void draw_view(void);           /* every cell, pixels and colour        */
void draw_view_cell(uint8_t vx, uint8_t vy);
void attr_view_cell(uint8_t vx, uint8_t vy);    /* colour only          */

/* --- The campaign overview -------------------------------------------- */
void draw_map(void);
void draw_cell(uint8_t cx, uint8_t cy);
void attr_map_cell(uint8_t cx, uint8_t cy);     /* colour only          */
void solid_map_cell(uint8_t cx, uint8_t cy, uint8_t attr);

/* --- Deferred repaints ------------------------------------------------
 * Orders are given outside the vblank window, so nothing there may draw
 * its own result: it marks the cell and render_tick() pays it off. */
void mark_dirty(uint8_t x, uint8_t y);
void start_page_flip(void);     /* repaint the whole page, N per frame  */
void recolour_page(void);       /* re-attribute the whole page          */
void render_discard(void);      /* forget what is owed; about to redraw */
void render_tick(void);         /* one frame's worth of owed repaints   */

/* --- Whole screens ----------------------------------------------------
 * One per state, painted on entry.  They set no game state; the caller
 * owns that. */
void render_title(void);
void render_play(void);
void render_map(void);
void render_over(void);
void render_won(void);

/* --- The long-operation banner ---------------------------------------- */
void render_busy(const char *msg);      /* red banner on the hint row   */
void render_hint(const char *hint);     /* put the legend back          */

#endif /* _RENDER_H_ */
