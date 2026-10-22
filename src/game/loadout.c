#include "loadout.h"

#include "entity.h"
#include "item.h"
#include "level.h"
#include "population.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * The LOADMAP graph walked from BASE0, taking the lowest level-table index
 * among the unvisited destinations at each step (see loadout.h). BASE2 is
 * left for BASE3 and returned to; POWER2 is left for WASTE1 and returned to
 * from WASTE4 before it opens COMMAND. Only first visits are listed, because
 * a level's supplies are collected once.
 */
const char *const q2_loadout_campaign[] = {
    "BASE0", "BASE1", "BASE2", "BASE3",                      /* unit 1 */
    "JAIL2", "JAIL3", "JAIL4", "JAIL5",                      /* unit 2 */
    "SECURITY", "POWER1", "POWER2",                          /* unit 3 */
    "WASTE1", "WASTE2", "WASTE3", "WASTE4",
    "COMMAND", "LAB", "BIGGUN",                              /* unit 4 */
    "BOSS1", "BOSS2",                                        /* unit 5 */
};
const u32 q2_loadout_campaign_count =
    (u32)(sizeof(q2_loadout_campaign) / sizeof(q2_loadout_campaign[0]));

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

int q2_loadout_campaign_index(const char *map)
{
    u32 i;

    if (!map)
        return -1;
    for (i = 0; i < q2_loadout_campaign_count; i++)
        if (name_eq(map, q2_loadout_campaign[i]))
            return (int)i;
    return -1;
}

bool q2_loadout_keeps(u32 effect)
{
    switch (q2_item_effect_def(effect)->kind) {
    case Q2_ITEMFX_WEAPON:
    case Q2_ITEMFX_AMMO:
    case Q2_ITEMFX_ARMOUR:
    case Q2_ITEMFX_ARMOUR_SHARD:
    case Q2_ITEMFX_POWER_SHIELD:
    case Q2_ITEMFX_ADRENALINE:
    case Q2_ITEMFX_BANDOLIER:
    case Q2_ITEMFX_AMMO_PACK:
        return true;
    default:
        return false;
    }
}

void q2_loadout_collect(q2_inventory *inv, const q2_item_table *items,
                        const u32 *effects, u32 count)
{
    static const s32 nowhere[3] = { 0, 0, 0 };
    q2_entity_world w;
    u32 i;

    if (!inv || !effects)
        return;

    /* A world with nothing in it but the item table: the touch wants one for
     * its liveness check, its sound and its caption, and none of those
     * outlive this call. */
    q2_entity_world_init(&w);
    w.items = items;

    for (i = 0; i < count; i++) {
        if (!q2_loadout_keeps(effects[i]))
            continue;
        q2_ent_events_clear(&w.events);
        q2_item_touch(effects[i], inv, nowhere, &w);
    }

    /* Nothing was just picked up as far as the status bar is concerned. */
    inv->last_item       = 0;
    inv->item_name_until = 0;
}

static s16 keep_share(s16 v)
{
    return (s16)((s32)v * Q2_LOADOUT_KEEP_NUM / Q2_LOADOUT_KEEP_DEN);
}

void q2_loadout_spend(q2_inventory *inv)
{
    int a;

    if (!inv)
        return;
    for (a = 0; a < Q2_AMMO_COUNT; a++)
        inv->ammo[a] = keep_share(inv->ammo[a]);
    inv->armour = keep_share(inv->armour);

    /* Armour worn through loses its class, as the per-frame upkeep says. */
    q2_inventory_armour_upkeep(inv);
}

void q2_loadout_arrive(q2_inventory *inv)
{
    if (!inv)
        return;
    if (inv->health < inv->health_max)
        inv->health = inv->health_max;
}

/* Every kept item effect one level places at SKILL, into OUT. */
static u32 level_effects(const disc *d, const q2_item_table *items,
                         const char *map, s32 skill, u32 *out, u32 max)
{
    char path[64];
    q2_buf buf;
    q2_common_file cf;
    q2_population pop;
    u32 n = 0, g;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK)
        return 0;
    if (q2_common_open(&cf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        return 0;
    }

    if (q2_population_parse(&pop, &cf) == Q2_OK) {
        for (g = 0; g < pop.group_count; g++) {
            q2_pop_group grp;
            q2_pop_place pl;
            u32 k;

            if (!q2_pop_get_group(&pop, g, &grp))
                continue;
            for (k = 0; q2_pop_get_place(&pop, &grp, k, &pl); k++) {
                const q2_item_def *e = q2_item_find(items, (s32)pl.id);

                if (!e || !q2_pop_place_allows_skill(pl.angle_flags, skill))
                    continue;
                if (n < max && q2_loadout_keeps(e->effect))
                    out[n++] = e->effect;
            }
        }
    }
    q2_common_close(&cf);
    return n;
}

u32 q2_loadout_for_level(q2_inventory *inv, const disc *d,
                         const q2_item_table *items, const char *map,
                         s32 skill)
{
    u32 effects[512];
    int at, i;

    if (!inv)
        return 0;
    q2_inventory_init(inv);

    at = q2_loadout_campaign_index(map);
    if (at <= 0 || !d || !items)
        return 0;

    for (i = 0; i < at; i++) {
        u32 n = level_effects(d, items, q2_loadout_campaign[i], skill,
                              effects, (u32)(sizeof(effects) / sizeof(effects[0])));

        q2_loadout_collect(inv, items, effects, n);
        q2_loadout_spend(inv);
    }
    q2_loadout_arrive(inv);
    return (u32)at;
}
