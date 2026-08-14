/*
 * sim.h — the fixed-rate game simulation.
 *
 * This is the first piece of actual *game* rather than data plumbing. It runs
 * the world forward in the original's own time units, using constants read out
 * of the executable rather than invented ones.
 *
 * ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 * The engine does not think in seconds or in frames. It has a `dt` counter in
 * units of 1/300 s, advanced by a per-field amount (6 on PAL, i.e. 50 fields per
 * second) and clamped so a long frame cannot integrate arbitrarily far. The
 * nominal logic step is 12 dt units, which is 25 Hz — the game simulates at half
 * the field rate and renders every field.
 *
 * That matters for faithfulness. Running the simulation at the display rate, or
 * at a fixed 60 Hz, changes jump arcs and monster timing even if every other
 * constant is right. So the tick is in dt units and the host converts, never the
 * other way around.
 *
 * The clamp (Q2_DT_MAX) is a real behaviour, not a safety net: on a slow frame
 * the original ran the world slower rather than taking a huge step, so physics
 * stayed stable and the game visibly slowed down. A port that instead sub-steps
 * to catch up would be more "correct" and less faithful. We clamp.
 *
 * ---------------------------------------------------------------------------
 * Units
 * ---------------------------------------------------------------------------
 * Everything is world units and integers, per worldscale.h. Positions are s32,
 * velocities are s32 in world units per dt unit scaled by Q2_VEL_DIV. No floats
 * anywhere: the original had no FPU, and its rounding is part of how it feels.
 *
 * ---------------------------------------------------------------------------
 * Collision
 * ---------------------------------------------------------------------------
 * Movement runs through trace.[ch] and collision.[ch], both transcribed from
 * the original rather than inferred. In outline: the zone's cells are EMPTY
 * convex volumes joined by portals, a move is clipped in the cell it starts in
 * and then crosses a portal into the next, and the frame's motion is a lift,
 * a slide and a drop rather than a single sweep.
 *
 * Two things about it are worth knowing before reading this file:
 *
 *   - The hull is SecondaryCol, not PrimaryColl, and it is PrimaryColl eroded
 *     by the player's own 286-unit half-extent. A point moving in it is the
 *     player's cube moving in the world.
 *   - `player.pos` is the FEET; the mover works from the entity ORIGIN, 286
 *     above them. q2_sim_origin_y / q2_sim_feet_y convert.
 *
 * ---------------------------------------------------------------------------
 * Combat
 * ---------------------------------------------------------------------------
 * Firing, damage and projectiles live in weapon.[ch], combat.[ch] and
 * projectile.[ch]; simcombat.c is the only place that knows about all three at
 * once plus the collision hull. What this file contributes is the clock and the
 * trace: the level clock the weapon gates read is this same dt counter, 300
 * units to the second, which is what makes the universal 30-tick refire a tenth
 * of a second.
 *
 * ---------------------------------------------------------------------------
 * What this does NOT do yet
 * ---------------------------------------------------------------------------
 * Creatures are not owned here — a caller registers the actors the player can
 * hit — and their own attack figures are still inside the relocated AI modules
 * (openquestions #6). Pickups are not wired.
 */
#ifndef Q2PSX_SIM_H
#define Q2PSX_SIM_H

#include "collision.h"
#include "combat.h"
#include "events_rt.h"
#include "projectile.h"
#include "trace.h"
#include "trigger.h"
#include "q2psx.h"
#include "weapon.h"
#include "worldscale.h"
#include "world.h"

/* ------------------------------------------------------------------------- */
/* Input, as the pad delivered it                                             */
/* ------------------------------------------------------------------------- */
typedef struct q2_input {
    s32  forward;     /* -1024..1023, analogue-style even from digital input */
    s32  strafe;
    s32  yaw_delta;   /* in the 4096-step circle                             */
    s32  pitch_delta;
    bool jump;
    bool crouch;
    bool attack;
} q2_input;

/* ------------------------------------------------------------------------- */
/* Player state                                                               */
/* ------------------------------------------------------------------------- */
typedef struct q2_player {
    s32  pos[3];        /* world units                                        */
    s32  vel[3];        /* world units per dt, pre-divide by Q2_VEL_DIV       */
    s32  yaw, pitch;    /* 4096-step circle                                   */
    s32  view_height;   /* current eye offset, interpolated toward the target */
    bool on_ground;
    bool crouching;
    s32  ground_y;      /* world Y of the surface under the player            */

    /*
     * The mover's own state, carried across ticks exactly as the original
     * carries it in the entity record: the cached collision cell at +0x4E, the
     * flags word at +0x44, the two contact normals, and the slope limit the
     * ground test compares against.
     */
    q2_move_ent  ent;
} q2_player;

