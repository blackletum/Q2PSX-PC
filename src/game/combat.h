/*
 * combat.h — firing weapons and delivering damage.
 *
 * ---------------------------------------------------------------------------
 * Where the numbers come from, and where they do not
 * ---------------------------------------------------------------------------
 * The ammo capacities in inventory.h were READ from the executable. The damage
 * and fire-rate figures here were NOT. I searched the region around the ammo
 * table and swept for an eleven-entry table of plausible damage values and
 * found nothing, so the per-weapon stats below are INFERRED from the PC
 * lineage on the strength of a pattern rather than a sighting.
 *
 * That pattern is real and documented in inventory.h — Hammerhead retuned every
 * length and time constant but copied the unitless gameplay values verbatim,
 * and damage is unitless. So the inference is well-founded. It is still an
 * inference, and it is separated from the confirmed data rather than mixed in
 * with it.
 *
 * The table is therefore replaceable: q2_combat_set_stats swaps it wholesale.
 * When the real table is located, dropping it in requires no change here. The
 * built-in set is exposed as q2_weapon_stats_inferred so its status is visible
 * at every use site rather than only in this comment.
 *
 * ---------------------------------------------------------------------------
 * Hitscan and projectiles
 * ---------------------------------------------------------------------------
 * Most weapons trace instantly. A few launch something that travels, and those
 * need per-tick simulation, so the two paths are separate: q2_combat_fire
 * resolves hitscan immediately and returns a projectile request otherwise.
 *
 * Splash damage falls off linearly to zero at its radius, which is the
 * behaviour of this lineage.
 */
#ifndef Q2PSX_COMBAT_H
#define Q2PSX_COMBAT_H

#include "inventory.h"
#include "monster.h"
#include "q2psx.h"
#include "sim.h"

typedef enum q2_fire_mode {
    Q2_FIRE_HITSCAN = 0,   /* resolves this instant                */
    Q2_FIRE_PROJECTILE,    /* spawns something that travels        */
    Q2_FIRE_MELEE          /* very short range, no trace           */
} q2_fire_mode;

typedef struct q2_weapon_stats {
    q2_fire_mode mode;
    s16 damage;        /* per pellet or per hit                     */
    s16 pellets;       /* 1 for a single shot                       */
    s16 spread;        /* lateral scatter, world units at range     */
    s16 refire;        /* AI-clock ticks between shots (10 Hz)      */
    s16 ammo_per_shot;
    s16 splash_damage; /* 0 when the weapon does not explode        */
    s16 splash_radius; /* world units                               */
    s32 speed;         /* projectile speed, world units per tick    */
} q2_weapon_stats;

/*
 * INFERRED, not read from this build. See the header comment.
 * Indexed by q2_weapon.
 */
extern const q2_weapon_stats q2_weapon_stats_inferred[Q2_WEAPON_COUNT];

/* What a shot did. */
typedef struct q2_fire_result {
    bool fired;              /* false when out of ammo or still refiring */
    bool spawn_projectile;
    s32  projectile_dir[3];  /* 1.3.12 unit vector, when spawning        */
    s32  projectile_speed;
    u32  hits;               /* monsters struck by a hitscan shot        */
    s16  damage_dealt;
    s32  killed;             /* index of a monster killed, -1 if none    */
} q2_fire_result;

typedef struct q2_combat {
    const q2_weapon_stats *stats;
    s32 next_fire;      /* AI-clock tick when firing is allowed again */
} q2_combat;

void q2_combat_init(q2_combat *c);

/* Replace the stats table. Passing NULL restores the inferred set. */
void q2_combat_set_stats(q2_combat *c, const q2_weapon_stats *stats);

/*
 * Fire the currently selected weapon from `origin` along `yaw`/`pitch`.
 *
 * `now` is on the AI clock (10 Hz), the same clock the refire delay uses.
 * Monsters are tested as spheres of `hit_radius`, which keeps the test cheap
 * and is accurate enough at the scale these weapons operate.
 */
q2_fire_result q2_combat_fire(q2_combat *c, q2_inventory *inv,
                              const s32 origin[3], s32 yaw, s32 pitch,
                              s32 now,
                              q2_monster *monsters, u32 monster_count,
                              s32 hit_radius);

/*
 * Apply splash damage around a point. Falls off linearly to zero at the
 * radius. Returns the number of monsters hurt.
 */
u32 q2_combat_splash(const s32 centre[3], s16 damage, s16 radius,
                     q2_monster *monsters, u32 monster_count);

/* Distance from a point to a ray, squared, in world units. Exposed because it
 * is the primitive the hitscan test rests on and is worth testing directly. */
s64 q2_combat_ray_dist_sq(const s32 origin[3], const s32 dir[3],
                          const s32 point[3], s64 *out_along);

#endif /* Q2PSX_COMBAT_H */
