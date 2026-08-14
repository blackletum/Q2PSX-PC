#include "inventory.h"

#include "weapontables.h"

#include <string.h>

/*
 * Read out of the executable at vaddr 0x8009C5C8 as s16[18], laid out as three
 * tiers of six ammo types. Not invented, and not copied from a PC build —
 * transcribed from this disc so that a differently-tuned release would be
 * caught by re-reading rather than silently inheriting the wrong numbers.
 */
const s16 q2_ammo_max[Q2_AMMO_TIER_COUNT][Q2_AMMO_COUNT] = {
    { 100, 200,  50,  50, 200,  50 },   /* base      */
    { 150, 250,  50,  50, 250,  75 },   /* bandolier */
    { 200, 300, 100, 100, 300, 100 }    /* pack      */
};

/* From the 12-byte-stride table at file offset 0x863A8. The index is the
 * weapon id the rest of the engine uses. */
const char *const q2_weapon_names[Q2_WEAPON_COUNT] = {
    "Blaster",
    "Shotgun",
    "Super Shotgun",
    "Machinegun",
    "Chaingun",
    "Hand Grenade",
    "Grenade Launcher",
    "Rocket Launcher",
    "Hyperblaster",
    "Railgun",
    "BFG"
};

/* -1 means the weapon needs no ammo. */
const s8 q2_weapon_ammo[Q2_WEAPON_COUNT] = {
    -1,                     /* Blaster          */
    Q2_AMMO_SHELLS,         /* Shotgun          */
    Q2_AMMO_SHELLS,         /* Super Shotgun    */
    Q2_AMMO_BULLETS,        /* Machinegun       */
    Q2_AMMO_BULLETS,        /* Chaingun         */
    Q2_AMMO_GRENADES,       /* Hand Grenade     */
    Q2_AMMO_GRENADES,       /* Grenade Launcher */
    Q2_AMMO_ROCKETS,        /* Rocket Launcher  */
    Q2_AMMO_CELLS,          /* Hyperblaster     */
    Q2_AMMO_SLUGS,          /* Railgun          */
    Q2_AMMO_CELLS           /* BFG              */
};

/* ------------------------------------------------------------------------- */
void q2_inventory_init(q2_inventory *inv)
{
    if (!inv)
        return;

    memset(inv, 0, sizeof(*inv));

    inv->health         = Q2_HEALTH_MAX_DEFAULT;
    inv->health_max     = Q2_HEALTH_MAX_DEFAULT;
    inv->current_weapon = Q2_WEAPON_BLASTER;
    inv->ammo_tier      = Q2_AMMO_TIER_BASE;

    /* The player always starts with the blaster, which needs no ammo. */
    inv->weapons = 1u << Q2_WEAPON_BLASTER;
}

s16 q2_inventory_ammo_max(const q2_inventory *inv, q2_ammo type)
{
    u8 tier;

    if (!inv || type < 0 || type >= Q2_AMMO_COUNT)
        return 0;

    tier = inv->ammo_tier;
    if (tier >= Q2_AMMO_TIER_COUNT)
        tier = Q2_AMMO_TIER_BASE;

    return q2_ammo_max[tier][type];
}

s16 q2_inventory_add_ammo(q2_inventory *inv, q2_ammo type, s16 amount)
{
    s16 cap, space;

    if (!inv || type < 0 || type >= Q2_AMMO_COUNT || amount <= 0)
        return 0;

    cap   = q2_inventory_ammo_max(inv, type);
    space = (s16)(cap - inv->ammo[type]);

    if (space <= 0)
        return 0;               /* already full — the pickup stays put */

    if (amount > space)
        amount = space;

    inv->ammo[type] = (s16)(inv->ammo[type] + amount);
    return amount;
}

bool q2_inventory_has_weapon(const q2_inventory *inv, q2_weapon w)
{
    if (!inv || w < 0 || w >= Q2_WEAPON_COUNT)
        return false;
    return (inv->weapons & (1u << w)) != 0;
}

