/*
 * test_statusbar.c — the bar that FORMATS.md §11.1 said did not exist.
 *
 * What is worth pinning here is the field arithmetic, because it is the part a
 * plausible-looking mistake survives: three counters of three digits with an
 * icon each, digits 24 apart because a numeral cell is 24 wide, values right
 * aligned so the units column does not move.
 */
#include "statusbar.h"

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

static void test_field_groups(void)
{
    int c, d;

    /* Every counter has three digit fields and one icon field, and no field is
     * claimed twice — the grouping is what makes the layout legible at all. */
    {
        int seen[Q2_SBAR_FIELDS];
        memset(seen, 0, sizeof(seen));

        for (c = 0; c < Q2_SBAR_COUNTERS; c++) {
            int icon = q2_sbar_icon_field((q2_sbar_counter)c);
            CHECK(icon >= 0 && icon < Q2_SBAR_FIELDS, "counter %d has an icon", c);
            if (icon >= 0) {
                CHECK(!seen[icon], "field %d claimed twice", icon);
                seen[icon] = 1;
            }
            for (d = 0; d < Q2_SBAR_COUNTER_DIGITS; d++) {
                int f = q2_sbar_digit_field((q2_sbar_counter)c, d);
                CHECK(f >= 0 && f < Q2_SBAR_FIELDS, "counter %d digit %d", c, d);
                if (f >= 0) {
                    CHECK(!seen[f], "field %d claimed twice", f);
                    seen[f] = 1;
                }
            }
        }
    }

    CHECK(q2_sbar_digit_field((q2_sbar_counter)Q2_SBAR_COUNTERS, 0) < 0,
          "an out-of-range counter has no field");
    CHECK(q2_sbar_digit_field(Q2_SBAR_HEALTH, Q2_SBAR_COUNTER_DIGITS) < 0,
          "an out-of-range digit has no field");
}

static void test_digit_pitch(void)
{
    int c, d;

    /*
     * Within a counter the digits step by exactly one numeral width. That is
     * the invariant tying the field table (read from 0x800337EC) to the numeral
     * table (read from 0x8009C598) — two independent reads that have to agree,
     * and if either were misread they would not.
     */
    for (c = 0; c < Q2_SBAR_COUNTERS; c++) {
        for (d = 1; d < Q2_SBAR_COUNTER_DIGITS; d++) {
            int a = q2_sbar_digit_field((q2_sbar_counter)c, d - 1);
            int b = q2_sbar_digit_field((q2_sbar_counter)c, d);
            int step;

            if (a < 0 || b < 0)
                continue;
            step = q2_sbar_fields[b].dx - q2_sbar_fields[a].dx;
            CHECK(step == Q2_SBAR_DIGIT_PITCH,
                  "counter %d digits %d..%d step %d, want %d",
                  c, d - 1, d, step, Q2_SBAR_DIGIT_PITCH);
        }
    }

    /* And the numeral cell really is that wide. */
    CHECK(Q2_SBAR_DIGIT_W == Q2_SBAR_DIGIT_PITCH,
          "the cell and the pitch are the same 24");
    CHECK(Q2_SBAR_DIGIT_V == 168, "the numerals are the row at v = 168");
}

static void test_counters_are_ordered(void)
{
    /* Left to right: health, ammo, armour — from retail capture, and the only
     * thing here that is not a transcription. If this ever has to change it is
     * this test that should fail first. */
    int h = q2_sbar_digit_field(Q2_SBAR_HEALTH, 0);
    int a = q2_sbar_digit_field(Q2_SBAR_AMMO, 0);
    int r = q2_sbar_digit_field(Q2_SBAR_ARMOUR, 0);

    CHECK(q2_sbar_fields[h].dx < q2_sbar_fields[a].dx,
          "health is left of ammo");
    CHECK(q2_sbar_fields[a].dx < q2_sbar_fields[r].dx,
          "ammo is left of armour");

    /* Each counter's icon sits past its last digit. */
    {
        int c;
        for (c = 0; c < Q2_SBAR_COUNTERS; c++) {
            int last = q2_sbar_digit_field((q2_sbar_counter)c,
                                           Q2_SBAR_COUNTER_DIGITS - 1);
            int icon = q2_sbar_icon_field((q2_sbar_counter)c);
            CHECK(q2_sbar_fields[icon].dx > q2_sbar_fields[last].dx,
                  "counter %d's icon follows its digits", c);
        }
    }
}

