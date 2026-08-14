/*
 * test_light.c — the lighting model's behaviour, where behaviour can be pinned
 * down without a PlayStation.
 *
 * The disc-side claims — that SpaceLights is partitioned by the secondary
 * collision node, that the flare tables match the executable's, that the
 * reciprocal square root table matches — are checked by `q2psx-inspect lights`,
 * because they need a disc. What is checked here is the arithmetic those claims
 * feed: the attenuation curve and its two boundary cases, the three-light
 * ranking, the 16-bit wrapping delta, the colour matrix's transposition, and
 * the GTE's NCS producing the colour that arithmetic implies.
 */
#include <stdio.h>
#include <string.h>

#include "flare.h"
#include "gte.h"
#include "lighting.h"
#include "weapon.h"

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

static void make_light(q2_light *l, s32 x, s32 y, s32 z,
                       u8 r, u8 g, u8 b, u32 radius)
{
    memset(l, 0, sizeof(*l));
    l->x = x; l->y = y; l->z = z;
    l->r = r; l->g = g; l->b = b;
    l->always_255 = 0xFF;
    l->type   = 7;                 /* style 0: lights, no flare */
    l->radius = (u16)radius;
    l->radius_sq       = radius * radius;
    l->inner_radius_sq = 0;
}

/* ------------------------------------------------------------------------- */
static void test_attenuation(void)
{
    q2_light l;
    s16 d[3];

    printf("attenuation\n");

    make_light(&l, 0, 0, 0, 255, 255, 255, 1000);

    /* At the centre the numerator is the whole outer radius squared. */
    d[0] = d[1] = d[2] = 0;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), Q2_LIGHT_ONE,
               "full strength at zero distance");

    /* Outside the radius nothing at all, and the test is on the SQUARE, so a
     * light exactly at its radius is already dark. */
    d[0] = 1000;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), 0,
               "dark exactly at the radius");

    /*
     * The falloff is linear in distance SQUARED, not in distance: at half the
     * radius, three quarters of the light remains rather than half.
     */
    d[0] = 500;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE),
               (Q2_LIGHT_ONE * 3) / 4,
               "three quarters at half the radius");

    /* inner == outer makes the denominator zero, which the original answers
     * with a flat 1.0 rather than a divide. That is what the fallback light
     * relies on. */
    l.inner_radius_sq = l.radius_sq;
    d[0] = 900;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), Q2_LIGHT_ONE,
               "inner == outer is a flat fill");

    /* Inside the inner radius the result EXCEEDS 1.0 and is not clamped here.
     * The entity path clamps it; the flare path grows the flare with it. */
    l.inner_radius_sq = (u32)(250 * 250);
    d[0] = 100;
    check(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE) > Q2_LIGHT_ONE,
          "inside the inner radius the attenuation exceeds one");

    /* The scale argument widens the reach: a flare at 4x sees a light whose
     * squared distance is four times its radius squared. */
    l.inner_radius_sq = 0;
    d[0] = 1500;
    check_eq_i(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE), 0,
               "out of reach at the entity scale");
    check(q2_light_attenuation(&l, d, Q2_LIGHT_ATTEN_SCALE * 4) > 0,
          "in reach at four times the scale");
}

/* ------------------------------------------------------------------------- */
static void test_wrapping_delta(void)
{
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    s16 d[3];

    printf("the delta is 16-bit\n");

    make_light(&l, 100, -200, 300, 255, 255, 255, 1000);
    q2_light_delta(&l, origin, d);
    check_eq_i(d[0], 100,  "x within range");
    check_eq_i(d[1], -200, "y within range");
    check_eq_i(d[2], 300,  "z within range");

    /*
     * 40,000 units away is 40,000 in 32 bits and -25,536 in 16. The original
     * computes the second, so a light far along an axis lights from the WRONG
     * SIDE rather than not at all. Reproducing that is the point.
     */
    make_light(&l, 40000, 0, 0, 255, 255, 255, 32767);
    q2_light_delta(&l, origin, d);
    check_eq_i(d[0], (s16)40000, "a far light wraps rather than saturating");
    check(d[0] < 0, "and comes out on the opposite side");
}

