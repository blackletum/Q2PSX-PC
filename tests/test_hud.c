/*
 * test_hud.c — the overlay's layout rules and its markup interpreter.
 *
 * These run without a disc. The glyph and icon tables come off the executable
 * at load time and cannot be checked here, so the tests build a synthetic table
 * with known coordinates and assert what the interpreter DOES with it: where
 * the pen lands, which sprite is emitted, how spaces differ from glyphs, when a
 * line breaks, and how the message ring ages.
 *
 * Three of the assertions are deliberately locking in the original's mistakes —
 * the off-by-one measure, `\r` behaving as `\n`, and the armour flash getting
 * weaker as the hit gets bigger until it wraps. A future tidy-up that "fixes"
 * any of them will fail here, which is the point.
 */
#include <stdio.h>
#include <string.h>

#include "hud.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_i(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* A font whose tables are known, so positions can be asserted exactly.       */
static q2_hud_tables g_tab;
static q2_hud_font   g_font;

static void build_font(void)
{
    int i;

    memset(&g_tab, 0, sizeof(g_tab));
    memset(&g_font, 0, sizeof(g_font));

    /* One distinguishable glyph per character: u = index, v = 0x80. */
    for (i = 0; i < Q2_HUD_GLYPH_COUNT; i++) {
        g_tab.glyph[i].u = (u8)i;
        g_tab.glyph[i].v = 0x80;
    }
    /* Icon 2 ("Blaster") is 56 wide in the real table; keep that so the
     * icon-advance assertion means something. */
    for (i = 0; i < Q2_HUD_ICON_COUNT; i++) {
        g_tab.icon[i].u = (u8)(i * 8);
        g_tab.icon[i].v = 0x98;
        g_tab.icon[i].w = 8;
        g_tab.icon[i].h = 8;
    }
    g_tab.icon[2].w = 56;

    for (i = 0; i < Q2_HUD_BOX_LEVELS; i++) {
        g_tab.box[i].u = (u8)(0x70 + i * 0x10);
        g_tab.box[i].v = 0xF0;
        g_tab.box_rgb[i][0] = 0x78;
        g_tab.box_rgb[i][1] = 0x78;
        g_tab.box_rgb[i][2] = 0x78;
    }

    strcpy(g_tab.weapon_glyph[0], "  ");
    strcpy(g_tab.weapon_glyph[1], "&B");
    strcpy(g_tab.weapon_glyph[8], "&O");
    strcpy(g_tab.weapon_name[1], "Blaster G");
    strcpy(g_tab.weapon_name[8], "RockLaunch G");

    g_tab.message_lines[1] = 4;
    g_tab.message_lines[2] = 2;

    g_tab.palette[Q2_HUD_PALETTE_FONT].present     = true;
    g_tab.palette[Q2_HUD_PALETTE_FONT].clut_id     = 0x1111;
    g_tab.palette[Q2_HUD_PALETTE_FONT_ALT].present = true;
    g_tab.palette[Q2_HUD_PALETTE_FONT_ALT].clut_id = 0x2222;
    g_tab.palette[Q2_HUD_PALETTE_BOX].present      = true;
    g_tab.palette[Q2_HUD_PALETTE_BOX].clut_id      = 0x3333;
    g_tab.palette_count = Q2_HUD_PALETTE_MAX;

    g_font.tab       = &g_tab;
    g_font.tpage     = 0x0010;
    g_font.clut_font = 0x1111;
    g_font.clut_alt  = 0x2222;
    g_font.clut_box  = 0x3333;
    g_font.resident  = true;
}

