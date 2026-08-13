/*
 * test_combat.c — firing, hit detection and splash damage.
 *
 * The stats table is inferred rather than read from the disc, so these tests
 * deliberately avoid asserting specific damage NUMBERS. Pinning a guessed value
 * would turn an open question into a regression test and make the real figure
 * look like a bug when it arrives. What is asserted is the mechanics, which
 * hold whatever the numbers turn out to be.
 */
#include <stdio.h>
#include <string.h>

#include "combat.h"
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

static void place(q2_monster *m, s32 x, s32 y, s32 z, s16 hp)
{
    q2_monster_init(m);
    m->in_use = true;
    m->pos[0] = x; m->pos[1] = y; m->pos[2] = z;
    m->health = hp;
    m->max_health = hp;
    m->gib_health = -60;
}

/* ------------------------------------------------------------------------- */
static void test_ray_distance(void)
{
    s32 origin[3] = { 0, 0, 0 };
    s32 dir[3];
    s32 point[3];
    s64 along = 0, d2;

    printf("ray distance\n");

    /* Facing +Z as a 1.3.12 unit vector. */
    dir[0] = 0; dir[1] = 0; dir[2] = Q2_ONE_12;

    /* Directly ahead: zero off-axis distance, along == the range. */
    point[0] = 0; point[1] = 0; point[2] = 1000;
    d2 = q2_combat_ray_dist_sq(origin, dir, point, &along);
    check_eq_i(d2, 0, "a point on the ray has zero off-axis distance");
    check_eq_i(along, 1000, "and its along-distance is the range");

    /* Off to one side by 300 at a range of 1000. */
    point[0] = 300; point[1] = 0; point[2] = 1000;
    d2 = q2_combat_ray_dist_sq(origin, dir, point, &along);
    check_eq_i(d2, 300 * 300, "off-axis distance squared");
    check_eq_i(along, 1000, "along-distance unaffected by the offset");

    /* Behind the shooter gives a negative along-distance, which is what lets
     * the caller reject targets behind it without a separate test. */
    point[0] = 0; point[1] = 0; point[2] = -1000;
    q2_combat_ray_dist_sq(origin, dir, point, &along);
    check(along < 0, "a point behind gives a negative along-distance");
}

/* ------------------------------------------------------------------------- */
static void test_firing(void)
{
    q2_combat c;
    q2_inventory inv;
    q2_monster mon;
    q2_fire_result r;
    s32 origin[3] = { 0, 0, 0 };

    printf("firing\n");

    q2_combat_init(&c);
    q2_inventory_init(&inv);
    place(&mon, 0, 0, 2000, 100);

    /* The blaster needs no ammo, so it always fires. */
    r = q2_combat_fire(&c, &inv, origin, 0, 0, 0, &mon, 1, 400);
    check(r.fired, "the blaster fires");

    /* It is a projectile weapon, so nothing is hit this instant. */
    check(r.spawn_projectile, "and requests a projectile");
    check_eq_i(r.hits, 0, "with no immediate hit");

    /* Refire: a second shot at the same instant must be refused. */
    r = q2_combat_fire(&c, &inv, origin, 0, 0, 0, &mon, 1, 400);
    check(!r.fired, "refire delay blocks a second shot");

    /* Far enough in the future it fires again. */
    r = q2_combat_fire(&c, &inv, origin, 0, 0, 100, &mon, 1, 400);
    check(r.fired, "and allows one later");
}

