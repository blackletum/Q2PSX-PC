/*
 * test_monster.c — the creature framework's arithmetic.
 *
 * The interesting things to pin here are the ones that are easy to get subtly
 * wrong and hard to notice in play: the unpadded frame stride, the distance
 * scaling, and the width of the forward cone.
 */
#include <stdio.h>
#include <string.h>

#include "monster.h"
#include "trig.h"

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
static void test_frame_stride(void)
{
    /* Three frames packed with no padding. Reading these at a stride of four
     * would return the wrong ai/dist/think on every frame after the first. */
    static const u8 image[] = {
        1, 10, 0,      /* ai_stand,  dist 10, think 0 */
        3, 21, 2,      /* ai_run,    dist 21, think 2 */
        4, (u8)-5, 6   /* ai_charge, dist -5, think 6 */
    };
    q2_mframe f;

    printf("frame stride\n");

    check_eq_i(Q2_MFRAME_SIZE, 3, "a frame is three bytes, not four");

    check(q2_mframe_read(image, sizeof(image), 0, &f), "reads frame 0");
    check_eq_i(f.ai, Q2_AI_STAND, "frame 0 verb");
    check_eq_i(f.dist, 10, "frame 0 distance");

    check(q2_mframe_read(image, sizeof(image), 3, &f), "reads frame 1 at +3");
    check_eq_i(f.ai, Q2_AI_RUN, "frame 1 verb");
    check_eq_i(f.dist, 21, "frame 1 distance");
    check_eq_i(f.think, 2, "frame 1 think");

    check(q2_mframe_read(image, sizeof(image), 6, &f), "reads frame 2 at +6");
    check_eq_i(f.ai, Q2_AI_CHARGE, "frame 2 verb");
    check_eq_i(f.dist, -5, "distance is signed");

    /* Bounds: a partial frame at the end must be refused, not read. */
    check(!q2_mframe_read(image, sizeof(image), 7, &f), "refuses a partial frame");
    check(!q2_mframe_read(image, sizeof(image), 99, &f), "refuses past the end");
}

/* ------------------------------------------------------------------------- */
static void test_move_record(void)
{
    u8 image[Q2_MMOVE_SIZE * 2];
    q2_mmove mv;

    printf("move record\n");
    memset(image, 0, sizeof(image));

    /* first 4, last 11, frames at 0x40, no end callback. */
    image[0] = 4;
    image[4] = 11;
    image[8] = 0x40;

    check(q2_mmove_read(image, sizeof(image), 0, &mv), "reads a move");
    check_eq_i(mv.first_frame, 4, "first frame");
    check_eq_i(mv.last_frame, 11, "last frame");
    check_eq_i(mv.frames_offset, 0x40, "frame array offset");
    check_eq_i(mv.endfunc_offset, 0, "no end callback");

    /* A move running backwards is malformed and must be refused rather than
     * producing a negative frame count later. */
    memset(image, 0, sizeof(image));
    image[0] = 20;
    image[4] = 5;
    check(!q2_mmove_read(image, sizeof(image), 0, &mv), "refuses last < first");
}

/* ------------------------------------------------------------------------- */
static void test_frame_distance(void)
{
    q2_monster m;
    q2_mframe f;

    printf("frame distance\n");

    q2_monster_init(&m);
    f.ai = Q2_AI_RUN;
    f.dist = 21;
    f.think = 0;

    /* dist * speed_scale * 12 / 10, with the neutral scale of 10. */
    check_eq_i(q2_monster_frame_dist(&m, &f), (21 * 10 * 12) / 10, "neutral scale");

    m.speed_scale = 20;
    check_eq_i(q2_monster_frame_dist(&m, &f), (21 * 20 * 12) / 10, "double scale");

    /* A held frame animates without advancing, which is what a wind-up needs. */
    m.speed_scale = 10;
    m.aiflags |= Q2_AI_HOLD_FRAME;
    check_eq_i(q2_monster_frame_dist(&m, &f), 0, "hold-frame freezes movement");

    m.aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
    check(q2_monster_frame_dist(&m, &f) != 0, "and releases it again");
}

/* ------------------------------------------------------------------------- */
static void test_infront(void)
{
    q2_monster m;
    s32 ahead[3], behind[3], side[3];

    printf("forward cone\n");

    q2_monster_init(&m);
    m.pos[0] = 0; m.pos[1] = 0; m.pos[2] = 0;
    m.angles[1] = 0;                     /* facing +Z */

    ahead[0] = 0;    ahead[1] = 0;  ahead[2] = 1000;
    behind[0] = 0;   behind[1] = 0; behind[2] = -1000;
    side[0] = 1000;  side[1] = 0;   side[2] = 0;

    check(q2_monster_infront(&m, ahead), "sees straight ahead");
    check(!q2_monster_infront(&m, behind), "does not see behind");
    check(!q2_monster_infront(&m, side), "does not see exactly sideways");

    /* The cone is wide: a dot threshold of 1230/4096 is about 0.30, so roughly
     * 72 degrees off-axis is still visible. Check a point well off centre. */
    {
        s32 oblique[3];
        oblique[0] = 900;  oblique[1] = 0; oblique[2] = 1000;
        check(q2_monster_infront(&m, oblique), "the cone is wide, not narrow");
    }

    /* Turning around must reverse the answers. */
    m.angles[1] = Q2_ANGLE_180;
    check(!q2_monster_infront(&m, ahead), "turning around loses the target");
    check(q2_monster_infront(&m, behind), "and acquires what was behind");
}

/* ------------------------------------------------------------------------- */
static void test_damage(void)
{
    q2_monster m;

    printf("damage and death\n");

    q2_monster_init(&m);
    m.in_use     = true;
    m.health     = 240;
    m.max_health = 240;
    m.gib_health = -60;

    check(!q2_monster_damage(&m, 100), "survives 100");
    check_eq_i(m.health, 140, "health drops");
    check(!m.dead, "still alive");

    check(q2_monster_damage(&m, 200), "dies when health passes zero");
    check(m.dead, "marked dead");

    /* A dead creature absorbs no further damage. */
    check(!q2_monster_damage(&m, 50), "further damage is ignored");
}

/* ------------------------------------------------------------------------- */
static void test_time_base(void)
{
    printf("AI clock\n");

    /* The AI clock is 10 Hz, distinct from the 25 Hz simulation tick. The
     * drowning timer is what establishes it: 120 units for twelve seconds. */
    check_eq_i(Q2_AI_HZ, 10, "ten AI ticks per second");
    check_eq_i(Q2_AI_SECONDS(12), 120, "twelve seconds is 120 ticks");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC monster framework tests\n\n");

    test_frame_stride();
    test_move_record();
    test_frame_distance();
    test_infront();
    test_damage();
    test_time_base();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
