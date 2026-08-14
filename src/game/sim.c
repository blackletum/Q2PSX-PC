#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "trig.h"

/* Defined below, but attach_gameplay needs it. */
static void build_volumes(q2_sim *sim);

void q2_sim_init(q2_sim *sim, const q2_world_zone *zone, int tick_rate_hz)
{
    if (!sim)
        return;

    memset(sim, 0, sizeof(*sim));
    sim->zone         = zone;
    sim->current_node = -1;
    sim->player.ent.node = -1;

    /*
     * SecondaryCol is the hull entities move in — read out of the zone loader,
     * which points the mover's context (0x800C8FE8) at it, and not guessed from
     * node counts. That also settles the old puzzle about SecondaryCol having
     * FEWER nodes than PrimaryColl on 9 of 115 zones: it is not a refinement of
     * the primary hull, it is a hull for a different job.
     */
    if (zone && q2_collision_parse(&sim->coll, &zone->zone, Q2_COLL_SECONDARY) == Q2_OK)
        sim->coll_ready = true;

    if (zone && q2_collision_parse(&sim->coll_primary, &zone->zone,
                                   Q2_COLL_PRIMARY) == Q2_OK)
        sim->coll_primary_ready = true;

    /*
     * A zone that ships no SecondaryCol still has to be walkable, so fall back
     * to the primary hull rather than to no collision at all. Every zone on the
     * PAL disc carries both, so this never fires there.
     */
    if (!sim->coll_ready && sim->coll_primary_ready) {
        sim->coll       = sim->coll_primary;
        sim->coll_ready = true;
    }

    /* dt advances by 300/field_rate per field. PAL fields run at 50 Hz giving
     * 6, NTSC at 60 giving 5. The engine's own PAL value is 6 (0x80018DB8). */
    sim->dt_per_field = (tick_rate_hz > 0) ? (Q2_DT_HZ / tick_rate_hz) : Q2_DT_PER_FIELD;
    if (sim->dt_per_field <= 0)
        sim->dt_per_field = Q2_DT_PER_FIELD;

    sim->gravity            = Q2_GRAVITY;
    sim->player.view_height = Q2_VIEW_STAND;

    q2_sim_combat_init(sim);
}