/* ------------------------------------------------------------------------- */
static void test_ranking(void)
{
    q2_light_set set;
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    int i;

    printf("the ranking keeps the three brightest\n");

    q2_light_set_begin(&set);
    check_eq_i(set.count, 0, "no lights to begin with");

    /* Four lights of increasing brightness at the same distance. The three
     * brightest must survive and the dimmest must not. */
    for (i = 1; i <= 4; i++) {
        make_light(&l, 100, 0, 0, (u8)(i * 40), (u8)(i * 40), (u8)(i * 40), 1000);
        q2_light_set_add(&set, origin, &l);
    }

    check(set.count >= 3, "at least three were accepted");

    {
        s16 first  = set.slot[set.rank[0]].total;
        s16 second = set.slot[set.rank[1]].total;
        s16 third  = set.slot[set.rank[2]].total;

        check(first >= second && second >= third,
              "the ranking is in descending brightness");
        check(first > 0, "the brightest slot holds a light");
    }

    /* A light out of range is not offered a slot at all. */
    {
        u32 before = set.count;
        make_light(&l, 100000, 0, 0, 255, 255, 255, 10);
        check(!q2_light_set_add(&set, origin, &l), "a distant light is refused");
        check_eq_i(set.count, before, "and does not bump the count");
    }
}

/* ------------------------------------------------------------------------- */
static void test_env(void)
{
    q2_light_set set;
    q2_light_env env;
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    u8 glow[3] = { 16, 32, 48 };

    printf("the environment the GTE gets\n");

    q2_light_set_begin(&set);
    make_light(&l, 0, 0, 200, 255, 0, 0, 1000);   /* red, ahead      */
    q2_light_set_add(&set, origin, &l);
    make_light(&l, 200, 0, 0, 0, 255, 0, 1000);   /* green, to the side */
    q2_light_set_add(&set, origin, &l);

    q2_light_env_build(&env, &set, Q2_LIGHT_ONE, Q2_LIGHT_ONE, glow);

    check_eq_i(env.active, 2, "two lights survived");

    /*
     * The colour matrix is filled by COLUMN: column j is light j's colour, so
     * row 0 is the three reds. With one red light and one green one, exactly
     * one entry of row 0 and one of row 1 are non-zero, and row 2 is empty.
     */
    check((env.colour.m[0][0] != 0) != (env.colour.m[0][1] != 0),
          "exactly one light contributes red");
    check((env.colour.m[1][0] != 0) != (env.colour.m[1][1] != 0),
          "exactly one light contributes green");
    check_eq_i(env.colour.m[2][0], 0, "neither contributes blue (light 0)");
    check_eq_i(env.colour.m[2][1], 0, "neither contributes blue (light 1)");
    check_eq_i(env.colour.m[0][2], 0, "the third column is empty");

    /* Both intensity fields neutral means the back colour is the glow itself. */
    check_eq_i(env.back[0], glow[0], "back colour red is the glow");
    check_eq_i(env.back[1], glow[1], "back colour green is the glow");
    check_eq_i(env.back[2], glow[2], "back colour blue is the glow");

    /* The directions are unit length in 1.3.12, scaled by the intensity, which
     * at neutral is 2.0 — the product of two 4096s shifted by 11. */
    {
        s32 len = 0;
        int c;
        for (c = 0; c < 3; c++)
            len += (s32)env.dir[0][c] * env.dir[0][c];
        check(len > 6000 * 6000 && len < 10000 * 10000,
              "the light direction is scaled unit length");
    }
}

/* ------------------------------------------------------------------------- */
static void test_normalise(void)
{
    s16 in[3], out[3];
    s32 len;
    int c;

    printf("VectorNormal\n");

    /* The table's first entry is exactly 4096, so an axis-aligned vector
     * normalises exactly. */
    in[0] = 1000; in[1] = 0; in[2] = 0;
    q2_vector_normal(in, out);
    check(out[0] > 4000 && out[0] <= 4096, "an axis vector normalises to one");
    check_eq_i(out[1], 0, "and leaves the other axes alone");

    in[0] = 300; in[1] = -400; in[2] = 1200;
    q2_vector_normal(in, out);
    len = 0;
    for (c = 0; c < 3; c++)
        len += (s32)out[c] * out[c];
    check(len > 3900 * 3900 && len < 4300 * 4300,
          "an oblique vector lands within a few percent of unit");
    check(out[1] < 0, "and keeps its signs");
}

