/*
 * test_menu.c — the menu's behaviour, not its data.
 *
 * `q2psx-inspect menu <disc>` already checks the page tables against a real
 * executable, so nothing here re-asserts a coordinate. What it pins down is the
 * behaviour that was read out of the code and cannot be checked against a table:
 * which items the cursor may land on, where it wraps, when an action fires,
 * what left and right do to each kind of widget, and what the GAME VARIABLES
 * page actually changes.
 */
#include "menu.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* One frame with `pad` held, then one with nothing, so a press-and-release
 * lands whichever way an item wants it. */
static void tap(q2_menu *m, u16 pad)
{
    q2_menu_advance(m, pad);
    q2_menu_advance(m, 0);
}

static void open_menu(q2_menu *m, q2_menu_settings *s, bool multiplayer)
{
    q2_menu_init(m, s, Q2_MENU_SCREEN_H);
    q2_menu_set_multiplayer(m, multiplayer);
    q2_menu_open(m);
    q2_menu_advance(m, 0);   /* burn the settle frame */
    (void)q2_menu_take_request(m);
}

/* ------------------------------------------------------------------------- */
static void test_defaults(void)
{
    q2_menu_settings s;

    q2_menu_settings_defaults(&s);

    /* 0x8001FA50 */
    CHECK(s.v[Q2_SET_MUSIC] == 48, "music default %d", s.v[Q2_SET_MUSIC]);
    CHECK(s.v[Q2_SET_SFX] == 96, "sfx default %d", s.v[Q2_SET_SFX]);
    CHECK(s.v[Q2_SET_STEREO] == 1, "stereo default %d", s.v[Q2_SET_STEREO]);
    /* 0x8001FA18 */
    CHECK(s.v[Q2_SET_SCREEN_Y] == 24, "screen y default %d", s.v[Q2_SET_SCREEN_Y]);
    CHECK(s.v[Q2_SET_HORIZONTAL_SPLIT] == 1, "split default");
    /* 0x8002048C */
    CHECK(s.v[Q2_SET_GRAVITY] == 64, "gravity default %d", s.v[Q2_SET_GRAVITY]);
    CHECK(s.v[Q2_SET_GAME_SPEED] == 64, "game speed default");
    CHECK(s.v[Q2_SET_BLAST_FORCE] == 64, "blast force default");
    CHECK(s.v[Q2_SET_FALLING_DAMAGE] == 1, "falling damage default");
    CHECK(s.v[Q2_SET_ONE_SHOT_KILL] == 0, "one shot kill default");
    /* 0x8001BDA8 */
    CHECK(s.v[Q2_SET_CROSSHAIR] == 1, "crosshair default");
    CHECK(s.v[Q2_SET_PAD_STYLE] == 6, "pad style default %d", s.v[Q2_SET_PAD_STYLE]);
    CHECK(strcmp(q2_menu_pad_style_name(6), "STANDARD A") == 0,
          "style 6 is %s", q2_menu_pad_style_name(6));
}

/*
 * 0x8001C698 — the defaults have to come out as the constants the game used
 * before the menu existed, or enabling the page would change the physics of a
 * session that never touched it.
 */
static void test_variables_apply(void)
{
    q2_menu_settings s;
    q2_menu_rules r;

    q2_menu_settings_defaults(&s);

    q2_menu_apply_variables(&s, true, 50, &r);
    CHECK(r.gravity == 32, "default gravity maps to %d, not 32", r.gravity);
    CHECK(r.tick_rate == 50, "default game speed maps to %d, not 50", r.tick_rate);
    CHECK(r.cheats == 0, "default cheats %04X", r.cheats);

    q2_menu_apply_variables(&s, false, 50, &r);
    CHECK(r.gravity == 32 && r.tick_rate == 50 && r.cheats == 0,
          "the disabled path must match the constants");

    /* Falling damage off is a cheat *bit*, the others are set when on. */
    s.v[Q2_SET_FALLING_DAMAGE] = 0;
    s.v[Q2_SET_INFINITE_AMMO]  = 1;
    s.v[Q2_SET_ALL_WEAPONS]    = 1;
    s.v[Q2_SET_ONE_SHOT_KILL]  = 1;
    q2_menu_apply_variables(&s, true, 50, &r);
    CHECK(r.cheats == (Q2_CHEAT_NO_FALL_DAMAGE | Q2_CHEAT_INFINITE_AMMO |
                       Q2_CHEAT_ALL_WEAPONS | Q2_CHEAT_ONE_SHOT_KILL),
          "cheat mask %04X", r.cheats);

    s.v[Q2_SET_GRAVITY]    = 0;
    s.v[Q2_SET_GAME_SPEED] = 127;
    q2_menu_apply_variables(&s, true, 50, &r);
    CHECK(r.gravity == 16, "minimum gravity %d", r.gravity);
    CHECK(r.tick_rate == (50 * 191) >> 7, "maximum speed %d", r.tick_rate);
}