bool q2_inventory_add_weapon(q2_inventory *inv, q2_weapon w)
{
    if (!inv || w < 0 || w >= Q2_WEAPON_COUNT)
        return false;
    if (q2_inventory_has_weapon(inv, w))
        return false;

    inv->weapons |= (u16)(1u << w);
    return true;
}

bool q2_inventory_select(q2_inventory *inv, q2_weapon w)
{
    if (!inv || !q2_inventory_has_weapon(inv, w))
        return false;

    inv->current_weapon = (s8)w;
    return true;
}

bool q2_inventory_can_fire(const q2_inventory *inv)
{
    s8 ammo;

    if (!inv || inv->current_weapon < 0 || inv->current_weapon >= Q2_WEAPON_COUNT)
        return false;
    if (!q2_inventory_has_weapon(inv, (q2_weapon)inv->current_weapon))
        return false;

    ammo = q2_weapon_ammo[inv->current_weapon];
    if (ammo < 0)
        return true;            /* the blaster never runs dry */

    return inv->ammo[ammo] > 0;
}

bool q2_inventory_consume(q2_inventory *inv, s16 amount)
{
    s8 ammo;

    if (!inv || amount <= 0)
        return false;
    if (inv->current_weapon < 0 || inv->current_weapon >= Q2_WEAPON_COUNT)
        return false;

    ammo = q2_weapon_ammo[inv->current_weapon];
    if (ammo < 0)
        return true;

    if (inv->ammo[ammo] < amount)
        return false;

    inv->ammo[ammo] = (s16)(inv->ammo[ammo] - amount);
    return true;
}

/* ------------------------------------------------------------------------- */
s16 q2_inventory_add_health(q2_inventory *inv, s16 amount, bool allow_overheal)
{
    s16 cap, before;

    if (!inv || amount <= 0)
        return 0;

    /* A mega-health style pickup exceeds the normal cap; an ordinary stimpack
     * does not. That distinction is the pickup's, not the inventory's. */
    cap    = allow_overheal ? (s16)(inv->health_max * 2) : inv->health_max;
    before = inv->health;

    if (inv->health >= cap)
        return 0;

    inv->health = (s16)(inv->health + amount);
    if (inv->health > cap)
        inv->health = cap;

    return (s16)(inv->health - before);
}

s16 q2_inventory_apply_damage(q2_inventory *inv, s16 damage)
{
    s16 absorbed;

    if (!inv || damage <= 0)
        return 0;

    /*
     * Armour takes a share and is consumed by it. The exact split is a tuning
     * value this port has not yet read out of the executable, so a third is
     * used as a placeholder — deliberately simple, and marked so it is not
     * mistaken for a recovered constant.
     */
    absorbed = (s16)(damage / 3);
    if (absorbed > inv->armour)
        absorbed = inv->armour;

    inv->armour = (s16)(inv->armour - absorbed);
    damage      = (s16)(damage - absorbed);

    inv->health = (s16)(inv->health - damage);
    if (inv->health < 0)
        inv->health = 0;

    return damage;
}

void q2_inventory_give_key(q2_inventory *inv, u32 mask)
{
    if (inv)
        inv->flags |= mask;
}

bool q2_inventory_has_keys(const q2_inventory *inv, u32 mask)
{
    if (!inv)
        return false;
    return (inv->flags & mask) == mask;
}

u32 q2_inventory_script_keys(const q2_inventory *inv)
{
    return inv ? (inv->flags & Q2_KEY_MASK) : 0u;
}

s16 q2_inventory_armour_max(const q2_inventory *inv)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    u8 cls;

    if (!inv || !wt)
        return 0;

    cls = inv->armour_class;
    if (cls >= Q2_WT_ARMOUR_CLASSES)
        cls = Q2_WT_ARMOUR_CLASSES - 1;

    return (s16)wt->armour[cls].max_count;
}
