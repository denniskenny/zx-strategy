/*
 * demo.c — Floating bus vsync demonstration
 *
 * Shows what vsync_wait() buys you:
 *
 *   - A status panel reporting the detected machine and which of the
 *     three vsync modes is active.
 *   - A fast horizontally moving vertical bar.  With sync ON the bar
 *     is drawn entirely during the border/vblank window, so it stays
 *     solid; with sync OFF (press S) the redraw races the beam and the
 *     bar visibly tears/flickers.
 *   - Border timing bars: the border turns RED while the frame's work
 *     is being done and BLACK while waiting for the beam, so the size
 *     of the red band on the border is the CPU budget actually used.
 *
 * Controls:  O / P  bar speed        S  toggle sync   R  reset counter
 *            G  show the ZX0-compressed Great Old One graphic
 *            M  play the Tritone tune (blocks; any key returns)
 *            Kempston: left/right speed, fire 1 sync, fire 2 reset
 */

#include "../config/app_config.h"
#include "../include/demo.h"
#include "../include/dzx0.h"
#include "../include/gfx.h"
#include "../include/goo_data.h"
#include "../include/hw.h"
#include "../include/input.h"
#include "../include/music.h"
#include "../include/vsync.h"

/* --- Layout --- */
#define ATTR_TITLE  0x45    /* bright cyan ink, black paper  */
#define ATTR_TEXT   0x47    /* bright white ink, black paper */
#define ATTR_BAR    0x46    /* bright yellow ink, black paper */
#define ATTR_BG     0x07    /* white ink, black paper        */

/* Demo action bits — keyboard and Kempston are folded into one byte so a
   single edge test debounces both. */
#define ACT_FASTER  0x01
#define ACT_SLOWER  0x02
#define ACT_SYNC    0x04
#define ACT_RESET   0x08
#define ACT_MUSIC   0x10
#define ACT_GOO     0x20

/* Keyboard half-rows used for the actions that aren't in input.h.
   Deliberately NOT the CAPS SHIFT row (0xFEFE) — S/R/M are mnemonic and
   can't be confused with a shifted key. */
#define KEY_ASDFG_ROW 0xFDFE    /* S = bit 1, G = bit 4 */
#define KEY_QWERT_ROW 0xFBFE    /* R = bit 3 */
#define KEY_BNMSS_ROW 0x7FFE    /* M = bit 2 */

/* Staging buffer for decompressed asset data.  Low RAM above the screen is
   free: code and data start at 0x8000 (-zorg=32768). */
#define SCRATCH_BUF ((uint8_t *)0x6000)

#define GOO_ATTR    0x44        /* bright green ink, black paper */

#define BAND_ROW    9       /* character row where the bar band starts */
#define BAND_ROWS   9
#define BAND_TOP    (BAND_ROW * 8)
#define BAND_BOT    (BAND_TOP + BAND_ROWS * 8)

static uint8_t bar_col;
static uint8_t bar_prev;
static uint8_t bar_speed;
static uint8_t bar_dir;
static uint8_t sync_on;
static uint16_t frame;

static uint8_t scan_actions(void)
{
    uint8_t k = scan_input();
    uint8_t a = 0;

    if (k & INPUT_RIGHT) a |= ACT_FASTER;
    if (k & INPUT_LEFT)  a |= ACT_SLOWER;
    if (k & INPUT_FIRE1) a |= ACT_SYNC;    /* Z / Kempston fire 1 */
    if (k & INPUT_FIRE2) a |= ACT_RESET;   /* X / Kempston fire 2 */

    if (!(read_keys(KEY_ASDFG_ROW) & 0x02)) a |= ACT_SYNC;    /* S */
    if (!(read_keys(KEY_ASDFG_ROW) & 0x10)) a |= ACT_GOO;     /* G */
    if (!(read_keys(KEY_QWERT_ROW) & 0x08)) a |= ACT_RESET;   /* R */
    if (!(read_keys(KEY_BNMSS_ROW) & 0x04)) a |= ACT_MUSIC;   /* M */

    return a;
}

/* The eight keyboard half-rows, in the usual port order. */
static const uint16_t key_rows[8] = {
    0xFEFE, 0xFDFE, 0xFBFE, 0xF7FE, 0xEFFE, 0xDFFE, 0xBFFE, 0x7FFE
};

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

static void wait_key(void)
{
    uint8_t idle = 0, down = 0;

    /* Sample once per frame rather than in a tight loop: hammering the
       ULA port thousands of times per frame picks up bus noise (and
       keeps the floating bus marker refreshed).  Two consecutive
       samples in each state debounce the release and the next press. */
    while (idle < 2) {
        vsync_wait();
        idle = any_key() ? 0 : (uint8_t)(idle + 1);
    }
    while (down < 2) {
        vsync_wait();
        down = any_key() ? (uint8_t)(down + 1) : 0;
    }
}

/* Decompress the ZX0'd Great Old One into low RAM and blit the cropped
   region back to its original screen position. */