/* ------------------------------------------------------------------------- */
static void test_ncs(void)
{
    gte_state g;
    q2_light_env env;
    q2_light_set set;
    q2_light l;
    s32 origin[3] = { 0, 0, 0 };
    u8 glow[3] = { 0, 0, 0 };

    printf("NCS shades a normal\n");

    gte_init(&g);

    q2_light_set_begin(&set);
    make_light(&l, 0, 0, 400, 255, 255, 255, 1000);
    q2_light_set_add(&set, origin, &l);
    q2_light_env_build(&env, &set, Q2_LIGHT_ONE, Q2_LIGHT_ONE, glow);
    q2_light_env_apply(&env, &g);

    /* A normal pointing straight at the light picks up its colour. */
    g.v[0].x = 0; g.v[0].y = 0; g.v[0].z = 4096;
    gte_ncs(&g);
    check(g.rgb_fifo[2].r > 128, "a normal facing the light is bright");
    check_eq_i(g.rgb_fifo[2].r, g.rgb_fifo[2].g, "a white light stays white");

    /* Facing away, the light matrix's limit-negative clamp zeroes the term and
     * only the back colour is left — which here is black. */
    g.v[0].x = 0; g.v[0].y = 0; g.v[0].z = -4096;
    gte_ncs(&g);
    check_eq_i(g.rgb_fifo[2].r, 0, "a normal facing away gets nothing");

    /* With a back colour, that is exactly what an unlit vertex gets. The
     * hardware stores r << 4 and the colour stage divides by 16 again. */
    gte_set_back_colour(&g, 64, 64, 64);
    gte_ncs(&g);
    check_eq_i(g.rgb_fifo[2].r, 64, "an unlit vertex is the back colour");
}

/* ------------------------------------------------------------------------- */
static void test_flare_styles(void)
{
    printf("flare styles\n");

    /* The five type values on the disc and what they select. */
    check_eq_i(q2_flare_style_of(7),  0, "type 7 has no flare");
    check_eq_i(q2_flare_style_of(15), 1, "type 15 is style 1");
    check_eq_i(q2_flare_style_of(23), 2, "type 23 is style 2");
    check_eq_i(q2_flare_style_of(31), 3, "type 31 is style 3");
    check_eq_i(q2_flare_style_of(39), 4, "type 39 is style 4");

    check_eq_i(q2_flare_size_of(39), Q2_FLARE_BASE_SIZE,
               "bits 6-7 are clear on the disc, so the size is the base");
    check_eq_i(q2_flare_size_of(39 | 0x40), Q2_FLARE_BASE_SIZE * 2,
               "and one step up doubles it");

    check_eq_i(q2_flare_style_table(0)->count, 0, "style 0 draws nothing");
    check_eq_i(q2_flare_style_table(1)->count, 6, "style 1 has six elements");
    check_eq_i(q2_flare_style_table(2)->count, 1, "style 2 is the core alone");
    check_eq_i(q2_flare_style_table(3)->count, 2, "style 3 has two");
    check_eq_i(q2_flare_style_table(4)->count, 5, "style 4 has five");

    /*
     * The styles are nested: 2 is the first element of 3, 3 the first two of 4,
     * 4 the first five of 1. If that ever stops holding, the table was
     * misread.
     */
    {
        const q2_flare_element *s1 = q2_flare_style_table(1)->element;
        const q2_flare_element *s4 = q2_flare_style_table(4)->element;
        const q2_flare_element *s3 = q2_flare_style_table(3)->element;
        const q2_flare_element *s2 = q2_flare_style_table(2)->element;

        check(memcmp(s1, s4, sizeof(q2_flare_element) * 5) == 0,
              "style 4 is style 1's first five");
        check(memcmp(s4, s3, sizeof(q2_flare_element) * 2) == 0,
              "style 3 is style 4's first two");
        check(memcmp(s3, s2, sizeof(q2_flare_element) * 1) == 0,
              "style 2 is style 3's first one");
    }

    /* The first element sits on the light itself; the rest are ghosts along the
     * line back through the screen centre, and most of them are behind it. */
    {
        const q2_flare_element *e = q2_flare_style_table(1)->element;
        int behind = 0, i;

        check_eq_i(e[0].pos, 4096, "the core sits on the light");
        for (i = 1; i < 6; i++)
            if (e[i].pos < 0)
                behind++;
        check(behind == 3, "three of the five ghosts are past the centre");
    }
}

