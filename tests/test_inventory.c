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

#include "combat.h"      /* the rules and mods the armour stage takes */
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

    /*
     * ARMOUR ABSORBS BY THE TABLE, NOT A THIRD. These used to assert only that
     * "part" was absorbed, because the split was a `damage / 3` placeholder.
     * It is 0x80057C7C's `(bias + protection * damage) >> 12` over the records
     * at 0x8009C5EC, and every case below disagrees with a third.
     */
    {
        q2_combat_rules rules, easy;
        s16 to_health, easy_hit, normal_hit;

        q2_combat_rules_default(&rules);      /* skill 1: the 2048 bias */
        easy       = rules;
        easy.skill = 0;                       /* 0x80057C20: the 4095 bias */

        q2_inventory_init(&inv);
        inv.armour       = 100;
        inv.armour_class = Q2_ARMOUR_BODY;
        to_health = q2_inventory_apply_damage(&inv, 100, Q2_MOD_BULLET, &rules);
        check_eq_i(inv.armour, 100 - ((2048 + 3277 * 100) >> 12),
                   "body armour spends (2048 + 3277*100) >> 12 = 80, not 33");
        check_eq_i(to_health, 20, "so 20 of 100 reach health");
        check_eq_i(inv.health, 80, "and health falls by exactly that");

        /* Jacket's energy column (+4) is zero, so a bolt goes straight in. */
        q2_inventory_init(&inv);
        inv.armour       = 100;
        inv.armour_class = Q2_ARMOUR_JACKET;
        to_health = q2_inventory_apply_damage(&inv, 30, Q2_MOD_ENERGY_BOLT,
                                              &rules);
        check_eq_i(inv.armour, 100, "jacket spends nothing on energy");
        check_eq_i(to_health, 30, "so the whole bolt reaches health");

        /* 0x80058358: mod 8 jumps past both stages. */
        q2_inventory_init(&inv);
        inv.armour       = 100;
        inv.armour_class = Q2_ARMOUR_BODY;
        to_health = q2_inventory_apply_damage(&inv, 30, Q2_MOD_NO_ARMOUR,
                                              &rules);
        check_eq_i(inv.armour, 100, "mod 8 leaves body armour untouched");
        check_eq_i(to_health, 30, "and takes all of it from health");

        /* The bias is the SKILL's (0x80057C10): jacket's 1229/4096 of one
         * point rounds up to a whole point on Easy and to nothing above it. */
        q2_inventory_init(&inv);
        inv.armour       = 100;
        inv.armour_class = Q2_ARMOUR_JACKET;
        easy_hit = q2_inventory_apply_damage(&inv, 1, Q2_MOD_BULLET, &easy);
        q2_inventory_init(&inv);
        inv.armour       = 100;
        inv.armour_class = Q2_ARMOUR_JACKET;
        normal_hit = q2_inventory_apply_damage(&inv, 1, Q2_MOD_BULLET, &rules);
        check(easy_hit == 0 && normal_hit == 1,
              "a 1-point jacket hit is saved at skill 0 and not at skill 1");

        /* Power armour runs first (0x80058364) and armour sees what is left. */
        q2_inventory_init(&inv);
        inv.flags               = Q2_INV_POWER_SHIELD;
        inv.ammo[Q2_AMMO_CELLS] = 100;
        to_health = q2_inventory_apply_damage(&inv, 30, Q2_MOD_BULLET, &rules);
        check_eq_i(to_health, 10, "the power shield takes two thirds first");
        check_eq_i(inv.ammo[Q2_AMMO_CELLS], 90,
                   "spending one cell per two points it saved");
    }

    /*
     * AND HEALTH IS NOT FLOORED. This asserted a clamp at zero; the client arm
     * stores the plain difference (0x800583EC..0x800583F8), which is what lets
     * a player's health reach corpse_think's -40 gib test at all.
     */
    q2_inventory_init(&inv);
    q2_inventory_apply_damage(&inv, 9999, Q2_MOD_BULLET, NULL);
    check_eq_i(inv.health, 100 - 9999, "health goes as far below zero as the hit");

    /*
     * AND THE DAMAGE FUNCTION'S CLIENT ARM STORES THE SAME. -9999 is
     * 0x800629B4's floor, inside T_Damage, and 0x800582C8 never lets a client
     * target reach T_Damage. q2_combat_damage applied it to a player anyway,
     * so the two disagreed for any hit that took a player past -9999.
     */
    {
        q2_actor     a;
        q2_inventory twin;

        q2_inventory_init(&inv);
        q2_inventory_apply_damage(&inv, 20000, Q2_MOD_BULLET, NULL);

        q2_inventory_init(&twin);
        q2_actor_init(&a);
        q2_actor_from_player(&a, &twin, NULL);
        q2_combat_damage(NULL, &a, 20000, Q2_MOD_BULLET, NULL, NULL);
        q2_actor_to_player(&a, &twin);
        check_eq_i(twin.health, inv.health,
                   "the damage function's client arm has no floor either");
    }
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
