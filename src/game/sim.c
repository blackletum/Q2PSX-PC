#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "trig.h"

/* Defined below, but needed by init and spawn. */
static s32 find_node(const q2_sim *sim, const s32 point[3], s32 hint);

void q2_sim_init(q2_sim *sim, const q2_world_zone *zone, int tick_rate_hz)
{
    if (!sim)
        return;

    memset(sim, 0, sizeof(*sim));
    sim->zone         = zone;
    sim->current_node = -1;

    /* The primary hull is the one the player moves in. SecondaryCol is NOT
     * reliably a finer version of it (it has fewer nodes on 9 of 115 zones), so
     * it is deliberately not used as a fallback. */
    if (zone && q2_collision_parse(&sim->coll, &zone->zone, Q2_COLL_PRIMARY) == Q2_OK)
        sim->coll_ready = true;

    /* dt advances by 300/field_rate per field. PAL fields run at 50 Hz giving
     * 6, NTSC at 60 giving 5. The engine's own PAL value is 6 (0x80018DB8). */
    sim->dt_per_field = (tick_rate_hz > 0) ? (Q2_DT_HZ / tick_rate_hz) : Q2_DT_PER_FIELD;
    if (sim->dt_per_field <= 0)
        sim->dt_per_field = Q2_DT_PER_FIELD;

    sim->player.view_height = Q2_VIEW_STAND;
}

void q2_sim_free(q2_sim *sim)
{
    if (!sim)
        return;
    q2_event_rt_free(&sim->event_rt);
    free(sim->trigger_inside);
    memset(sim, 0, sizeof(*sim));
}

q2_result q2_sim_attach_gameplay(q2_sim *sim, const q2_common_file *common)
{
    if (!sim || !common)
        return Q2_ERR_INVALID_ARG;

    if (q2_triggers_parse(&sim->triggers, common) == Q2_OK) {
        sim->triggers_ready = true;

        free(sim->trigger_inside);
        sim->trigger_capacity = sim->triggers.count;
        sim->trigger_inside   = (u8 *)calloc(sim->trigger_capacity ? sim->trigger_capacity : 1, 1);
        if (!sim->trigger_inside) {
            sim->triggers_ready = false;
            return Q2_ERR_NO_MEMORY;
        }
    }

    if (q2_events_parse_common(&sim->events, common) == Q2_OK) {
        if (q2_event_rt_init(&sim->event_rt, &sim->events) == Q2_OK)
            sim->events_ready = true;
    }

    return Q2_OK;
}

bool q2_sim_take_zone_change(q2_sim *sim, u32 *out_zone)
{
    if (!sim || !sim->zone_change_pending)
        return false;

    if (out_zone)
        *out_zone = sim->zone_change_target;

    sim->zone_change_pending = false;
    return true;
}

/*
 * Fire any trigger the player has just ENTERED.
 *
 * Edge-triggered, not level-triggered: standing inside a volume must not run
 * its script 25 times a second. The previous-inside bitmap is what makes that
 * distinction, and it is why the trigger state has to persist across ticks.
 */
static void update_triggers(q2_sim *sim)
{
    u32 i;

    if (!sim->triggers_ready || !sim->events_ready)
        return;

    for (i = 0; i < sim->triggers.count && i < sim->trigger_capacity; i++) {
        bool inside = q2_trigger_contains(&sim->triggers, i, sim->player.pos);
        bool was    = sim->trigger_inside[i] != 0;

        sim->trigger_inside[i] = inside ? 1u : 0u;

        if (!inside || was)
            continue;

        {
            q2_trigger trig;
            if (!q2_trigger_get(&sim->triggers, i, &trig))
                continue;
            if (trig.event_offset == Q2_TRIGGER_NO_EVENT)
                continue;
            q2_event_rt_trigger(&sim->event_rt, trig.event_offset);
        }
    }

    if (q2_event_rt_update(&sim->event_rt) == Q2_EVENT_ZONE_CHANGE) {
        sim->zone_change_pending = true;
        sim->zone_change_target  = sim->event_rt.pending_zone;
        sim->event_rt.has_zone_change = false;
    }
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

    /* Locate the cell we spawned into. A spawn point that lands outside every
     * hull leaves current_node at -1, and the trace then lets the player move
     * freely rather than wedging them at the origin. */
    sim->current_node = find_node(sim, sim->player.pos, -1);

    /* Clear the entered-set so a spawn inside a volume fires it, rather than
     * being treated as "already inside". */
    if (sim->trigger_inside && sim->trigger_capacity)
        memset(sim->trigger_inside, 0, sim->trigger_capacity);
}

/* ------------------------------------------------------------------------- */
/* Collision seam                                                             */
/* ------------------------------------------------------------------------- */
/* Which convex cell contains `point`, or -1. Searches from `hint` outward,
 * because the player is nearly always still in the cell they were in. */
static s32 find_node(const q2_sim *sim, const s32 point[3], s32 hint)
{
    u32 i;

    if (!sim->coll_ready)
        return -1;

    if (hint >= 0 && (u32)hint < sim->coll.node_count &&
        q2_collision_point_in_node(&sim->coll, (u32)hint, point))
        return hint;

    for (i = 0; i < sim->coll.node_count; i++) {
        if (q2_collision_point_in_node(&sim->coll, i, point))
            return (s32)i;
    }
    return -1;
}

