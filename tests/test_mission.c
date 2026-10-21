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
    /*
     * And the step PAST the third line, which only happens when the line is
     * drawn (0x80021CB0, skipped by the `beq` at 0x80021C58). Without it the
     * header row is eight pixels high whenever the objective wraps, and the
     * two are tied together: the same eight is what PreProcessInterMission
     * adds to the panel for a wrapped objective.
     */
    CHECK(Q2_MISSION_BODY_STEP_3 == 8, "the step past the third line is 8");
    CHECK(Q2_MISSION_BODY_Y + Q2_MISSION_BODY_STEP_1 +
          Q2_MISSION_BODY_STEP_2 + Q2_MISSION_HEADER_DROP == 60,
          "an unwrapped objective puts the header at 60");
    CHECK(Q2_MISSION_BODY_Y + Q2_MISSION_BODY_STEP_1 +
          Q2_MISSION_BODY_STEP_2 + Q2_MISSION_BODY_STEP_3 +
          Q2_MISSION_HEADER_DROP == 68,
          "a wrapped one puts it at 68");
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

/*
 * 0x800222B8 — the rows are claimed BY NAME, not in visit order.
 *
 * The distinction is the whole of the console's model and it is invisible
 * until a level is re-entered: registering by order gives that level a second
 * row and two half-filled tallies for one place, which is what this fails on.
 */
static void test_register_is_by_name(void)
{
    q2_mission m;
    int a, b, cc, again;

    q2_mission_init(&m);
    a  = q2_mission_register(&m, "Strogg Outpost");
    b  = q2_mission_register(&m, "Outer Base");
    cc = q2_mission_register(&m, "Installation");
    CHECK(a == 0 && b == 1 && cc == 2, "three levels take rows 0, 1, 2");

    /* Walking back into a level it has already seen. */
    again = q2_mission_register(&m, "Outer Base");
    CHECK(again == 1, "a level already in the table keeps its row");
    CHECK(m.row[3].name[0] == 0, "...and does not take a fourth");

    /* The counters live in the row, and re-registering must not wipe them —
     * the console's find arm returns before it touches anything. */
    q2_mission_set_counts(&m, 1, 2, 2, 5, 8);
    CHECK(q2_mission_register(&m, "Outer Base") == 1, "found again");
    CHECK(m.row[1].secrets == 2 && m.row[1].kills == 5,
          "re-registering keeps the counters");

    CHECK(q2_mission_find(&m, "Installation") == 2, "find by name");
    CHECK(q2_mission_find(&m, "Nowhere") < 0, "a name not in the table");
}

/* Six rows, and the seventh distinct level gets none — the console's search
 * falls off the end of the sixth record the same way. */
static void test_seventh_level_gets_no_row(void)
{
    q2_mission m;
    const char *name[7] = { "L1", "L2", "L3", "L4", "L5", "L6", "L7" };
    int i;

    q2_mission_init(&m);
    for (i = 0; i < 6; i++)
        CHECK(q2_mission_register(&m, name[i]) == i, "the six fill in order");
    CHECK(q2_mission_register(&m, name[6]) < 0, "the seventh gets no row");
    /* And it must not have overwritten anybody. */
    CHECK(strcmp(m.row[5].name, "L6") == 0, "the sixth row is still L6");
}

/*
 * 0x800220C8's wrap, which is a cut at the last space and not a column count.
 */
static void test_objective_wrap(void)
{
    q2_mission m;
    const char *shortish = "Locate unit exit.";
    const char *longish  =
        "Locate the unit exit and kill all resistance on the way to it.";
    size_t cut;

    q2_mission_init(&m);
    q2_mission_set_objective(&m, shortish);
    CHECK(strcmp(m.subtitle, shortish) == 0, "under 36 goes on one line");
    CHECK(m.footer[0] == 0, "...and the second line stays empty");

    q2_mission_set_objective(&m, longish);
    CHECK(m.subtitle[0] != 0 && m.footer[0] != 0, "over 36 uses both lines");
    CHECK(strlen(m.subtitle) < 36, "line one is under the wrap length");
    /* The break is AT a space and the space goes with line two — the console
     * passes `src + len`, where len is the index the space sat at. */
    CHECK(m.footer[0] == ' ', "line two begins with the space it broke on");
    cut = strlen(m.subtitle);
    CHECK(longish[cut] == ' ', "the cut landed on a space");
    CHECK(strncmp(m.subtitle, longish, cut) == 0, "line one is the head");
    CHECK(strcmp(m.footer, longish + cut) == 0, "line two is the rest");

    q2_mission_set_objective(&m, NULL);
    CHECK(m.subtitle[0] == 0 && m.footer[0] == 0, "no objective, no lines");
}

