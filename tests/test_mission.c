/*
 * test_mission.c — the level-completion screen's composition.
 *
 * The screen is markup the game builds with `sprintf` and hands to the same
 * interpreter every other string goes through, so what is worth pinning here is
 * the composition rather than the pixels: the title's wording, the centring
 * rule, and the one place where the original's choice of `%03X` over `%3X`
 * changes where the pen lands.
 */
#include "mission.h"

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

static void test_title(void)
{
    q2_mission m;
    char buf[64];

    q2_mission_init(&m);
    m.unit = 1;
    q2_mission_title(&m, buf, (u32)sizeof(buf));
    /* 0x800AB9DC, doubled spaces and all. */
    CHECK(strcmp(buf, "Mission  1  -  Complete") == 0,
          "the title is the original's wording, got \"%s\"", buf);

    m.unit = 7;
    q2_mission_title(&m, buf, (u32)sizeof(buf));
    CHECK(strstr(buf, "7") != NULL, "the unit number is formatted in");
}

static void test_centring(void)
{
    /*
     * 0x80022550 centres against the box using the HUD's own measurer, which
     * returns strlen - 1 (hud.h). That off-by-one is deliberate: correcting it
     * would move every centred row half a glyph from where the console puts it.
     */
    int x = q2_mission_centre_x("ABCD", 0, 512);
    int w = q2_hud_measure("ABCD") * Q2_HUD_GLYPH_W;

    CHECK(q2_hud_measure("ABCD") == 3,
          "the measurer is still short by one, got %d", q2_hud_measure("ABCD"));
    CHECK(x == (512 - w) / 2, "centred at %d for a width of %d", x, w);

    /* Never left of the box, however long the string. */
    CHECK(q2_mission_centre_x("A VERY LONG LINE INDEED", 100, 140) >= 100,
          "an overlong line clamps to the left edge");
}

static void test_field_placement(void)
{
    /*
     * 0x8002260C centres in a 56-pixel field and DECREMENTS AN EVEN LENGTH
     * first. Both halves are checked, because reading it as a right-align (the
     * obvious guess) puts every counter in the wrong place, and "fixing" the
     * even-length quirk moves the six-character totals four pixels.
     */
    CHECK(q2_mission_field_x("4/4", 280) == 280 + (56 - 3 * 8) / 2,
          "an odd length centres on its true width, got %d",
          q2_mission_field_x("4/4", 280));
    CHECK(q2_mission_field_x("99/100", 352) == 352 + (56 - 5 * 8) / 2,
          "an even length measures one short, got %d",
          q2_mission_field_x("99/100", 352));
    /* "20/20" and "10/10" are five characters — odd, so unadjusted. */
    CHECK(q2_mission_field_x("10/10", 280) == 280 + (56 - 5 * 8) / 2,
          "five characters are not adjusted");
    /* It is not a right-align: a short string does NOT end at the field's x. */
    CHECK(q2_mission_field_x("9/9", 352) > 352,
          "the field is centred, not right-aligned");
}

static void test_layout_steps(void)
{
    /* The one irregular gap on the screen: +10 then +8 (0x80021C00 and
     * 0x80021C4C), not a uniform run. */
    CHECK(Q2_MISSION_BODY_Y == 36, "the first body line is at 36");
    CHECK(Q2_MISSION_BODY_STEP_1 == 10 && Q2_MISSION_BODY_STEP_2 == 8,
          "the body steps are 10 then 8");
    CHECK(Q2_MISSION_LABEL_INDENT == 20, "the labels are inset 20 from the box");

    /*
     * The header is ONE row of three columns, not a stacked column — the
     * mistake that produces a plausible screen bearing no relation to the
     * console's. The offsets are confirmed twice over: the delay-slot `addiu`
     * at 0x80021D14 and 0x80021D3C, and retail screenshots.
     */
    CHECK(Q2_MISSION_COL_LOCATION == 0 && Q2_MISSION_COL_SECRETS == 176 &&
          Q2_MISSION_COL_KILLS == 256, "three columns at +0, +176, +256");
    /* The Kills label and the Kills field are deliberately different. */
    CHECK(Q2_MISSION_COL_KILLS != Q2_MISSION_VAL_KILLS,
          "the label is placed and the value is centred in a box");
    CHECK(Q2_MISSION_RECORD_STRIDE == 25 && Q2_MISSION_ROWS == 6,
          "six 25-byte records");
}

static void test_empty_rows_are_skipped(void)
{
    q2_mission m;
    int s, st, k, kt;

    q2_mission_init(&m);
    q2_mission_set_row(&m, 0, "Strogg Outpost", 4, 4, 9, 9);
    q2_mission_set_row(&m, 2, "Outer Base", 2, 2, 20, 20);

    CHECK(m.row[1].name[0] == '\0', "an unfilled slot stays empty");

    /* The totals sum EVERY record, drawn or not (0x80021E58 runs after the
     * `blez` that skips the drawing). */
    q2_mission_totals(&m, &s, &st, &k, &kt);
    CHECK(s == 6 && st == 6 && k == 29 && kt == 29,
          "totals are %d/%d and %d/%d", s, st, k, kt);
}

static void test_counts_clamp(void)
{
    q2_mission m;

    /* The record's counters are u8, so a port must clamp rather than wrap —
     * 300 kills must not display as 44. */
    q2_mission_init(&m);
    q2_mission_set_row(&m, 0, "Test", 0, 0, 300, 300);
    CHECK(m.row[0].kills == Q2_MISSION_COUNT_MAX,
          "a count over 255 clamps, got %u", m.row[0].kills);

    /* And the 21-byte name field truncates rather than overrunning. */
    q2_mission_set_row(&m, 1, "A name far longer than the record allows",
                       0, 0, 0, 0);
    CHECK(strlen(m.row[1].name) == Q2_MISSION_NAME_LEN,
          "the name truncates to the record, got %u",
          (unsigned)strlen(m.row[1].name));
}

static void test_colours(void)
{
    /* The three colours the format strings spell out. */
    CHECK(q2_mission_rgb_label[0] == 0xBE && q2_mission_rgb_label[1] == 0xF0 &&
          q2_mission_rgb_label[2] == 0xE6, "labels are BEF0E6");
    CHECK(q2_mission_rgb_value[0] == 0xDC && q2_mission_rgb_value[1] == 0xF0 &&
          q2_mission_rgb_value[2] == 0x82, "values are DCF082");
    CHECK(q2_mission_rgb_total[0] == 0xC8 && q2_mission_rgb_total[1] == 0xF0 &&
          q2_mission_rgb_total[2] == 0x00, "totals are C8F000");
}

static void test_counters_are_inputs(void)
{
    q2_mission m;
    int s, st, k, kt;

    /* An uncounted caller must get zeroes, not invented figures — the sim does
     * not tally kills or secrets yet and the screen must not pretend. */
    q2_mission_init(&m);
    q2_mission_totals(&m, &s, &st, &k, &kt);
    CHECK(s == 0 && st == 0 && k == 0 && kt == 0, "nothing counted, nothing shown");
}

int main(void)
{
    test_title();
    test_centring();
    test_field_placement();
    test_layout_steps();
    test_empty_rows_are_skipped();
    test_counts_clamp();
    test_colours();
    test_counters_are_inputs();

    if (g_fail) {
        printf("\n%d mission check%s failed\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("mission: all checks passed\n");
    return 0;
}
