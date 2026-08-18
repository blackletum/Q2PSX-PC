/*
 * test_gte.c — conformance tests for the fidelity core.
 *
 * Scope, stated honestly: these tests check the properties that can be
 * established without a PlayStation. They verify the reciprocal table against
 * its defining formula, the divide's documented clamping and flag behaviour,
 * the saturation limits, and the ordering table's draw order.
 *
 * What they do NOT do is compare against values captured from real hardware.
 * That is the only test that can prove bit-exactness, and until it exists the
 * claim in docs/FIDELITY.md is "built to be exact" rather than "measured to be
 * exact". The gap is deliberate and recorded rather than papered over.
 *
 * A vector capture would slot straight into gte_known_vectors() below.
 */
#include <stdio.h>
#include <string.h>

#include "gpu.h"
#include "gte.h"
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
/* The perspective divide                                                     */
/* ------------------------------------------------------------------------- */
static void test_divide(void)
{
    gte_state g;
    u32 h;

    printf("gte_divide\n");
    gte_init(&g);

    /* A zero divisor overflows and clamps rather than trapping. Games relied on
     * the clamp, so this is behaviour, not an error path. */
    g.flag = 0;
    check_eq_i(gte_divide(&g, 320, 0), 0x1FFFF, "sz3 == 0 clamps");
    check(g.flag & GTE_FLAG_DIV_OVERFLOW, "sz3 == 0 raises the overflow flag");

    /* h >= sz3*2 is the documented overflow condition. */
    g.flag = 0;
    check_eq_i(gte_divide(&g, 200, 100), 0x1FFFF, "h == sz3*2 clamps");
    check(g.flag & GTE_FLAG_DIV_OVERFLOW, "h == sz3*2 raises the overflow flag");

    g.flag = 0;
    check_eq_i(gte_divide(&g, 201, 100), 0x1FFFF, "h > sz3*2 clamps");

    /* Just inside the boundary must NOT overflow. */
    g.flag = 0;
    gte_divide(&g, 199, 100);
    check(!(g.flag & GTE_FLAG_DIV_OVERFLOW), "h just under sz3*2 does not overflow");

    /*
     * The result approximates (h << 16) / sz3. The hardware's Newton-Raphson
     * reciprocal is deliberately imprecise, so this asserts closeness, not
     * equality -- being exactly right here would mean the implementation is
     * wrong in an interesting way.
     */
    {
        u32 worst = 0;
        int sz;

        for (sz = 1; sz <= 4096; sz++) {
            u32 exact, got, diff;

            h = (u32)sz;              /* keep h < sz3*2 so it never overflows */
            g.flag = 0;
            got = gte_divide(&g, (u16)h, (u16)sz);

            exact = (u32)(((u64)h << 16) / (u32)sz);
            diff  = got > exact ? got - exact : exact - got;
            if (diff > worst)
                worst = diff;
        }
        printf("  worst deviation from an exact divide over 4096 cases: %u\n", worst);
        check(worst <= 2, "reciprocal stays within 2 units of an exact divide");
    }

    /* Monotonicity: for fixed sz3, a larger h must never give a smaller result. */
    {
        u32 prev = 0;
        bool monotonic = true;
        int i;

        for (i = 1; i < 1000; i++) {
            u32 got;
            g.flag = 0;
            got = gte_divide(&g, (u16)i, 1000);
            if (got < prev)
                monotonic = false;
            prev = got;
        }
        check(monotonic, "the divide is monotonic in h");
    }
}

/* ------------------------------------------------------------------------- */
/* Saturation                                                                 */
/* ------------------------------------------------------------------------- */
static void test_saturation(void)
{
    gte_state g;

    printf("saturation\n");

    /* Screen coordinates clamp to the hardware's 11-bit signed range and raise
     * the corresponding flags. Geometry far off-screen must not wrap around to
     * the other side, which is what an unclamped s16 would do. */
    gte_init(&g);
    gte_set_projection(&g, 320, 160, 120);

    /* Push a vertex far to one side with a tiny Z so the projection explodes. */
    g.v[0].x = 32000;
    g.v[0].y = 0;
    g.v[0].z = 1;
    gte_set_translation(&g, 0, 0, 200);
    gte_rtps(&g, false);

    check(g.sxy[2].x >= -1024 && g.sxy[2].x <= 1023,
          "projected x stays inside the 11-bit range");
    check(g.sxy[2].y >= -1024 && g.sxy[2].y <= 1023,
          "projected y stays inside the 11-bit range");
}

/* ------------------------------------------------------------------------- */
/* Vertex snapping — the wobble itself                                        */
/* ------------------------------------------------------------------------- */
static void test_vertex_snapping(void)
{
    gte_state g;
    int i;
    int distinct = 0;
    s16 last = -32768;

    printf("vertex snapping\n");

    gte_init(&g);
    gte_set_projection(&g, 320, 160, 120);
    gte_set_translation(&g, 0, 0, 1000);

    /*
     * Slide a vertex smoothly and confirm the projected position advances in
     * whole-pixel steps rather than continuously. This is the property that
     * produces the PlayStation's characteristic shimmer, so if it ever stopped
     * holding the look would be wrong even though nothing crashed.
     */
    for (i = 0; i < 200; i++) {
        g.v[0].x = (s16)i;
        g.v[0].y = 0;
        g.v[0].z = 0;
        gte_rtps(&g, false);

        if (g.sxy[2].x != last) {
            distinct++;
            last = g.sxy[2].x;
        }
    }

    check(distinct > 1, "the projected position actually moves");
    check(distinct < 200, "the projected position snaps rather than moving every step");
    printf("  200 sub-pixel input steps produced %d distinct pixel positions\n", distinct);
}

