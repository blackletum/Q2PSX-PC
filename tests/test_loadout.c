/*
 * test_loadout.c — the inventory handed out on launching into a level.
 *
 * The derivation over the disc is checked by `q2psx-inspect loadout`; these
 * check the rules it is made of, against the built-in item table, so a change
 * to what persists or to the spend share fails here first.
 */
#include <stdio.h>
#include <string.h>

#include "inventory.h"
#include "item.h"
#include "loadout.h"

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

/* The first live effect of KIND, so the tests name kinds rather than ids. */
static u32 effect_of(q2_item_fx_kind kind)
{
    u32 e;

    for (e = Q2_ITEM_EFFECT_FIRST; e <= Q2_ITEM_EFFECT_LAST; e++)
        if (q2_item_effect_def(e)->kind == kind &&
            q2_item_effect_is_live(NULL, e))
            return e;
    return 0;
}

/* ------------------------------------------------------------------------- */
static void test_campaign(void)
{
    printf("campaign order\n");

    check_eq_i(q2_loadout_campaign_count, 20, "twenty single-player levels");
    check_eq_i(q2_loadout_campaign_index("BASE0"), 0, "a new game's level is first");
    check_eq_i(q2_loadout_campaign_index("boss2"), 19, "BOSS2 is last, any case");
    check(q2_loadout_campaign_index("COMMAND") < q2_loadout_campaign_index("LAB"),
          "COMMAND is entered before the LAB it opens");
    check(q2_loadout_campaign_index("WASTE4") < q2_loadout_campaign_index("COMMAND"),
          "POWER2 opens COMMAND only after WASTE4 returns to it");
    check_eq_i(q2_loadout_campaign_index("MAGDEMO"), -1, "the demo map is off the walk");
    check_eq_i(q2_loadout_campaign_index("MATRIX1"), -1, "arenas are off the walk");
}

static void test_keeps(void)
{
    printf("what persists\n");

    check(q2_loadout_keeps(effect_of(Q2_ITEMFX_WEAPON)), "weapons");
    check(q2_loadout_keeps(effect_of(Q2_ITEMFX_AMMO)), "ammo");
    check(q2_loadout_keeps(effect_of(Q2_ITEMFX_ARMOUR)), "armour");
    check(q2_loadout_keeps(effect_of(Q2_ITEMFX_BANDOLIER)), "the bandolier");
    check(!q2_loadout_keeps(effect_of(Q2_ITEMFX_KEY)), "not keys");
    check(!q2_loadout_keeps(effect_of(Q2_ITEMFX_POWERUP)), "not timed powerups");
    check(!q2_loadout_keeps(effect_of(Q2_ITEMFX_HEALTH)), "not health");
}

static void test_collect_and_spend(void)
{
    q2_inventory inv;
    u32 weapon = 0, e;
    u32 list[4];
    const q2_item_fx *fx;

    printf("collect through the real pickup, then spend\n");

    /* The shotgun's effect: the weapon whose subject is the 1-based shotgun. */
    for (e = Q2_ITEM_EFFECT_FIRST; e <= Q2_ITEM_EFFECT_LAST; e++) {
        fx = q2_item_effect_def(e);
        if (fx->kind == Q2_ITEMFX_WEAPON && fx->subject == Q2_WEAPON_SHOTGUN + 1)
            weapon = e;
    }
    check(weapon != 0, "the shotgun has an effect");

    q2_inventory_init(&inv);
    list[0] = weapon;
    list[1] = effect_of(Q2_ITEMFX_KEY);
    list[2] = effect_of(Q2_ITEMFX_POWERUP);
    list[3] = effect_of(Q2_ITEMFX_ARMOUR);
    q2_loadout_collect(&inv, NULL, list, 4);

    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_SHOTGUN), "the shotgun is held");
    check(inv.ammo[Q2_AMMO_SHELLS] > 0, "and came with shells");
    check_eq_i(inv.flags & Q2_KEY_MASK, 0, "the key was left behind");
    check_eq_i(inv.quad_until | inv.invuln_until | inv.enviro_until |
               inv.breather_until, 0, "the powerup was left behind");
    check(inv.armour > 0, "the armour was worn");
    check_eq_i(inv.item_name_until, 0, "no pickup caption is left up");

    {
        s16 shells = inv.ammo[Q2_AMMO_SHELLS];
        s16 armour = inv.armour;

        q2_loadout_spend(&inv);
        check_eq_i(inv.ammo[Q2_AMMO_SHELLS],
                   shells * Q2_LOADOUT_KEEP_NUM / Q2_LOADOUT_KEEP_DEN,
                   "a level's end keeps the share of ammo");
        check_eq_i(inv.armour, armour * Q2_LOADOUT_KEEP_NUM / Q2_LOADOUT_KEEP_DEN,
                   "and of armour");
        check(q2_inventory_has_weapon(&inv, Q2_WEAPON_SHOTGUN),
              "but never a weapon");
    }

    /* Spent down to nothing, the armour class goes with the points. */
    {
        int k;
        for (k = 0; k < 16; k++)
            q2_loadout_spend(&inv);
        check_eq_i(inv.armour, 0, "armour wears out");
        check_eq_i(inv.flags & Q2_INV_ARMOUR_MASK, 0, "and loses its class");
    }

    inv.health = 40;
    q2_loadout_arrive(&inv);
    check_eq_i(inv.health, inv.health_max, "arrival is at full health");
}

static void test_off_the_walk(void)
{
    q2_inventory inv, fresh;

    printf("levels with nothing before them\n");

    q2_inventory_init(&fresh);
    check_eq_i(q2_loadout_for_level(&inv, NULL, NULL, "BASE0", 1), 0,
               "BASE0 has no earlier level");
    check(memcmp(&inv, &fresh, sizeof(inv)) == 0, "so it is the plain start");
    check_eq_i(q2_loadout_for_level(&inv, NULL, NULL, "MATRIX6", 1), 0,
               "an arena gets nothing");
    check(memcmp(&inv, &fresh, sizeof(inv)) == 0, "and is the plain start too");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC launch loadout tests\n\n");

    test_campaign();
    test_keeps();
    test_collect_and_spend();
    test_off_the_walk();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