/* ------------------------------------------------------------------------- */
static void test_navigation(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    CHECK(m.page_id == Q2_PAGE_PAUSE_SP, "single player opens page %d", m.page_id);
    CHECK(m.page->count == 5,
          "the single-player pause menu keeps %u items, not 5", m.page->count);
    CHECK(m.cursor == 0, "cursor starts at %d", m.cursor);

    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 1, "down -> %d", m.cursor);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 4, "four downs -> %d", m.cursor);

    /* 0x80019E8C: past the end wraps to the first selectable item. */
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 0, "wrap down -> %d", m.cursor);
    tap(&m, Q2_PAD_UP);
    CHECK(m.cursor == 4, "wrap up -> %d", m.cursor);
}

/* The multiplayer pause menu is a different table with a different graph. */
static void test_multiplayer_pause(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, true);

    CHECK(m.page_id == Q2_PAGE_PAUSE_MP, "multiplayer opens page %d", m.page_id);
    CHECK(m.page->count == 6, "%u items", m.page->count);
    CHECK(strcmp(m.page->items[3].label, "GAME VARIABLES") == 0,
          "item 3 is %s", m.page->items[3].label);
}

/*
 * 0x80019CD4 — a label beginning 'g' is greyed *and* skipped. The death screen
 * is where that matters: with no resupplies left the middle line is unreachable
 * and the cursor steps straight over it.
 */
static void test_grey_is_skipped(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_set_resupplies(&m, 0);
    q2_menu_goto(&m, Q2_PAGE_DEATH);
    m.arm_ticks = 0;              /* the 600-tick countdown at 0x800205B0 */

    CHECK(!q2_menu_item_selectable(&m, 1),
          "the greyed resupply line must not be selectable");
    CHECK(m.cursor == 0, "cursor starts at %d", m.cursor);
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 2, "down from 0 skips to %d, not 2", m.cursor);

    /* With resupplies in hand the same line is live. A fresh menu, because the
     * engine remembers the cursor per page (0x8001A3B0) and would otherwise
     * restore the one the first half left behind. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_set_resupplies(&m, 3);
    q2_menu_goto(&m, Q2_PAGE_DEATH);
    m.arm_ticks = 0;
    CHECK(q2_menu_item_selectable(&m, 1), "an affordable resupply is selectable");
    CHECK(strcmp(q2_menu_item_text(&m, 1), "RESUPPLY AND RESTART (3 LEFT)") == 0,
          "runtime label is \"%s\"", q2_menu_item_text(&m, 1));
    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 1, "down from 0 -> %d", m.cursor);
}

/* The death screen ignores the pad until its countdown expires. */
static void test_death_is_inert_at_first(void)
{
    q2_menu_settings s;
    q2_menu m;
    int i;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_set_resupplies(&m, 1);
    q2_menu_goto(&m, Q2_PAGE_DEATH);

    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 0, "the screen moved while still inert");

    for (i = 0; i < 700; i++)
        q2_menu_advance(&m, 0);

    tap(&m, Q2_PAD_DOWN);
    CHECK(m.cursor == 1, "once armed, down -> %d", m.cursor);
}

/* ------------------------------------------------------------------------- */
/* 0x8001B720: left means ON because the row reads "LABEL  ON  OFF". */
static void test_toggle(void)
{
    q2_menu_settings s;
    q2_menu m;
    char line[80];

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_PLAYER);

    CHECK(m.cursor == 0, "cursor %d", m.cursor);
    s.v[Q2_SET_CROSSHAIR] = 0;

    tap(&m, Q2_PAD_LEFT);
    CHECK(s.v[Q2_SET_CROSSHAIR] == 1, "left must select ON");

    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_CROSSHAIR] == 0, "right must select OFF");

    /* The composed line is what the screen shows, codes and all. */
    s.v[Q2_SET_CROSSHAIR] = 1;
    q2_menu_item_display(&m, 0, line, (u32)sizeof(line));
    CHECK(strcmp(line, "bCROSSHAIR bON dOFF") == 0, "selected line \"%s\"", line);

    m.cursor = 1;
    q2_menu_item_display(&m, 0, line, (u32)sizeof(line));
    CHECK(strcmp(line, "CROSSHAIR ON") == 0, "unselected line \"%s\"", line);
}

