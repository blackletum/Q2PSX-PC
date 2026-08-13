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
 * What this does NOT do yet
 * ---------------------------------------------------------------------------
 * Collision response is deliberately minimal. The collision plane POINT encoding
 * is only 95.6% confirmed (see collision.h), so resolving movement against real
 * hulls would build player physics on a reading we cannot vouch for. Until that
 * reaches 100%, the simulation integrates and applies a ground plane, and
 * `q2_sim_trace` is the single seam where real collision drops in.
 *
 * Monsters, weapons, damage and pickups are not here either. The Events opcode
 * payloads are still undecoded, so triggers cannot fire, and CastList animation
 * data is not read yet.
 */
#ifndef Q2PSX_SIM_H
#define Q2PSX_SIM_H

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

    s32  dt_accum;      /* leftover dt units not yet consumed by a tick       */
    u32  tick_count;
    s32  dt_per_field;  /* 6 on PAL, 5 on NTSC — the build's field rate       */
} q2_sim;

void q2_sim_init(q2_sim *sim, const q2_world_zone *zone, int tick_rate_hz);

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
/* Collision seam                                                             */
/*                                                                            */
/* The single place real hull collision will attach. Returns the fraction of   */
/* the move that was possible, in 1.0.12 fixed point (4096 == unobstructed),  */
/* and writes the surface normal when something was hit.                      */
/*                                                                            */
/* The current implementation is a flat ground plane. It is honest about being */
/* a placeholder rather than pretending to trace geometry.                     */
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
