#include "sim.h"

#include <string.h>

#include "trig.h"

void q2_sim_init(q2_sim *sim, const q2_world_zone *zone, int tick_rate_hz)
{
    if (!sim)
        return;

    memset(sim, 0, sizeof(*sim));
    sim->zone = zone;

    /* dt advances by 300/field_rate per field. PAL fields run at 50 Hz giving
     * 6, NTSC at 60 giving 5. The engine's own PAL value is 6 (0x80018DB8). */
    sim->dt_per_field = (tick_rate_hz > 0) ? (Q2_DT_HZ / tick_rate_hz) : Q2_DT_PER_FIELD;
    if (sim->dt_per_field <= 0)
        sim->dt_per_field = Q2_DT_PER_FIELD;

    sim->player.view_height = Q2_VIEW_STAND;
}

void q2_sim_spawn(q2_sim *sim, const s32 pos[3], s32 yaw)
{
    if (!sim || !pos)
        return;

    sim->player.pos[0] = pos[0];
    sim->player.pos[1] = pos[1];
    sim->player.pos[2] = pos[2];

    sim->player.vel[0] = sim->player.vel[1] = sim->player.vel[2] = 0;
    sim->player.yaw    = yaw;
    sim->player.pitch  = 0;

    sim->player.on_ground   = false;
    sim->player.crouching   = false;
    sim->player.view_height = Q2_VIEW_STAND;
    sim->player.ground_y    = pos[1];
}

/* ------------------------------------------------------------------------- */
/* Collision seam                                                             */
/* ------------------------------------------------------------------------- */
void q2_sim_trace(const q2_sim *sim, const s32 start[3], const s32 end[3],
                  q2_trace *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->fraction = Q2_ONE_12;

    if (!start || !end)
        return;

    out->end[0] = end[0];
    out->end[1] = end[1];
    out->end[2] = end[2];

    /*
     * PLACEHOLDER. Real hull tracing waits on the collision plane point
     * encoding, which is only 95.6% confirmed — building player movement on a
     * reading we cannot vouch for would produce a game that mostly works and
     * occasionally walks through walls, which is worse than one that obviously
     * does not collide yet.
     *
     * Everything above this line is the real interface; only the body changes.
     */
    (void)sim;
}

/* ------------------------------------------------------------------------- */
/* One logic tick                                                             */
/* ------------------------------------------------------------------------- */
void q2_sim_tick(q2_sim *sim, const q2_input *input, s32 dt)
{
    q2_player *p;
    s32 target_view;

    if (!sim || !input || dt <= 0)
        return;

    p = &sim->player;

    /* --- look ----------------------------------------------------------- */
    p->yaw   += input->yaw_delta;
    p->pitch += input->pitch_delta;

    if (p->pitch >  Q2_ANGLE_90) p->pitch =  Q2_ANGLE_90;
    if (p->pitch < -Q2_ANGLE_90) p->pitch = -Q2_ANGLE_90;

    /* --- wish direction -------------------------------------------------- */
    {
        s32 sy = q2_sin12(p->yaw);
        s32 cy = q2_cos12(p->yaw);

        /* Movement is horizontal regardless of pitch, as a player controller
         * wants. Forward is +Z rotated by yaw; strafe is its perpendicular. */
        s32 wish_x = (s32)(((s64)sy * input->forward + (s64)cy * input->strafe) >> Q2_FRAC_12);
        s32 wish_z = (s32)(((s64)cy * input->forward - (s64)sy * input->strafe) >> Q2_FRAC_12);

        /* Accelerate toward the wish velocity. The engine divides accumulated
         * velocity by Q2_VEL_DIV when applying it to position, so velocities
         * are carried pre-divide to keep the integer precision. */
        p->vel[0] += (wish_x * dt) / Q2_DT_NOMINAL;
        p->vel[2] += (wish_z * dt) / Q2_DT_NOMINAL;
    }

    /* --- gravity --------------------------------------------------------- */
    if (!p->on_ground) {
        p->vel[1] += Q2_GRAVITY * dt;
        if (p->vel[1] > Q2_TERMINAL_VY)
            p->vel[1] = Q2_TERMINAL_VY;
    } else if (input->jump) {
        /* Jump clears the ground flag immediately so the same tick integrates
         * upward rather than being re-grounded. */
        p->vel[1]     = -Q2_TERMINAL_VY / 4;
        p->on_ground  = false;
    }

    /* --- friction -------------------------------------------------------- */
    if (p->on_ground) {
        p->vel[0] -= p->vel[0] / 4;
        p->vel[2] -= p->vel[2] / 4;
    }

    /* --- integrate ------------------------------------------------------- */
    {
        s32 want[3];
        q2_trace tr;

        want[0] = p->pos[0] + (s32)(((s64)p->vel[0] * dt) / Q2_VEL_DIV);
        want[1] = p->pos[1] + (s32)(((s64)p->vel[1] * dt) / Q2_VEL_DIV);
        want[2] = p->pos[2] + (s32)(((s64)p->vel[2] * dt) / Q2_VEL_DIV);

        q2_sim_trace(sim, p->pos, want, &tr);

        p->pos[0] = tr.end[0];
        p->pos[1] = tr.end[1];
        p->pos[2] = tr.end[2];

        /* Ground test against the placeholder plane. World Y increases
         * downward on this disc, so "below the ground" is a larger Y. */
        if (p->pos[1] >= p->ground_y) {
            p->pos[1]    = p->ground_y;
            p->vel[1]    = 0;
            p->on_ground = true;
        }
    }

    /* --- view height ----------------------------------------------------- */
    p->crouching = input->crouch;
    target_view  = p->crouching ? Q2_VIEW_CROUCH : Q2_VIEW_STAND;

    /* Ease toward the target rather than snapping; the original interpolated
     * so crouching does not jolt the camera. */
    if (p->view_height < target_view) {
        p->view_height += (target_view - p->view_height + 3) / 4;
        if (p->view_height > target_view)
            p->view_height = target_view;
    } else if (p->view_height > target_view) {
        p->view_height -= (p->view_height - target_view + 3) / 4;
        if (p->view_height < target_view)
            p->view_height = target_view;
    }

    sim->tick_count++;
}

/* ------------------------------------------------------------------------- */
u32 q2_sim_advance(q2_sim *sim, const q2_input *input, double elapsed_seconds)
{
    s32 dt;
    u32 ticks = 0;

    if (!sim || !input || elapsed_seconds <= 0.0)
        return 0;

    /* Real time -> the engine's 1/300 s units. */
    dt = (s32)(elapsed_seconds * (double)Q2_DT_HZ);

    /* The engine's own long-frame clamp. Slowing the world down on a bad frame
     * is the original's behaviour; sub-stepping to catch up would be more
     * correct and less faithful. */
    if (dt > Q2_DT_MAX)
        dt = Q2_DT_MAX;

    sim->dt_accum += dt;

    while (sim->dt_accum >= Q2_DT_NOMINAL) {
        q2_sim_tick(sim, input, Q2_DT_NOMINAL);
        sim->dt_accum -= Q2_DT_NOMINAL;
        ticks++;
    }

    return ticks;
}

void q2_sim_eye(const q2_sim *sim, s32 out_pos[3])
{
    if (!sim || !out_pos)
        return;

    out_pos[0] = sim->player.pos[0];
    /* World Y increases downward, so the eye sits at a smaller Y than the feet. */
    out_pos[1] = sim->player.pos[1] - sim->player.view_height;
    out_pos[2] = sim->player.pos[2];
}