void q2_sim_trace(const q2_sim *sim, const s32 start[3], const s32 end[3],
                  q2_trace *out)
{
    q2_coll_node here, next;
    s32 node;
    s32 best_frac = Q2_ONE_12;
    s32 best_plane = -1;
    u32 k;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->fraction = Q2_ONE_12;

    if (!sim || !start || !end)
        return;

    out->end[0] = end[0];
    out->end[1] = end[1];
    out->end[2] = end[2];

    node = sim->current_node;
    if (node < 0 || !sim->coll_ready)
        return;             /* no cell known — move freely rather than wedge */

    if (!q2_collision_get_node(&sim->coll, (u32)node, &here) ||
        !q2_collision_get_node(&sim->coll, (u32)node + 1, &next))
        return;

    /*
     * The cell is empty space bounded by outward-facing planes, so the move is
     * clipped where it would cross one from inside to outside. Distances come
     * back scaled by 4096 (the normals are 1.3.12), and the ratio is taken in
     * that same scale so no divide is needed until the very end.
     */
    for (k = here.first_plane; k < next.first_plane; k++) {
        s32 d0 = q2_coll_plane_distance(&sim->coll, (u32)node, k, start);
        s32 d1 = q2_coll_plane_distance(&sim->coll, (u32)node, k, end);
        s32 frac;

        if (d0 >= 0)
            continue;       /* already outside this plane — see the note below */
        if (d1 <= 0)
            continue;       /* stays inside, nothing to clip */

        /* Crosses from inside to outside: the fraction where it hits zero. */
        {
            s64 denom = (s64)d0 - (s64)d1;
            if (denom == 0)
                continue;
            frac = (s32)(((s64)d0 * Q2_ONE_12) / denom);
        }

        if (frac < 0)
            frac = 0;
        if (frac < best_frac) {
            best_frac  = frac;
            best_plane = (s32)k;
        }
    }

    /*
     * Starting outside a plane is not treated as a collision. With 0.15% of
     * planes geometrically inconsistent, and the player able to stand exactly
     * on a boundary, refusing to move whenever d0 >= 0 would wedge them
     * permanently. Ignoring those planes lets the move proceed and the next
     * tick re-resolve, which degrades gracefully instead of locking up.
     */

    if (best_plane < 0)
        return;

    out->fraction = best_frac;
    out->hit      = true;

    out->end[0] = start[0] + (s32)(((s64)(end[0] - start[0]) * best_frac) >> Q2_FRAC_12);
    out->end[1] = start[1] + (s32)(((s64)(end[1] - start[1]) * best_frac) >> Q2_FRAC_12);
    out->end[2] = start[2] + (s32)(((s64)(end[2] - start[2]) * best_frac) >> Q2_FRAC_12);

    {
        q2_coll_plane pl;
        if (q2_collision_get_plane(&sim->coll, (u32)best_plane, &pl)) {
            out->normal[0] = pl.nx;
            out->normal[1] = pl.ny;
            out->normal[2] = pl.nz;
        }
    }
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

    /* --- integrate and collide ------------------------------------------- */
    {
        s32 want[3];
        q2_trace tr;
        int attempt;

        want[0] = p->pos[0] + (s32)(((s64)p->vel[0] * dt) / Q2_VEL_DIV);
        want[1] = p->pos[1] + (s32)(((s64)p->vel[1] * dt) / Q2_VEL_DIV);
        want[2] = p->pos[2] + (s32)(((s64)p->vel[2] * dt) / Q2_VEL_DIV);

        p->on_ground = false;

        /*
         * Clip, slide along whatever was hit, and try again. Three attempts is
         * enough to resolve a corner (two walls plus the floor) and bounds the
         * work when the player is jammed into a crevice.
         */
        for (attempt = 0; attempt < 3; attempt++) {
            q2_sim_trace(sim, p->pos, want, &tr);

            p->pos[0] = tr.end[0];
            p->pos[1] = tr.end[1];
            p->pos[2] = tr.end[2];

            if (!tr.hit)
                break;

            /*
             * Ground detection.
             *
             * Cell planes face OUTWARD, so the interior is on their negative
             * side. Gravity increases Y, so the surface a falling player lands
             * on bounds the cell in the +Y direction — and its outward normal
             * therefore points +Y, not -Y.
             *
             * This was inverted at first, which made collision look completely
             * broken (the player was correctly stopped by the floor every tick
             * but never registered as grounded, so gravity kept accumulating).
             * Verified against BASE1 node 161, an axis-aligned box whose six
             * planes are exactly +/-4096 on each axis.
             */
            if (tr.normal[1] > Q2_ONE_12 / 2) {
                p->on_ground = true;
                p->vel[1]    = 0;
            }

            /* Slide: remove the velocity component running into the plane, and
             * do the same to the remaining move so the next attempt continues
             * along the surface instead of re-hitting it. */
            {
                s64 vd = ((s64)p->vel[0] * tr.normal[0]
                        + (s64)p->vel[1] * tr.normal[1]
                        + (s64)p->vel[2] * tr.normal[2]) >> Q2_FRAC_12;
                s64 md = ((s64)(want[0] - p->pos[0]) * tr.normal[0]
                        + (s64)(want[1] - p->pos[1]) * tr.normal[1]
                        + (s64)(want[2] - p->pos[2]) * tr.normal[2]) >> Q2_FRAC_12;
                int j;

                for (j = 0; j < 3; j++) {
                    p->vel[j] -= (s32)((vd * tr.normal[j]) >> Q2_FRAC_12);
                    want[j]   -= (s32)((md * tr.normal[j]) >> Q2_FRAC_12);
                }
            }
        }

        /* Moving between cells is normal; re-locate rather than assuming. */
        sim->current_node = find_node(sim, p->pos, sim->current_node);

        /* Without collision data, fall back to the seeded ground plane so the
         * player does not drop forever in a zone whose hull failed to parse. */
        if (!sim->coll_ready && p->pos[1] >= p->ground_y) {
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

    /* After movement, so a trigger sees where the player actually ended up. */
    update_triggers(sim);

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