static void test_two_rows(void)
{
    /* The upper row sits 24 to 25 above the main one — a second row, not a
     * continuation of the first. Capture shows a pickup caption's icon on its
     * left and a two-digit counter on its right. */
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_UP_LEFT].dy == -25,
          "the upper-left icon is 25 above");
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT0].dy == -24 &&
          q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT1].dy == -24,
          "the upper digits are 24 above");
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT1].dx -
          q2_sbar_fields[Q2_SBAR_FIELD_UP_DIGIT0].dx == Q2_SBAR_DIGIT_PITCH,
          "and they step by one numeral");
    /* The frag field is on the MAIN row, far right. */
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_FRAGS].dy == 0,
          "the frag count is on the main row");
    CHECK(q2_sbar_fields[Q2_SBAR_FIELD_FRAGS].dx >
          q2_sbar_fields[q2_sbar_icon_field(Q2_SBAR_ARMOUR)].dx,
          "and right of everything else");
}

static void test_two_player_layout(void)
{
    int i;

    /*
     * The 2P hook builds its OWN table — two counters, digits 20 apart. This is
     * the thing a port gets wrong by assuming one layout scaled: the console
     * does not scale, it has another table.
     */
    for (i = 1; i < 3; i++)
        CHECK(q2_sbar_fields_2p[i + 1].dx - q2_sbar_fields_2p[i].dx == 20,
              "2P health digits step 20, got %d",
              q2_sbar_fields_2p[i + 1].dx - q2_sbar_fields_2p[i].dx);
    CHECK(q2_sbar_fields_2p[6].dx - q2_sbar_fields_2p[5].dx == 20,
          "2P ammo digits step 20");
    /* Every 2P offset is positive: a split viewport's anchor is near its left
     * edge, so negative offsets would fall outside it. */
    for (i = 0; i < Q2_SBAR_FIELDS_2P; i++)
        CHECK(q2_sbar_fields_2p[i].dx >= 0,
              "2P field %d is inside the viewport", i);
}

static void test_quad_layout(void)
{
    int i, lower = 0;

    /*
     * The quad layout is SIXTEEN fields, not a scaled copy of either of the
     * others — four counters over two rows, because in four-player split the
     * bar carries every player's counters at once instead of being drawn per
     * viewport.
     */
    for (i = 1; i < 3; i++) {
        CHECK(q2_sbar_fields_4p[i + 1].dx - q2_sbar_fields_4p[i].dx == 20,
              "quad digits step 20, got %d",
              q2_sbar_fields_4p[i + 1].dx - q2_sbar_fields_4p[i].dx);
        CHECK(q2_sbar_fields_4p[i + 5].dx - q2_sbar_fields_4p[i + 4].dx == 20,
              "and so does the second counter");
    }
    /* The icon sits one more step past the last digit, both times. */
    CHECK(q2_sbar_fields_4p[0].dx - q2_sbar_fields_4p[3].dx == 20,
          "the upper-left icon follows its digits by one step");
    CHECK(q2_sbar_fields_4p[4].dx - q2_sbar_fields_4p[7].dx == 20,
          "and so does the upper-right one");

    /* The two icon fields are the wide ones; every digit field is 8. */
    CHECK(q2_sbar_fields_4p[0].init_w == 38 && q2_sbar_fields_4p[4].init_w == 38,
          "the upper row's two icons are 38 wide");
    CHECK(q2_sbar_fields_4p[1].init_w == 8, "and a digit is 8");

    /* Eight fields hang off the bottom of the screen rather than the top. */
    for (i = 0; i < Q2_SBAR_FIELDS_4P; i++)
        if (q2_sbar_field_4p_is_lower(i))
            lower++;
    CHECK(lower == 7, "seven fields are measured from the bottom, got %d", lower);
    CHECK(!q2_sbar_field_4p_is_lower(12),
          "the field at x=400 is on the upper row despite its index");

    /* Every offset is positive: a quarter viewport's anchor is near its left
     * edge, so a negative offset would fall outside it. */
    for (i = 0; i < Q2_SBAR_FIELDS_4P; i++)
        CHECK(q2_sbar_fields_4p[i].dx >= 0, "quad field %d is inside", i);
}