/* ------------------------------------------------------------------------- */
static void test_escapes(void)
{
    psx_ot ot;
    q2_hud_ctx ctx;
    q2_hud_pen pen;

    printf("markup escapes\n");

    check(psx_ot_init(&ot, 4, 512) == Q2_OK, "ot init");
    q2_hud_ctx_default(&ctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
    q2_hud_pen_default(&pen);

    /* Shipped defaults, read out of the data image at 0x800AE8DC. */
    check_eq_i(pen.space_advance, 4,  "default space advance");
    check_eq_i(pen.glyph_advance, 8,  "glyph advance is a constant 8");
    check_eq_i(pen.palette, Q2_HUD_PALETTE_FONT_ALT, "default palette is 74");
    check(!pen.wrap, "word wrap starts off");

    /* @XXXYY: three hex digits of x, two of y. */
    psx_ot_clear(&ot);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@1A0F0");
    check_eq_i(pen.x, 0x1A0, "@ sets x from three hex digits");
    check_eq_i(pen.y, 0xF0,  "@ sets y from two hex digits");
    check_eq_i(ot.prim_count, 0, "@1A0F0 is five digits and draws nothing");

    psx_ot_clear(&ot);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@1A0F0A");
    check_eq_i(ot.prim_count, 1, "the sixth character is a glyph, not a digit");
    check_eq_i(pen.x, 0x1A0 + 8, "and it advances the pen past the position");

    /* ^RRGGBB into the context colour. */
    psx_ot_clear(&ot);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "^C8F000");
    check_eq_i(ctx.rgb[0], 0xC8, "^ red");
    check_eq_i(ctx.rgb[1], 0xF0, "^ green");
    check_eq_i(ctx.rgb[2], 0x00, "^ blue");
    check_eq_i(ot.prim_count, 0, "^ consumes exactly six digits");

    /* |0 picks palette 72 and clears the sub-mode; anything else picks 74. */
    psx_ot_clear(&ot);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "|0");
    check_eq_i(pen.palette, Q2_HUD_PALETTE_FONT, "|0 selects palette 72");
    check_eq_i(pen.palette_sub, 0, "|0 clears the sub-mode");
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "|1");
    check_eq_i(pen.palette, Q2_HUD_PALETTE_FONT_ALT, "|1 selects palette 74");
    check_eq_i(pen.palette_sub, 2, "|1 makes spaces draw");
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "|2");
    check_eq_i(pen.palette_sub, 1, "|2 is palette 74 without the space rule");

    /* ~N, in range 1..15, else 4. */
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "~8");
    check_eq_i(pen.space_advance, 8, "~8 widens the space");
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "~0");
    check_eq_i(pen.space_advance, 4, "~0 is out of range and falls back to 4");
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "~?");
    check_eq_i(pen.space_advance, 15, "~? is the top of the range");

    /* #XXXYYY sets both margins and turns wrapping on. */
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "#06A196");
    check_eq_i(pen.left, 0x06A, "# left margin");
    check_eq_i(pen.right, 0x196, "# right margin");
    check(pen.wrap, "# with a non-zero margin enables wrapping");
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "#000000");
    check(!pen.wrap, "# with both margins zero disables wrapping");

    psx_ot_free(&ot);
}