void q2_sim_free(q2_sim *sim)
{
    if (!sim)
        return;
    q2_event_rt_free(&sim->event_rt);
    free(sim->trigger_inside);
    free(sim->volumes);
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

    /* The same volumes serve three jobs: firing scripts, answering the contents
     * query, and being swept against. Build the sweep list once here. */
    build_volumes(sim);

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

/* ------------------------------------------------------------------------- */
/* Volumes                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * 0x8006FE3C — move `v` toward `target` by at most `rate`.
 *
 * The engine uses it for the two liquid velocity rules and for the view-height
 * ease. It is a clamped approach, not a lerp: overshoot is impossible.
 */
static s32 ease_toward(s32 v, s32 target, s32 rate)
{
    if (rate < 0)
        rate = -rate;

    if (v < target)
        return (target - v <= rate) ? target : v + rate;
    if (v > target)
        return (v - target <= rate) ? target : v - rate;
    return v;
}

/*
 * Turn the map's trigger volumes into the target list the sweep and the
 * contents query walk. This is the port's stand-in for 0x800C9114, and the
 * records are the same 36-byte TrigBounds triggers the original reads there —
 * `q2_move_target.mask` is the trigger's own `flags` halfword.
 */
static void build_volumes(q2_sim *sim)
{
    u32 i;

    free(sim->volumes);
    sim->volumes      = NULL;
    sim->volume_count = 0;

    memset(&sim->move_world, 0, sizeof(sim->move_world));
    sim->move_world.half_extent = Q2_SWEEP_HALF_EXTENT;

    if (!sim->triggers_ready || sim->triggers.count == 0)
        return;

    sim->volumes = (q2_move_target *)calloc(sim->triggers.count,
                                            sizeof(*sim->volumes));
    if (!sim->volumes)
        return;

    for (i = 0; i < sim->triggers.count; i++) {
        q2_trigger t;
        int k;

        if (!q2_trigger_get(&sim->triggers, i, &t))
            continue;

        for (k = 0; k < 3; k++) {
            sim->volumes[i].min[k] = t.min[k];
            sim->volumes[i].max[k] = t.max[k];
        }
        sim->volumes[i].mask   = t.flags;
        sim->volumes[i].kind   = Q2_MOVE_KIND_VOLUME;
        sim->volumes[i].id     = (s32)i;
        sim->volumes[i].dy     = 0;
        sim->volumes[i].active = true;
    }

    sim->volume_count           = sim->triggers.count;
    sim->move_world.targets     = sim->volumes;
    sim->move_world.count       = sim->volume_count;

    /*
     * 0x8005553C: the mask an entity sweeps volumes with is 0x810 when its own
     * flag bit 0 is set and 0 otherwise. Which entities set that bit was not
     * traced, so the port leaves it at 0 — meaning volumes are queried for
     * contents but do not block movement. Set `sim->move_world.mask` to 0x810
     * to turn them solid once that is known.
     */
    sim->move_world.mask = 0;
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

    /*
     * The mover works in the ENTITY ORIGIN's frame, which sits Q2_EYE_BASE
     * above the feet — see the note on q2_sim_origin_y in sim.h. `pos` is a
     * StartPos, i.e. the feet, so it is lifted here and lowered again after
     * every move.
     */
    memset(&sim->player.ent, 0, sizeof(sim->player.ent));
    sim->player.ent.pos[0] = pos[0];
    sim->player.ent.pos[1] = q2_sim_origin_y(pos[1]);
    sim->player.ent.pos[2] = pos[2];

    /*
     * Locate the cell we spawned into. A spawn that lands outside every hull
     * leaves the cached cell at -1, which is what the original stores in a
     * fresh entity: the first move then pays for one brute-force sweep and
     * self-corrects (0x80044C74).
     */
    sim->player.ent.node = sim->coll_ready
        ? q2_coll_find_node(&sim->coll, sim->player.ent.pos, -1, true)
        : -1;
    sim->current_node = sim->player.ent.node;

    /* Clear the entered-set so a spawn inside a volume fires it, rather than
     * being treated as "already inside". */
    if (sim->trigger_inside && sim->trigger_capacity)
        memset(sim->trigger_inside, 0, sim->trigger_capacity);
}

/*
 * 0x800458A0 — query the volumes the entity is standing in and turn the answer
 * into its own flags.
 *
 * Transcribed from 0x800458B4…0x80045970. Three of the mask's bits are
 * understood; the query passes the whole 0x3360 because the original does, and
 * the bits nothing consumes yet are simply not acted on.
 */
static void update_contents(q2_sim *sim)
{
    q2_player *p = &sim->player;
    u16 contents;

    if (!sim->volume_count) {
        p->ent.flags &= ~(Q2_ENT_LIQUID_SINK | Q2_ENT_LIQUID_FLOAT | 0x800u);
        return;
    }

    contents = q2_move_contents(&sim->move_world, p->ent.pos, Q2_CONTENTS_MASK);

    /*
     * 0x800458B4: inside a 0x1000 volume, flag 0x800 is set when the entity's
     * last contact normal is nearly horizontal — `-1023 <= ny < 1024` on
     * ent+0x14, which is the |ny|-maximising normal, not the velocity. So the
     * flag means "in this volume AND touching a wall rather than a floor",
     * which is a ladder-shaped condition.
     */
    if (contents & 0x1000u) {
        s32 ny = p->ent.last_normal[1];

        if (ny < 1024 && ny >= -1023)
            p->ent.flags |= 0x800u;
        else
            p->ent.flags &= ~0x800u;
    } else {
        p->ent.flags &= ~0x800u;
    }

    /* 0x800458F8: 0x0200 — sinks slowly. */
    if (contents & 0x0200u)
        p->ent.flags |= Q2_ENT_LIQUID_SINK;
    else
        p->ent.flags &= ~Q2_ENT_LIQUID_SINK;

    /* 0x80045920: 0x2000 — buoyant, and boosted when already on the ground. */
    if (contents & 0x2000u) {
        if (p->ent.flags & Q2_ENT_GROUNDED_MASK)
            p->vel[1] += Q2_LIQUID_BOOST;
        p->ent.flags |= Q2_ENT_LIQUID_FLOAT;
    } else {
        p->ent.flags &= ~Q2_ENT_LIQUID_FLOAT;
    }
}

/* ------------------------------------------------------------------------- */
/* Collision seam                                                             */
/* ------------------------------------------------------------------------- */
/*
 * A swept segment through the hull, for callers that want one — weapon fire,
 * line of sight, the walk diagnostic. Entity movement does NOT go through here;
 * it goes through q2_move_step, which is what the original does.
 *
 * The fraction is reconstructed from the clipped end point along the longest
 * axis. The engine never forms one: it carries an exact rational and scales an
 * int16 delta by it, so any fraction here is the port's convenience and can be
 * one unit out on a long move. Do not feed it back into the geometry.
 */
void q2_sim_trace(q2_sim *sim, const s32 start[3], const s32 end[3],
                  q2_trace *out)
{
    s32 pos[3];
    s32 node = -1;
    bool complete;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->fraction = Q2_ONE_12;
    out->node     = -1;

    if (!sim || !start || !end)
        return;

    out->end[0] = end[0];
    out->end[1] = end[1];
    out->end[2] = end[2];

    if (!sim->coll_ready)
        return;             /* no hull — move freely rather than wedge */

    complete = q2_coll_move(&sim->coll, start, end, sim->current_node,
                            pos, &node);

    out->end[0] = pos[0];
    out->end[1] = pos[1];
    out->end[2] = pos[2];
    out->node   = node;

    if (node >= 0) {
        q2_coll_node n;
        if (q2_collision_get_node(&sim->coll, (u32)node, &n))
            out->contents = n.contents;
    }

    if (complete)
        return;

    out->hit = true;

    if (sim->coll.hit_plane_index >= 0) {
        q2_coll_plane pl;

        if (q2_collision_get_plane(&sim->coll,
                                   (u32)sim->coll.hit_plane_index, &pl)) {
            out->normal[0] = pl.nx;
            out->normal[1] = pl.ny;
            out->normal[2] = pl.nz;
        }
    }

    /* Longest-axis ratio: the axis with the largest intended travel is the one
     * whose quotient carries the least rounding error. */
    {
        int axis = 0, i;
        s32 span = 0;

        for (i = 0; i < 3; i++) {
            s32 d = end[i] - start[i];
            if (d < 0) d = -d;
            if (d > span) { span = d; axis = i; }
        }

        if (span > 0) {
            s64 got = (s64)(pos[axis] - start[axis]);
            s64 all = (s64)(end[axis] - start[axis]);
            s32 f   = (s32)((got * Q2_ONE_12) / all);

            if (f < 0)          f = 0;
            if (f > Q2_ONE_12)  f = Q2_ONE_12;
            out->fraction = f;
        } else {
            out->fraction = Q2_ONE_12;
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

    /* --- jump ------------------------------------------------------------ */
    if (p->on_ground && input->jump) {
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
    /*
     * 0x80045FA4..0x80046090, the airborne integrator, verbatim:
     *
     *     dv   = gravity * dt
     *     dY   = (vel.y*dt + (dv*dt)/2) / 320
     *     dX   = vel.x*dt / 320          dZ = vel.z*dt / 320
     *     vel.y += dv
     *
     * The half-step on gravity is not decoration — dropping it changes every
     * jump arc — and both divisions truncate toward zero, which is what the
     * 0x66666667 magic sequence with its sign correction computes.
     */
    {
        s16 delta[3];
        s32 dv   = (sim->gravity ? sim->gravity : Q2_GRAVITY) * dt;
        s32 half = (dv * dt) / 2;

        /* The mover's frame: the entity origin, Q2_EYE_BASE above the feet. */
        p->ent.pos[0] = p->pos[0];
        p->ent.pos[1] = q2_sim_origin_y(p->pos[1]);
        p->ent.pos[2] = p->pos[2];

        /* --- contents ---------------------------------------------------- */
        /*
         * 0x800458A0 runs the volume query BEFORE the integration, with mask
         * 0x3360, and turns the answer into the entity's own liquid flags. Those
         * flags then choose which of the three vertical rules below applies and
         * halve the step height, so this has to come first.
         */
        update_contents(sim);

        delta[0] = (s16)(((s64)p->vel[0] * dt) / Q2_VEL_DIV);
        delta[1] = (s16)((((s64)p->vel[1] * dt) + half) / Q2_VEL_DIV);
        delta[2] = (s16)(((s64)p->vel[2] * dt) / Q2_VEL_DIV);

        p->vel[1] += dv;

        /*
         * Three mutually exclusive rules, in the original's own order
         * (0x8004601C onward). The 8192 clamp DOES apply on the player path —
         * 0x80046084 tests the newly stored velocity with `slti 8193` and
         * rewrites it — but only in the arm where neither liquid flag is set.
         * An earlier note in FORMATS.md said the clamp lived only in the mover
         * at 0x800463E8; that was reading the wrong one of the two.
         */
        if (p->ent.flags & Q2_ENT_LIQUID_FLOAT) {
            p->vel[1] = ease_toward(p->vel[1], Q2_LIQUID_FLOAT_VY, dt * 64);
        } else if (p->ent.flags & Q2_ENT_LIQUID_SINK) {
            if (p->vel[1] < Q2_LIQUID_SINK_VY)
                p->vel[1] = ease_toward(p->vel[1], Q2_LIQUID_SINK_VY, dt * 64);
            else
                p->vel[1] -= dt * 24;
        } else if (p->vel[1] > Q2_TERMINAL_VY) {
            p->vel[1] = Q2_TERMINAL_VY;
        }

        /* --- collide ----------------------------------------------------- */
        if (sim->coll_ready) {
            /*
             * Lift, slide, drop — 0x8004583C. Ground is decided by the drop
             * alone, which is why walking into a wall does not read as landing
             * on it, and why a step up to Q2_STEP_HEIGHT costs nothing.
             */
            q2_move_step(&sim->coll, &p->ent, delta,
                         sim->volume_count ? &sim->move_world : NULL);

            p->pos[0] = p->ent.pos[0];
            p->pos[1] = q2_sim_feet_y(p->ent.pos[1]);
            p->pos[2] = p->ent.pos[2];

            p->on_ground      = (p->ent.flags & Q2_ENT_GROUNDED_MASK) != 0;
            sim->current_node = p->ent.node;

            if (p->on_ground)
                p->vel[1] = 0;
        } else {
            /* Without a hull, fall back to the seeded ground plane so the
             * player does not drop forever in a zone that failed to parse. */
            p->pos[0] += delta[0];
            p->pos[1] += delta[1];
            p->pos[2] += delta[2];

            p->on_ground = false;
            if (p->pos[1] >= p->ground_y) {
                p->pos[1]    = p->ground_y;
                p->vel[1]    = 0;
                p->on_ground = true;
            }
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

    /*
     * The level clock the weapons gate on is this same dt counter: 300 units to
     * the second, which is what makes the universal 30-tick refire a tenth of a
     * second and what the mover scripting's own time unit is (userfuncs.h).
     */
    sim->level_time += dt;

    if (input->attack)
        q2_sim_fire(sim);

    q2_sim_combat_tick(sim);

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