/* 0x8001C018: two units a frame while held, clamped to 0..127. */
static void test_slider(void)
{
    q2_menu_settings s;
    q2_menu m;
    int i;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_SOUND);

    CHECK(m.page->items[0].widget == Q2_WIDGET_SLIDER, "item 0 is not a slider");
    CHECK(s.v[Q2_SET_MUSIC] == 48, "music starts at %d", s.v[Q2_SET_MUSIC]);

    q2_menu_advance(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_MUSIC] == 50, "one held frame -> %d", s.v[Q2_SET_MUSIC]);
    q2_menu_advance(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_MUSIC] == 52, "two held frames -> %d", s.v[Q2_SET_MUSIC]);

    for (i = 0; i < 200; i++)
        q2_menu_advance(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_MUSIC] == Q2_MENU_SLIDER_MAX,
          "clamps high at %d", s.v[Q2_SET_MUSIC]);

    for (i = 0; i < 200; i++)
        q2_menu_advance(&m, Q2_PAD_LEFT);
    CHECK(s.v[Q2_SET_MUSIC] == 0, "clamps low at %d", s.v[Q2_SET_MUSIC]);
}

/* 0x8001C944: the controller style wraps inside its class's three names. */
static void test_choice(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_CONTROLLER);

    CHECK(m.page->items[0].widget == Q2_WIDGET_CHOICE, "item 0 is not a choice");
    CHECK(s.v[Q2_SET_PAD_STYLE] == 6, "style starts at %d", s.v[Q2_SET_PAD_STYLE]);

    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_PAD_STYLE] == 7, "right -> %d", s.v[Q2_SET_PAD_STYLE]);
    tap(&m, Q2_PAD_RIGHT);
    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_PAD_STYLE] == 6, "wraps back to %d", s.v[Q2_SET_PAD_STYLE]);
    tap(&m, Q2_PAD_LEFT);
    CHECK(s.v[Q2_SET_PAD_STYLE] == 8, "left wraps to %d", s.v[Q2_SET_PAD_STYLE]);

    /* A digital pad has no sticks, so those two rows are out (0x8001CA28). */
    CHECK(!q2_menu_item_selectable(&m, 2), "SWAP Y AXIS should be disabled");
    CHECK(!q2_menu_item_selectable(&m, 3), "USE MOUSE should be disabled");
}

/* ------------------------------------------------------------------------- */
/* 0x8001A0D8: the pause items fire on release, the options items on press. */
static void test_press_versus_release(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    /* RETURN TO GAME is an on-release item: holding must not fire it. */
    q2_menu_advance(&m, Q2_PAD_CROSS);
    CHECK(m.open, "an on-release item fired on the press");
    q2_menu_advance(&m, 0);
    CHECK(!m.open, "an on-release item did not fire on the release");
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_RESUME, "resume was not requested");

    /* RESET TO DEFAULTS on the sound page is an on-press item. */
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_SOUND);
    m.cursor = 3;
    s.v[Q2_SET_MUSIC] = 5;
    q2_menu_advance(&m, Q2_PAD_CROSS);
    CHECK(s.v[Q2_SET_MUSIC] == 48, "the press did not reset (%d)", s.v[Q2_SET_MUSIC]);
}

/* The page graph: down into the options tree and back out again. */
static void test_page_graph(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    m.cursor = 2;                       /* OPTIONS */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_OPTIONS, "cross on OPTIONS -> page %d", m.page_id);

    tap(&m, Q2_PAD_CROSS);              /* PLAYER OPTIONS */
    CHECK(m.page_id == Q2_PAGE_PLAYER, "-> page %d", m.page_id);

    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_OPTIONS, "triangle -> page %d", m.page_id);
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_PAUSE_SP, "triangle -> page %d", m.page_id);

    /* The root has no parent, so triangle there does nothing. */
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.page_id == Q2_PAGE_PAUSE_SP && m.open,
          "triangle at the root left page %d", m.page_id);

    /* The cursor is remembered per page (0x8001A3B0). */
    m.cursor = 2;
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_OPTIONS, "back into OPTIONS");
    tap(&m, Q2_PAD_TRIANGLE);
    CHECK(m.cursor == 2, "the pause cursor came back as %d", m.cursor);
}

static void test_quit_flow(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    m.cursor = 4;                        /* QUIT GAME */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_QUIT_CONFIRM, "-> page %d", m.page_id);
    CHECK(m.cursor == 1, "the confirmation starts on %s",
          q2_menu_item_text(&m, m.cursor));
    CHECK(strcmp(q2_menu_item_text(&m, m.cursor), "NO") == 0,
          "the confirmation must start on NO");

    /* NO goes back where it came from. */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_PAUSE_SP, "NO -> page %d", m.page_id);

    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_QUIT_CONFIRM, "-> page %d", m.page_id);
    tap(&m, Q2_PAD_DOWN);                /* NO -> YES, which is drawn above */
    CHECK(strcmp(q2_menu_item_text(&m, m.cursor), "YES") == 0,
          "down from NO must reach YES");
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_QUITTING, "YES -> page %d", m.page_id);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_QUIT, "quit was not requested");
}

