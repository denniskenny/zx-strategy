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

#include "../config/app_config.h"
#include "../config/game_config.h"
#include "../include/board.h"
#include "../include/dzx0.h"
#include "../include/gfx.h"
#include "../include/hw.h"
#include "../include/level_1.h"
#include "../include/render.h"
#include "../include/tiles_map.h"
#include "../include/tiles_view.h"
#include "../include/units_map.h"
#include "../include/units_view.h"
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

/* ------------------------------------------------------------- state */

uint8_t page_x, page_y;
uint8_t cells_left;
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
static uint8_t map_tiles[TILES_MAP_RAW_SIZE];
static uint8_t view_tiles[TILES_VIEW_RAW_SIZE];
static uint8_t unit_map_tiles[UNITS_MAP_RAW_SIZE];
static uint8_t unit_view_tiles[UNITS_VIEW_RAW_SIZE];

/* Tile t's attribute block within each sheet. */
#define map_attr_of(t) \
    (map_tiles + TILES_MAP_ATTR_OFF + (uint16_t)(t) * TILES_MAP_ATTR_SIZE)
#define view_attr_of(t) \
    (view_tiles + TILES_VIEW_ATTR_OFF + (uint16_t)(t) * TILES_VIEW_ATTR_SIZE)
#define unit_map_attr_of(t) \
    (unit_map_tiles + UNITS_MAP_ATTR_OFF + (uint16_t)(t) * UNITS_MAP_ATTR_SIZE)
#define unit_view_attr_of(t) \
    (unit_view_tiles + UNITS_VIEW_ATTR_OFF + (uint16_t)(t) * UNITS_VIEW_ATTR_SIZE)

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
        print_at(10, ROW_UNIT, "-                     ");
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
void set_page(void)
{
    page_x = (uint8_t)(cursor_x - cursor_x % VIEW_COLS);
    page_y = (uint8_t)(cursor_y - cursor_y % VIEW_ROWS);
}

/* The colours of one play-view cell, and the single place that decides
   them.  Attributes only: laying a movement range down or taking it
   away changes up to 25 cells' colour and not one pixel of their art,
   so this costs 16 bytes a cell where draw_view_cell() costs 144 —
   the difference between recolouring the page in four frames and
   repainting it in thirteen.

   Order matters.  The cursor wins over everything, because the player
   has to be able to find it.  A unit outranks the range highlight, but
   only nominally: the flood fill refuses occupied cells, so no cell is
   ever both.  Bare ground gets the terrain sheet's own block, which is
   the only path that paints authored per-cell colour rather than a
   flat wash. */
void attr_view_cell(uint8_t vx, uint8_t vy)
{
    uint8_t col = (uint8_t)(VIEW_COL + vx * VIEW_CW);
    uint8_t row = (uint8_t)(VIEW_ROW + vy * VIEW_CH);
    uint8_t wx = (uint8_t)(page_x + vx);
    uint8_t wy = (uint8_t)(page_y + vy);
    uint8_t cell, u;

    if (wx >= GRID_COLS || wy >= GRID_ROWS) {
        set_attr_rect(col, row, VIEW_CW, VIEW_CH, ATTR_VOID);
        return;
    }
    if (wx == cursor_x && wy == cursor_y) {
        set_attr_rect(col, row, VIEW_CW, VIEW_CH, ATTR_CURSOR);
        return;
    }

    cell = cell_of(wx, wy);
    u = occupancy[cell];

    if (u != NO_UNIT) {
        if (u == selected)
            set_attr_rect(col, row, VIEW_CW, VIEW_CH, ATTR_HINT);
        else
            attr_unit_cell(col, row, VIEW_CW, VIEW_CH, u,
                           unit_view_attr_of(u_type[u]));
    } else if (selected != NO_UNIT && cost[cell] != NO_COST) {
        set_attr_rect(col, row, VIEW_CW, VIEW_CH, ATTR_RANGE);
    } else {
        blit_attr_rect(col, row, VIEW_CW, VIEW_CH,
                       view_attr_of(terrain[cell]), 0);
    }
}

/* One cell of the play view: a terrain tile from tiles_view.zxp with
   its occupant blitted over it, or blank for cells outside the world,
   then the colours from attr_view_cell(). */
void draw_view_cell(uint8_t vx, uint8_t vy)
{
    uint8_t col = (uint8_t)(VIEW_COL + vx * VIEW_CW);
    uint8_t row = (uint8_t)(VIEW_ROW + vy * VIEW_CH);
    uint8_t wx = (uint8_t)(page_x + vx);
    uint8_t wy = (uint8_t)(page_y + vy);

    if (wx >= GRID_COLS || wy >= GRID_ROWS) {
        clear_blit((int8_t)col, (uint8_t)(row << 3),
                   VIEW_CW, TILES_VIEW_TILE_H);
    } else {
        uint8_t cell = cell_of(wx, wy);
        uint8_t u = occupancy[cell];

        write_blit((int8_t)col, (uint8_t)(row << 3),
                   view_tiles + (uint16_t)terrain[cell] * TILES_VIEW_TILE_SIZE,
                   TILES_VIEW_TILE_W, TILES_VIEW_TILE_H);
        if (u != NO_UNIT)
            write_blit((int8_t)col, (uint8_t)(row << 3),
                       unit_view_tiles +
                           (uint16_t)u_type[u] * UNITS_VIEW_TILE_SIZE,
                       UNITS_VIEW_TILE_W, UNITS_VIEW_TILE_H);
    }
    attr_view_cell(vx, vy);
}