static void test_icon_vocabulary(void)
{
    /*
     * The fifth byte of a rect record is the item's `effect` dispatch index.
     * The proof is that every weapon's ammo entry names its own ammunition —
     * an eleven-way agreement nothing in the decode arranged. These check the
     * six ammo types by name, which is the part that would break first if the
     * reading were wrong.
     */
    static const struct { u8 effect; const char *name; } ammo[] = {
        { 18, "Shells P"  },
        { 19, "Bullets P" },
        { 20, "Grenade P" },
        { 21, "Rockets P" },
        { 22, "Cells P"   },
        { 23, "Slugs P"   }
    };
    size_t i;

    for (i = 0; i < sizeof(ammo) / sizeof(ammo[0]); i++) {
        const char *got = q2_icon_name_for_id(ammo[i].effect);
        CHECK(got && strcmp(got, ammo[i].name) == 0,
              "effect %u is %s, got %s", ammo[i].effect, ammo[i].name,
              got ? got : "(none)");
    }

    /* The icons the bar names for itself. */
    CHECK(q2_icon_name_for_id(Q2_SBAR_ICON_MEDIKIT) != NULL,
          "the medikit icon names an item");
    CHECK(q2_icon_name_for_id(Q2_SBAR_ICON_ARMOUR_JACKET) != NULL,
          "the jacket armour icon names an item");

    /* Zero is "no icon" and must not match the front of the table. */
    CHECK(q2_icon_name_for_id(0) == NULL, "effect 0 names nothing");
}

static void test_digits_of(void)
{
    u8 d[Q2_SBAR_COUNTER_DIGITS];

    CHECK(q2_sbar_digits_of(100, d) == 3 && d[0] == 1 && d[1] == 0 && d[2] == 0,
          "100 is three digits");
    CHECK(q2_sbar_digits_of(50, d) == 2 && d[0] == 5 && d[1] == 0,
          "50 is two");
    /* No leading zeroes: capture shows "2", not "002". */
    CHECK(q2_sbar_digits_of(2, d) == 1 && d[0] == 2, "2 is one");
    CHECK(q2_sbar_digits_of(0, d) == 1 && d[0] == 0, "zero still shows");

    /* Three cells is the ceiling, and a negative must not underflow. */
    CHECK(q2_sbar_digits_of(1234, d) == 3, "over 999 clamps");
    CHECK(q2_sbar_digits_of(-5, d) == 1 && d[0] == 0, "negative reads zero");
}

static void test_split_screen_sizes(void)
{
    /*
     * The reduction is a CLAMP, not a scale: two players force 24 x 18 and
     * three or more 16 x 12 whatever the source rect, so a 24-wide numeral and
     * a 32-wide icon come out the same size in split screen. Single player
     * passes the source through, which is what keeps numerals unstretched.
     */
    CHECK(q2_icon_draw_size_of(1, 1, 24, 24).w == 24,
          "single player keeps a numeral at 24");
    CHECK(q2_icon_draw_size_of(1, 1, 32, 24).w == 32,
          "single player keeps an icon at 32");
    CHECK(q2_icon_draw_size_of(2, 1, 24, 24).w == 24 &&
          q2_icon_draw_size_of(2, 1, 32, 24).w == 24,
          "two players clamp both to 24");
    CHECK(q2_icon_draw_size_of(4, 1, 24, 24).w == 16 &&
          q2_icon_draw_size_of(4, 1, 32, 24).h == 12,
          "four players clamp to 16x12");

    /* No weapon collapses to the blank rather than scaling a 1x1 up. */
    CHECK(q2_icon_draw_size_of(1, 0, 32, 24).w == 1,
          "no weapon draws the blank");
}

int main(void)
{
    test_field_groups();
    test_digit_pitch();
    test_counters_are_ordered();
    test_two_rows();
    test_two_player_layout();
    test_quad_layout();
    test_icon_vocabulary();
    test_digits_of();
    test_split_screen_sizes();

    if (g_fail) {
        printf("\n%d status-bar check%s failed\n", g_fail,
               g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("statusbar: all checks passed\n");
    return 0;
}
