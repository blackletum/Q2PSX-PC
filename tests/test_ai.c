/*
 * test_ai.c — creatures noticing, turning, closing and striking.
 *
 * These assert BEHAVIOUR rather than tuning values. The range thresholds are
 * inferred from the world scale, not read from the disc, so pinning them here
 * would convert an open question into a regression test.
 */
#include <stdio.h>
#include <string.h>

#include "ai.h"
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

static void place(q2_monster *m, s32 x, s32 z, s32 yaw)
{
    q2_monster_init(m);
    m->in_use = true;
    m->health = 200;
    m->max_health = 200;
    m->pos[0] = x; m->pos[1] = 0; m->pos[2] = z;
    m->angles[1] = yaw;
    m->ideal_yaw = yaw;
}

/* ------------------------------------------------------------------------- */
static void test_range_bands(void)
{
    q2_monster m;
    s32 t[3] = { 0, 0, 0 };

    printf("range bands\n");

    place(&m, 0, 0, 0);

    t[2] = 100;
    check_eq_i(q2_ai_range(&m, t), Q2_RANGE_MELEE, "very close is melee range");

    t[2] = 3000;
    check_eq_i(q2_ai_range(&m, t), Q2_RANGE_NEAR, "a few thousand units is near");

    t[2] = 8000;
    check_eq_i(q2_ai_range(&m, t), Q2_RANGE_MID, "further out is mid");

    t[2] = 50000;
    check_eq_i(q2_ai_range(&m, t), Q2_RANGE_FAR, "across the level is far");

    /* The bands must be ordered: a nearer target never reports a further band. */
    {
        s32 prev = -1;
        s32 d;
        bool monotonic = true;
        for (d = 100; d < 40000; d += 500) {
            t[2] = d;
            {
                s32 band = (s32)q2_ai_range(&m, t);
                if (band < prev) monotonic = false;
                prev = band;
            }
        }
        check(monotonic, "bands never go backwards as distance grows");
    }
}

/* ------------------------------------------------------------------------- */
static void test_facing(void)
{
    q2_monster m;
    s32 t[3] = { 0, 0, 0 };

    printf("facing\n");

    /* A target on +Z should give a yaw near zero. */
    place(&m, 0, 0, 0);
    t[0] = 0; t[2] = 1000;
    q2_ai_face(&m, t);
    check(m.ideal_yaw < 64 || m.ideal_yaw > Q2_ANGLE_360 - 64,
          "a target on +Z gives a yaw near 0");

    /* A target on +X should give roughly a quarter turn. */
    t[0] = 1000; t[2] = 0;
    q2_ai_face(&m, t);
    check(m.ideal_yaw > Q2_ANGLE_90 - 64 && m.ideal_yaw < Q2_ANGLE_90 + 64,
          "a target on +X gives a yaw near a quarter turn");

    /* Facing the computed yaw must actually put the target in front. */
    m.angles[1] = m.ideal_yaw;
    check(q2_monster_infront(&m, t), "after facing, the target is in front");
}

/* ------------------------------------------------------------------------- */
static void test_turning(void)
{
    q2_monster m;

    printf("turning\n");

    place(&m, 0, 0, 0);
    m.ideal_yaw = Q2_ANGLE_90;

    check(!q2_ai_turn_toward(&m, 100), "a big turn is not finished in one step");
    check(m.angles[1] > 0, "but it made progress");

    /* Repeated steps must converge. */
    {
        int i;
        for (i = 0; i < 100; i++)
            q2_ai_turn_toward(&m, 100);
        check_eq_i(m.angles[1], Q2_ANGLE_90, "and eventually arrives exactly");
    }

    /* The short way round: from just past zero, turning to just under a full
     * circle must go backwards, not most of the way round. */
    place(&m, 0, 0, 32);
    m.ideal_yaw = Q2_ANGLE_360 - 32;
    q2_ai_turn_toward(&m, 16);
    check(m.angles[1] < 32, "takes the short way round through zero");
}

/* ------------------------------------------------------------------------- */
static void test_acquisition(void)
{
    q2_monster m;
    s32 ahead[3] = { 0, 0, 3000 };
    s32 behind[3] = { 0, 0, -3000 };
    s32 miles[3] = { 0, 0, 400000 };

    printf("target acquisition\n");

    place(&m, 0, 0, 0);
    check(q2_ai_find_target(&m, ahead, 7), "notices a player in front");
    check_eq_i(m.enemy, 7, "and records which one");

    /* Already having a target means no re-acquisition. */
    check(!q2_ai_find_target(&m, ahead, 9), "does not re-acquire while engaged");
    check_eq_i(m.enemy, 7, "keeps the original target");

    place(&m, 0, 0, 0);
    check(!q2_ai_find_target(&m, behind, 1), "does not notice a player behind");

    place(&m, 0, 0, 0);
    check(!q2_ai_find_target(&m, miles, 1), "does not notice one out of range");

    /* A dead creature notices nothing. */
    place(&m, 0, 0, 0);
    m.dead = true;
    check(!q2_ai_find_target(&m, ahead, 1), "a dead creature acquires nothing");
}

/* ------------------------------------------------------------------------- */
static void test_thinking(void)
{
    q2_monster m;
    s32 t[3] = { 0, 0, 3000 };
    q2_ai_action a;

    printf("thinking\n");

    /* First tick in sight range reports the sighting. */
    place(&m, 0, 0, 0);
    a = q2_ai_think(&m, t, 0, 0);
    check_eq_i(a, Q2_AI_ACT_SIGHTED, "reports the moment it notices");
    check_eq_i(m.enemy, 0, "and takes the target");

    /* Then it closes the distance. */
    {
        s32 before = m.pos[2];
        int i;
        for (i = 1; i < 10; i++)
            q2_ai_think(&m, t, 0, i);
        check(m.pos[2] > before, "moves toward the target");
    }

    /* In melee range it strikes, on a cadence rather than every tick. */
    place(&m, 0, 0, 0);
    m.enemy = 0;
    t[2] = 400;
    {
        int i, strikes = 0;
        for (i = 0; i < 30; i++) {
            if (q2_ai_think(&m, t, 0, i) == Q2_AI_ACT_MELEE)
                strikes++;
        }
        check(strikes > 0, "strikes in melee range");
        check(strikes < 30, "but not on every single tick");
    }

    /* A creature with no target and nothing in range does nothing. */
    place(&m, 0, 0, 0);
    t[2] = 500000;
    a = q2_ai_think(&m, t, 0, 0);
    check_eq_i(a, Q2_AI_ACT_NONE, "idles when nothing is near");
    check_eq_i(m.enemy, -1, "and acquires nothing");
}

/* ------------------------------------------------------------------------- */
static void test_movement_verbs(void)
{
    q2_monster m;
    s32 t[3] = { 3000, 0, 0 };

    printf("movement verbs\n");

    /* Running toward a target on +X must move along +X. */
    place(&m, 0, 0, 0);
    q2_ai_run(&m, 200, t);
    {
        int i;
        for (i = 0; i < 40; i++)
            q2_ai_run(&m, 200, t);
    }
    check(m.pos[0] > 0, "running toward +X moves along +X");

    /* ai_move does not turn: it is for pain and death animations. */
    place(&m, 0, 0, 0);
    m.ideal_yaw = Q2_ANGLE_90;
    q2_ai_move(&m, 100);
    check_eq_i(m.angles[1], 0, "ai_move does not turn the creature");
    check(m.pos[2] > 0, "but does advance it along its facing");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC AI tests\n\n");

    test_range_bands();
    test_facing();
    test_turning();
    test_acquisition();
    test_thinking();
    test_movement_verbs();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
