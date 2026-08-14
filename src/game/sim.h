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
 * (openquestions #6).
 *
 * ---------------------------------------------------------------------------
 * Entities
 * ---------------------------------------------------------------------------
 * Items ARE owned here, because the engine owns them the same way: one entity
 * set, one think sweep, run at the end of the tick so an item's touch test sees
 * where the player actually ended up. `q2_sim_attach_items` spawns a map's
 * Population place records; without it the set is empty and the sweep is free.
 * See entity.h and item.h.
 */
#ifndef Q2PSX_SIM_H
#define Q2PSX_SIM_H

#include "collision.h"
#include "combat.h"
#include "effect.h"
#include "entity.h"
#include "events_rt.h"
#include "item.h"
#include "projectile.h"
#include "trace.h"
#include "trigger.h"
#include "userfuncs.h"
#include "q2psx.h"
#include "weapon.h"
#include "worldscale.h"
#include "world.h"

/* The map's CastList, so an item's model can be resolved at spawn. Declared
 * rather than included: sim.h has no other use for the model format. */
struct q2_model_bank;

/* ------------------------------------------------------------------------- */
/* Input, as the pad delivered it                                             */
/* ------------------------------------------------------------------------- */
/*
 * This is the 12-byte struct 0x80019154 fills and 0x8003A1C8 consumes, not a
 * shape the port chose. Two things about it are worth stating up front:
 *
 *   - The two movement axes are SIGNED PAD AXES at +-Q2_INPUT_FULL, because
 *     the wish velocity is `(maxspeed * axis) >> 7` and that reaches exactly
 *     maxspeed at 128. Feeding it +-1024 makes the player eight times too fast.
 *   - The two look axes are not angle deltas. They are a target TURN RATE,
 *     eased, and the angle integrates from the rate. Feeding a delta straight
 *     in produces a turn that keeps going after the stick is released.
 *
 * There is no crouch here, and that is a finding rather than an omission: see
 * Q2_ENT_INCROUCH in worldscale.h.
 */
/*
 * Every bit is now accounted for. They are produced by 0x80019154's shared tail
 * at 0x80019A04, which turns each of four configurable PAD MASKS into up to
 * three bits — a press edge, a held bit, and a held-and-was-held bit — plus
 * four the arms set directly. See pad.h for which pad button feeds which mask
 * under each of the nine control styles.
 */
#define Q2_BTN_SWIM_UP   0x00200000u /* bit 21, jump mask HELD    @0x8003AB88 */
#define Q2_BTN_JUMP      0x00400000u /* bit 22, jump mask PRESSED @0x8003A918 */
#define Q2_BTN_ATTACK_PRESS 0x00800000u /* bit 23, fire mask pressed          */
#define Q2_BTN_ATTACK_REPEAT 0x01000000u/* bit 24, fire mask held since last  */
#define Q2_BTN_ATTACK    0x02000000u /* bit 25, fire mask held                */
#define Q2_BTN_WEAP_NEXT 0x04000000u /* bit 26, 0x8004ECD8                    */
#define Q2_BTN_WEAP_PREV 0x08000000u /* bit 27, 0x8004ED00                    */
#define Q2_BTN_PAUSE     0x10000000u /* bit 28, START's press edge @0x80019190*/
#define Q2_BTN_LOOK_DOWN 0x20000000u /* bit 29, the look-down button held     */
#define Q2_BTN_LOOK_UP   0x40000000u /* bit 30, the look-up button held       */
#define Q2_BTN_MOVING    0x80000000u /* bit 31, the FORWARD axis is deflected */

typedef struct q2_input {
    s16  forward;     /* -Q2_PAD_FULL..+Q2_PAD_FULL                           */
    s16  side;
    s16  pitch;       /* look axis, NOT an angle delta — see above            */
    s16  yaw;
    u32  buttons;     /* Q2_BTN_*                                             */

    /*
     * Bit 25 of `buttons`, kept as its own field because the fire path takes a
     * bool. q2_pad_read sets both and they cannot disagree.
     */
    bool attack;
} q2_input;

/* ------------------------------------------------------------------------- */
/* Player state                                                               */
/* ------------------------------------------------------------------------- */
typedef struct q2_player {
    s32  pos[3];        /* world units                                        */
    s32  vel[3];        /* world units per dt, pre-divide by Q2_VEL_DIV       */

    /*
     * The view angles at entity+0xE6: pitch, yaw, roll in the 4096-step circle.
     * `yaw` and `pitch` are kept as their own names because everything outside
     * the sim reads them, and `roll` is derived from the wish velocity rather
     * than from input.
     */
    s32  yaw, pitch, roll;

    /*
     * The wish velocity at client+0x04: side, vertical, forward. The engine
     * eases these toward the stick and then rotates them into `vel`, so this is
     * the state that carries a player's momentum intent across ticks.
     */
    s16  wish[3];

    /* The look rates at client+0x0A and client+0x0C. The angles integrate from
     * these; the stick only asks for a rate. */
    s16  pitch_rate, yaw_rate;

    /*
     * The impulse accumulator at entity+0x2F8 and its arming bit. Everything
     * that wants to change velocity from outside the integrator posts here.
     */
    s16  impulse[3];
    bool impulse_armed;

    /*
     * The frame delta at entity+0xEC, and the reason it has to be state.
     *
     * The mover's three moves consume this and the integrator writes it, in that
     * order, inside one function (0x80045B70 reads it, 0x800461DC writes it). So
     * a tick moves by the delta the PREVIOUS tick computed. Keeping it in the
     * player rather than as a local is what reproduces that one-tick offset.
     */
    s16  frame_delta[3];

    s16  jump_hold;     /* client+0x20                                        */
    s32  view_height;   /* entity+0xF6, eased toward the flags' target        */
    bool on_ground;
    bool crouching;     /* derived from the env flags, not from input         */
    s32  ground_y;      /* world Y of the surface under the player            */

    /*
     * The entity's SECOND flag word, entity+0x10C. Three of its bits reach the
     * movement code and they are not interchangeable with the +0x98 word above:
     *
     *   0x1000  set from the GAME VARIABLES flag at 0x800B2AA4 (0x8003A41C).
     *           It turns gravity OFF at 0x80045EE0, blocks the jump at
     *           0x8003A8F0, blocks the view recentre and the water-exit jump —
     *           i.e. it is the fly cheat, not merely a movement-basis switch.
     *   0x4000  the impulse accumulator is armed; `impulse_armed` mirrors it.
     *   0x8000  set by the item path; not read by movement.
     */
    u32  ent2_flags;

    /*
     * The view recentre, 0x8003A780. Not a setting and not a key: holding the
     * two LOOK buttons together for a second frame arms it, and it then walks
     * the pitch to zero and disarms itself.
     */
    u8   look_hist;     /* client+0x24, a shift register of "a look button
                         * was held", one bit per tick                       */
    bool recentring;    /* client+0x25                                       */
    s16  autocentre;    /* client+0x26, cleared whenever the pitch axis moves */

    /* Fall damage's own outputs: a view kick and how long it lasts. */
    s16  fall_value;    /* client+0x9A, in the 4096 circle                    */
    s32  fall_time;     /* client+0xD4, a level-clock deadline                */

    /*
     * The other two view kicks 0x80038260 composes, kept here because it reads
     * all three out of the client record:
     *
     *   `kick` / `kick_time`   client+0xA0..0xA4 and +0xCC — pitch, yaw and
     *                          roll, decaying over 30 ticks. What a shot posts.
     *   `hurt_kick` / `pain_time`  client+0x9C, +0x9E and +0xD0 — pitch and
     *                          roll from taking damage, decaying over 150.
     *
     * `pain_time` is genuinely one field doing two jobs: 0x8003AF50 also uses
     * it as the pain SOUND throttle and sets it to now+210, which is longer
     * than the kick's own 150 — so for the first 60 ticks the kick is scaled by
     * more than one rather than less. That is the console's arithmetic, not a
     * bug in this transcription.
     */
    s16  kick[3];
    s32  kick_time;
    s16  hurt_kick[2];
    s32  pain_time;

    /*
     * entity+0x10A and client+0x50: last tick's health and armour, compared at
     * 0x8003AE10 to decide whether the player was hurt this tick. The engine
     * has no "was damaged" flag — it diffs these two, which is why a heal and a
     * hit in the same tick cancel and make no sound.
     */
    s16  prev_health, prev_armour;

    s32  footstep_time; /* client+0x8C                                        */
    int  foot;          /* client+0x90, which of the two sounds is next       */

    /*
     * Which control style the pad is configured for, 0..8 — see pad.h. Styles
     * BELOW Q2_PAD_STYLE_EASED_FROM set the look rate outright and styles from
     * it up ease it (0x8003A670 is `slti style, 6` branching to the eased arm
     * when the test FAILS). The port had this comparison inverted, which made
     * the mouse and stick styles glide and the digital ones snap.
     */
    int  look_scheme;

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
     * The environment mask each volume asserts while you are standing in it —
     * the port's form of the volume dispatcher at 0x80027E64.
     *
     * INWATER, UNDERWATER, INCROUCH, INLOWCROUCH, INLAVA and DONTJUMP are
     * `UserFuncs` primitives whose whole body is one OR into entity+0x98. So
     * rather than execute a volume's record 25 times a second to reach six
     * one-line functions, this scans each record's CALL items ONCE at attach
     * and keeps the bits they would set.
     *
     * That is a divergence in mechanism and not in behaviour, and it is the
     * honest one: a level-triggered re-execution would also re-run whatever
     * else the record does, which is not what the dispatcher's predicate arm
     * amounts to.
     *
     * NULL when the map has no UserFuncs table, in which case the flags fall
     * back to `env_flags` alone.
     */
    u32         *volume_env;
    q2_userfuncs userfuncs;
    bool         userfuncs_ready;

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

    /*
     * Environment flags asserted by the CALLER, OR'd in alongside whatever the
     * map's own volumes assert (`volume_env`).
     *
     * This used to be the only source, because the port could not resolve a
     * volume's record to its UserFuncs primitive. It can now, so a map with
     * water and crouch volumes crouches and swims on its own and this is what
     * is left: a way for a test, a tool or a debug key to hold a state the
     * geometry does not.
     */
    u32  env_flags;

    /*
     * 0x800AEBCC: zero in single player. It gates the -3072 impulse ceiling and
     * the end-of-frame basis rebuild, so it is real behaviour rather than a
     * mode flag the port invented.
     */
    bool multiplayer;

    /*
     * 0x800B2AA4, a GAME VARIABLES setting. Non-zero routes all movement through
     * the full view-basis rotation instead of the yaw-only path, which lets pitch
     * steer horizontal movement.
     */
    bool full_basis_movement;

    /* 0x800B29EC & 0x40 — when set, fall damage is suppressed. */
    bool no_fall_damage;

    /*
     * 0x800B29EC itself, the GAME VARIABLES word the pause menu writes (see
     * gamevars.h). `no_fall_damage` above is bit 0x40 of it, kept separate
     * because the movement code reads it every tick; the item dispatch reads
     * bit 0 from here instead of being handed a second copy.
     */
    u32  cheats;

    /*
     * The entity set and the world its thinks run in — items today, and
     * whatever else moves onto the think pointer next.
     *
     * The sim owns it rather than a caller because the touch sweep needs the
     * player's inventory and position every tick, and those live here. It is
     * empty until q2_sim_attach_items is called, and a sim without it runs
     * exactly as before: q2_entity_run over an empty set does nothing.
     *
     * `ent_world.level_time` is kept in step with `level_time`, not maintained
     * separately — one clock, as in the original.
     */
    q2_entity_set   entities;
    q2_entity_world ent_world;
    bool            entities_ready;

    /*
     * The presentation layer.
     *
     * It lives in the sim rather than in the client for the reason the original
     * puts it on the entity list: an effect is spawned by a gameplay event —
     * a rocket detonating, a creature bleeding — and integrated by the same
     * clock as everything else. A headless caller gets the bursts and their
     * motion without a screen; the client only has to draw them.
     *
     * `fx.tab` is NULL until q2_sim_attach_effects is called with the tables
     * out of the executable, and a sim without them spawns nothing: no ramp
     * means no colour, and inventing one would be the port making up an effect
     * the console does not have.
     *
     * `fx_rng` is separate from the weapon generator on purpose. Effects draw
     * far more numbers than firing does, and sharing one stream would make a
     * shot's spread depend on how many sparks happened to be alive — which the
     * original does do, since it has one rand(), but which makes a seeded
     * replay of the SIMULATION impossible to reproduce independently of the
     * frame rate. This is a deliberate divergence and it is why it is a second
     * generator rather than the same one.
     */
    q2_fx_world  fx;
    q2_rng       fx_rng;
    bool         fx_ready;

    /* The map's glint — its `GlintMod` mesh and the band state a level script
     * would fill. Points into `common`'s bytes. */
    q2_fx_glint      glint;
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

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Attach the effect tables, which live in the boot executable. `tab` must
 * outlive the sim. Until this is called the sim runs exactly as it did before:
 * every spawn point below is a no-op, so a caller that has no executable to
 * hand still simulates.
 *
 * `seed` seeds the effect generator. Pass the same value for a reproducible
 * replay of the visuals.
 */
void q2_sim_attach_effects(q2_sim *sim, const q2_fx_tables *tab, u32 seed);

/*
 * Attach the map's glint mesh — its `GlintMod` chunk. Optional: only BIGGUN
 * carries one, and a map without it has no glint rather than a substitute.
 * `common` must outlive the sim, because the mesh points into its bytes.
 *
 * Returns false when the map has no chunk or the chunk is too small to hold
 * vertices.
 */
bool q2_sim_attach_glint(q2_sim *sim, const q2_common_file *common);

/*
 * Throw a burst of debris. `bmin`/`bmax` are a Scene node's box and `at` an
 * optional fixed point — see q2_fx_debris_burst. The pieces are stepped by
 * q2_sim_combat_tick against PRIMARY collision, which is the hull the original
 * moves them in.
 *
 * This is what a breaking pane calls. The port exposes it rather than wiring it
 * into the GLASS primitive, because which Scene node a given pane owns is part
 * of the UserFuncs runtime-object mapping and not of the effect system.
 */
u32 q2_sim_debris_burst(q2_sim *sim, const s32 bmin[3], const s32 bmax[3],
                        const s32 *at, u32 count, u8 area);

/*
 * Where the sim fires effects from, so a reader does not have to find them:
 *
 *   a projectile detonating          -> explosion  (0x800486EC)
 *   a bolt striking anything         -> spark      (0x8003E0C0)
 *   a creature taking damage         -> blood      (0x80048C08)
 *   the player taking damage         -> blood
 *   a creature reaching zero health  -> gib        (0x800596B0)
 *
 * The hitscan weapons do not spawn anything here yet: the port resolves a
 * pellet against the target list rather than against the hull, so it has a
 * victim but not a contact point, and putting the burst at the muzzle would be
 * worse than putting it nowhere. Called out rather than approximated.
 */

/*
 * Attach the map's trigger volumes and event script.
 *
 * Separate from init because these live in COMMON.DAT, which is per MAP, while
 * the zone geometry is per ZONE. Optional: a sim without them still runs, it
 * just has no gameplay.
 */
q2_result q2_sim_attach_gameplay(q2_sim *sim, const q2_common_file *common);

/*
 * Spawn the map's items and start running their thinks.
 *
 * Separate from q2_sim_attach_gameplay for the same reason that one is separate
 * from init: Population is per MAP, and a caller that only wants to walk around
 * need not pay for it. `table` may be NULL, in which case the built-in item
 * table is used — which is the right default for the catalogued PAL build and
 * refuses nothing.
 *
 * `zone` is which of the map's zones is loaded, and it matters because
 * Population is per MAP while a session is in one ZONE: a group the disc names
 * after a different zone is dropped rather than left standing in this one. Pass
 * -1 to spawn every group, which is what an offline pass over a whole map wants.
 * See q2_item_spawn_zone for what that filter can and cannot decide.
 *
 * `bank` is the map's CastList and may also be NULL. When it is given, each
 * item's model is resolved at spawn as the engine does it (0x80058850), so the
 * renderer never has to look one up mid-frame; without it, the resolve happens
 * on first draw instead.
 */
q2_result q2_sim_attach_items(q2_sim *sim, const q2_common_file *common,
                              int zone, const q2_item_table *table,
                              const struct q2_model_bank *bank);

/* What the item thinks asked for since the last call: pickup sounds, the
 * materialise sound, and the glow lights. Cleared at the start of every tick. */
const q2_ent_events *q2_sim_entity_events(const q2_sim *sim);

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

/*
 * 0x80038260 — the view angles to render from: pitch, yaw and roll in the
 * 4096-step circle, with every kick the player is carrying folded in.
 *
 * `sim->player.pitch/yaw/roll` are the AIM. They are not what the camera uses,
 * and the difference is three decaying offsets rather than one:
 *
 *     kick        30 ticks   what firing posts; pitch, yaw and roll
 *     hurt_kick  150 ticks   what taking damage posts; pitch and roll
 *     fall_value  90 ticks   what landing posts; pitch only
 *
 * Each is scaled by `(deadline - now) / period` computed in 1.0.12 — so a kick
 * whose deadline is FURTHER out than its own period is scaled by more than one,
 * which is a real consequence of `pain_time` being 210 while the hurt kick
 * decays over 150 and not something to clamp away.
 *
 * The camera negates the pitch (0x8004F41C); this returns the entity's own
 * sign, because that is what the aim and the weapon both use.
 */
void q2_sim_view_angles(const q2_sim *sim, s32 out[3]);

/*
 * Post a view kick — what a weapon's recoil and a damage hit do. `period` picks
 * which of the three decays it joins; use Q2_VIEW_KICK_* below.
 */
#define Q2_VIEW_KICK_FIRE   30    /* client+0xCC, 0x800382D4 */
#define Q2_VIEW_KICK_HURT  150    /* client+0xD0, 0x80038364 */
#define Q2_VIEW_KICK_FALL   90    /* client+0xD4, 0x800383D4 */

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
