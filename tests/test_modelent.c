/*
 * test_modelent.c — model entities: the spawn at 0x8005A778 and the think at
 * 0x8005A5F8.
 *
 * These pin the parts a plausible version gets wrong. All of them are branches
 * or shifts rather than table values, so none of them can be checked by
 * decoding a corpus:
 *
 *   - the clock advances at TWICE the item rate (`sll v1, v1, 1`, 0x8005A618);
 *   - it does NOT wrap against the clip length the way an item's does — it runs
 *     past the end and the entity dies instead;
 *   - the lifetime is clip_length x 10, not clip_length (0x8005A630);
 *   - the scale ramp holds at the ceiling before it falls, because the clamp is
 *     applied to `25 * (320 - t)` and that starts at 8000;
 *   - `fade` defaults to Q2_ONE_12 and not to zero, because the draw MULTIPLIES
 *     it by `scale` (0x8006B298) and the allocator seeds both with 4096.
 *
 * The model bank is a real one only in `q2psx-inspect modelents`; here the
 * entity is driven directly so the file needs no disc.
 */
#include <stdio.h>
#include <string.h>

#include "entity.h"
#include "modelent.h"

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

static void check_eq(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
static void test_names(void)
{
    puts("the two names, in the order the fourth argument selects them");

    check(strcmp(q2_model_ent_name(Q2_MODEL_ENT_EXPLOSION),
                 "Explosion") == 0, "kind 0 is Explosion (0x800ACDF4)");
    check(strcmp(q2_model_ent_name(Q2_MODEL_ENT_HEXPLOSION),
                 "Hexplosion") == 0, "kind 1 is Hexplosion (0x800ACE00)");
    check(q2_model_ent_name(Q2_MODEL_ENT_KIND_COUNT) == NULL,
          "there is no third");
}

static void test_lifetime(void)
{
    puts("the lifetime is the clip length TIMES TEN (0x8005A630)");

    check_eq(q2_model_ent_lifetime(40), 400,
             "BASE0's Explosion is 40 frames and lives 400 units");
    check_eq(q2_model_ent_lifetime(1), 10, "one frame is ten units");
    check_eq(q2_model_ent_lifetime(0), 0,
             "a model with no clip dies on its first think");
}

static void test_scale_ramp(void)
{
    puts("the scale ramp holds full and then falls to nothing (0x8005A65C)");

    /* 25 * (320 - 0) == 8000, clamped to the ceiling. */
    check_eq(q2_model_ent_scale(0), Q2_ONE_12, "full size at t = 0");
    check_eq(q2_model_ent_scale(100), Q2_ONE_12, "still full at t = 100");

    /* It leaves the ceiling when 25 * (320 - t) < 4096, i.e. t > 156.16. */
    check_eq(q2_model_ent_scale(156), Q2_ONE_12, "still full at t = 156");
    check(q2_model_ent_scale(157) < Q2_ONE_12, "and shrinking by t = 157");
    check_eq(q2_model_ent_scale(157), 25 * (320 - 157), "linear once it goes");

    check_eq(q2_model_ent_scale(320), 0, "nothing left at t = 320");
    check_eq(q2_model_ent_scale(400), 0,
             "and the clamp holds it there for the rest of the life");
}

static void test_flash_ramp(void)
{
    puts("the quad's ramp is a different multiplier (0x8005A69C)");

    /* 51 * (320 - t) clamped, then * 1300 >> 12. */
    check_eq(q2_model_ent_flash(0), (Q2_ONE_12 * 1300) >> 12,
             "at the ceiling it is 1300");
    check(q2_model_ent_flash(300) < q2_model_ent_flash(280),
          "and it falls as the clock runs");
    check_eq(q2_model_ent_flash(320), 0, "to nothing at t = 320");

    /* The two ramps are NOT the same curve: 51 vs 25 means the quad leaves the
     * ceiling later than the model does. */
    check(q2_model_ent_scale(200) < Q2_ONE_12,
          "the model is shrinking at t = 200");
    check_eq(q2_model_ent_flash(200), (Q2_ONE_12 * 1300) >> 12,
             "while the quad is still at full size");
}

/* ------------------------------------------------------------------------- */
static void test_think_clock(void)
{
    q2_entity e;
    q2_entity_world w;

    puts("the clock runs at twice an item's and does not wrap");

    memset(&w, 0, sizeof(w));
    w.dt = 6;

    q2_entity_init(&e);
    e.in_use      = true;
    e.think       = q2_model_ent_think;
    e.clip_length = 40;          /* -> a 400-unit life */

    check_eq(e.fade, Q2_ONE_12,
             "a fresh entity starts at full second-scale (0x8006C1B8)");

    q2_model_ent_think(&e, &w);
    check_eq(e.frame, 12, "one think of dt 6 advances the clock by 12, not 6");

    q2_model_ent_think(&e, &w);
    check_eq(e.frame, 24, "and again");

    /* Run it to just under the end. The clock must NOT wrap at 40. */
    while (e.frame < 380 && e.in_use)
        q2_model_ent_think(&e, &w);

    check(e.in_use, "still alive at t = 380");
    check(e.frame >= 380,
          "the clock ran past the clip length instead of wrapping");
    check_eq(e.fade, 0, "and it has shrunk to nothing");

    /* And the next few finish it. */
    while (e.in_use && e.frame < 500)
        q2_model_ent_think(&e, &w);

    check(!e.in_use, "removed once the clock passes clip_length * 10");
}

static void test_think_removes_unanimated(void)
{
    q2_entity e;
    q2_entity_world w;

    puts("a model the bank does not animate dies on its first think");

    memset(&w, 0, sizeof(w));
    w.dt = 6;

    q2_entity_init(&e);
    e.in_use      = true;
    e.think       = q2_model_ent_think;
    e.clip_length = 0;

    q2_model_ent_think(&e, &w);
    check(!e.in_use, "clip_length 0 gives a lifetime of 0, so it goes");
}

static void test_spawn_needs_a_bank(void)
{
    q2_entity_set set;
    s32 at[3] = { 100, 200, 300 };

    puts("a spawn with no model is no entity at all (0x8005A894)");

    memset(&set, 0, sizeof(set));

    check(q2_model_ent_spawn(&set, NULL, Q2_MODEL_ENT_EXPLOSION, at, 0) == NULL,
          "no bank, no entity");
    check_eq(set.count, 0, "and nothing was taken from the pool");

    q2_entity_set_free(&set);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    puts("model entities - 0x8005A778 and 0x8005A5F8\n");

    test_names();
    test_lifetime();
    test_scale_ramp();
    test_flash_ramp();
    test_think_clock();
    test_think_removes_unanimated();
    test_spawn_needs_a_bank();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
