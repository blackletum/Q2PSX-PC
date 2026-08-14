/*
 * projectile.h — the things a weapon puts into the world.
 *
 * ---------------------------------------------------------------------------
 * Five kinds, five spawners
 * ---------------------------------------------------------------------------
 * Four weapons fire something that travels, and each has its own spawner in the
 * executable. They are reached by class NAME, not by index: the spawner copies
 * a twelve-byte literal to the stack and looks it up (0x8006D008), which is why
 * the names sit in a 12-byte-stride block just before the weapon sound table.
 *
 *     0x800ACB8C  "Grenade2"   0x8004A088   grenade launcher
 *     0x800ACB98  "Grenade3"   0x8004AA6C   hand grenade
 *     0x800ACBA4  "Rocket"     0x8004AF28   rocket launcher
 *     0x800ACBB0  "Spin"       0x8004B678
 *     0x800ACBBC  "BFGBlast"   0x8004BE04   BFG
 *
 * The blaster and hyperblaster share a fifth path, 0x8004D70C, which does not
 * name a class: it allocates a bare entity, gives it a speed of 2560 and eight
 * hull corners read from 0x8009DB1C, and links it. The hyperblaster's bolt sets
 * bit 2 of its flags word and gets those corners rotated into place; the
 * blaster's does not, so its bolt has no orientation. That flag is the only
 * difference between the two projectiles.
 *
 * ---------------------------------------------------------------------------
 * Splash
 * ---------------------------------------------------------------------------
 * Read from the argument pairs at the seven call sites of the radius-damage
 * routine 0x80050810:
 *
 *     grenade   radius 1000   mod 13   0x8004A038, 0x8004A934, 0x8004A9CC
 *     rocket    radius 1300   mod 15   0x8004AE2C
 *     BFG       radius 1300   mod  1   0x8004BBBC, 0x8004BC74
 *
 * The rocket's direct hit is applied separately and first, at full damage with
 * mod 15 (0x8004AE14), and only then does the blast go off — so a direct hit is
 * damage plus splash, exactly as in the lineage.
 *
 * Three different call sites carry the grenade's numbers because a grenade can
 * end three ways: its fuse expiring, touching a target, and being destroyed.
 * All three use the same radius and the same mod.
 *
 * ---------------------------------------------------------------------------
 * The entity list, and what a bolt's numbers mean
 * ---------------------------------------------------------------------------
 * Projectiles live in an 88-byte-record array at 0x800C91C0 which a per-frame
 * sweep at 0x80047C6C walks. Reading that sweep settles two fields the bolt
 * spawner writes that would otherwise have to be guessed at:
 *
 *   +0x1A  is a LIFETIME, not a speed. The sweep subtracts the frame's dt from
 *          it and retires the entity when it reaches zero, so the bolt's 2560
 *          is about eight and a half seconds on the 300 Hz clock.
 *   +0x52  is the VELOCITY, in world units per dt unit. The sweep multiplies it
 *          by the frame's dt to get the move.
 *
 * That means the direction argument the bolt spawner is handed IS its velocity:
 * the blaster's `aim >> 6` gives 64 units per dt and the hyperblaster's
 * `aim >> 7` gives 32, so the hyperblaster's bolt is half the speed of the
 * blaster's. Nothing normalises, and nothing needs to.
 *
 * The grenade and the rocket are different: their spawners take an explicit
 * speed (900 and 20000) and the direction only points. Those two numbers are
 * READ; what is NOT established is the unit they are in, so the port treats
 * them as world units per second and says so.
 *
 * ---------------------------------------------------------------------------
 * What is transcribed and what is modelled
 * ---------------------------------------------------------------------------
 * The speeds, lifetimes, radii, damage and means of death above are read. The
 * per-tick INTEGRATION is not: the spawners hand their entities to the shared
 * mover and this module runs them through the caller's own trace instead.
 * Where a constant is modelled rather than read it says so at its definition.
 */
#ifndef Q2PSX_PROJECTILE_H
#define Q2PSX_PROJECTILE_H

#include "combat.h"
#include "q2psx.h"
#include "weapon.h"

/* ------------------------------------------------------------------------- */
typedef enum q2_proj_kind {
    Q2_PROJ_NONE = 0,
    Q2_PROJ_BOLT,          /* blaster and hyperblaster, 0x8004D70C          */
    Q2_PROJ_GRENADE,       /* "Grenade2", 0x8004A088                        */
    Q2_PROJ_HAND_GRENADE,  /* "Grenade3", 0x8004AA6C                        */
    Q2_PROJ_ROCKET,        /* "Rocket",   0x8004AF28                        */
    Q2_PROJ_BFG            /* "BFGBlast", 0x8004BE04                        */
} q2_proj_kind;