void draw_view(void)
{
    uint8_t i;

    for (i = 0; i < VIEW_CELLS; i++)
        draw_view_cell((uint8_t)(i % VIEW_COLS), (uint8_t)(i / VIEW_COLS));
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

/* Repaint the whole page, PAGE_CELLS tiles a frame. */
void start_page_flip(void)
{
    cells_left = VIEW_CELLS;
    attrs_left = 0;         /* the flip rewrites every attribute itself */
}

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
    cells_left = 0;
    attrs_left = 0;
    dirty_n = 0;
}

/* One frame's worth of what the renderer owes the screen, in the order
   that keeps each frame inside its budget.
   
   The three are mutually exclusive per frame on purpose.  A flip is
   PAGE_CELLS full cells (~256 bytes) and rewrites every attribute
   anyway, so nothing else need run during one.  A move's two dirty
   cells are already a frame's worth at 144 bytes each.  Only when
   neither is outstanding does the cheap attribute pass get the frame. */
void render_tick(void)
{
    if (cells_left) {
        uint8_t n = PAGE_CELLS;

        while (n-- && cells_left) {
            uint8_t i = (uint8_t)(VIEW_CELLS - cells_left);

            draw_view_cell((uint8_t)(i % VIEW_COLS),
                           (uint8_t)(i / VIEW_COLS));
            cells_left--;
        }
        return;
    }

    if (dirty_n) {
        while (dirty_n) {
            uint8_t i = --dirty_n;

            if (dirty_x[i] >= page_x && dirty_x[i] < page_x + VIEW_COLS &&
                dirty_y[i] >= page_y && dirty_y[i] < page_y + VIEW_ROWS)
                draw_view_cell((uint8_t)(dirty_x[i] - page_x),
                               (uint8_t)(dirty_y[i] - page_y));
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
    print_at(1, ROW_HINT, msg);
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_BUSY);
}

void render_hint(const char *hint)
{
    print_at(1, ROW_HINT, hint);
    set_attr_rect(0, ROW_HINT, 32, 1, ATTR_HINT);
}

void render_title(void)
{
    draw_header("ZX STRATEGY");

    print_at(1, 3, "MACHINE :");
    print_at(11, 3, is_128k ? "128K" : "48K");

    print_at(1, 4, "KEMPSTON:");
    print_at(11, 4, has_kempston ? "YES" : "NO");

    print_at(1, 5, "VSYNC   :");
    switch (vsync_mode) {
        case VSYNC_MODE_48K:
            print_at(11, 5, "FLOATING BUS 0X40FF");
            break;
        case VSYNC_MODE_128K:
            print_at(11, 5, "FLOATING BUS 0X0FFD");
            break;
        default:
            print_at(11, 5, "HALT FALLBACK      ");
            break;
    }
    set_attr_rect(0, 3, 32, 3, ATTR_TEXT);

    print_at(1, 10, "SPACE / FIRE   START");
    set_attr_rect(0, 10, 32, 1, ATTR_HINT);

    print_at(1, 20, "QAOP / KEMPSTON TO MOVE");
    set_attr_rect(0, 20, 32, 1, ATTR_TEXT);

    /* The hint row is where the tune's banner goes, and where it is
       cleared back to. */
    render_hint(TITLE_HINT);

    /* Row 22 is left blank on purpose — it holds the floating bus sync
       marker written by vsync_wait(). */
}

void render_play(void)
{
    draw_header("THE FIELD");
    set_page();
    draw_view();
    render_discard();       /* draw_view() already used the real colours */
    draw_status("CURSOR :", cursor_x, cursor_y);
    render_hint(PLAY_HINT);
}

void render_map(void)
{
    draw_header("CAMPAIGN MAP");
    draw_map();
    solid_map_cell(cursor_x, cursor_y, ATTR_HINT);  /* the play cursor */
    solid_map_cell(cur_x, cur_y, ATTR_CURSOR);
    draw_status("CURSOR :", cur_x, cur_y);
    render_hint("QAOP LOOK AROUND  SPACE CLOSE  ");
}

/* A level ended.  player_won says which message to show; the exit is
   handled in handle_input(), which is where the level advances. */
void render_over(void)
{
    draw_header(player_won ? "VICTORY" : "DEFEAT");

    print_at(1, 10, player_won ? "LEVEL TAKEN    :" : "LEVEL LOST     :");
    print_num(18, 10, level, 2);
    set_attr_rect(0, 10, 32, 1, ATTR_TEXT);

    render_hint(player_won ? "SPACE FOR THE NEXT LEVEL       "
                           : "SPACE TO RETURN TO THE TITLE   ");
}

void render_won(void)
{
    draw_header("CAMPAIGN COMPLETE");

    print_at(4, 9,  "EVERY LEVEL TAKEN");
    print_at(4, 11, "LEVELS WON     :");
    print_num(21, 11, LEVEL_COUNT, 2);
    set_attr_rect(0, 9, 32, 3, ATTR_TEXT);

    render_hint("PRESS A KEY                    ");
}