/*
 * MissionRect, 0x800AE760 — and it is initialised data, not a guess.
 *
 *     800AE760  54 00 1E 00 58 01 7C 00
 *
 * The x matters twice over: the column indent is `box.x + 20`, so the whole
 * Location/Secrets/Kills table moves with it. The port used to use 96 and every
 * column sat twelve pixels right of the console's — invisible on the three
 * centred lines, because 84 + 344/2 and 96 + 320/2 are both 256, which is why
 * it went unnoticed.
 */
static void test_box_is_the_image_rectangle(void)
{
    q2_mission m;

    q2_mission_init(&m);
    CHECK(m.box.x == 84, "x is 0x0054, got %d", m.box.x);
    CHECK(m.box.y == 30, "y is 0x001E, got %d", m.box.y);
    CHECK(m.box.w == 344, "w is 0x0158, got %d", m.box.w);
    CHECK(m.box.h == 124, "h seeds at 0x007C, got %d", m.box.h);

    /* The Location column, which is what the 12-pixel shift moved. */
    CHECK(m.box.x + Q2_MISSION_LABEL_INDENT == 104,
          "the label indent lands at 104, got %d",
          m.box.x + Q2_MISSION_LABEL_INDENT);

    /* ...and the centred lines really are unaffected, which is the reason the
     * wrong x survived: both rectangles centre on 256. */
    CHECK(q2_mission_centre_x("ABCD", 84, 84 + 344) ==
          q2_mission_centre_x("ABCD", 96, 96 + 320),
          "the two rectangles centre identically");
}

/*
 * PreProcessInterMission, 0x80022664 — the height SetInterMission stores into
 * gp+358. 42 for a one-line objective, 50 for a wrapped one, +10 a row, +14.
 */
static void test_box_height(void)
{
    q2_mission m;
    int i;

    q2_mission_init(&m);
    /* Nothing registered and no objective: 24 + 18 + 14. */
    CHECK(q2_mission_box_height(&m) == 56,
          "an empty board is 56, got %d", q2_mission_box_height(&m));

    q2_mission_register(&m, "Outer Base");
    CHECK(q2_mission_box_height(&m) == 66,
          "one row adds ten, got %d", q2_mission_box_height(&m));

    /* A short objective leaves the SECOND wrapped line empty, and the branch at
     * 0x80022690 tests that line and not the first. */
    q2_mission_set_objective(&m, "Locate unit exit.");
    CHECK(m.footer[0] == '\0', "the objective fitted on one line");
    CHECK(q2_mission_box_height(&m) == 66,
          "an unwrapped objective does not grow it, got %d",
          q2_mission_box_height(&m));

    q2_mission_set_objective(&m,
        "Locate the unit exit and kill all resistance on the way to it.");
    CHECK(m.footer[0] != '\0', "the objective wrapped");
    CHECK(q2_mission_box_height(&m) == 74,
          "a wrapped objective adds eight, got %d",
          q2_mission_box_height(&m));

    /* And the .data seed is the six-row, wrapped-objective answer, which is the
     * confirmation that the formula is read right: 50 + 14 + 6*10 = 124. */
    for (i = 1; i < Q2_MISSION_ROWS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "L%d", i);
        q2_mission_register(&m, name);
    }
    CHECK(q2_mission_box_height(&m) == 124,
          "six rows and a wrapped objective is the .data seed 124, got %d",
          q2_mission_box_height(&m));
}

/*
 * PadEndInterMission, 0x800224F4 — the board cannot be dismissed for its first
 * 1200 ticks, and the port used to close it on the first press of the first
 * frame.
 */
