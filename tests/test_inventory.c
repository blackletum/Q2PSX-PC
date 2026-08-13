/*
 * test_inventory.c — the player's inventory rules.
 *
 * The values here came out of the executable, so the tests assert the values
 * themselves as well as the behaviour. If a future change transcribes the ammo
 * table wrongly, these fail rather than quietly capping the player at the wrong
 * number.
 */
#include <stdio.h>
#include <string.h>

#include "inventory.h"

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
static void test_tables(void)
{
    printf("tables read from the executable\n");

    /* Base tier, transcribed from vaddr 0x8009C5C8. */
    check_eq_i(q2_ammo_max[Q2_AMMO_TIER_BASE][Q2_AMMO_SHELLS],   100, "base shells");
    check_eq_i(q2_ammo_max[Q2_AMMO_TIER_BASE][Q2_AMMO_BULLETS],  200, "base bullets");
    check_eq_i(q2_ammo_max[Q2_AMMO_TIER_BASE][Q2_AMMO_GRENADES],  50, "base grenades");
    check_eq_i(q2_ammo_max[Q2_AMMO_TIER_BASE][Q2_AMMO_ROCKETS],   50, "base rockets");
    check_eq_i(q2_ammo_max[Q2_AMMO_TIER_BASE][Q2_AMMO_CELLS],    200, "base cells");
    check_eq_i(q2_ammo_max[Q2_AMMO_TIER_BASE][Q2_AMMO_SLUGS],     50, "base slugs");

    /* Each tier must be at least as generous as the one below it. */
    {
        int a, t;
        bool monotonic = true;
        for (a = 0; a < Q2_AMMO_COUNT; a++) {
            for (t = 1; t < Q2_AMMO_TIER_COUNT; t++) {
                if (q2_ammo_max[t][a] < q2_ammo_max[t - 1][a])
                    monotonic = false;
            }
        }
        check(monotonic, "capacity never decreases as the tier rises");
    }

    check_eq_i(Q2_WEAPON_COUNT, 11, "eleven weapons");
    check(q2_weapon_ammo[Q2_WEAPON_BLASTER] < 0, "the blaster uses no ammo");
    check_eq_i(q2_weapon_ammo[Q2_WEAPON_RAILGUN], Q2_AMMO_SLUGS, "railgun uses slugs");
    check_eq_i(q2_weapon_ammo[Q2_WEAPON_BFG], Q2_AMMO_CELLS, "bfg uses cells");

    /* Every weapon must name a real ammo type or none at all. */
    {
        int i;
        bool ok = true;
        for (i = 0; i < Q2_WEAPON_COUNT; i++) {
            if (q2_weapon_ammo[i] >= Q2_AMMO_COUNT)
                ok = false;
            if (!q2_weapon_names[i] || !q2_weapon_names[i][0])
                ok = false;
        }
        check(ok, "every weapon has a name and a valid ammo type");
    }
}

/* ------------------------------------------------------------------------- */
static void test_ammo(void)
{
    q2_inventory inv;

    printf("ammo\n");
    q2_inventory_init(&inv);

    check_eq_i(q2_inventory_add_ammo(&inv, Q2_AMMO_SHELLS, 30), 30, "takes 30 shells");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 30, "and holds them");

    /* Overfilling takes only the remaining space, and the return value is what
     * decides whether a pickup vanishes. */
    check_eq_i(q2_inventory_add_ammo(&inv, Q2_AMMO_SHELLS, 500), 70, "clamps to capacity");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 100, "capped at the base tier");
    check_eq_i(q2_inventory_add_ammo(&inv, Q2_AMMO_SHELLS, 10), 0, "full means nothing taken");

    /* Raising the tier makes room again without granting ammo. */
    inv.ammo_tier = Q2_AMMO_TIER_PACK;
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 100, "a pack does not itself add ammo");
    check_eq_i(q2_inventory_add_ammo(&inv, Q2_AMMO_SHELLS, 500), 100, "but raises the cap");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 200, "pack tier capacity");

    check_eq_i(q2_inventory_add_ammo(&inv, Q2_AMMO_SHELLS, -5), 0, "negative is rejected");
}

