/*
 * loadout.h — what a player would be carrying on arriving at a level.
 *
 * Launching straight into a level (`--map`) used to hand the player a blaster
 * and nothing else, which makes BOSS2 a different game from the one the
 * campaign builds up to. This derives the inventory the player would plausibly
 * have instead, from the disc and the game's own rules rather than a
 * hand-written table:
 *
 *   1. The campaign order. `q2_loadout_campaign` is the walk the LOADMAP graph
 *      takes from BASE0, the level a new game loads. All 28 LOADMAP calls on
 *      the disc were read out (`q2psx-inspect zonescript`); where a level
 *      offers more than one way on, the walk takes the unvisited destination
 *      with the lowest level-table index, and the level table is itself laid
 *      out in campaign order. That settles both hubs: JAIL3 opens JAIL4, then
 *      JAIL5, then SECURITY; COMMAND opens LAB, then BIGGUN, then BOSS1.
 *
 *   2. The supplies. Every place record in each earlier level's Population,
 *      kept or dropped by the skill filter the spawner itself applies, is
 *      collected through `q2_item_touch` — the real pickup. Carry caps, the
 *      bandolier and ammo-pack tiers, the armour cross-conversion and the
 *      grenade launcher's grenades all come out of the code that does them in
 *      play, not out of a second copy here.
 *
 *   3. The fighting. Nobody arrives with everything a level offered. At each
 *      level's end the player keeps Q2_LOADOUT_KEEP_NUM/Q2_LOADOUT_KEEP_DEN of
 *      their ammo and armour, so what they carry is weighted toward the last
 *      level or two — which is what a player low on rockets in WASTE4 actually
 *      experiences — while weapons, once found, are never lost.
 *
 * Only what persists is collected. Keys belong to the level they open; the
 * timed powerups, health, mega health and the silencer are spent where they
 * are found. The player arrives at full health, holding the strongest weapon
 * they have ammunition for.
 *
 * A level the walk never reaches — MAGDEMO, the arenas, the front end — gets
 * the plain starting inventory.
 */
#ifndef Q2PSX_LOADOUT_H
#define Q2PSX_LOADOUT_H

#include "inventory.h"
#include "itemtable.h"
#include "disc.h"

/* The share of ammo and armour carried out of each level. */
#define Q2_LOADOUT_KEEP_NUM 1
#define Q2_LOADOUT_KEEP_DEN 2

/* The campaign's levels, by directory, in first-visit order. */
extern const char *const q2_loadout_campaign[];
extern const u32         q2_loadout_campaign_count;

/* Where MAP falls in the campaign, or -1 when the walk never reaches it.
 * Compared case-insensitively, as directories are. */
int q2_loadout_campaign_index(const char *map);

/* True for an item effect that outlives the level it is found in. */
bool q2_loadout_keeps(u32 effect);

/* Collect `count` item effects into INV through the real pickup, skipping the
 * ones that do not persist. `items` may be NULL for the built-in table. */
void q2_loadout_collect(q2_inventory *inv, const q2_item_table *items,
                        const u32 *effects, u32 count);

/* A level is over: keep the share of ammo and armour the rule above allows. */
void q2_loadout_spend(q2_inventory *inv);

/* Arrive: full health, and the strongest weapon with ammunition in hand. */
void q2_loadout_arrive(q2_inventory *inv);

/*
 * The whole derivation for one level: reset INV, then collect and spend every
 * campaign level before MAP on the disc, at SKILL (0..2). Returns how many
 * levels contributed — 0 for the first level and for anything off the walk,
 * which leaves INV as `q2_inventory_init` makes it.
 */
u32 q2_loadout_for_level(q2_inventory *inv, const disc *d,
                         const q2_item_table *items, const char *map,
                         s32 skill);

#endif /* Q2PSX_LOADOUT_H */