static void test_dismissal_gate(void)
{
    s32 gate = Q2_MISSION_GATE_TICKS;
    int ticks;

    CHECK(Q2_MISSION_GATE_TICKS == 1200,
          "the .data seed at 0x800AE76A is 0x04B0");

    /* A press on the very first frame does nothing — the pad is not even read
     * while the counter is positive (the `bgtz` at 0x80022514). */
    CHECK(!q2_mission_pad_end(&gate, 10, true),
          "fire on frame one does not dismiss the board");
    CHECK(gate == 1190, "the counter came down by the delta, got %d", gate);

    /*
     * 1200 ticks at the headless delta of 10 is 120 frames. Frame n leaves the
     * counter at 1200 - 10n, and `bgtz` keeps the board up while that is
     * strictly positive — so frames 1..119 are locked and frame 120 is the
     * first that reads the pad at all.
     */
    for (ticks = 2; ticks <= 119; ticks++)
        CHECK(!q2_mission_pad_end(&gate, 10, true),
              "still locked at frame %d", ticks);
    CHECK(gate == 10, "one delta left at frame 119, got %d", gate);

    /* Frame 120: the gate opens, and it still wants an EDGE. */
    CHECK(!q2_mission_pad_end(&gate, 10, false),
          "the gate opens on frame 120 but nobody pressed anything");
    CHECK(gate == 0, "the counter clamps to zero, got %d", gate);

    CHECK(!q2_mission_pad_end(&gate, 10, false),
          "no press, no dismissal");
    CHECK(gate == 0, "and the counter stays clamped, got %d", gate);

    CHECK(q2_mission_pad_end(&gate, 10, true),
          "the first fire edge past the gate dismisses it");
    /* 0x80022538: re-armed by the DISMISSAL, not by the next raise. */
    CHECK(gate == Q2_MISSION_GATE_TICKS,
          "the dismissal re-arms the counter to 1200, got %d", gate);

    /* A counter below the delta wraps as a u16 and is then rejected by the
     * SIGNED 16-bit test — which is what `sll v0,16` before the `bgtz` is for,
     * and what stops the board locking for another 65,000 ticks. */
    gate = 5;
    CHECK(!q2_mission_pad_end(&gate, 30, false),
          "an underflowing counter does not read as positive");
    CHECK(gate == 0, "...and clamps to zero, got %d", gate);
}

/* QuakeCameraInterMission's table, 0x8009C668 — four 24-byte records. */
static void test_camera_table(void)
{
    q2_mission_camera cam;

    CHECK(q2_mission_camera_for_unit(1, &cam), "unit 1 has a record");
    CHECK(cam.pos[0] == -9314 && cam.pos[1] == -2562 && cam.pos[2] == 10594,
          "unit 1's position is 0x8009C668's three words, got (%d,%d,%d)",
          cam.pos[0], cam.pos[1], cam.pos[2]);
    CHECK(cam.pitch == 500, "and its pitch is 0x01F4, got %d", cam.pitch);
    /* 0x800387FC negates the yaw on the way in. */
    CHECK(cam.yaw == -5490, "the yaw is negated, got %d", cam.yaw);
    CHECK(cam.cell == 170 && cam.area == 31,
          "the record's collision cell and area travel with it");

    /*
     * Unit 4's raw yaw is 0xFD95, and the two readings differ: the console
     * loads it with `lhu` and negates the UNSIGNED value, which gives +619.
     * Negating the s16 would give -619 and point the camera the other way.
     */
    CHECK(q2_mission_camera_for_unit(4, &cam), "unit 4 has a record");
    CHECK(cam.yaw == 619,
          "0xFD95 negated as an unsigned halfword is +619, got %d", cam.yaw);

    /* The table is four records long: 0x8009C6C8 is the string "QFront". */
    CHECK(!q2_mission_camera_for_unit(5, &cam),
          "unit 5 has no record and keeps the player's camera");
    CHECK(!q2_mission_camera_for_unit(0, &cam), "nor does unit 0");
}

/*
 * ClearLevelIntermission, 0x80021A0C — the six records go at a unit boundary,
 * and nothing else does. This port used to keep them, which filled the table
 * after unit 1 and stopped the Secrets and Kills counters for the rest of the
 * campaign.
 */
static void test_unit_boundary_clears_the_rows(void)
{
    q2_mission m;
    int i;

    q2_mission_init(&m);
    m.unit = 2;
    q2_mission_set_objective(&m, "Locate unit exit.");
    for (i = 0; i < Q2_MISSION_ROWS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "L%d", i);
        q2_mission_register(&m, name);
        q2_mission_set_counts(&m, i, 1, 2, 3, 4);
    }
    CHECK(q2_mission_register(&m, "L9") < 0, "the table is full");

    q2_mission_clear_rows(&m);

    for (i = 0; i < Q2_MISSION_ROWS; i++) {
        CHECK(m.row[i].name[0] == '\0', "row %d's name is gone", i);
        CHECK(m.row[i].secrets == 0 && m.row[i].kills_total == 0,
              "row %d's counters went with it", i);
    }
    /* ...so the next unit's first level gets row 0 rather than -1. */
    CHECK(q2_mission_register(&m, "Warehouse") == 0,
          "the next unit's first level takes row 0");

    /* The unit number and the objective are outside the 150 bytes. */
    CHECK(m.unit == 2, "the unit number survives the memset");
    CHECK(m.subtitle[0] != '\0', "and so does the objective");
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
    test_register_is_by_name();
    test_seventh_level_gets_no_row();
    test_objective_wrap();
    test_box_is_the_image_rectangle();
    test_box_height();
    test_dismissal_gate();
    test_camera_table();
    test_unit_boundary_clears_the_rows();

    if (g_fail) {
        printf("\n%d mission check%s failed\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("mission: all checks passed\n");
    return 0;
}