/* Splash, as read. Radius is world units. */
#define Q2_SPLASH_RADIUS_GRENADE 1000
#define Q2_SPLASH_RADIUS_ROCKET  1300
#define Q2_SPLASH_RADIUS_BFG     1300

/*
 * Fuses, in level ticks (300 to the second).
 *
 * The hand grenade's 1650 is READ — it is argument 3 at 0x8004EC3C. The
 * launcher's is not: its spawner stores 380 into the entity at +0x4C
 * (0x8004A430) alongside a second timer, and which of the two is the fuse was
 * not established, so the value is used as one and marked.
 */
#define Q2_FUSE_HAND_GRENADE 1650   /* READ     */
#define Q2_FUSE_GRENADE       380   /* INFERRED */

/*
 * A bolt's lifetime, entity +0x1A written by 0x8004D764 and counted down by
 * the sweep at 0x80047D08. READ. Nothing else in this module has one — a
 * rocket flies until it meets something.
 */
#define Q2_LIFETIME_BOLT     2560

/*
 * Gravity on a thrown grenade. MODELLED: the grenade is handed to the shared
 * entity physics, so it falls at the world's gravity rather than at a constant
 * of its own, and the port therefore takes the simulation's value rather than
 * inventing one. Passed in by the caller.
 */

typedef struct q2_projectile {
    bool in_use;
    q2_proj_kind kind;

    s32  pos[3];
    s32  vel[3];        /* world units per tick, pre-divided by 4096         */
    s16  damage;
    s16  mod;
    s16  splash_radius;

    s32  expires;       /* level tick at which the fuse runs out, 0 = never  */
    s32  owner;         /* index of whoever fired it, -1 for the world       */
    bool bounced;       /* a grenade that has hit something at least once    */

    /*
     * The collision cell this projectile is in, carried across ticks exactly as
     * the mover carries the player's. A rocket that has flown across the map is
     * nowhere near the shooter's cell, so tracing it from the shooter's would
     * ask the hull the wrong question and the rocket would sail through walls.
     * -1 means "unknown", which costs one brute-force sweep and self-corrects,
     * the same contract a freshly spawned entity has (0x80044C74).
     */
    s32  node;
} q2_projectile;

#define Q2_PROJ_MAX 32

typedef struct q2_projectiles {
    q2_projectile p[Q2_PROJ_MAX];
    u32 live;
} q2_projectiles;

void q2_projectiles_init(q2_projectiles *list);

/*
 * Launch what a fire result asked for. Does nothing for a hitscan or rail
 * result. Returns the slot used, or -1 when the list is full — which the
 * console also has to cope with, since its entity pool is finite.
 */
s32 q2_projectile_launch(q2_projectiles *list, const q2_fire_result_v2 *fire,
                         s32 owner, s32 now);

/*
 * What a projectile did on one tick. The caller supplies the trace and applies
 * the outcome, so this module never needs to know about collision hulls.
 */
typedef struct q2_proj_step {
    s32  from[3];
    s32  to[3];         /* where it wants to be                              */
    bool expired;       /* the fuse ran out this tick                        */
} q2_proj_step;

/*
 * Advance one projectile by a tick and report where it wants to move. Apply
 * gravity to the grenades only, in the caller's own units.
 *
 * The caller then traces `from`..`to`, and either commits the move or calls
 * q2_projectile_impact.
 */
void q2_projectile_step(q2_projectiles *list, u32 index, s32 gravity, s32 now,
                        q2_proj_step *out);

/* Commit a move that met nothing. */
void q2_projectile_commit(q2_projectiles *list, u32 index, const s32 to[3]);

/*
 * Resolve an impact at `point`.
 *
 * `hit` is the actor struck, or NULL for the world. Grenades bounce off the
 * world — reflected about `normal` and losing half their speed, which is
 * MODELLED, not read — and detonate on anything else. Everything else
 * detonates on contact.
 *
 * Returns true when the projectile was consumed.
 */
bool q2_projectile_impact(q2_projectiles *list, u32 index,
                          const s32 point[3], const s32 normal[3],
                          q2_actor *attacker, q2_actor *hit,
                          q2_actor **targets, u32 target_count,
                          const q2_combat_rules *rules);

/* How much of its speed a grenade keeps when it bounces. MODELLED. */
#define Q2_GRENADE_BOUNCE_NUM 1
#define Q2_GRENADE_BOUNCE_DEN 2

/* Detonate a projectile where it stands, applying its splash. Used by the fuse
 * path and by a caller that wants to clear the world. */
u32 q2_projectile_detonate(q2_projectiles *list, u32 index,
                           q2_actor *attacker, q2_actor **targets, u32 count,
                           const q2_combat_rules *rules);

#endif /* Q2PSX_PROJECTILE_H */