/* ------------------------------------------------------------------------- */
/* Combat state                                                               */
/*                                                                            */
/* The level clock the fire functions gate on is the same dt clock the sim     */
/* already runs: 300 units to the second, which is what makes the universal    */
/* 30-tick refire a tenth of a second. So there is one clock here, not two.    */
/*                                                                            */
/* Creatures are NOT owned by the sim. A caller registers an array of actors   */
/* and the sim shoots at them, so the same combat code serves the client, the  */
/* offline `walk` harness and a test with three actors in a line.              */
/* ------------------------------------------------------------------------- */
typedef struct q2_sim_combat {
    q2_inventory    inv;
    q2_combat_rules rules;
    q2_rng          rng;
    q2_projectiles  projectiles;

    int  weapon_id;         /* 1-based, 0 for "no weapon"                    */
    s32  next_fire;         /* level tick the refire gate opens              */
    s16  kick[3];           /* the last shot's view kick, for the renderer   */

    /*
     * How many bullets the chaingun spends per shot. The console reads this
     * from the weapon's spin state (0x8004CAE0 loads it from the view model's
     * runtime object at +0x2C); the view model is not reconstructed yet, so
     * the port holds it here and defaults to one.
     */
    int  chaingun_bullets;

    /* Registered by the caller. */
    q2_actor **targets;
    u32        target_count;

    /* The player as something that can be hurt. Kept in step with `inv`. */
    q2_actor   self;

    /* The last shot, so a caller can draw tracers and play the sound. */
    q2_fire_result_v2 last_shot;
} q2_sim_combat;

/* ------------------------------------------------------------------------- */
/* Simulation                                                                 */
/* ------------------------------------------------------------------------- */
typedef struct q2_sim {
    const q2_world_zone *zone;
    q2_player            player;
    q2_sim_combat        combat;

    /* The level clock the weapon gates and the damage throttles use, in dt
     * units. Advanced by q2_sim_tick alongside the physics. */
    s32                  level_time;

    /*
     * The hull entities move in is SecondaryCol, not PrimaryColl. The zone
     * loader builds two contexts and the mover at 0x80045144 loads the second
     * one (0x800C8FE8, set from "SecondaryCol" at 0x8007B648). PrimaryColl is
     * kept alongside because everything that is not movement — AI, line of
     * sight, spawn validation — uses it through 0x800C8E90.
     */
    q2_collision coll;          /* SecondaryCol: the movement hull            */
    bool         coll_ready;
    q2_collision coll_primary;  /* PrimaryColl: queries other than movement   */
    bool         coll_primary_ready;
    s32          current_node;  /* which cell the player is in, -1 if unknown */

    /* Trigger volumes and the script they fire. Both are optional: a zone with
     * neither still simulates, it just has no gameplay. */
    q2_triggers  triggers;
    bool         triggers_ready;
    q2_events    events;
    q2_event_rt  event_rt;
    bool         events_ready;

    /* Which triggers the player was inside last tick, so a volume fires on
     * ENTRY rather than every tick while standing in it. One bit per trigger. */
    u8          *trigger_inside;
    u32          trigger_capacity;

    /*
     * The same volumes as sweep targets and contents sources — the port's
     * stand-in for the engine's 0x800C9114 table. Built from `triggers`.
     */
    q2_move_target *volumes;
    u32             volume_count;
    q2_move_world   move_world;

    /* Set when a zone gate fires; the caller performs the load. */
    bool         zone_change_pending;
    u32          zone_change_target;

    s32  dt_accum;      /* leftover dt units not yet consumed by a tick       */
    u32  tick_count;
    s32  dt_per_field;  /* 6 on PAL, 5 on NTSC — the build's field rate       */

    /*
     * Downward acceleration. A constant in the original *until* the GAME
     * VARIABLES menu exists: 0x8001C6D0 recomputes the global at 0x800AE924
     * as (slider + 64) >> 2 whenever the variables are enabled, and writes
     * Q2_GRAVITY back when they are not. Initialised to Q2_GRAVITY, so a caller
     * that never opens the menu sees exactly the previous behaviour.
     */
    s32  gravity;
} q2_sim;

/* ------------------------------------------------------------------------- */
/* Feet and origin                                                            */
/* ------------------------------------------------------------------------- */
/*
 * `q2_player.pos` is the FEET, because that is what a StartPos names and what
 * the renderer, HUD and client all expect. The mover works in the entity
 * ORIGIN's frame instead — the centre of the 572-unit cube, Q2_EYE_BASE above
 * the feet — because that is the point the movement hull is built for.
 *
 * This is not a convention the port invented. SecondaryCol is PrimaryColl
 * ERODED BY 286 ON EVERY AXIS, measured over 5,275 axis probes across all 115
 * zones: the difference between the two hulls' free space is exactly 286 in
 * 37% of probes and within two units in 52%, against a distribution that would
 * be flat if they were unrelated. So SecondaryCol is the configuration-space
 * hull of the player's own cube, and a POINT moving in it is that cube moving
 * in the world. Which in turn is why the whole player call chain contains no
 * per-entity bounds access at all: the box is baked into the geometry.
 *
 * It also settles the "is 286 the real player hull or a broad-phase margin"
 * question in FORMATS.md §9.12: it is the real hull.
 *
 * +Y points down, so the origin sits at a SMALLER Y than the feet.
 */