/* ------------------------------------------------------------------------- */
/* Fixed-point trig                                                           */
/* ------------------------------------------------------------------------- */
static void test_trig(void)
{
    printf("fixed-point trig\n");

    check_eq_i(q2_sin12(0), 0, "sin(0) == 0");
    check_eq_i(q2_sin12(Q2_ANGLE_90), Q2_ONE_12, "sin(90 deg) == 1.0 exactly");
    check_eq_i(q2_cos12(0), Q2_ONE_12, "cos(0) == 1.0 exactly");
    check_eq_i(q2_sin12(Q2_ANGLE_180), 0, "sin(180 deg) == 0");
    check_eq_i(q2_cos12(Q2_ANGLE_90), 0, "cos(90 deg) == 0");

    /* Wrapping must be exact in both directions, or a camera that turns past a
     * full circle would jitter. */
    check_eq_i(q2_sin12(Q2_ANGLE_360), q2_sin12(0), "angle wraps at a full turn");
    check_eq_i(q2_sin12(-Q2_ANGLE_90), -Q2_ONE_12, "negative angles wrap correctly");

    /* sin^2 + cos^2 == 1 within fixed-point rounding, swept over the circle. */
    {
        int a;
        s32 worst = 0;

        for (a = 0; a < Q2_ANGLE_360; a++) {
            s32 s = q2_sin12(a), c = q2_cos12(a);
            s32 sum = (s32)(((s64)s * s + (s64)c * c) >> Q2_FRAC_12);
            s32 err = sum > Q2_ONE_12 ? sum - Q2_ONE_12 : Q2_ONE_12 - sum;
            if (err > worst)
                worst = err;
        }
        printf("  worst sin^2+cos^2 error over the circle: %d / 4096\n", worst);
        check(worst <= 2, "the trig identity holds within 2/4096");
    }
}

/* ------------------------------------------------------------------------- */
/* Ordering table                                                             */
/* ------------------------------------------------------------------------- */
typedef struct visit_log {
    u16 order[16];
    int count;
} visit_log;

static void log_visit(const psx_prim *prim, void *user)
{
    visit_log *log = (visit_log *)user;
    if (log->count < 16)
        log->order[log->count++] = prim->otz;
}

static void test_ordering_table(void)
{
    psx_ot ot;
    visit_log log;
    psx_prim *p;

    printf("ordering table\n");

    if (psx_ot_init(&ot, 64, 16) != Q2_OK) {
        printf("  FAIL  could not allocate the ordering table\n");
        g_failures++;
        return;
    }

    /* Far to near: a higher bucket index is more distant and must draw first. */
    p = psx_ot_add(&ot, 10); check(p != NULL, "add to bucket 10");
    p = psx_ot_add(&ot, 30); check(p != NULL, "add to bucket 30");
    p = psx_ot_add(&ot, 20); check(p != NULL, "add to bucket 20");

    memset(&log, 0, sizeof(log));
    psx_ot_walk(&ot, log_visit, &log);

    check_eq_i(log.count, 3, "all three primitives were visited");
    check_eq_i(log.order[0], 30, "the most distant bucket draws first");
    check_eq_i(log.order[1], 20, "then the middle bucket");
    check_eq_i(log.order[2], 10, "then the nearest bucket");

    /*
     * Within one bucket the hardware built its list by prepending, so the LAST
     * primitive added draws FIRST. Getting this backwards produces subtly wrong
     * overlaps in coplanar geometry -- the kind of bug that looks like a
     * texture problem for a week.
     */
    psx_ot_clear(&ot);
    p = psx_ot_add(&ot, 5); p->clut = 1;
    p = psx_ot_add(&ot, 5); p->clut = 2;
    p = psx_ot_add(&ot, 5); p->clut = 3;

    {
        /* A depth is not a bucket index: the table is walked forward, so depth
         * 5 lands five buckets down from the far end. */
        u32 bucket = psx_ot_depth_bucket(&ot, 5);
        const psx_prim *first;

        /* psx_ot_init counts in CONSOLE buckets and the table holds the
         * subdivided ones, so 64 asked for is 64 * PSX_OT_SUBDIV held. */
        check_eq_i((int)bucket, 64 * PSX_OT_SUBDIV - 1 - 5,
                   "depth 5 counts back from the far end");
        first = &ot.prims[ot.bucket_head[bucket]];
        check_eq_i(first->clut, 3, "within a bucket, the last added draws first");
    }

    psx_ot_free(&ot);
}

/* ------------------------------------------------------------------------- */
/* Placeholder for hardware-captured vectors                                  */
/* ------------------------------------------------------------------------- */
static void test_known_vectors(void)
{
    printf("hardware-captured vectors\n");
    printf("  SKIPPED - no capture available.\n");
    printf("  This is the only test that can prove bit-exactness. Until it\n");
    printf("  exists, treat the fidelity claim as 'built to be exact', not\n");
    printf("  'measured to be exact'.\n");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC fidelity conformance tests\n\n");

    test_divide();
    test_saturation();
    test_vertex_snapping();
    test_trig();
    test_ordering_table();
    test_known_vectors();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