/* ------------------------------------------------------------------------- */
static void test_weapons(void)
{
    q2_inventory inv;

    printf("weapons\n");
    q2_inventory_init(&inv);

    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_BLASTER), "starts with the blaster");
    check(!q2_inventory_has_weapon(&inv, Q2_WEAPON_RAILGUN), "and nothing else");
    check(q2_inventory_can_fire(&inv), "the blaster fires with no ammo");

    check(!q2_inventory_select(&inv, Q2_WEAPON_RAILGUN), "cannot select what is not held");

    check(q2_inventory_add_weapon(&inv, Q2_WEAPON_RAILGUN), "picks up the railgun");
    check(!q2_inventory_add_weapon(&inv, Q2_WEAPON_RAILGUN), "a duplicate is refused");
    check(q2_inventory_select(&inv, Q2_WEAPON_RAILGUN), "now selectable");

    check(!q2_inventory_can_fire(&inv), "but not with zero slugs");
    q2_inventory_add_ammo(&inv, Q2_AMMO_SLUGS, 5);
    check(q2_inventory_can_fire(&inv), "fires once it has slugs");

    check(q2_inventory_consume(&inv, 1), "consumes a slug");
    check_eq_i(inv.ammo[Q2_AMMO_SLUGS], 4, "slug count drops");
    check(!q2_inventory_consume(&inv, 99), "cannot consume more than held");
    check_eq_i(inv.ammo[Q2_AMMO_SLUGS], 4, "and a failed consume takes nothing");
}

/* ------------------------------------------------------------------------- */
static void test_health_and_damage(void)
{
    q2_inventory inv;

    printf("health, armour and damage\n");
    q2_inventory_init(&inv);

    check_eq_i(inv.health, 100, "starts at full health");
    check_eq_i(q2_inventory_add_health(&inv, 25, false), 0, "already full");

    inv.health = 50;
    check_eq_i(q2_inventory_add_health(&inv, 25, false), 25, "heals 25");
    check_eq_i(inv.health, 75, "to 75");
    check_eq_i(q2_inventory_add_health(&inv, 100, false), 25, "clamped at the cap");
    check_eq_i(inv.health, 100, "at 100");

    /* Mega-health style pickups exceed the normal cap. */
    check_eq_i(q2_inventory_add_health(&inv, 100, true), 100, "overheal is allowed");
    check_eq_i(inv.health, 200, "up to twice the cap");

    /* Armour absorbs and is consumed. */
    q2_inventory_init(&inv);
    inv.armour = 100;
    {
        s16 to_health = q2_inventory_apply_damage(&inv, 30);
        check(to_health < 30, "armour absorbs part of the damage");
        check(inv.armour < 100, "and is consumed doing so");
        check_eq_i(inv.health, (s16)(100 - to_health), "health drops by the remainder");
    }

    /* Health floors at zero rather than going negative. */
    q2_inventory_init(&inv);
    q2_inventory_apply_damage(&inv, 9999);
    check_eq_i(inv.health, 0, "health floors at zero");
}

/* ------------------------------------------------------------------------- */
static void test_keys(void)
{
    q2_inventory inv;

    printf("keys\n");
    q2_inventory_init(&inv);

    check(!q2_inventory_has_keys(&inv, 0x0001), "starts with no keys");

    q2_inventory_give_key(&inv, 0x0001);
    check(q2_inventory_has_keys(&inv, 0x0001), "holds the key it was given");
    check(!q2_inventory_has_keys(&inv, 0x0003), "a two-bit mask needs both");

    q2_inventory_give_key(&inv, 0x0002);
    check(q2_inventory_has_keys(&inv, 0x0003), "and passes once it has both");

    /* An empty mask is vacuously satisfied, which is what makes an unlocked
     * door work without a special case. */
    check(q2_inventory_has_keys(&inv, 0), "an empty mask always passes");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC inventory tests\n\n");

    test_tables();
    test_ammo();
    test_weapons();
    test_health_and_damage();
    test_keys();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