static void test_advance(void)
{
    psx_ot ot;
    q2_hud_ctx ctx;
    q2_hud_pen pen;

    printf("pen advance\n");

    psx_ot_init(&ot, 4, 512);
    q2_hud_ctx_default(&ctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
    q2_hud_pen_default(&pen);

    /* A drawn glyph always advances 8; a space advances by ~N. */
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@00000AB CD");
    check_eq_i(ot.prim_count, 4, "four glyphs, the space draws nothing");
    check_eq_i(pen.x, 4 * 8 + 4, "four glyphs at 8 plus one space at 4");

    /* `|1` makes the space draw as a glyph, and then it advances 8 like one. */
    psx_ot_clear(&ot);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@00000|1A B");
    check_eq_i(ot.prim_count, 3, "with |1 the space is a drawn glyph");
    check_eq_i(pen.x, 3 * 8, "and advances the full 8");

    /* An icon advances by its own width, not by 8. */
    psx_ot_clear(&ot);
    q2_hud_pen_default(&pen);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@00000&B");
    check_eq_i(ot.prim_count, 1, "&B emits one icon");
    check_eq_i(pen.x, 56, "and advances by the icon's own width");

    /* Two-icon escapes emit both. */
    psx_ot_clear(&ot);
    q2_hud_pen_default(&pen);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@00000&O");
    check_eq_i(ot.prim_count, 2, "&O is Rocket plus Launcher");

    /* A letter with no arm draws nothing at all. */
    psx_ot_clear(&ot);
    q2_hud_pen_default(&pen);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@00000&I");
    check_eq_i(ot.prim_count, 0, "&I is one of the six dead arms");

    psx_ot_free(&ot);
}

static void test_newline_and_wrap(void)
{
    psx_ot ot;
    q2_hud_ctx ctx;
    q2_hud_pen pen;

    printf("newlines and word wrap\n");

    psx_ot_init(&ot, 4, 512);
    q2_hud_ctx_default(&ctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
    q2_hud_pen_default(&pen);

    /* Without wrapping, a newline homes x to 24 and drops y by 8. */
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@0400A" "A\nB");
    check_eq_i(pen.x, Q2_HUD_MSG_X + 8, "newline homes x to 24");
    check_eq_i(pen.y, 0x0A + 8, "newline drops y by one 8-pixel line");

    /*
     * `\r` is NOT a carriage return. The arm at 0x80042544 falls straight
     * through into the newline arm, so it moves down a line as well.
     */
    q2_hud_pen_default(&pen);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@0400A" "A\rB");
    check_eq_i(pen.y, 0x0A + 8, "\\r falls through into the newline arm");

    /* With wrapping on, a word that will not fit moves down before it starts. */
    psx_ot_clear(&ot);
    q2_hud_pen_default(&pen);
    q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "#01003C" "@0100A" "AB CDEFGH");
    check_eq_i(pen.y, 0x0A + 8, "the second word wraps to the next line");
    check_eq_i(pen.x, 0x010 + 6 * 8, "and restarts at the left margin");

    psx_ot_free(&ot);
}

static void test_backdrop(void)
{
    psx_ot ot;
    q2_hud_ctx ctx;
    q2_hud_pen pen;
    u32 cells;

    printf("the *N backdrop\n");

    psx_ot_init(&ot, 4, 512);
    q2_hud_ctx_default(&ctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
    q2_hud_pen_default(&pen);

    cells = q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "@0400AABC");
    check_eq_i(cells, 3, "three glyph cells recorded");
    check_eq_i(ot.prim_count, 3, "and no backdrop without a *N");

    psx_ot_clear(&ot);
    q2_hud_pen_default(&pen);
    cells = q2_hud_print(&g_font, &ctx, &pen, &ot, 0, "*7@0400AABC");
    check_eq_i(cells, 3, "still three cells");
    check_eq_i(ot.prim_count, 6, "*7 adds one 16x16 tile behind each");

    /*
     * The tiles are added after the glyphs, so within the bucket they come out
     * FIRST — which is what puts them behind. Walking the bucket, the last
     * three primitives inserted are the ones at the head.
     */
    {
        /* Depth 0 is the front of the table, which the forward walk reaches
         * last — so the overlay's bucket is the far end of the array. */
        s32 head = ot.bucket_head[psx_ot_depth_bucket(&ot, 0)];
        check(head >= 0, "bucket is not empty");
        check_eq_i(ot.prims[head].uv[0].v, 0xF0,
                   "the first primitive drawn is a backdrop tile");
    }

    psx_ot_free(&ot);
}

static void test_messages(void)
{
    q2_hud hud;
    int i;

    printf("the notification ring\n");

    q2_hud_init(&hud, &g_tab, 1);
    check_eq_i(hud.msg_max, 4, "single player shows four lines");

    q2_hud_init(&hud, &g_tab, 2);
    check_eq_i(hud.msg_max, 2, "a two-way split shows two");

    q2_hud_init(&hud, &g_tab, 1);
    for (i = 0; i < 3; i++)
        q2_hud_message(&hud, "line");
    check_eq_i(hud.msg_count, 3, "three messages held");

    /* A fourth fits; a fifth pushes the oldest out. */
    q2_hud_message(&hud, "four");
    check_eq_i(hud.msg_count, 4, "four fit");
    q2_hud_message(&hud, "five");
    check_eq_i(hud.msg_count, 4, "the fifth displaces the oldest");

    /* One line retires every 60 ticks. */
    q2_hud_tick(&hud, Q2_HUD_MSG_TICKS - 1);
    check_eq_i(hud.msg_count, 4, "nothing retires before 60 ticks");
    q2_hud_tick(&hud, 1);
    check_eq_i(hud.msg_count, 3, "one retires at 60");
    q2_hud_tick(&hud, Q2_HUD_MSG_TICKS * 3);
    check_eq_i(hud.msg_count, 0, "and the rest follow");

    /* Truncation matches the original's strncpy(dst, src, 63). */
    {
        char longmsg[128];
        memset(longmsg, 'x', sizeof(longmsg) - 1);
        longmsg[sizeof(longmsg) - 1] = '\0';
        q2_hud_message(&hud, longmsg);
        check_eq_i((s64)strlen(hud.msg[hud.msg_write == 0
                                       ? Q2_HUD_MSG_SLOTS - 1
                                       : hud.msg_write - 1]),
                   Q2_HUD_MSG_LEN - 1, "a long line is cut to 63 characters");
    }
}

static void test_weapon_glyphs(void)
{
    q2_hud hud;
    int slot;

    printf("the weapon-selected line\n");

    q2_hud_init(&hud, &g_tab, 1);

    /* Consumed 1-based: id 0 is "no weapon" and prints the blank slot. */
    q2_hud_weapon_selected(&hud, &g_tab, 0);
    slot = hud.msg_write == 0 ? Q2_HUD_MSG_SLOTS - 1 : hud.msg_write - 1;
    check(strcmp(hud.msg[slot], "Selected   ") == 0,
          "id 0 selects the deliberately blank glyph");

    q2_hud_weapon_selected(&hud, &g_tab, 1);
    slot = hud.msg_write == 0 ? Q2_HUD_MSG_SLOTS - 1 : hud.msg_write - 1;
    check(strcmp(hud.msg[slot], "Selected &B") == 0,
          "id 1 is the blaster's icon escape");

    check_eq_i(q2_hud_weapon_by_model(&g_tab, "Blaster G"), 1,
               "the view model maps back to its weapon id");
    check_eq_i(q2_hud_weapon_by_model(&g_tab, "rocklaunch g"), 8,
               "and does so case-insensitively");
    check_eq_i(q2_hud_weapon_by_model(&g_tab, "Soldier"), 0,
               "a non-weapon model maps to the blank slot");
}

static void test_measure_and_flash(void)
{
    q2_hud hud;

    printf("the measurer and the damage flash\n");

    /* 0x800702A0 returns strlen - 1. Reproduced on purpose. */
    check_eq_i(q2_hud_measure(""), 0, "empty measures 0");
    check_eq_i(q2_hud_measure("a"), 0, "one character measures 0");
    check_eq_i(q2_hud_measure("abcd"), 3, "four characters measure 3");

    q2_hud_init(&hud, &g_tab, 1);

    /* The first call is only a baseline. */
    q2_hud_track(&hud, 100, 0);
    check_eq_i(hud.flash.strength, 0, "the first sample raises nothing");

    /* Health damage: damage/2 + 2, saturating at 5, and tinted red. */
    q2_hud_track(&hud, 98, 0);
    check_eq_i(hud.flash.strength, 3, "2 points of health damage gives 3");
    check_eq_i(hud.flash.rgb[0], q2_hud_flash_health_rgb[0], "red channel");
    check_eq_i(hud.flash.rgb[1], 0, "health damage has no green");

    q2_hud_init(&hud, &g_tab, 1);
    q2_hud_track(&hud, 100, 0);
    q2_hud_track(&hud, 60, 0);
    check_eq_i(hud.flash.strength, Q2_HUD_FLASH_MAX,
               "a big hit saturates at 5");

    /*
     * Armour damage runs the subtraction the other way round, so a small hit
     * gives a WEAKER flash and a large one wraps through the unsigned test to
     * the maximum. This is the original's arithmetic, not a transcription slip.
     */
    q2_hud_init(&hud, &g_tab, 1);
    q2_hud_track(&hud, 100, 50);
    q2_hud_track(&hud, 100, 48);
    check_eq_i(hud.flash.strength, 1, "a 2-point armour hit flashes at 1");
    check_eq_i(hud.flash.rgb[0], q2_hud_flash_armour_rgb[0], "armour is grey");
    check_eq_i(hud.flash.rgb[1], q2_hud_flash_armour_rgb[1], "grey, not red");

    q2_hud_init(&hud, &g_tab, 1);
    q2_hud_track(&hud, 100, 50);
    q2_hud_track(&hud, 100, 20);
    check_eq_i(hud.flash.strength, Q2_HUD_FLASH_MAX,
               "a 30-point armour hit wraps to the maximum");

    /* Armour wins when both fell — the branch order says so. */
    q2_hud_init(&hud, &g_tab, 1);
    q2_hud_track(&hud, 100, 50);
    q2_hud_track(&hud, 80, 40);
    check_eq_i(hud.flash.rgb[1], q2_hud_flash_armour_rgb[1],
               "a hit that costs both flashes grey");

    /*
     * The join. On the console the raise and the tile are the same halfwords —
     * ctx+0x2A0 IS view+672 — so nothing has to be handed over. Here they are
     * two modules, and the return value is the only moment at which the screen
     * can be told, so it has to be true exactly when a flash was raised and
     * false on every other call. A false negative loses the flash entirely; a
     * false positive restarts its countdown every frame and it never fades.
     */
    q2_hud_init(&hud, &g_tab, 1);
    check(!q2_hud_track(&hud, 100, 50), "the baseline sample raises nothing");
    check(!q2_hud_track(&hud, 100, 50), "standing still raises nothing");
    check( q2_hud_track(&hud,  90, 50), "losing health raises one");
    check(!q2_hud_track(&hud,  90, 50), "and only on the frame it fell");
    check( q2_hud_track(&hud,  90, 40), "losing armour raises one too");
    check(!q2_hud_track(&hud, 100, 50), "healing raises nothing");

    /*
     * The armour arm's dead band, which is the one case where the arithmetic
     * says "damage taken" and the flash is still not raised: `2 - loss/2` is
     * zero for a 4-point hit, and a strength of zero is dropped.
     */
    q2_hud_init(&hud, &g_tab, 1);
    q2_hud_track(&hud, 100, 50);
    check(!q2_hud_track(&hud, 100, 46),
          "a 4-point armour hit lands in the dead band and raises nothing");
}

static void test_layout(void)
{
    q2_hud_ctx ctx;

    printf("the layout space\n");

    q2_hud_ctx_default(&ctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
    check_eq_i(ctx.height, 248, "the console's framebuffer is 248 rows");
    check(ctx.height <= 255,
          "y must fit the two hex digits the @ escape carries");

    q2_hud_ctx_centre_in(&ctx, 640, 480);
    check_eq_i(ctx.width, Q2_HUD_SPACE_W, "centring does not rescale");
    check_eq_i(ctx.origin_x, (640 - 512) / 2, "centred horizontally");
    check_eq_i(ctx.origin_y, (480 - 248) / 2, "centred vertically");
}

int main(void)
{
    printf("HUD\n\n");

    build_font();

    test_escapes();
    test_advance();
    test_newline_and_wrap();
    test_backdrop();
    test_messages();
    test_weapon_glyphs();
    test_measure_and_flash();
    test_layout();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
