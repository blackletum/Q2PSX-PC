/*
 * inventory.h — the player's health, armour, ammo, weapons and keys.
 *
 * Hammerhead retuned every LENGTH and TIME constant for the console but copied
 * the unitless gameplay values from PC Quake II verbatim. The max-ammo table is
 * the proof: at vaddr 0x8009C5C8 it reads 100/200/50/50/200/50, then the
 * bandolier tier 150/250/50/50/250/75, then the pack tier 200/300/100/100/
 * 300/100 — id's numbers exactly.
 *
 * That is a useful licence. Where a constant is unitless, the PC value is
 * almost certainly right and can be used with confidence. Where a constant is a
 * length or a duration, it was retuned and the PC value is almost certainly
 * wrong. worldscale.h makes the same distinction for the physics.
 *
 * ---------------------------------------------------------------------------
 * Weapons
 * ---------------------------------------------------------------------------
 * Eleven, read from the 12-byte-stride name table at file offset 0x863A8. The
 * order is the weapon id, and it matches PC Quake II's progression.
 *
 * ---------------------------------------------------------------------------
 * Keys
 * ---------------------------------------------------------------------------
 * A single bitfield at 0x800B29D0, which the script's ONKEYDO primitive tests
 * with four masks (all-set, any-set, all-clear, any-clear). Movers test it too
 * for locked doors. So keys are not items in a list — they are bits, and the
 * whole conditional layer of the scripting language is built on them.
 */
#ifndef Q2PSX_INVENTORY_H
#define Q2PSX_INVENTORY_H

#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Ammo                                                                       */
/* ------------------------------------------------------------------------- */
typedef enum q2_ammo {
    Q2_AMMO_SHELLS = 0,
    Q2_AMMO_BULLETS,
    Q2_AMMO_GRENADES,
    Q2_AMMO_ROCKETS,
    Q2_AMMO_CELLS,
    Q2_AMMO_SLUGS,
    Q2_AMMO_COUNT
} q2_ammo;

/* Capacity tiers, in the table's own order. A bandolier raises the first tier
 * to the second; an ammo pack raises it to the third. */
typedef enum q2_ammo_tier {
    Q2_AMMO_TIER_BASE = 0,
    Q2_AMMO_TIER_BANDOLIER,
    Q2_AMMO_TIER_PACK,
    Q2_AMMO_TIER_COUNT
} q2_ammo_tier;

/* [tier][ammo], read out of the executable rather than invented. */
extern const s16 q2_ammo_max[Q2_AMMO_TIER_COUNT][Q2_AMMO_COUNT];

/* ------------------------------------------------------------------------- */
/* Weapons                                                                    */
/* ------------------------------------------------------------------------- */
typedef enum q2_weapon {
    Q2_WEAPON_BLASTER = 0,
    Q2_WEAPON_SHOTGUN,
    Q2_WEAPON_SUPER_SHOTGUN,
    Q2_WEAPON_MACHINEGUN,
    Q2_WEAPON_CHAINGUN,
    Q2_WEAPON_HAND_GRENADE,
    Q2_WEAPON_GRENADE_LAUNCHER,
    Q2_WEAPON_ROCKET_LAUNCHER,
    Q2_WEAPON_HYPERBLASTER,
    Q2_WEAPON_RAILGUN,
    Q2_WEAPON_BFG,
    Q2_WEAPON_COUNT
} q2_weapon;

extern const char *const q2_weapon_names[Q2_WEAPON_COUNT];

/* Which ammo a weapon draws on. The blaster draws none. */
extern const s8 q2_weapon_ammo[Q2_WEAPON_COUNT];

/* ------------------------------------------------------------------------- */
/* Player inventory                                                           */
/* ------------------------------------------------------------------------- */
#define Q2_HEALTH_MAX_DEFAULT 100
#define Q2_ARMOUR_MAX_DEFAULT 200

typedef struct q2_inventory {
    s16 health;
    s16 health_max;
    s16 armour;

    s16 ammo[Q2_AMMO_COUNT];
    u16 weapons;        /* bit per q2_weapon */
    s8  current_weapon;
    u8  ammo_tier;

    u32 keys;           /* the bitfield the script's ONKEYDO tests */
} q2_inventory;

void q2_inventory_init(q2_inventory *inv);

/* Capacity for one ammo type at the current tier. */
s16 q2_inventory_ammo_max(const q2_inventory *inv, q2_ammo type);

/* Add ammo, clamped to capacity. Returns how much was actually taken, which is
 * 0 when already full — that is what decides whether a pickup disappears. */
s16 q2_inventory_add_ammo(q2_inventory *inv, q2_ammo type, s16 amount);

/* Grant a weapon. Returns false if already held. */
bool q2_inventory_add_weapon(q2_inventory *inv, q2_weapon w);
bool q2_inventory_has_weapon(const q2_inventory *inv, q2_weapon w);

/* Switch weapons. Fails if not held. */
bool q2_inventory_select(q2_inventory *inv, q2_weapon w);

/* True when the current weapon can fire: held, and ammo available. */
bool q2_inventory_can_fire(const q2_inventory *inv);

/* Consume one shot's ammo. Returns false when it could not. */
bool q2_inventory_consume(q2_inventory *inv, s16 amount);

/* Health and armour. Armour absorbs a share of incoming damage, as in the
 * original lineage; returns the damage actually applied to health. */
s16 q2_inventory_add_health(q2_inventory *inv, s16 amount, bool allow_overheal);
s16 q2_inventory_apply_damage(q2_inventory *inv, s16 damage);

/* Keys are bits, not items. */
void q2_inventory_give_key(q2_inventory *inv, u32 mask);
bool q2_inventory_has_keys(const q2_inventory *inv, u32 mask);

#endif /* Q2PSX_INVENTORY_H */