/* ------------------------------------------------------------------------- */
static void test_glow_fade(void)
{
    u8 cur[3] = { 0, 0, 0 };
    const u8 target[3] = { 100, 100, 100 };
    int i;

    printf("the ambient fade\n");

    q2_light_glow_fade(cur, target, 4);
    check_eq_i(cur[0], 25, "a quarter of the way in one step");

    for (i = 0; i < 64; i++)
        q2_light_glow_fade(cur, target, 4);
    check_eq_i(cur[0], 100, "and it does arrive");

    /* Falling is immediate: the signed comparison against the step count is
     * always true for a negative difference. */
    {
        const u8 down[3] = { 10, 10, 10 };
        q2_light_glow_fade(cur, down, 4);
        check_eq_i(cur[0], 10, "a falling glow arrives at once");
    }

    /* A zero step count is read as one rather than dividing by zero. */
    {
        const u8 up[3] = { 200, 200, 200 };
        q2_light_glow_fade(cur, up, 0);
        check_eq_i(cur[0], 200, "zero steps means one step");
    }
}

/* ------------------------------------------------------------------------- */
static void test_dynamic(void)
{
    q2_light_world w;
    s32 pos[3] = { 100, 200, 300 };
    u8  rgb[3] = { 255, 128, 0 };
    int i;

    printf("the runtime light list\n");

    memset(&w, 0, sizeof(w));

    check(q2_light_add_dynamic(&w, pos, rgb, 100, 400, 4, 1),
          "a light is appended");
    check_eq_i(w.dynamic_world_count, 1, "and the list grows");

    {
        const q2_light *l = &w.dynamic_world[0];

        check_eq_i(l->radius, 400, "the radius is the outer one");
        check_eq_i(l->radius_sq, 400 * 400, "which is squared into radiusSq");
        check_eq_i(l->inner_radius_sq, 100 * 100, "and the inner likewise");

        /* Style and size come back out through the same accessors the disc
         * lights use, which is the point of packing them the same way. */
        check_eq_i(q2_flare_style_of(l->type), 4, "the style round-trips");
        check_eq_i(q2_flare_size_of(l->type), Q2_FLARE_BASE_SIZE * 2,
                   "and so does the size shift");
        check_eq_i(l->type & 7, 0,
                   "bits 0-2 are left clear, unlike every light on the disc");
    }

    /* Sixteen and no more; the seventeenth is dropped rather than replacing. */
    for (i = 1; i < Q2_DYNLIGHT_MAX; i++)
        check(q2_light_add_dynamic(&w, pos, rgb, 100, 400, 1, 0), "fills up");
    check(!q2_light_add_dynamic(&w, pos, rgb, 100, 400, 1, 0),
          "the seventeenth is refused");
    check_eq_i(w.dynamic_world_count, Q2_DYNLIGHT_MAX, "and nothing is lost");

    q2_light_world_begin_frame(&w);
    check_eq_i(w.dynamic_world_count, 0, "a new frame empties the list");
}

/* ------------------------------------------------------------------------- */

/*
 * The muzzle flash's radii, against the shift-add chain 0x8004C978 actually
 * emits. The compiler wrote `r*100` as ((r*2 + r) << 3 + r) << 2 and `r*200` as
 * the same with one more shift; this recomputes both the long way and checks the
 * port's multiply agrees at the ends of the range and at a value in the middle.
 *
 * The bases are the two `addiu` immediates, 250 and 700, and one rand draw feeds
 * both radii -- so a shot's inner and outer always move together.
 */
