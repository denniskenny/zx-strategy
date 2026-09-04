/*
 * render_screens.c -- the once-per-state screens.
 *
 * Split out of render.c and placed in the CONTENDED window.  These five
 * painters run exactly once each per state change and never inside the
 * frame budget:
 *
 *     render_title  render_map  render_over  render_won  render_cutscene
 *
 * render.c was 9,502 bytes -- 40% of all code -- in a 16K window that had
 * 132 bytes left.  What stays there composes cells and presents them,
 * which is per-frame work and must be uncontended.  A full-screen repaint
 * that happens on a keypress need not be.
 *
 * Same argument as logic.c and hw_detect.c (src/logic_org.asm).  This is a
 * separate FILE because #pragma codeseg is per translation unit: there is
 * no way to mark five functions inside one.
 *
 * render_play() stays in render.c deliberately.  It looks like a sibling
 * of these, but the walk and the enemy turn call it mid-sequence, so it is
 * not once-per-state.
 */

#pragma codeseg LOGIC

#include <stdint.h>
#include <string.h>

#include "../config/app_config.h"
#include "../config/game_config.h"
#include "../include/board.h"
#include "../include/cutscenes.h"
#include "../include/dzx0.h"
#include "../include/gfx.h"
#include "../include/hw.h"
#include "../include/render.h"
#include "../include/strings.h"
#include "../include/vsync.h"

/* render.c keeps these private; the cutscene needs both. */
#define SCREEN_0    ((uint8_t *)0x4000)
#define SCREEN_1    ((uint8_t *)0xC000)

/* Paging for the cutscene, in helpers of their own.
 *
 * Two __asm blocks with C between them in one function made SDCC report
 * a syntax error on the comment that followed -- the same class of thing
 * as .claude/skills/zx-memory § zcc will not evaluate #if inside a
 * function containing __asm.  One asm block per function, always. */
static volatile uint8_t cs_bank;

static void cs_page_in(void)
{
    __asm
        di
        ld  a, (_page_reg)
        and #0xF8
        ld  hl, #_cs_bank
        or  (hl)                ; only the bank bits change
        ld  bc, #0x7FFD
        out (c), a
        ld  (0x5B5C), a
    __endasm;
}

static void cs_page_out(void)
{
    __asm
        ld  bc, #0x7FFD
        ld  a, (_page_reg)      ; page 7 back, as the renderer expects
        out (c), a
        ld  (0x5B5C), a
        ei
    __endasm;
}

/* A level's cutscene screen, out of its bank and onto the display.
 *
 * Paging evicts everything at 0xC000 -- the shadow screen and every
 * buffer.  Survivable here and nowhere else: no board is being drawn,
 * the decompressor's code is at 0x8000, its workspace is on the stack
 * below, and the destination is the screen at 0x4000.  Interrupts are
 * off across the window.
 *
 * The picture goes into BOTH screens.  A 128K flips between them, and
 * anything that presents afterwards -- render_show(), a busy banner, the
 * first frame of ST_PLAY -- swaps the display to the other one.  Drawing
 * into only one meant the first flip revealed a screen the cutscene had
 * never touched.
 *
 * 128K only; game.c skips the state on a 48K, which has no bank. */
void render_cutscene(uint8_t idx)
{
    uint16_t src;

    if (idx >= CUTSCENE_COUNT) idx = CUTSCENE_COUNT - 1;
    cs_bank = cutscene_bank[idx];
    src = (uint16_t)(0xC000 + cutscene_off[idx]);

    cs_page_in();
    dzx0_decompress((const uint8_t *)src, (uint8_t *)0x4000);
    cs_page_out();

    if (shadow_ok) {
        memcpy(SCREEN_1, SCREEN_0, 6912);
        gfx_target(SCREEN_0);
    }
}

