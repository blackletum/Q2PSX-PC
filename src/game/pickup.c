#include "pickup.h"

#include <stdlib.h>
#include <string.h>

static q2_pickup *pickup_push(q2_pickup_set *set)
{
    if (set->count >= set->capacity) {
        u32 want = set->capacity ? set->capacity * 2 : 64;
        q2_pickup *bigger = (q2_pickup *)realloc(set->items, want * sizeof(q2_pickup));
        if (!bigger)
            return NULL;
        set->items    = bigger;
        set->capacity = want;
    }

    {
        q2_pickup *p = &set->items[set->count++];
        memset(p, 0, sizeof(*p));
        return p;
    }
}

q2_result q2_pickups_build(q2_pickup_set *out, const q2_population *pop)
{
    u32 gi;

    if (!out || !pop)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /* Every group can carry a place list, including the path group — the two
     * lists are independent, so this does not skip path groups the way the
     * spawn walk has to. */
    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;
        u32 slot;

        if (!q2_pop_get_group(pop, gi, &g))
            continue;

        for (slot = 0; ; slot++) {
            q2_pop_place place;
            q2_pickup *item;

            if (!q2_pop_get_place(pop, &g, slot, &place))
                break;

            item = pickup_push(out);
            if (!item)
                return Q2_ERR_NO_MEMORY;

            item->pos[0] = place.x;
            item->pos[1] = place.y;
            item->pos[2] = place.z;
            item->id     = place.id;
            item->flags  = place.unk;
        }
    }

    return Q2_OK;
}

void q2_pickups_free(q2_pickup_set *set)
{
    if (!set)
        return;
    free(set->items);
    memset(set, 0, sizeof(*set));
}

void q2_pickups_set_defs(q2_pickup_set *set, const q2_pickup_def *defs, u32 count)
{
    if (!set)
        return;
    set->defs      = defs;
    set->def_count = count;
}

const q2_pickup_def *q2_pickup_find_def(const q2_pickup_set *set, u16 id)
{
    u32 i;

    if (!set || !set->defs)
        return NULL;

    for (i = 0; i < set->def_count; i++) {
        if (set->defs[i].id == id)
            return &set->defs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/*
 * Apply an item's effect. Returns false when it could not be applied, which is
 * what keeps the item in the world: a player at full health should be able to
 * leave a stimpack and come back for it.
 */
static bool apply_pickup(const q2_pickup_def *def, q2_inventory *inv)
{
    if (!def || !inv)
        return false;

    switch (def->kind) {
    case Q2_PICKUP_HEALTH:
        return q2_inventory_add_health(inv, def->amount, def->overheal) > 0;

    case Q2_PICKUP_ARMOUR:
        if (inv->armour >= Q2_ARMOUR_MAX_DEFAULT)
            return false;
        inv->armour = (s16)(inv->armour + def->amount);
        if (inv->armour > Q2_ARMOUR_MAX_DEFAULT)
            inv->armour = Q2_ARMOUR_MAX_DEFAULT;
        return true;

    case Q2_PICKUP_AMMO:
        if (def->subtype < 0 || def->subtype >= Q2_AMMO_COUNT)
            return false;
        return q2_inventory_add_ammo(inv, (q2_ammo)def->subtype, def->amount) > 0;

    case Q2_PICKUP_WEAPON:
        if (def->subtype < 0 || def->subtype >= Q2_WEAPON_COUNT)
            return false;
        {
            bool got = q2_inventory_add_weapon(inv, (q2_weapon)def->subtype);
            /* A weapon already held still yields its ammo, which is why this
             * returns true even when the weapon itself was refused. */
            if (def->amount > 0) {
                s8 ammo = q2_weapon_ammo[def->subtype];
                if (ammo >= 0 &&
                    q2_inventory_add_ammo(inv, (q2_ammo)ammo, def->amount) > 0)
                    got = true;
            }
            return got;
        }

    case Q2_PICKUP_KEY:
        /* Keys are bits and are never refused; a duplicate is harmless. */
        q2_inventory_give_key(inv, (u32)1u << (def->subtype & 31));
        return true;

    case Q2_PICKUP_NONE:
    default:
        return false;
    }
}

u32 q2_pickups_collect(q2_pickup_set *set, const s32 pos[3], q2_inventory *inv)
{
    u32 taken = 0, i;
    const s64 radius_sq = (s64)Q2_PICKUP_RADIUS * Q2_PICKUP_RADIUS;

    if (!set || !pos || !inv)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_pickup *item = &set->items[i];
        s64 dx, dy, dz, d2;
        const q2_pickup_def *def;

        if (item->taken)
            continue;

        dx = (s64)item->pos[0] - pos[0];
        dy = (s64)item->pos[1] - pos[1];
        dz = (s64)item->pos[2] - pos[2];
        d2 = dx * dx + dy * dy + dz * dz;

        if (d2 > radius_sq)
            continue;

        def = q2_pickup_find_def(set, item->id);
        if (!def) {
            /* No table attached, or an id it does not cover. The item is left
             * in place rather than silently vanishing, so a missing definition
             * shows up as an item you cannot collect instead of one that
             * disappears and does nothing. */
            continue;
        }

        if (!apply_pickup(def, inv))
            continue;

        item->taken = true;
        taken++;
    }

    return taken;
}