static void test_muzzle_light(void)
{
    static const s32 draws[] = { 0, 1, 16384, 32767 };
    u32 i;

    for (i = 0; i < sizeof(draws) / sizeof(draws[0]); i++) {
        s32 r = draws[i], inner = -1, outer = -1;
        s32 want_in, want_out, t;

        /* r * 100, as the shift-add chain builds it. */
        t = r << 1; t += r; t <<= 3; t += r; t <<= 2;
        want_in = (t >> 15) + 250;

        /* r * 200 -- the same chain with the last shift one wider. */
        t = r << 1; t += r; t <<= 3; t += r; t <<= 3;
        want_out = (t >> 15) + 700;

        q2_weapon_muzzle_light(r, &inner, &outer);
        check_eq_i(inner, want_in,  "muzzle inner radius");
        check_eq_i(outer, want_out, "muzzle outer radius");
    }

    /* The documented ranges, which are what a reader will sanity-check against. */
    {
        s32 lo_in, lo_out, hi_in, hi_out;
        q2_weapon_muzzle_light(0, &lo_in, &lo_out);
        q2_weapon_muzzle_light(32767, &hi_in, &hi_out);
        check_eq_i(lo_in, 250,  "inner at rand 0");
        check_eq_i(hi_in, 349,  "inner at rand 32767");
        check_eq_i(lo_out, 700, "outer at rand 0");
        check_eq_i(hi_out, 899, "outer at rand 32767");
    }

    /* The creature flash: two draws, bigger bases, same colour. */
    {
        s32 in0, out0, in1, out1;
        q2_creature_muzzle_light(0, 0, &in0, &out0);
        q2_creature_muzzle_light(32767, 32767, &in1, &out1);
        check_eq_i(in0,   850, "creature inner at rand 0");
        check_eq_i(in1,  1049, "creature inner at rand 32767");
        check_eq_i(out0, 1200, "creature outer at rand 0");
        check_eq_i(out1, 1599, "creature outer at rand 32767");

        /* Two independent draws: the second must not move the first. */
        q2_creature_muzzle_light(0, 32767, &in0, &out0);
        check_eq_i(in0,   850, "inner ignores the second draw");
        check_eq_i(out0, 1599, "outer ignores the first");
    }

    check(q2_weapon_has_muzzle_light(Q2_WID_MACHINEGUN), "machinegun flashes");
    check(q2_weapon_has_muzzle_light(Q2_WID_CHAINGUN),   "chaingun flashes");
    check(!q2_weapon_has_muzzle_light(Q2_WID_BFG),       "the BFG does not");
}


/*
 * FLKLIGHT's phase. The durations are the operand table's own formulas, and the
 * behaviour worth pinning is the turn-over: a flicker starts lit, flips when its
 * time comes, and a long frame that crosses several turn-overs must not leave
 * the phase behind the clock.
 */
static void test_flklight(void)
{
    q2_flklights set;
    q2_rng rng;
    s32 at[3] = { 10, 20, 30 };
    u8  rgb[3] = { 200, 40, 40 };

    check_eq_i(q2_flklight_on_time(0),      400, "on at rand 0");
    check_eq_i(q2_flklight_on_time(32767),  899, "on at rand 32767");
    check_eq_i(q2_flklight_off_time(0),    1000, "off at rand 0");
    check_eq_i(q2_flklight_off_time(32767),1499, "off at rand 32767");

    memset(&set, 0, sizeof(set));
    q2_rng_seed(&rng, 1);

    check(q2_flklight_add(&set, at, rgb, 7, &rng, 0), "adds");
    check_eq_i(set.count, 1, "one in the set");
    check(set.f[0].lit, "starts lit");

    /* The same light_id again is the same script light, not a second one. */
    check(q2_flklight_add(&set, at, rgb, 7, &rng, 0), "re-entry accepted");
    check_eq_i(set.count, 1, "and does NOT stack");

    /* Nothing turns over before its time. */
    check_eq_i(q2_flklights_tick(&set, &rng, set.f[0].next_toggle - 1), 1,
               "still lit just before the turn-over");

    /* At its time it flips, and the next turn-over moves forward. */
    {
        s32 was = set.f[0].next_toggle;
        check_eq_i(q2_flklights_tick(&set, &rng, was), 0, "dark after flipping");
        check(set.f[0].next_toggle > was, "next turn-over is later");
    }

    /* A jump far past several turn-overs leaves the phase AHEAD of the clock,
     * not behind it — this is what the `while` in the tick is for. */
    q2_flklights_tick(&set, &rng, 100000);
    check(set.f[0].next_toggle > 100000, "phase caught up after a long frame");
}

int main(void)
{
    printf("light model tests\n\n");

    test_attenuation();
    test_wrapping_delta();
    test_ranking();
    test_env();
    test_normalise();
    test_ncs();
    test_flare_styles();
    test_glow_fade();
    test_dynamic();
    test_muzzle_light();
    test_flklight();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