Q2PSX_INLINE s32 q2_sim_origin_y(s32 feet_y) { return feet_y - Q2_EYE_BASE; }
Q2PSX_INLINE s32 q2_sim_feet_y(s32 origin_y) { return origin_y + Q2_EYE_BASE; }

void q2_sim_init(q2_sim *sim, const q2_world_zone *zone, int tick_rate_hz);
void q2_sim_free(q2_sim *sim);

/*
 * Attach the map's trigger volumes and event script.
 *
 * Separate from init because these live in COMMON.DAT, which is per MAP, while
 * the zone geometry is per ZONE. Optional: a sim without them still runs, it
 * just has no gameplay.
 */
q2_result q2_sim_attach_gameplay(q2_sim *sim, const q2_common_file *common);

/* Consume a pending zone change, if any. Returns false when none is queued. */
bool q2_sim_take_zone_change(q2_sim *sim, u32 *out_zone);

/* Place the player, e.g. at a StartPos. */
void q2_sim_spawn(q2_sim *sim, const s32 pos[3], s32 yaw);

/*
 * Advance the world by `elapsed_seconds` of real time.
 *
 * Converts to dt units, clamps as the original did, and runs whole logic ticks.
 * Returns the number of ticks actually run, which is 0 on a fast frame — the
 * caller should still render, interpolating if it wants smoothness.
 */
u32 q2_sim_advance(q2_sim *sim, const q2_input *input, double elapsed_seconds);

/* One logic tick at the nominal step. Exposed so tests can drive it exactly. */
void q2_sim_tick(q2_sim *sim, const q2_input *input, s32 dt);

/* The eye position to render from, accounting for view height. */
void q2_sim_eye(const q2_sim *sim, s32 out_pos[3]);

/* ------------------------------------------------------------------------- */
/* Collision                                                                  */
/*                                                                            */
/* A thin convenience over q2_coll_move for callers that want a swept segment  */
/* rather than an entity move. `fraction` is 1.0.12 and is DERIVED from the    */
/* clipped end point, not carried through the trace: the engine works in exact */
/* rationals and never forms a fraction at all.                               */
/* ------------------------------------------------------------------------- */
typedef struct q2_trace {
    s32  fraction;      /* 1.0.12: 4096 means the whole move succeeded */
    s32  end[3];
    s32  normal[3];     /* 1.3.12 unit normal, valid when hit          */
    s32  node;          /* the cell the trace ended in, -1 if none     */
    s32  contents;      /* that cell's contents id                     */
    bool hit;
} q2_trace;

void q2_sim_trace(q2_sim *sim, const s32 start[3], const s32 end[3],
                  q2_trace *out);

/* ------------------------------------------------------------------------- */
/* Combat                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Register the creatures the player can shoot. The sim borrows the array; the
 * caller keeps ownership and may pass NULL to clear it.
 */
void q2_sim_set_targets(q2_sim *sim, q2_actor **targets, u32 count);

/* Reset the combat state to a freshly spawned player: the blaster, no other
 * weapon, an empty projectile list. Called by q2_sim_init. */
void q2_sim_combat_init(q2_sim *sim);

/* Advance the projectiles by one tick and resolve what they hit. Called by
 * q2_sim_tick after movement, so a rocket meets the wall the player is
 * standing against rather than the one they were standing against. */
void q2_sim_combat_tick(q2_sim *sim);

/* Give a weapon and select it if nothing better is held, exactly as the pickup
 * path at 0x80037E28 does: the switch happens only when the blaster is out. */
bool q2_sim_give_weapon(q2_sim *sim, int weapon_id);

/* Step to the next or previous usable weapon. Returns false when there is
 * nothing to switch to, which is what the original's cycle reports. */
bool q2_sim_cycle_weapon(q2_sim *sim, int dir);

/*
 * Fire the held weapon.
 *
 * Hitscan and rail shots are traced against the zone's own hull, so a bullet
 * stops at a wall rather than reaching through it, and the surviving fraction
 * is what limits which creatures it can reach. Projectiles are put in the
 * sim's list and advanced by q2_sim_tick.
 *
 * Returns what was fired, which is also left in `sim->combat.last_shot`.
 */
q2_fire_result_v2 q2_sim_fire(q2_sim *sim);

/* Hurt the player, going through the same damage path everything else does. */
q2_damage_result q2_sim_hurt_player(q2_sim *sim, q2_actor *attacker,
                                    s16 damage, s16 mod, const s32 point[3]);

/* The aim vector the fire functions read, built from the current view. 1.3.12,
 * which is the scale every weapon's own shift assumes. */
void q2_sim_aim(const q2_sim *sim, s16 out[3]);

#endif /* Q2PSX_SIM_H */
