#include "cmd_loadout.h"

#include <stdio.h>
#include <stdlib.h>

#include "ident.h"
#include "inventory.h"
#include "itemtable.h"
#include "loadout.h"

/* Short enough that eleven fit on a line. */
static const char *const k_short[Q2_WEAPON_COUNT] = {
    "BL", "SG", "SSG", "MG", "CG", "HG", "GL", "RL", "HB", "RG", "BFG"
};
static const char *const k_armour[Q2_ARMOUR_CLASS_COUNT] = {
    "jacket", "combat", "body"
};
static const char *const k_tier[Q2_AMMO_TIER_COUNT] = {
    "", "bandolier", "pack"
};

int cmd_loadout(disc *d, const char *skill_arg)
{
    q2_build_id id;
    q2_item_table table;
    s32 skill = skill_arg ? (s32)atoi(skill_arg) : 1;
    u32 i;

    if (!d)
        return 1;
    if (q2_identify(d, &id) != Q2_OK ||
        q2_item_table_load(&table, d, &id) != Q2_OK) {
        fprintf(stderr, "no item table for this disc\n");
        return 1;
    }

    printf("Launch loadout at skill %d: what is carried on arriving at each "
           "level,\nkeeping %d/%d of ammo and armour per level played.\n\n",
           (int)skill, Q2_LOADOUT_KEEP_NUM, Q2_LOADOUT_KEEP_DEN);
    printf("  %-9s %3s %-32s %5s %5s %5s %5s %5s %5s  %s\n", "level", "hp", "weapons",
           "shell", "bull", "gren", "rock", "cell", "slug", "armour");

    for (i = 0; i < q2_loadout_campaign_count; i++) {
        const char *map = q2_loadout_campaign[i];
        q2_inventory inv;
        char weapons[64] = "";
        char armour[48]  = "-";
        int  w, at = 0;

        q2_loadout_for_level(&inv, d, &table, map, skill);

        for (w = 0; w < Q2_WEAPON_COUNT; w++)
            if (q2_inventory_has_weapon(&inv, (q2_weapon)w))
                at += snprintf(weapons + at, sizeof(weapons) - (size_t)at,
                               "%s%s", at ? " " : "", k_short[w]);

        if (inv.armour > 0 && inv.armour_class < Q2_ARMOUR_CLASS_COUNT)
            snprintf(armour, sizeof(armour), "%d %s", inv.armour,
                     k_armour[inv.armour_class]);

        printf("  %-9s %3d %-32s %5d %5d %5d %5d %5d %5d  %s%s%s%s\n", map,
               inv.health, weapons,
               inv.ammo[Q2_AMMO_SHELLS], inv.ammo[Q2_AMMO_BULLETS],
               inv.ammo[Q2_AMMO_GRENADES], inv.ammo[Q2_AMMO_ROCKETS],
               inv.ammo[Q2_AMMO_CELLS], inv.ammo[Q2_AMMO_SLUGS], armour,
               (inv.flags & Q2_INV_POWER_SHIELD) ? ", power shield" : "",
               inv.ammo_tier ? ", " : "",
               k_tier[inv.ammo_tier < Q2_AMMO_TIER_COUNT ? inv.ammo_tier : 0]);
    }
    return 0;
}