static void test_restart_flow(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    open_menu(&m, &s, false);

    m.cursor = 3;                        /* RESTART LEVEL */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_RESTART_CONFIRM, "-> page %d", m.page_id);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_RESTARTING, "YES -> page %d", m.page_id);
    CHECK(q2_menu_take_request(&m) == Q2_MREQ_RESTART, "restart was not requested");
}

/* A text-only page has nothing to land on and swallows the pad. */
static void test_text_only_page(void)
{
    q2_menu_settings s;
    q2_menu m;
    const q2_menu_page *p;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_RESTARTING);

    p = m.page;
    CHECK(p->first == p->count, "a text page must have no navigable range");
    CHECK(!q2_menu_item_selectable(&m, 0), "item 0 must not be selectable");

    (void)q2_menu_take_request(&m);
    tap(&m, Q2_PAD_DOWN);
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_RESTARTING, "the page moved to %d", m.page_id);
}

/* The SCREEN POSITION page has no items: the d-pad moves the display. */
static void test_screen_position(void)
{
    q2_menu_settings s;
    q2_menu m;

    q2_menu_settings_defaults(&s);
    q2_menu_init(&m, &s, Q2_MENU_SCREEN_H);
    m.open = true;
    q2_menu_goto(&m, Q2_PAGE_VIDEO);
    m.cursor = 0;                        /* SCREEN POSITION */
    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_SCREEN_POSITION, "-> page %d", m.page_id);

    tap(&m, Q2_PAD_RIGHT);
    CHECK(s.v[Q2_SET_SCREEN_X] == 1, "right -> %d", s.v[Q2_SET_SCREEN_X]);
    tap(&m, Q2_PAD_UP);
    CHECK(s.v[Q2_SET_SCREEN_Y] == 23, "up -> %d", s.v[Q2_SET_SCREEN_Y]);

    tap(&m, Q2_PAD_CROSS);
    CHECK(m.page_id == Q2_PAGE_VIDEO, "cross leaves to page %d", m.page_id);
}

/* ------------------------------------------------------------------------- */
/* 0x8001D510: the number of variables you get is how many cheats you have. */
static void test_variables_pages(void)
{
    static const u8 want[4] = { 3, 5, 7, 9 };
    int lvl;

    for (lvl = 0; lvl < 4; lvl++) {
        const q2_menu_page *p = q2_menu_variables_page(lvl);
        CHECK(p->count == want[lvl], "cheat level %d gives %u items, not %u",
              lvl, p->count, want[lvl]);
    }

    CHECK(strcmp(q2_menu_cheat_level_name(0), "NONE") == 0, "level 0 name");
    CHECK(strcmp(q2_menu_cheat_level_name(3), "GOLD") == 0, "level 3 name");
}

/* The VIDEO page only offers HORIZONTAL SPLIT in a multiplayer session. */
static void test_video_variant(void)
{
    const q2_menu_page *sp = q2_menu_video_page(false);
    const q2_menu_page *mp = q2_menu_video_page(true);

    CHECK(sp->count == 2, "single player video has %u items", sp->count);
    CHECK(mp->count == 3, "multiplayer video has %u items", mp->count);
    CHECK(strcmp(mp->items[0].label, "HORIZONTAL SPLIT") == 0,
          "multiplayer item 0 is %s", mp->items[0].label);
}

/* 0x8001FD18 counts everything but the four control letters. */
static void test_text_length(void)
{
    CHECK(q2_menu_text_length("bON") == 2, "codes must not count");
    CHECK(q2_menu_text_length("gRESUPPLY") == 8, "grey prefix must not count");
    CHECK(q2_menu_text_length("QUIT GAME") == 9, "plain text");
    CHECK(q2_menu_is_code('b') && q2_menu_is_code('d') &&
          q2_menu_is_code('g') && q2_menu_is_code('u'), "the four codes");
    CHECK(!q2_menu_is_code('a') && !q2_menu_is_code('G'), "nothing else is one");
}

/* 0x8001CF74 on a PAL framebuffer. */
static void test_title_y(void)
{
    CHECK(q2_menu_title_y(248) == 40, "PAL title y is %d", q2_menu_title_y(248));
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    test_defaults();
    test_variables_apply();
    test_navigation();
    test_multiplayer_pause();
    test_grey_is_skipped();
    test_death_is_inert_at_first();
    test_toggle();
    test_slider();
    test_choice();
    test_press_versus_release();
    test_page_graph();
    test_quit_flow();
    test_restart_flow();
    test_text_only_page();
    test_screen_position();
    test_variables_pages();
    test_video_variant();
    test_text_length();
    test_title_y();

    if (g_fail) {
        printf("\n%d menu check%s failed\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("menu: all checks passed\n");
    return 0;
}