static void show_goo(void)
{
    screen_clear(0x00);

    dzx0_decompress(goo_final, SCRATCH_BUF);
    write_blit(GOO_CROP_COL, GOO_CROP_ROW, SCRATCH_BUF,
               GOO_CROP_W, GOO_CROP_H);

    set_attr_rect(GOO_CROP_COL, GOO_CROP_ROW >> 3,
                  GOO_CROP_W, (GOO_CROP_H + 7) >> 3, GOO_ATTR);

    print_at(3, 23, "THE GREAT OLD ONE - ANY KEY");
    set_attr_rect(0, 23, 32, 1, ATTR_TEXT);

    wait_key();
}

static void draw_band_col(uint8_t col, uint8_t val)
{
    uint8_t y;
    uint8_t x = col << 3;
    for (y = BAND_TOP; y < BAND_BOT; y++)
        SCREEN[scr_off(x, y)] = val;
}

static void print_num(uint8_t col, uint8_t row, uint16_t v, uint8_t digits)
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

static void draw_static_screen(void)
{
    screen_clear(ATTR_BG);

    print_at(3, 0, "ZX MAP - FLOATING BUS DEMO");
    set_attr_rect(0, 0, 32, 1, ATTR_TITLE);

    print_at(1, 2, "MACHINE :");
    print_at(11, 2, is_128k ? "128K" : "48K");

    print_at(1, 3, "KEMPSTON:");
    print_at(11, 3, has_kempston ? "YES" : "NO");

    print_at(1, 4, "VSYNC   :");
    switch (vsync_mode) {
        case VSYNC_MODE_48K:
            print_at(11, 4, "FLOATING BUS 0X40FF");
            break;
        case VSYNC_MODE_128K:
            print_at(11, 4, "FLOATING BUS 0X0FFD");
            break;
        default:
            print_at(11, 4, "HALT FALLBACK      ");
            break;
    }

    print_at(1, 5, "FRAME   :");
    print_at(1, 6, "SYNC    :");
    print_at(1, 7, "SPEED   :");
    set_attr_rect(0, 2, 32, 6, ATTR_TEXT);

    set_attr_rect(0, BAND_ROW, 32, BAND_ROWS, ATTR_BAR);

    print_at(1, 19, "O/P SPEED  S SYNC  R RESET");
    print_at(1, 20, "G GREAT OLD ONE   M MUSIC");
    print_at(1, 21, "RED BORDER = CPU WORK TIME");
    set_attr_rect(0, 19, 32, 3, ATTR_TEXT);

    /* Row 22 is left blank on purpose — it holds the floating bus
       sync marker written by vsync_wait(). */
}

void demo_run(void)
{
    uint8_t acts, last_acts = 0xFF, stable, prev_stable = 0xFF, edge;

    bar_col = 0;
    bar_prev = 0;
    bar_speed = 2;
    bar_dir = 1;
    sync_on = 1;
    frame = 0;

    draw_static_screen();

    for (;;) {
        /* ---- Sync to the beam --------------------------------------
           After this returns the beam is at attribute row 22, so the
           bottom border + vblank + top border (~28 000 T-states on a
           48K) are free for tear-free updates. */
        if (sync_on) vsync_wait();

        border(2);          /* RED: start of the frame's work */

        /* ---- Move + redraw the tear-test bar ---- */
        bar_prev = bar_col;
        if (bar_dir) {
            bar_col += bar_speed;
            if (bar_col >= 31) { bar_col = 31; bar_dir = 0; }
        } else {
            if (bar_col <= bar_speed) { bar_col = 0; bar_dir = 1; }
            else bar_col -= bar_speed;
        }
        draw_band_col(bar_prev, 0x00);
        draw_band_col(bar_col, 0xFF);

        /* ---- Status panel ---- */
        frame++;
        print_num(11, 5, frame, 5);
        print_at(11, 6, sync_on ? "ON " : "OFF");
        print_num(11, 7, bar_speed, 1);

        border(0);          /* BLACK: work done, idle from here */

        /* ---- Input ----
           An action counts only when the same bit is seen in two
           consecutive frames, then fires on its rising edge: held keys
           act once, and a single noisy read is ignored. */
        acts = scan_actions();
        stable = (uint8_t)(acts & last_acts);
        last_acts = acts;
        edge = (uint8_t)(stable & ~prev_stable);
        prev_stable = stable;

        if ((edge & ACT_FASTER) && bar_speed < 8) bar_speed++;
        if ((edge & ACT_SLOWER) && bar_speed > 1) bar_speed--;
        if (edge & ACT_SYNC) sync_on = (uint8_t)(sync_on ^ 1);
        if (edge & ACT_RESET) frame = 0;

        if (edge & ACT_GOO) {
            show_goo();
            draw_static_screen();
            last_acts = prev_stable = 0xFF;
        }

        if (edge & ACT_MUSIC) {
            /* Blocking: the Tritone player owns the speaker and returns on
               the next key/joystick press. */
            screen_clear(ATTR_TEXT);
            print_at(4, 11, "PLAYING - PRESS A KEY");
            lowlands_play();
            draw_static_screen();
            last_acts = prev_stable = 0xFF;  /* swallow the stopping key */
        }
    }
}