void render_title(void)
{
    render_compose();
    draw_header(TXT_ZX_STRATEGY);

    print_at(1, 3, TXT_MACHINE);
    print_at(11, 3, is_128k ? TXT_T_128K : TXT_T_48K);

    print_at(1, 4, TXT_KEMPSTON);
    print_at(11, 4, has_kempston ? TXT_YES : TXT_NO);

    print_at(1, 5, TXT_VSYNC);
    switch (vsync_mode) {
        /* The two bus lines differ by four characters, so they share a
           prefix rather than carrying it twice. */
        case VSYNC_MODE_48K:
        case VSYNC_MODE_128K:
            print_at(11, 5, TXT_FLOATING_BUS_0X);
            print_at(26, 5, vsync_mode == VSYNC_MODE_48K ? TXT_T_40FF : TXT_T_0FFD);
            break;
        default:
            print_at(11, 5, TXT_HALT_FALLBACK);
            break;
    }
    /* Whether the shadow screen is actually in use.  On the title
       screen because it is the only place a tester without a debugger
       can see it, and because "is the 128K path live?" has been the
       hardest question to answer all the way through this. */
    print_at(1, 6, TXT_SCREEN);
    print_at(11, 6, shadow_ok ? TXT_DOUBLE : TXT_SINGLE);

    set_attr_rect(0, 3, 32, 4, ATTR_TEXT);

    print_at(1, 10, TXT_SPACE_FIRE_1_START);
    set_attr_rect(0, 10, 32, 1, ATTR_HINT);

    /* The whole control scheme, in the two lines it takes.  Every screen
       obeys it, so this is the only place it needs saying. */
    print_at(1, 19, TXT_QAOP_KEMPSTON_MOVE);
    print_at(1, 20, TXT_SPACE_FIRE_1_DO);
    print_at(1, 21, TXT_ENTER_FIRE_2_BACK);
    set_attr_rect(0, 20, 32, 1, ATTR_TEXT);

    /* The hint row is where the tune's banner goes, and where it is
       cleared back to. */
    render_hint(TITLE_HINT);

    /* Row 22 is left blank on purpose — it holds the floating bus sync
       marker written by vsync_wait(). */
    render_show();
}

/* render_map() stayed in render.c: moving all five put SECTION LOGIC 130
   bytes past the end of the contended window.  It is the cheapest one to
   leave behind -- everything it calls (draw_map, solid_map_cell) is in
   render.c anyway, so it is the only one that gains nothing from moving
   except the bytes themselves. */

/* A level ended.  player_won says which message to show; the exit is
   handled in handle_input(), which is where the level advances. */
void render_over(void)
{
    render_compose();
    draw_header(player_won ? TXT_VICTORY : TXT_DEFEAT);

    print_at(1, 10, player_won ? TXT_LEVEL_TAKEN : TXT_LEVEL_LOST);
    print_num(18, 10, level, 2);
    print_at(1, 11, TXT_TURNS_TAKEN);
    print_num(18, 11, (uint8_t)(turn > 99 ? 99 : turn), 2);

    /* Score is SCORE_PAR less the turns it took, so a quick win scores
       high and a long one scores nothing -- never negative, because a
       level that took longer than par is worth zero rather than a debt.
       Only on a win: there is no score for losing. */
    if (player_won) {
        print_at(1, 12, TXT_LEVEL_SCORE);
        print_num(18, 12, level_score(), 2);
        print_at(1, 13, TXT_TOTAL_SCORE);
        print_num(17, 13,
                  (uint8_t)(campaign_score > 999 ? 999 : campaign_score), 3);
    }
    set_attr_rect(0, 10, 32, 4, ATTR_TEXT);

    render_hint(player_won ? TXT_SPACE_FOR_THE_NEXT_LEVEL
                           : TXT_SPACE_TO_RETURN_TO_THE_TITLE);
    render_show();
}

/* render_won() stayed in render.c too: with it here, SECTION LOGIC ran 49
   bytes past the contended window.  Three of the five moved; the window
   simply has no room for more, which is worth knowing before anyone tries
   to move a fourth. */