/* ------------------------------------------------------------------------- */
static void test_hitscan(void)
{
    q2_combat c;
    q2_inventory inv;
    q2_monster mon[2];
    q2_fire_result r;
    s32 origin[3] = { 0, 0, 0 };

    printf("hitscan\n");

    q2_combat_init(&c);
    q2_inventory_init(&inv);
    q2_inventory_add_weapon(&inv, Q2_WEAPON_RAILGUN);
    q2_inventory_select(&inv, Q2_WEAPON_RAILGUN);
    q2_inventory_add_ammo(&inv, Q2_AMMO_SLUGS, 10);

    /* One ahead, one behind. */
    place(&mon[0], 0, 0,  2000, 500);
    place(&mon[1], 0, 0, -2000, 500);

    r = q2_combat_fire(&c, &inv, origin, 0, 0, 0, mon, 2, 400);
    check(r.fired, "the railgun fires");
    check_eq_i(r.hits, 1, "hits exactly the one in front");
    check(mon[0].health < 500, "the target in front took damage");
    check_eq_i(mon[1].health, 500, "the one behind was untouched");

    /* Ammo was consumed. */
    check(inv.ammo[Q2_AMMO_SLUGS] < 10, "a slug was spent");

    /* Out of ammo means no shot and no ammo change. */
    inv.ammo[Q2_AMMO_SLUGS] = 0;
    r = q2_combat_fire(&c, &inv, origin, 0, 0, 1000, mon, 2, 400);
    check(!r.fired, "cannot fire with no ammo");
}

/* ------------------------------------------------------------------------- */
static void test_aim(void)
{
    q2_combat c;
    q2_inventory inv;
    q2_monster mon;
    q2_fire_result r;
    s32 origin[3] = { 0, 0, 0 };

    printf("aim\n");

    q2_combat_init(&c);
    q2_inventory_init(&inv);
    q2_inventory_add_weapon(&inv, Q2_WEAPON_RAILGUN);
    q2_inventory_select(&inv, Q2_WEAPON_RAILGUN);
    q2_inventory_add_ammo(&inv, Q2_AMMO_SLUGS, 50);

    /* A target off to the +X side is missed when facing +Z... */
    place(&mon, 4000, 0, 100, 500);
    r = q2_combat_fire(&c, &inv, origin, 0, 0, 0, &mon, 1, 400);
    check_eq_i(r.hits, 0, "misses a target well off-axis");

    /* ...and hit after turning a quarter circle toward it. */
    place(&mon, 4000, 0, 0, 500);
    r = q2_combat_fire(&c, &inv, origin, Q2_ANGLE_90, 0, 1000, &mon, 1, 400);
    check_eq_i(r.hits, 1, "hits it after turning to face it");
}

/* ------------------------------------------------------------------------- */
static void test_splash(void)
{
    q2_monster mon[3];
    u32 hurt;
    s32 centre[3] = { 0, 0, 0 };

    printf("splash\n");

    place(&mon[0], 0,   0, 0,    500);   /* at the centre    */
    place(&mon[1], 0,   0, 500,  500);   /* half a radius out */
    place(&mon[2], 0,   0, 5000, 500);   /* well outside      */

    hurt = q2_combat_splash(centre, 100, 1000, mon, 3);

    check_eq_i(hurt, 2, "hurts the two inside the radius");
    check(mon[2].health == 500, "and misses the one outside");

    /* Falloff is linear, so the nearer target must take strictly more. */
    check(mon[0].health < mon[1].health, "damage falls off with distance");

    /* A target exactly at the radius takes nothing. */
    place(&mon[0], 0, 0, 1000, 500);
    q2_combat_splash(centre, 100, 1000, mon, 1);
    check_eq_i(mon[0].health, 500, "zero damage exactly at the radius");
}

/* ------------------------------------------------------------------------- */
static void test_stats_are_replaceable(void)
{
    q2_combat c;

    printf("stats table\n");

    q2_combat_init(&c);
    check(c.stats == q2_weapon_stats_inferred, "defaults to the inferred set");

    /* The whole point of the indirection: the real table can be swapped in
     * without touching combat.c. */
    {
        static q2_weapon_stats custom[Q2_WEAPON_COUNT];
        memcpy(custom, q2_weapon_stats_inferred, sizeof(custom));
        custom[Q2_WEAPON_BLASTER].damage = 999;

        q2_combat_set_stats(&c, custom);
        check(c.stats == custom, "accepts a replacement table");
        check_eq_i(c.stats[Q2_WEAPON_BLASTER].damage, 999, "and uses it");

        q2_combat_set_stats(&c, NULL);
        check(c.stats == q2_weapon_stats_inferred, "NULL restores the default");
    }
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC combat tests\n\n");

    test_ray_distance();
    test_firing();
    test_hitscan();
    test_aim();
    test_splash();
    test_stats_are_replaceable();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
