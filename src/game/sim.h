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
 * Movement is resolved against the zone's real convex hulls. The plane encoding
 * is confirmed at 99.84% by the convexity test described in collision.h, which
 * is good enough to move a player with — and the 0.15% of inconsistent planes
 * are handled defensively rather than treated as a reason to distrust the data.
 *
 * The hulls are EMPTY convex cells, not solid blockers: the player is inside a
 * node and a move that would leave it is clipped. That is the sector model the
 * portal graph in AreaConx implies, and it is why the trace looks for the node
 * containing the player rather than sweeping against everything.
 *
 * ---------------------------------------------------------------------------
 * What this does NOT do yet
 * ---------------------------------------------------------------------------
 * Monsters, weapons, damage and pickups. The Events opcode payloads are still
 * undecoded so triggers cannot fire, and CastList animation data is unread.
 */
#ifndef Q2PSX_SIM_H
#define Q2PSX_SIM_H

#include "collision.h"
#include "events_rt.h"
#include "trigger.h"
#include "q2psx.h"
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
} q2_player;

/* ------------------------------------------------------------------------- */
/* Simulation                                                                 */
/* ------------------------------------------------------------------------- */
typedef struct q2_sim {
    const q2_world_zone *zone;
    q2_player            player;

    q2_collision coll;          /* the zone's primary hull                   */
    bool         coll_ready;
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

    /* Set when a zone gate fires; the caller performs the load. */
    bool         zone_change_pending;
    u32          zone_change_target;

    s32  dt_accum;      /* leftover dt units not yet consumed by a tick       */
    u32  tick_count;
    s32  dt_per_field;  /* 6 on PAL, 5 on NTSC — the build's field rate       */
} q2_sim;

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
/* Clips a move against the convex cell the player occupies. `fraction` is     */
/* 1.0.12 — 4096 means the whole move succeeded — and `normal` is the plane    */
/* that stopped it, valid only when something was hit.                        */
/* ------------------------------------------------------------------------- */
typedef struct q2_trace {
    s32  fraction;      /* 1.0.12: 4096 means the whole move succeeded */
    s32  end[3];
    s32  normal[3];     /* 1.3.12 unit normal, valid when fraction < 4096 */
    bool hit;
} q2_trace;

void q2_sim_trace(const q2_sim *sim, const s32 start[3], const s32 end[3],
                  q2_trace *out);

#endif /* Q2PSX_SIM_H */
