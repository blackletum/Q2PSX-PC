#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "entitydraw.h"   /* q2_entity_resolve_model */
#include "pad.h"          /* the default control style */
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
    sim->player[sim->cur_player].ent.node = -1;

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
    sim->player[sim->cur_player].view_height = Q2_VIEW_STAND;

    /*
     * STANDARD A, which is what 0x8001BDA8 writes for every player. It matters
     * that this is not left at zero: zero is RIGHT MOUSE, and the two are on
     * opposite sides of the branch that decides whether the look rate is eased
     * or set — so a caller that never configures a pad would otherwise get the
     * mouse's instant turn on a keyboard.
     */
    sim->player[sim->cur_player].look_scheme = Q2_PAD_STYLE_STANDARD_A;

    q2_sim_combat_init(sim);
}

void q2_sim_free(q2_sim *sim)
{
    if (!sim)
        return;
    q2_event_rt_free(&sim->event_rt);
    q2_entity_set_free(&sim->entities);
    free(sim->trigger_inside);
    free(sim->volumes);
    free(sim->volume_env);
    memset(sim, 0, sizeof(*sim));
}

q2_result q2_sim_attach_items(q2_sim *sim, const q2_common_file *common,
                              int zone, const q2_item_table *table,
                              const struct q2_model_bank *bank)
{
    q2_population pop;
    q2_result r;

    if (!sim || !common)
        return Q2_ERR_INVALID_ARG;

    q2_entity_set_free(&sim->entities);
    q2_entity_world_init(&sim->ent_world);
    sim->entities_ready = false;

    /*
     * The world's view of the session. `multiplayer` is 0x800AEBCC, which is
     * the same flag the item handlers read to double an ammo box and the same
     * one that decides whether a collected item respawns at all.
     */
    sim->ent_world.deathmatch = sim->multiplayer;
    sim->ent_world.items      = table;
    sim->ent_world.level_time = sim->level_time;

    /* One player: this sim models one. The touch sweep walks
     * `player_count`, so registering slot 0 is what makes it run. */
    q2_entity_world_add_player(&sim->ent_world, 0, &sim->combat.inv,
                               sim->player[sim->cur_player].pos);

    r = q2_population_parse(&pop, common);
    if (r != Q2_OK)
        return r;

    r = q2_item_spawn_zone(&sim->entities, &pop, zone, table, NULL);
    if (r != Q2_OK)
        return r;

    if (bank) {
        u32 i;
        for (i = 0; i < sim->entities.count; i++)
            q2_entity_resolve_model(&sim->entities.ent[i], bank);
    }

    sim->entities_ready = true;
    return Q2_OK;
}

const q2_ent_events *q2_sim_entity_events(const q2_sim *sim)
{
    return sim ? &sim->ent_world.events : NULL;
}

/*
 * 0x80027E64's predicate arm, resolved once instead of run every tick.
 *
 * Six of the primitives a volume can call have a body that is one OR into
 * entity+0x98 and nothing else (0x8002E574, 0x8002E594, 0x8002E5B4, 0x8002F214
 * and two more). Which bit each sets is already in userfuncs.h, so a volume's
 * environment is a property of its record and can be read at load.
 *
 * Four of the six are here. INACID and INLAVA are not, and the reason is the
 * frame's own clear at 0x8003A25C: it clears 0x4, 0x8, 0x100, 0x200, 0x400,
 * 0x4000 and 0x20000, and INLAVA's 0x1000 is not among them. A bit the frame
 * does not clear is not a per-tick environment — it latches, and something else
 * owns its lifetime — so asserting it here would set a flag nothing takes back.
 * Their damage is not this function's to do either.
 */
static u32 record_env_mask(const q2_events *ev, const q2_userfuncs *uf,
                           u32 record_offset)
{
    q2_event_record rec;
    u32 mask = 0;
    u32 i;

    if (!q2_events_record_at(ev, record_offset, &rec))
        return 0;

    for (i = 0; i < rec.n_items; i++) {
        q2_event_item item;
        u8            call_index;
        q2_uf_prim    prim;

        if (!q2_events_get_item(ev, &rec, i, &item))
            break;
        if (item.opcode != Q2_EVOP_CALL)
            continue;
        if (!q2_events_get_call_index(&item, &call_index))
            continue;

        prim = q2_userfuncs_prim(uf, call_index);

        switch (prim) {
        case Q2_UF_INWATER:      mask |= Q2_ENV_INWATER;     break;
        case Q2_UF_UNDERWATER:   mask |= Q2_ENV_UNDERWATER;  break;
        case Q2_UF_INCROUCH:     mask |= Q2_ENV_INCROUCH;    break;
        case Q2_UF_INLOWCROUCH:  mask |= Q2_ENV_INLOWCROUCH; break;

        /*
         * The answer to worldscale.h's "nothing in the image sets it". Nothing
         * in the EXECUTABLE does; DONTJUMP does, and it is the same bit. So a
         * no-jump zone is authored per map exactly as a crouch zone is, and the
         * gate in the jump was never dead code — it was waiting for its caller.
         */
        case Q2_UF_DONTJUMP:     mask |= Q2_ENV_DONTJUMP;    break;

        default: break;
        }
    }

    return mask;
}

/* Build the per-volume environment masks. Silently leaves them all zero when
 * the map has no UserFuncs table, which is the same as having no such volume. */
static void build_volume_env(q2_sim *sim)
{
    u32 i;

    free(sim->volume_env);
    sim->volume_env = NULL;

    if (!sim->triggers_ready || !sim->events_ready ||
        !sim->userfuncs_ready || sim->triggers.count == 0)
        return;

    sim->volume_env = (u32 *)calloc(sim->triggers.count, sizeof(u32));
    if (!sim->volume_env)
        return;

    for (i = 0; i < sim->triggers.count; i++) {
        q2_trigger t;

        if (!q2_trigger_get(&sim->triggers, i, &t))
            continue;
        if (t.event_offset == Q2_TRIGGER_NO_EVENT)
            continue;

        sim->volume_env[i] = record_env_mask(&sim->events, &sim->userfuncs,
                                             t.event_offset);
    }
}

q2_result q2_sim_attach_gameplay(q2_sim *sim, const q2_common_file *common)
{
    if (!sim || !common)
        return Q2_ERR_INVALID_ARG;

    sim->userfuncs_ready =
        q2_userfuncs_parse(&sim->userfuncs, common) == Q2_OK;

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

    /* And a fourth: asserting an environment while you stand in one. */
    build_volume_env(sim);

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
        bool inside = q2_trigger_contains(&sim->triggers, i, sim->player[sim->cur_player].pos);
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
 * 0x8006FE3C — move `*p` toward `target` by at most `rate`.
 *
 * Not a lerp and not a helper the port invented: this one function is the whole
 * of the engine's acceleration, deceleration, view-height ease, turn-rate ease
 * and liquid buoyancy. There is no separate friction term anywhere in the
 * player's movement, because a clamped approach toward a zero target already
 * decelerates.
 *
 * Two details that matter for bit-exactness:
 *
 *   - It operates on a HALFWORD. The step is computed in 32 bits and stored
 *     with `sh`, so a step that would overshoot the s16 range wraps rather than
 *     saturating. Callers pass values well inside the range, but the truncation
 *     is what the console does and it is free to reproduce.
 *   - `rate` is used raw. The original does not take its absolute value, so a
 *     negative rate walks AWAY from the target. No caller does that; the port
 *     does not add a guard the hardware does not have.
 *
 * Returns true when the value has arrived, which is what the original's return
 * value at 0x8006FEB4 reports.
 */
static bool ease16(s16 *p, s32 target, s32 rate)
{
    s32 cur = *p;             /* lh: sign-extended to 32 bits           */
    s16 tgt = (s16)target;    /* sll 16 / sra 16 at 0x8006FE3C          */
    s32 r16 = (s16)rate;      /* the TEST uses the 16-bit-truncated rate */

    /*
     * The test is a 32-bit comparison (`addu v0, a3, v0` at 0x8006FE60 with no
     * narrowing) while the store truncates (`sh` at 0x8006FE9C). Narrowing the
     * test as well looks harmless and is not: a step that would overshoot the
     * s16 range wraps to the far side, compares as smaller than the target, and
     * the value is stored instead of being snapped.
     */
    if (cur < tgt)
        *p = (cur + r16 < tgt) ? (s16)(cur + rate) : tgt;
    else
        *p = (tgt < cur - r16) ? (s16)(cur - rate) : tgt;

    return *p == tgt;
}

/* The same, for the s32-typed fields the port keeps wider than the original's
 * halfwords. The arithmetic still wraps at 16 bits so the answer is identical. */
static s32 ease32(s32 v, s32 target, s32 rate)
{
    s16 t = (s16)v;

    ease16(&t, target, rate);
    return t;
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

    sim->player[sim->cur_player].pos[0] = pos[0];
    sim->player[sim->cur_player].pos[1] = pos[1];
    sim->player[sim->cur_player].pos[2] = pos[2];

    sim->player[sim->cur_player].vel[0] = sim->player[sim->cur_player].vel[1] = sim->player[sim->cur_player].vel[2] = 0;
    sim->player[sim->cur_player].yaw    = yaw;
    sim->player[sim->cur_player].pitch  = 0;
    sim->player[sim->cur_player].roll   = 0;

    sim->player[sim->cur_player].wish[0] = sim->player[sim->cur_player].wish[1] = sim->player[sim->cur_player].wish[2] = 0;
    sim->player[sim->cur_player].pitch_rate = sim->player[sim->cur_player].yaw_rate = 0;

    sim->player[sim->cur_player].impulse[0] = sim->player[sim->cur_player].impulse[1] = sim->player[sim->cur_player].impulse[2] = 0;
    sim->player[sim->cur_player].impulse_armed = false;
    sim->player[sim->cur_player].jump_hold     = 0;

    sim->player[sim->cur_player].fall_value    = 0;
    sim->player[sim->cur_player].fall_time     = 0;
    sim->player[sim->cur_player].footstep_time = 0;
    sim->player[sim->cur_player].foot          = 0;

    /*
     * The view's own state. A spawn that kept these would put the player into
     * the world already flinching from a hit taken in the last life, or with
     * the recentre half way through walking their pitch to zero.
     *
     * `prev_health`/`prev_armour` are seeded from the inventory rather than
     * zeroed, because update_pain diffs them: seeded at zero, the first tick
     * would read a 100-point rise and, worse, a later drop back to 100 would
     * read as damage.
     */
    sim->player[sim->cur_player].kick[0] = sim->player[sim->cur_player].kick[1] = sim->player[sim->cur_player].kick[2] = 0;
    sim->player[sim->cur_player].kick_time     = 0;
    sim->player[sim->cur_player].hurt_kick[0]  = sim->player[sim->cur_player].hurt_kick[1] = 0;
    sim->player[sim->cur_player].pain_time     = 0;
    sim->player[sim->cur_player].look_hist     = 0;
    sim->player[sim->cur_player].recentring    = false;
    sim->player[sim->cur_player].autocentre    = 0;
    sim->player[sim->cur_player].ent2_flags    = 0;
    sim->player[sim->cur_player].prev_health   = sim->combat.inv.health;
    sim->player[sim->cur_player].prev_armour   = sim->combat.inv.armour;

    sim->player[sim->cur_player].on_ground   = false;
    sim->player[sim->cur_player].crouching   = false;
    sim->player[sim->cur_player].view_height = Q2_VIEW_STAND;
    sim->player[sim->cur_player].ground_y    = pos[1];

    /*
     * The mover works in the ENTITY ORIGIN's frame, which sits Q2_EYE_BASE
     * above the feet — see the note on q2_sim_origin_y in sim.h. `pos` is a
     * StartPos, i.e. the feet, so it is lifted here and lowered again after
     * every move.
     */
    memset(&sim->player[sim->cur_player].ent, 0, sizeof(sim->player[sim->cur_player].ent));
    sim->player[sim->cur_player].ent.pos[0] = pos[0];
    sim->player[sim->cur_player].ent.pos[1] = q2_sim_origin_y(pos[1]);
    sim->player[sim->cur_player].ent.pos[2] = pos[2];

    /*
     * Locate the cell we spawned into. A spawn that lands outside every hull
     * leaves the cached cell at -1, which is what the original stores in a
     * fresh entity: the first move then pays for one brute-force sweep and
     * self-corrects (0x80044C74).
     */
    sim->player[sim->cur_player].ent.node = sim->coll_ready
        ? q2_coll_find_node(&sim->coll, sim->player[sim->cur_player].ent.pos, -1, true)
        : -1;
    sim->current_node = sim->player[sim->cur_player].ent.node;

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
    q2_player *p = &sim->player[sim->cur_player];
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
/* The player's own frame — 0x8003A1C8                                        */
/* ------------------------------------------------------------------------- */
/*
 * Everything below is one function in the original, transcribed in its own
 * order because the order is load-bearing: the view offset is chosen from LAST
 * frame's environment flags, the jump reads the ground the previous move left,
 * and the look angles integrate before the wish velocity is rotated by them.
 */

/*
 * 0x8003A208 — the view offset target, and the environment flag reset.
 *
 * Read before the flags are cleared, so the height follows the volume the
 * player was in on the previous tick. That one-tick lag is visible when you
 * walk out of a crouch zone and is not worth "fixing".
 */
static void update_view_offset(q2_sim *sim, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 target;
    s16 v;

    if (p->ent.flags & Q2_ENT_INCROUCH)
        target = Q2_VIEW_MID;
    else if (p->ent.flags & Q2_ENT_INLOWCROUCH)
        target = Q2_VIEW_CROUCH;
    else
        target = Q2_VIEW_STAND;

    v = (s16)p->view_height;
    ease16(&v, target, dt * Q2_VIEW_EASE_RATE);
    p->view_height = v;

    p->crouching = (p->ent.flags & (Q2_ENT_INCROUCH | Q2_ENT_INLOWCROUCH)) != 0;
}

/*
 * 0x8003A25C — clear the environment flags, then let the volumes re-assert them.
 *
 * The original clears them and calls the volume dispatcher at 0x80027E64, whose
 * INWATER / UNDERWATER / INCROUCH / INLOWCROUCH events do nothing but OR a bit
 * back in. `sim->env_flags` stands in for that dispatch until the port can
 * resolve a volume's event to its UserFuncs primitive.
 */
static void update_env_flags(q2_sim *sim)
{
    u32 env = sim->env_flags;
    u32 i;

    /*
     * Level-triggered, not edge-triggered, and that is the whole point: these
     * bits describe where the player IS. update_triggers' entered-set is for
     * scripts that must fire once; this must be re-asserted every tick or the
     * player stands up in the middle of a crouch tunnel.
     *
     * Tested against the position the previous tick's move left, because the
     * dispatcher runs at the top of the frame — which is also why walking out
     * of a crouch zone takes one tick to stand up.
     */
    if (sim->volume_env && sim->triggers_ready) {
        for (i = 0; i < sim->triggers.count; i++) {
            if (!sim->volume_env[i])
                continue;
            if (q2_trigger_contains(&sim->triggers, i, sim->player[sim->cur_player].pos))
                env |= sim->volume_env[i];
        }
    }

    sim->player[sim->cur_player].ent.flags &= ~(u32)Q2_ENT_ENV_MASK;
    sim->player[sim->cur_player].ent.flags |= env & Q2_ENT_ENV_MASK;
}

/* 0x8003A3B0 — max speed. Three cases in the original's order. */
static s32 max_speed(u32 flags)
{
    if (flags & Q2_ENT_INLOWCROUCH)
        return Q2_SPEED_LOWCROUCH;
    if (flags & (Q2_ENT_INWATER | Q2_ENT_UNDERWATER | Q2_ENT_INCROUCH))
        return Q2_SPEED_WET;
    return Q2_SPEED_NORMAL;
}

/*
 * 0x8003A4EC — the water-exit jump.
 *
 * Having been UNDERWATER last tick and not being now, with the last contact not
 * flat enough to be a floor, synthesises a jump press and hands the entity a
 * flat ground normal so the jump's own grounded test passes. It is what lifts
 * you out of a pool onto its edge; without it you swim into the wall forever.
 */
static void water_exit_jump(q2_sim *sim, bool was_underwater, u32 *buttons)
{
    q2_player *p = &sim->player[sim->cur_player];

    if (!was_underwater)
        return;
    if (p->ent.flags & (Q2_ENT_UNDERWATER | Q2_ENT_WALL_CONTACT))
        return;
    if (p->ent.last_normal[1] >= 2048)
        return;
    /* 0x8003A544: not while flying — a weightless entity has nothing to climb
     * out of. Last of the five tests, and the only one that is not about the
     * water itself. */
    if (p->ent2_flags & Q2_ENT2_FLY)
        return;

    *buttons |= Q2_BTN_JUMP;
    p->ent.flags &= ~(u32)Q2_ENT_JUMP_LATCH;
    p->jump_hold  = 0;

    p->ent.ground_normal[0] = 0;
    p->ent.ground_normal[1] = 4096;   /* 0x800AE928: (0, 4096, 0), flat floor */
    p->ent.ground_normal[2] = 0;
}

/*
 * 0x8003A584 — the wish velocity.
 *
 * One clamped approach per horizontal axis toward `(maxspeed * axis) >> 7`, so
 * the stick asks for a SPEED and the rate limits how fast that speed is
 * reached. The rate is the only thing the environment changes, and the two
 * underwater values are asymmetric on purpose: 30 while the stick is off centre
 * and 14 while it is centred, so swimming drifts.
 */
static void update_wish(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p     = &sim->player[sim->cur_player];
    s32        speed = max_speed(p->ent.flags);
    bool       moving;
    s32        rate;

    /* 0x8003A4F8: the two axes are OR'd as unsigned halfwords, so any non-zero
     * deflection on either counts — including one that cancels the other. */
    moving = ((u16)in->forward | (u16)in->side) != 0;

    if (p->ent.flags & Q2_ENT_UNDERWATER)
        rate = moving ? Q2_WISH_RATE_SWIM : Q2_WISH_RATE_DRIFT;
    else
        rate = Q2_WISH_RATE;

    rate *= dt;

    /*
     * The >>7 is applied to the 32-bit product and then narrowed, exactly as
     * `sll 9 / sra 16` does: bits above 22 of the product are discarded rather
     * than saturated.
     */
    ease16(&p->wish[2], (s16)(((s32)speed * in->forward) >> Q2_WISH_SHIFT), rate);
    ease16(&p->wish[0], (s16)(((s32)speed * in->side)    >> Q2_WISH_SHIFT), rate);
}

/*
 * 0x8003A658 — the look rates, and 0x8003A990 — the angles.
 *
 * The stick does not move the view. It asks for a TURN RATE, that rate is eased,
 * and the angle integrates from it — which is why the PSX view keeps gliding for
 * a moment after the stick is released, and why a port that adds the stick
 * straight to the angle feels wrong however the sensitivity is tuned.
 *
 * The ease rate is dt normally and dt*4 when the stick opposes the current rate
 * or has returned to centre, so reversing and stopping are both four times
 * sharper than starting.
 */
static void update_look(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 want_pitch = ((s32)in->pitch * Q2_LOOK_SCALE_NUM) >> Q2_LOOK_SCALE_SHIFT;
    s32 want_yaw   = ((s32)in->yaw   * Q2_LOOK_SCALE_NUM) >> Q2_LOOK_SCALE_SHIFT;

    /*
     * 0x8003A670 — `slti v0, style, 6` branching to the EASED arm when the test
     * fails. So the six mouse and stick styles set the rate outright and the
     * three STANDARD ones ease it, which is the right way round for the input
     * each is reading: a stick already carries a proportional rate, a button
     * only carries "now".
     *
     * This comparison used to be inverted here, which made the analogue styles
     * glide after the stick was released and the digital ones snap.
     */
    if (p->look_scheme < Q2_LOOK_SCHEME_ANALOGUE) {
        /* 0x8003A67C. */
        p->pitch_rate = (s16)want_pitch;
        p->yaw_rate   = (s16)want_yaw;
    } else {
        /* 0x8003A6B0: pitch snaps when the product of rate and input is
         * negative, i.e. when they disagree. */
        s32 pr = ((s32)p->pitch_rate * in->pitch < 0)
               ? dt * Q2_LOOK_RATE_SNAP : dt * Q2_LOOK_RATE;
        ease16(&p->pitch_rate, want_pitch, pr);

        /*
         * 0x8003A6F8: yaw is spelled out as a sign comparison rather than a
         * product, and a centred stick takes the snap rate with a target of
         * zero — which is the only reason letting go stops you at all.
         */
        if (in->yaw > 0) {
            ease16(&p->yaw_rate, want_yaw,
                   (p->yaw_rate < 0) ? dt * Q2_LOOK_RATE_SNAP : dt * Q2_LOOK_RATE);
        } else if (in->yaw < 0) {
            ease16(&p->yaw_rate, want_yaw,
                   (p->yaw_rate > 0) ? dt * Q2_LOOK_RATE_SNAP : dt * Q2_LOOK_RATE);
        } else {
            ease16(&p->yaw_rate, 0, dt * Q2_LOOK_RATE_SNAP);
        }
    }
}

/*
 * 0x8003A780 — the view recentre.
 *
 * The AUTOCENTRE row on the player page does not do this; it feeds a separate
 * timer (`autocentre`, client+0x26). What actually pulls the view level is a
 * CHORD: hold both look buttons together and the pitch walks back to zero.
 *
 * The arming is a three-tick shift register rather than a flag, and the exact
 * sequence matters. Frame one of the chord sets history bit 0, `(hist & 7)` is
 * 1 so the arm is set — and then `(hist & 3) == 1` immediately cancels it.
 * Frame two has history 0b11, so it arms and is not cancelled. From frame three
 * `(hist & 7) == 7` and the arm is no longer re-asserted, but it is already set
 * and stays set until the pitch reaches zero. So the chord needs a second tick
 * to take, and a single-tick tap does nothing — which is what stops a scheme
 * that maps both look buttons to one control from recentring constantly.
 *
 * `s0` in the original is "the look rate was SET rather than eased", so the
 * decay in the else arm is exactly the styles that ease.
 */
static void update_recentre(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    u32 look_mask = Q2_BTN_LOOK_DOWN | Q2_BTN_LOOK_UP;
    bool eased = p->look_scheme >= Q2_LOOK_SCHEME_ANALOGUE;

    /* 0x8003A780: shift, then set bit 0 when either look button is held. */
    p->look_hist = (u8)(p->look_hist << 1);
    if (in->buttons & look_mask)
        p->look_hist |= 1u;

    /* 0x8003A7A8: both held, and not already held for three ticks. */
    if ((in->buttons & look_mask) == look_mask && (p->look_hist & 7u) != 7u)
        p->recentring = true;

    /* 0x8003A7D4: the first tick of any press cancels it again. */
    if ((p->look_hist & 3u) == 1u)
        p->recentring = false;

    if (in->pitch != 0) {
        /* 0x8003A8AC: looking at all cancels the AUTOCENTRE timer. */
        p->autocentre = 0;
        return;
    }

    if (p->recentring &&
        !(p->ent2_flags & Q2_ENT2_FLY) &&
        !(p->ent.flags & Q2_ENT_WALL_CONTACT)) {
        /*
         * 0x8003A838. Two stages, and both are needed: a geometric 5/6 that
         * makes a large pitch fall away fast, then the same clamped approach
         * everything else uses so the last few units actually reach zero
         * rather than converging on it.
         */
        s16 v;

        p->pitch = ((s32)(s16)p->pitch * Q2_LOOK_CENTRE_NUM) / Q2_LOOK_CENTRE_DEN;

        v = (s16)p->pitch;
        ease16(&v, 0, dt);
        p->pitch = v;

        p->pitch_rate = 0;

        if (p->pitch == 0)
            p->recentring = false;
        return;
    }

    /*
     * 0x8003A88C. With the pitch stick centred and nothing to recentre, the
     * eased styles bleed their pitch rate at dt*4 — four times what
     * update_look's own zero-target approach uses, and the reason letting go of
     * the look button stops the view rather than coasting it.
     */
    if (eased)
        ease16(&p->pitch_rate, 0, dt * Q2_LOOK_RATE_SNAP);
}

/* 0x8003A990 — integrate the angles from the rates and clamp the pitch. */
static void integrate_look(q2_sim *sim, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];

    p->pitch = (s16)(p->pitch + ((s32)p->pitch_rate * dt) / Q2_LOOK_DIV);
    p->yaw   = (s16)(p->yaw   + ((s32)p->yaw_rate   * dt) / Q2_LOOK_DIV);

    /*
     * +-1024 is +-90 degrees in the 4096-step circle. Hitting the clamp also
     * zeroes the rate (0x8003AA1C / 0x8003AA38), so the view stops dead at
     * straight up rather than pressing against the limit.
     */
    if (p->pitch < -Q2_PITCH_LIMIT) {
        p->pitch      = -Q2_PITCH_LIMIT;
        p->pitch_rate = 0;
    }
    if (p->pitch > Q2_PITCH_LIMIT) {
        p->pitch      = Q2_PITCH_LIMIT;
        p->pitch_rate = 0;
    }
}

/*
 * 0x8003E110 — the jump.
 *
 * It does not touch velocity. It posts an impulse and arms the accumulator, and
 * the integrator applies it later in the same frame — which is why a jump has no
 * frame of delay even though the position update runs on the previous frame's
 * delta.
 *
 * `jump_hold` is not a hold duration. It blocks a repeat, and it is cancelled
 * the moment vertical velocity turns downward, so on flat ground you can jump
 * again as soon as you start falling rather than after 576 ticks.
 */
static bool player_jump(q2_sim *sim, bool pressed, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];
    bool grounded;

    if (p->jump_hold != 0) {
        if (p->vel[1] >= 0 || p->jump_hold < dt)
            p->jump_hold = 0;
        else
            p->jump_hold = (s16)(p->jump_hold - dt);
        return false;
    }

    if (!pressed)
        return false;

    /*
     * 0x8003E198. Nothing in the image ever sets this bit — the player's own
     * frame clears it and no store to entity+0x98 ORs it in — so the gate is
     * dead in practice. It is transcribed rather than dropped because the
     * water-exit path takes the trouble to clear it, which only makes sense if
     * Hammerhead meant it to work.
     */
    if (p->ent.flags & Q2_ENT_JUMP_LATCH)
        return false;

    /* 0x8003E1AC: the same test the drop move uses to declare ground, run
     * against the normal that move left behind. */
    grounded = p->ent.max_slope_ny < p->ent.ground_normal[1];

    if (!grounded && !(p->ent.flags & Q2_ENT_WALL_CONTACT))
        return false;

    /*
     * 0x8003E1D0 — pushing off a wall. The horizontal impulse is the reversed
     * contact normal at full 1.3.12 scale, so a wall jump throws you away from
     * the surface as hard as it throws you up.
     */
    if (p->ent.flags & Q2_ENT_WALL_CONTACT) {
        p->impulse[0] = (s16)(-p->ent.last_normal[0]);
        p->impulse[2] = (s16)(-p->ent.last_normal[2]);
    }

    p->impulse[1]     = Q2_JUMP_IMPULSE;
    p->jump_hold      = Q2_JUMP_HOLD;
    p->impulse_armed  = true;
    return true;
}

/*
 * 0x8003ABC4 — turn the wish velocity into world velocity.
 *
 * Three paths, and which one runs decides whether looking up makes you move up:
 *
 *   - Touching a wall inside a ladder volume: the full view basis, applied
 *     instantly, plus a fixed push along the contact normal (0x8006EFDC).
 *   - The full basis without the push, when the GAME VARIABLES setting is on or
 *     when swimming with the stick off centre (0x8006F11C). Instant, no ease.
 *   - Otherwise the yaw-only rotation, eased into the current velocity
 *     (0x8006FF08 + two 0x8006FE3C calls). This is the normal walking path, and
 *     the ease is why you keep moving briefly after letting go.
 */
static void wish_to_world(q2_sim *sim, bool moving, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];


    /* 0x8006EFDC / 0x8006F11C both rotate by the basis matrix RotMatrix built
     * from the view angles, and both negate the Y row because +Y is down. The
     * port rebuilds the same rotation from the angles directly. */
    if ((p->ent.flags & Q2_ENT_WALL_CONTACT) ||
        sim->full_basis_movement ||
        ((p->ent.flags & Q2_ENT_UNDERWATER) && moving)) {
        s32 sy = q2_sin12(p->yaw),   cy = q2_cos12(p->yaw);
        s32 sp = q2_sin12(p->pitch), cp = q2_cos12(p->pitch);
        s32 fwd = p->wish[2], side = p->wish[0];

        /*
         * The pitched forward axis. The basis is built from the view angles, so
         * a pitched view tilts movement — that is the whole point of this path.
         */
        s32 vx = (cy * side + ((sy * cp) >> Q2_FRAC_12) * fwd) >> Q2_FRAC_12;
        s32 vz = (((cy * cp) >> Q2_FRAC_12) * fwd - sy * side) >> Q2_FRAC_12;
        s32 vy = -((sp * fwd) >> Q2_FRAC_12);

        if (p->ent.flags & Q2_ENT_WALL_CONTACT) {
            /* 0x8006F0E4: the normal is added at >>5, i.e. 128 units at full
             * 1.3.12 scale. */
            vx += p->ent.last_normal[0] >> 5;
            vz += p->ent.last_normal[2] >> 5;
        }

        p->vel[0] = (s16)vx;
        p->vel[1] = (s16)vy;
        p->vel[2] = (s16)vz;
    } else {
        s32 sy = q2_sin12(p->yaw), cy = q2_cos12(p->yaw);
        s32 fwd = p->wish[2], side = p->wish[0];

        /* 0x8006FF08. Only X and Z are written; Y is left to gravity. */
        s32 wx = (cy * side + sy * fwd) >> Q2_FRAC_12;
        s32 wz = (cy * fwd  - sy * side) >> Q2_FRAC_12;

        s32 rate = ((p->ent.flags & Q2_ENT_UNDERWATER) && !moving)
                 ? dt * Q2_VEL_RATE_SLOW : dt * Q2_VEL_RATE;

        p->vel[0] = ease32(p->vel[0], wx, rate);
        p->vel[2] = ease32(p->vel[2], wz, rate);
    }

    /*
     * 0x8003AD0C — the ground stick.
     *
     * A downward bias of 768 whenever the player is asking to move, or whenever
     * they are submerged with the full-basis setting off. Combined with the
     * step-down in the mover it is what keeps you glued to a floor across a ledge
     * or a slope instead of taking off; drop it and walking downhill becomes a
     * series of small jumps.
     *
     * The condition is spelled out here rather than folded into the branch above,
     * because it is NOT the same predicate: 0x8003AD1C tests UNDERWATER on its
     * own, whereas the branch above tests UNDERWATER *and* moving.
     */
    if (p->ent.flags & Q2_ENT_WALL_CONTACT)
        return;

    if (moving ||
        ((p->ent.flags & Q2_ENT_UNDERWATER) && !sim->full_basis_movement))
        p->vel[1] = ease32(p->vel[1], Q2_GROUND_STICK_VY,
                           dt * Q2_GROUND_STICK_RATE);
}

/* 0x8003AB98 — swimming up. Only while fully submerged, and only up to -768. */
static void swim_up(q2_sim *sim, const q2_input *in, s32 dt)
{
    q2_player *p = &sim->player[sim->cur_player];

    if (!(p->ent.flags & Q2_ENT_UNDERWATER))
        return;
    if (!(in->buttons & Q2_BTN_SWIM_UP))
        return;
    if (p->vel[1] < Q2_SWIM_UP_VY)
        return;

    p->vel[1] = ease32(p->vel[1], Q2_SWIM_UP_VY, dt * Q2_SWIM_UP_RATE);
}

/* 0x8003AB18 — the strafe roll. Derived from the wish, not from the input, so it
 * decays with the same ease everything else uses. Skipped entirely when the
 * entity carries entity+0x10C bit 19, which nothing on the disc sets. */
static void update_roll(q2_sim *sim)
{
    if (sim->player[sim->cur_player].ent2_flags & 0x80000u)
        return;
    sim->player[sim->cur_player].roll = -(s32)sim->player[sim->cur_player].wish[0] / Q2_ROLL_DIV;
}

/*
 * 0x800459E8 — project the velocity onto the ground plane.
 *
 * This is the rule that was missing from the port, and it is the one that makes
 * standing still work. At the TOP of the mover, before any move runs, an entity
 * that was on the ground last frame has its vertical velocity REPLACED by
 * whatever makes the velocity vector parallel to the surface it is standing on:
 *
 *     vel.y = -(vel.x*n.x + vel.z*n.z) / n.y
 *
 * On flat ground n is (0, 4096, 0) and that is simply zero, which is why gravity
 * does not accumulate while you stand there. On a slope it is the vertical
 * component needed to follow the surface, which is why you walk down a ramp
 * instead of stepping off it and re-landing.
 *
 * It is also what drives fall damage. The mover itself never clears velocity on
 * contact, so a landing leaves the full falling speed in place; it is THIS, on
 * the following tick, that snaps it to the surface — and 0x80039AA4 measures the
 * difference. Fall damage therefore lands one tick after the impact, which is
 * exactly what the console does.
 *
 * The exclusions are as important as the rule: not on a ladder, not underwater,
 * not with flag 0x80 — all three of which need to keep a velocity that is not
 * parallel to anything.
 */
static void ground_project(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 ny = p->ent.ground_normal[1];

    if (p->ent.max_slope_ny >= ny)
        return;
    if (!(p->ent.flags & (Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY | 0x2000u)))
        return;
    if (p->ent.flags & (Q2_ENT_WALL_CONTACT | Q2_ENT_UNDERWATER |
                        Q2_ENT_NO_FOOTSTEP))
        return;
    if (ny == 0)
        return;                 /* the original would trap; it cannot happen */

    p->vel[1] = (s16)(-((s32)(s16)p->vel[0] * p->ent.ground_normal[0] +
                        (s32)(s16)p->vel[2] * p->ent.ground_normal[2]) / ny);
}

/*
 * 0x80045E74..0x80046200 — the integrator, and the delta the NEXT frame moves by.
 *
 * The single most surprising thing about this engine's physics is the order. The
 * three moves at 0x80045B24 consume `entity+0xEC`, and `entity+0xEC` is written
 * HERE, at the end of the same function. So a frame moves by the delta the
 * previous frame computed: velocity and position are permanently one tick apart.
 * Reproducing that is not pedantry — it is a tick of input latency and a tick of
 * gravity, and jump arcs land in different places without it.
 *
 * There are two arms, and which one runs decides whether gravity exists at all:
 *
 *   arm 1, no gravity — UNDERWATER (or flag 0x80), or `entity+0x10C & 0x1000`,
 *                       or on a ladder with no impulse pending. Applies the
 *                       impulse unconditionally and drops the ground flags.
 *   arm 2, gravity    — everything else. Applies the impulse only when armed.
 *
 * So swimming and climbing are weightless, which is why neither needs a special
 * case anywhere else.
 */
static void integrate_vertical(q2_sim *sim, s32 dt, s16 out_delta[3])
{
    q2_player *p  = &sim->player[sim->cur_player];
    s32        g  = sim->gravity ? sim->gravity : Q2_GRAVITY;
    s32        dv = g * dt;
    s32        dvdt;
    s32        half;
    bool       no_gravity;

    /* 0x80045E74: the horizontal deltas are computed for both arms, before any
     * impulse, and only the gravity arm's re-derivation can replace them. */
    out_delta[0] = (s16)(((s32)(s16)p->vel[0] * dt) / Q2_VEL_DIV);
    out_delta[2] = (s16)(((s32)(s16)p->vel[2] * dt) / Q2_VEL_DIV);

    /*
     * 0x80045EE0..0x80045F0C, in the original's own short-circuit order.
     *
     * The FIRST test is entity+0x10C bit 12, which this transcription described
     * and did not implement — so the fly cheat steered movement by the view
     * basis while still falling. It is checked before the +0x98 word because
     * that is the order the branches sit in, and being first is what lets it
     * override the liquid and ladder arms rather than compete with them.
     */
    no_gravity = (p->ent2_flags & Q2_ENT2_FLY) != 0
              || (p->ent.flags & (Q2_ENT_UNDERWATER | Q2_ENT_NO_FOOTSTEP)) != 0
              || ((p->ent.flags & Q2_ENT_WALL_CONTACT) && !p->impulse_armed);

    if (no_gravity) {
        /* 0x80045F14. The vertical delta comes from the velocity BEFORE the
         * impulse, so a jump off a ladder shows up on the following frame. */
        out_delta[1] = (s16)(((s32)(s16)p->vel[1] * dt) / Q2_VEL_DIV);

        p->ent.flags &= ~(u32)(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY);
        p->on_ground  = false;

        p->vel[0] = (s16)(p->vel[0] + p->impulse[0]);
        p->vel[1] = (s16)(p->vel[1] + p->impulse[1]);
        p->vel[2] = (s16)(p->vel[2] + p->impulse[2]);

        p->impulse[0] = p->impulse[1] = p->impulse[2] = 0;
        p->impulse_armed = false;
        return;
    }

    /*
     * 0x80045FA4. (dv*dt)/2 carries the sign-bit bias the compiler emitted at
     * 0x80045FE0, which makes the halving truncate toward zero and not -inf.
     */
    dvdt = dv * dt;
    half = (dvdt + (s32)(((u32)dvdt) >> 31)) >> 1;

    out_delta[1] = (s16)((((s32)(s16)p->vel[1] * dt) + half) / Q2_VEL_DIV);
    p->vel[1]    = (s16)(p->vel[1] + dv);

    /*
     * The three vertical rules, mutually exclusive and in the original's order.
     * The 0x08 arm is the one an earlier pass had inverted: 0x80046050 branches
     * on `vel.y < 1024` into the `-= dt*24` arm, so a 0x0200 volume pushes you
     * UP until you are falling faster than 1024 and only then caps the fall.
     */
    if (p->ent.flags & Q2_ENT_LIQUID_FLOAT) {
        p->vel[1] = ease32(p->vel[1], Q2_LIQUID_FLOAT_VY,
                           dt * Q2_LIQUID_EASE_RATE);
    } else if (p->ent.flags & Q2_ENT_LIQUID_SINK) {
        if ((s16)p->vel[1] < Q2_LIQUID_SINK_VY)
            p->vel[1] = (s16)(p->vel[1] - dt * Q2_LIQUID_SINK_PUSH);
        else
            p->vel[1] = ease32(p->vel[1], Q2_LIQUID_SINK_VY,
                               dt * Q2_LIQUID_EASE_RATE);
    } else if ((s16)p->vel[1] > Q2_TERMINAL_VY) {
        p->vel[1] = Q2_TERMINAL_VY;
    }

    /* 0x80046094 — the impulse, and only when the poster armed it. */
    if (!p->impulse_armed)
        return;

    /*
     * 0x800460A4..0x800460FC: the entity leaves the ground, its ground normal
     * is cleared and its last contact is replaced by the flat (0, 4096, 0) at
     * 0x800AE928. Without this a jump that starts on a floor keeps the floor's
     * normal and the velocity clip takes the jump straight back off.
     */
    p->ent.flags &= ~(u32)(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY |
                           Q2_ENT_WALL_CONTACT | 0x2000u);
    p->on_ground = false;
    p->ent.ground_normal[0] = p->ent.ground_normal[1] = p->ent.ground_normal[2] = 0;
    p->ent.last_normal[0]   = 0;
    p->ent.last_normal[1]   = 4096;
    p->ent.last_normal[2]   = 0;

    p->vel[0] = (s16)(p->vel[0] + p->impulse[0]);
    p->vel[1] = (s16)(p->vel[1] + p->impulse[1]);

    /*
     * 0x80046140. Single player only, and it is the same -3072 the jump posts —
     * so a plain jump lands exactly on the ceiling and no stack of impulses,
     * rocket jump included, can beat it.
     */
    if (!sim->multiplayer && (s16)p->vel[1] < Q2_IMPULSE_CEILING)
        p->vel[1] = Q2_IMPULSE_CEILING;

    p->vel[2] = (s16)(p->vel[2] + p->impulse[2]);

    /*
     * 0x800461DC — all three deltas re-derived from the post-impulse velocity,
     * with no gravity half-step. It still lands on the NEXT frame's move, but it
     * lands one tick earlier than waiting for the following integration would.
     */
    out_delta[0] = (s16)(((s32)(s16)p->vel[0] * dt) / Q2_VEL_DIV);
    out_delta[1] = (s16)(((s32)(s16)p->vel[1] * dt) / Q2_VEL_DIV);
    out_delta[2] = (s16)(((s32)(s16)p->vel[2] * dt) / Q2_VEL_DIV);

    p->impulse[0] = p->impulse[1] = p->impulse[2] = 0;
    p->impulse_armed = false;
}

/*
 * 0x80039AE4 — remove the velocity that went into the surface just hit.
 *
 * Runs only when the contact was NOT flat enough to stand on, which is what
 * keeps a landing out of it: the ground case is handled by the mover and the
 * fall-damage delta below depends on it.
 *
 * Each term of the dot product is shifted separately with a +4095 bias on
 * negatives, so the whole thing truncates toward zero per term rather than once
 * at the end. Doing it in one 64-bit product gives a different answer.
 */
static s32 dot_term(s32 n, s32 v)
{
    s32 prod = n * v;

    if (prod < 0)
        prod += 4095;
    return prod >> Q2_FRAC_12;
}

/*
 * And the multiply BACK is not the same operation. 0x80039B80, 0x80039B90 and
 * 0x80039BA4 are bare `sra ..., 12` with no bias, so the second stage truncates
 * toward -inf while the dot product's terms truncate toward zero. Using one
 * helper for both — the obvious tidy-up — is off by one on negative components.
 */
static s32 scale_term(s32 n, s32 d)
{
    return (n * d) >> Q2_FRAC_12;
}

static void clip_velocity(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    const s16 *n = p->ent.last_normal;
    s32 d, keep;

    if (n[1] >= p->ent.max_slope_ny)
        return;

    d = dot_term(n[0], p->vel[0]) + dot_term(n[1], p->vel[1]) +
        dot_term(n[2], p->vel[2]);

    /* 0x80039B50: captured from the velocity BEFORE the clip, and floored at
     * zero so a rising entity keeps nothing. */
    keep = (p->vel[1] >= 0) ? (s16)p->vel[1] : 0;

    p->vel[0] = (s16)(p->vel[0] - scale_term(n[0], d));
    p->vel[1] = (s16)(p->vel[1] - scale_term(n[1], d));
    p->vel[2] = (s16)(p->vel[2] - scale_term(n[2], d));

    /*
     * 0x80039BB4 — restore the fall rate.
     *
     * If the clip slowed the descent, the whole vector is rescaled so the
     * vertical component is what it was and the horizontal components grow to
     * match. That is what makes a steep slope shed you sideways instead of
     * catching you. When `keep` is zero — an entity that was rising — the scale
     * is zero and all velocity is lost, which is exactly what hitting an
     * overhang mid-jump does.
     */
    if ((s16)p->vel[1] >= keep)
        return;

    if ((s16)p->vel[1] == 0) {
        /* The original divides here and would trap. It cannot be reached from
         * a non-floor contact that leaves vel.y at zero with keep > 0, but the
         * port does not fault where the console would not. */
        return;
    }

    {
        s32 vy = (s16)p->vel[1];

        p->vel[0] = (s16)(((s32)(s16)p->vel[0] * keep) / vy);
        p->vel[2] = (s16)(((s32)(s16)p->vel[2] * keep) / vy);
        p->vel[1] = (s16)keep;
    }
}

/*
 * 0x80039CB4 — fall damage.
 *
 * Driven by how much LANDING changed vertical velocity, not by how far the fall
 * was, so a lift that stops under you hurts and a slope that bleeds the speed
 * off does not.
 */
static void fall_damage(q2_sim *sim, s32 vy_before)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 delta, sev, dmg;

    if (!(p->ent.flags & Q2_ENT_ON_GROUND))
        return;

    delta = (s16)p->vel[1] - vy_before;
    delta >>= Q2_FALL_SHIFT;
    sev    = (delta * delta) / Q2_FALL_DIV;

    if (sev < Q2_FALL_MIN)
        return;

    /* 0x80039CF4: no fall damage in water, shallow or deep. */
    if (p->ent.flags & (Q2_ENT_INWATER | Q2_ENT_UNDERWATER))
        return;

    /*
     * The view kick, in degrees converted to the 4096-step circle — 4096/360
     * exactly, and the 455 cap is 40 degrees. That the cap is id's own 40 while
     * the conversion is the PSX's own angle unit is the clearest evidence yet
     * that Hammerhead ported the tables and rewrote the arithmetic.
     */
    p->fall_value = (s16)((sev * 4096) / 360);
    if (p->fall_value > Q2_FALL_KICK_MAX)
        p->fall_value = Q2_FALL_KICK_MAX;
    p->fall_time = sim->level_time + Q2_FALL_KICK_TIME;

    /*
     * 0x80039D50. Below 31 there is no sound at all — only the view kick above.
     * The landing thump and the damage share one threshold, which is why a drop
     * you can feel in the camera is silent until it is a drop that hurts.
     */
    if (sev < Q2_FALL_SOFT)
        return;

    /* 0x80039D5C: the sound is gated on being alive, so a killing fall lands
     * silently and the death sound has the frame to itself. */
    if (sim->combat.inv.health > 0)
        q2_ent_sound_at(&sim->ent_world.events, Q2_SND_LAND, p->pos);

    dmg = (sev - Q2_FALL_DMG_BASE) / 2;
    if (dmg <= 0)
        dmg = 1;

    if (sim->no_fall_damage)
        return;

    q2_sim_hurt_player(sim, &sim->combat.self, (s16)dmg, Q2_MOD_FALLING,
                       sim->player[sim->cur_player].pos);
}

/*
 * 0x8003AA3C — footsteps.
 *
 * Gated on the WISH velocity rather than the actual one, so you get footsteps
 * while walking into a wall and none while sliding to a halt.
 */
static void update_footsteps(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    s32 half = max_speed(p->ent.flags) / 2;
    s32 side = p->wish[0] < 0 ? -p->wish[0] : p->wish[0];
    s32 fwd  = p->wish[2] < 0 ? -p->wish[2] : p->wish[2];

    if (!(p->ent.flags & (Q2_ENT_ON_GROUND | Q2_ENT_WALL_CONTACT)))
        return;
    if (p->ent.flags & Q2_ENT_NO_FOOTSTEP)
        return;
    if (side <= half && fwd <= half)
        return;
    if (sim->level_time <= p->footstep_time)
        return;

    /*
     * 0x8003AAB8. The wet case does NOT advance the alternation — one splash
     * every 320 ticks rather than a left and a right — so a player wading keeps
     * whichever foot they arrived with.
     */
    if (p->ent.flags & Q2_ENT_INWATER) {
        p->footstep_time = sim->level_time + Q2_FOOTSTEP_PERIOD_WET;
        q2_ent_sound_at(&sim->ent_world.events, Q2_SND_FOOTSTEP_WET, p->pos);
    } else {
        p->footstep_time = sim->level_time + Q2_FOOTSTEP_PERIOD;
        p->foot = p->foot ? 0 : 1;
        q2_ent_sound_at(&sim->ent_world.events,
                        p->foot ? Q2_SND_FOOTSTEP_A : Q2_SND_FOOTSTEP_B,
                        p->pos);
    }
}

/*
 * 0x8003AE10 — the pain sound, and the diff that decides there was any pain.
 *
 * The engine keeps no "was hurt" flag. It compares health and armour with last
 * tick's copies, and either falling is enough — so armour absorbing a hit still
 * makes the player grunt, and a hit healed in the same tick makes no sound at
 * all.
 *
 * The throttle is client+0xD0 and it is the SAME field the damage view kick
 * decays against, which is why the two cannot be separated: 210 ticks of
 * silence, 150 ticks of kick, one deadline.
 */
static void update_pain(q2_sim *sim)
{
    q2_player *p = &sim->player[sim->cur_player];
    s16 health = sim->combat.inv.health;
    s16 armour = sim->combat.inv.armour;
    bool hurt;

    hurt = (health < p->prev_health) || (armour < p->prev_armour);

    if (hurt && sim->level_time > p->pain_time) {
        q2_ent_sound which;

        /* 0x8003AF54: four brackets on the health that is LEFT, so the voice
         * gets worse as the player does. */
        if (health < 25)      which = Q2_SND_PAIN_25;
        else if (health < 50) which = Q2_SND_PAIN_50;
        else if (health < 75) which = Q2_SND_PAIN_75;
        else                  which = Q2_SND_PAIN_100;

        p->pain_time = sim->level_time + 210;
        q2_ent_sound_at(&sim->ent_world.events, which, p->pos);
    }

    /* 0x8003AFA8: unconditionally, at the very end of the frame. */
    p->prev_health = health;
    p->prev_armour = armour;
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
    bool run_world;
    bool was_underwater;
    bool moving;
    u32  buttons;
    s32  vy_before;

    if (!sim || !input || dt <= 0)
        return;

    p = &sim->player[sim->cur_player];

    /*
     * The WORLD half of a tick runs once per frame, not once per player.
     *
     * A tick is two things wearing one name: the player's frame — movement,
     * view, weapon, the volumes they are standing in — and the world's, which
     * is the entity sweep, the effects, the glint and the clock. With one
     * player they are indistinguishable. With four they must not be: running
     * the entity sweep four times would age every item respawn four times as
     * fast and step the effects four times a frame.
     *
     * So the world half is gated on being player 0. `q2_sim_advance` sets
     * `cur_player` to 0 and behaves exactly as it always has; the extra players
     * go through `q2_sim_advance_player`, which runs their frame against the
     * same world without advancing it again.
     */
    run_world = (sim->cur_player == 0);

    /*
     * The order below is 0x8003A1C8's, address by address, because several
     * steps only behave correctly in it. In particular the view offset is
     * chosen from the PREVIOUS tick's environment flags, the jump reads the
     * ground the previous move left behind, and the look angles integrate
     * before the wish velocity is rotated by them.
     */

    /*
     * The event queue is drained by the caller between ticks, so it is cleared
     * HERE rather than beside the entity sweep that used to own it. The player's
     * own frame emits into it — footsteps, the landing thump, the pain grunt —
     * and all three happen before the sweep runs.
     */
    if (run_world)
        q2_ent_events_clear(&sim->ent_world.events);

    /*
     * 0x8003A41C — the fly bit, mirrored out of the GAME VARIABLES word before
     * anything reads it. It has to be here rather than wherever the setting is
     * written, because the original re-derives it every frame and a caller that
     * clears `full_basis_movement` mid-game must see the flag go with it.
     */
    if (sim->full_basis_movement)
        p->ent2_flags |=  (u32)Q2_ENT2_FLY;
    else
        p->ent2_flags &= ~(u32)Q2_ENT2_FLY;

    /* 0x8003A208 — view offset, from flags that have not been cleared yet. */
    update_view_offset(sim, dt);

    /* 0x8003A25C — clear the environment flags, then let the volumes re-assert
     * them. `was_underwater` is latched at 0x8003A264, before the clear. */
    was_underwater = (p->ent.flags & Q2_ENT_UNDERWATER) != 0;
    update_env_flags(sim);

    /* The mover's frame: the entity origin, Q2_EYE_BASE above the feet. */
    p->ent.pos[0] = p->pos[0];
    p->ent.pos[1] = q2_sim_origin_y(p->pos[1]);
    p->ent.pos[2] = p->pos[2];

    /* 0x8003A4A4 — the pad. 0x8003A4EC may add a jump the player did not ask
     * for, so the button word is taken by value from here on. */
    buttons = input->buttons;
    water_exit_jump(sim, was_underwater, &buttons);

    /* 0x8003A584 — the wish velocity. */
    moving = ((u16)input->forward | (u16)input->side) != 0;
    update_wish(sim, input, dt);

    /* 0x8003A658 — the look rates. */
    update_look(sim, input, dt);

    /* 0x8003A780 — the look-button chord, and the pitch it walks back to zero.
     * Between the rate and the integration, so a recentring tick contributes no
     * rate of its own. */
    update_recentre(sim, input, dt);

    /*
     * 0x8003A8FC — the jump, and the two gates in front of it: entity+0x10C bit
     * 12 (flying) at 0x8003A8F0, and flags & 0x700, so you cannot jump while
     * crouching, low-crouching or submerged. The whole call is skipped, which
     * means the hold timer does not tick down either — walk into a crouch zone
     * mid-jump and the timer freezes.
     */
    if (!(p->ent2_flags & Q2_ENT2_FLY) &&
        !(p->ent.flags & (Q2_ENT_UNDERWATER | Q2_ENT_INCROUCH |
                          Q2_ENT_INLOWCROUCH)))
        player_jump(sim, (buttons & Q2_BTN_JUMP) != 0, dt);

    /* 0x8003A990 — integrate the angles from the rates. */
    integrate_look(sim, dt);

    /* 0x8003AA3C, 0x8003AB18 — footsteps and the strafe roll. */
    update_footsteps(sim);
    update_roll(sim);

    /* 0x8003AB68 — swimming up. */
    swim_up(sim, input, dt);

    /* 0x8003ABC4 — wish velocity into world velocity, plus the ground stick. */
    wish_to_world(sim, moving, dt);

    /*
     * 0x8003AD80 — the wall-contact flag is cleared immediately before the move,
     * so it describes the PREVIOUS frame's contact everywhere above and is
     * re-established by the contents query below.
     */
    p->ent.flags &= ~(u32)Q2_ENT_WALL_CONTACT;

    /* 0x80039AC4 — the move. Everything from here to fall_damage is 0x80039AA4
     * and the mover it calls. */
    vy_before = (s16)p->vel[1];

    /*
     * 0x800458A0 runs the volume query at the top of the mover, with mask
     * 0x3360, and turns the answer into the entity's own liquid and wall flags.
     * Those flags choose the integrator's arm and halve the step height, so it
     * has to come before both.
     */
    update_contents(sim);

    /*
     * 0x800459E8 — snap the velocity to the surface being stood on. This is the
     * rule that makes standing still stand still, and the one that produces the
     * velocity change fall damage measures.
     */
    ground_project(sim);

    /*
     * 0x80045AC8 — the last contact normal is reset to the flat (0, 4096, 0) at
     * 0x800AE928 before the moves, so a move that hits nothing leaves a normal
     * the velocity clip will ignore rather than last frame's wall.
     */
    p->ent.last_normal[0] = 0;
    p->ent.last_normal[1] = 4096;
    p->ent.last_normal[2] = 0;

    if (sim->coll_ready) {
        /*
         * Lift, slide, drop — 0x80045B24. Ground is decided by the drop alone,
         * which is why walking into a wall does not read as landing on it, and
         * why a step up to Q2_STEP_HEIGHT costs nothing.
         *
         * The delta is LAST tick's, per q2_player.frame_delta.
         */
        q2_move_step(&sim->coll, &p->ent, p->frame_delta,
                     sim->volume_count ? &sim->move_world : NULL);

        p->pos[0] = p->ent.pos[0];
        p->pos[1] = q2_sim_feet_y(p->ent.pos[1]);
        p->pos[2] = p->ent.pos[2];

        p->on_ground      = (p->ent.flags & Q2_ENT_GROUNDED_MASK) != 0;
        sim->current_node = p->ent.node;
    } else {
        /* Without a hull, fall back to the seeded ground plane so the player
         * does not drop forever in a zone that failed to parse. */
        p->pos[0] += p->frame_delta[0];
        p->pos[1] += p->frame_delta[1];
        p->pos[2] += p->frame_delta[2];

        p->on_ground = false;
        if (p->pos[1] >= p->ground_y) {
            p->pos[1]    = p->ground_y;
            p->ent.flags |= Q2_ENT_ON_GROUND;
            p->ent.ground_normal[1] = 4096;
            p->on_ground = true;
        }
    }

    /* 0x80045E74 — and now next tick's delta. */
    integrate_vertical(sim, dt, p->frame_delta);

    /*
     * 0x80039AE4 — clip the velocity into the surface just hit, but only when
     * that surface was too steep to stand on. A landing is left alone, because
     * the ground state and the fall-damage delta both depend on it.
     */
    clip_velocity(sim);

    /*
     * Nothing zeroes the velocity on landing, and that is deliberate: the mover
     * stops the POSITION and leaves the full falling speed in vel.y. The next
     * tick's ground_project is what removes it, which is why fall damage arrives
     * a tick after the impact. Adding a "landing kills velocity" rule here — the
     * obvious thing to write, and what this function used to do — destroys the
     * measurement fall damage depends on and makes every landing painless.
     */

    /* 0x80039CB4 — fall damage, from how much the landing changed vel.y. */
    fall_damage(sim, vy_before);

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

    /*
     * The effects, after the combat that spawns them, so a burst created this
     * tick does not also integrate this tick. The console runs the integrator
     * inside the draw, once per frame rather than once per viewport, which puts
     * it after everything gameplay did — the same place.
     */
    /* Once per frame, not once per player: the comment below already said
     * "once per tick, not once per viewport", and a second player is a second
     * viewport by another name. */
    if (run_world) {
        q2_fx_tick(&sim->fx);

        /* The timed beams age by the frame delta, as 0x80048CE8 does. */
        q2_fx_timed_tick(&sim->fx, dt);

        /* The glint's bands advance once per tick, not once per viewport — see
         * q2_fx_glint_advance. */
        q2_fx_glint_advance(&sim->glint);
    }

    /*
     * The entity sweep, last, so an item's touch test sees where the player
     * actually ended up this tick — the same reason the trigger update runs
     * after movement.
     *
     * The world's clock is not advanced independently: q2_entity_run adds `dt`
     * to it, and it was seeded from `level_time` at attach, so the two stay in
     * step. Mega health's decay is the player's own and is stepped here too,
     * because the engine keeps it in the client rather than on an entity.
     */
    if (sim->entities_ready) {
        /*
         * Every player publishes where they are — the entity world has taken a
         * player INDEX since it was written, and nothing had ever passed
         * anything but 0 — but only player 0's tick runs the sweep.
         */
        q2_entity_world_move_player(&sim->ent_world, sim->cur_player,
                                    sim->player[sim->cur_player].pos);
        if (run_world) {
            sim->ent_world.dt         = dt;
            sim->ent_world.deathmatch = sim->multiplayer;
            sim->ent_world.cheats     = sim->cheats;
            q2_entity_run(&sim->entities, &sim->ent_world);
        }
    }
    q2_item_mega_health_tick(&sim->combat.inv, sim->level_time);

    /* 0x8003AE10, the last thing the player's frame does: notice damage, grunt,
     * and take this tick's copy of the two figures. After the item sweep,
     * because a medkit collected this tick counts as healing. */
    update_pain(sim);

    if (run_world)
        sim->tick_count++;
}

/*
 * One extra player's frame, against the world `q2_sim_advance` has already
 * advanced this frame.
 *
 * `index` must not be 0: player 0 is the one `q2_sim_advance` runs, and running
 * it again here would advance the world twice. Everything a player owns —
 * position, view, the volumes they stand in, their weapon — is theirs; the
 * entity list, the script, the effects and the clock are the world's and are
 * not touched.
 */
/* Park the live player's combat half and load another's. */
static void combat_swap_to(q2_sim *sim, int index)
{
    q2_player_combat *from = &sim->pcombat[sim->cur_player];
    q2_player_combat *to   = &sim->pcombat[index];

    if (index == sim->cur_player)
        return;

    from->inv              = sim->combat.inv;
    from->weapon_id        = sim->combat.weapon_id;
    from->next_fire        = sim->combat.next_fire;
    from->kick[0]          = sim->combat.kick[0];
    from->kick[1]          = sim->combat.kick[1];
    from->kick[2]          = sim->combat.kick[2];
    from->chaingun_bullets = sim->combat.chaingun_bullets;
    from->self             = sim->combat.self;
    from->last_shot        = sim->combat.last_shot;

    sim->combat.inv              = to->inv;
    sim->combat.weapon_id        = to->weapon_id;
    sim->combat.next_fire        = to->next_fire;
    sim->combat.kick[0]          = to->kick[0];
    sim->combat.kick[1]          = to->kick[1];
    sim->combat.kick[2]          = to->kick[2];
    sim->combat.chaingun_bullets = to->chaingun_bullets;
    sim->combat.self             = to->self;
    sim->combat.last_shot        = to->last_shot;

    sim->cur_player = index;
}

void q2_sim_advance_player(q2_sim *sim, int index, const q2_input *input,
                           s32 dt)
{
    int saved;

    if (!sim || !input || dt <= 0)
        return;
    if (index <= 0 || index >= Q2_SIM_MAX_PLAYERS)
        return;

    saved = sim->cur_player;
    combat_swap_to(sim, index);
    q2_sim_tick(sim, input, dt);
    combat_swap_to(sim, saved);
}

/*
 * Give player `index` the inventory and weapon a level start hands out, and
 * park it. Called once per extra player, after `q2_sim_spawn` has placed them.
 */
void q2_sim_player_reset_combat(q2_sim *sim, int index)
{
    int saved;

    if (!sim || index <= 0 || index >= Q2_SIM_MAX_PLAYERS)
        return;

    saved = sim->cur_player;
    combat_swap_to(sim, index);
    q2_inventory_init(&sim->combat.inv);
    sim->combat.weapon_id = 0;
    sim->combat.next_fire = 0;

    /*
     * And the actor, from the inventory, so the pair starts IN STEP. Leaving it
     * zeroed is not harmless: a caller that copies the actor's health back into
     * the inventory — which is what has to happen for a player hit while parked
     * — would write 0 over a full one, and three of four players ended a
     * capture dead without anything having shot them.
     */
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv,
                         sim->player[index].pos);
    combat_swap_to(sim, saved);
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
    sim->dt_accum += dt;

    if (sim->dt_accum < Q2_DT_NOMINAL)
        return 0;

    /*
     * ONE tick per frame, with a VARIABLE dt. Read out of the frame loop at
     * 0x80018440: the step is either `gamespeed * 2` (0x80018468, giving the
     * nominal 12) or the count of fields that actually elapsed (0x80018480),
     * it is clamped to 30 at 0x800184B8, and then 0x800184D8 runs the world
     * exactly once. The engine has no sub-stepping loop anywhere.
     *
     * That is why this does not run several 12-unit ticks to catch up. Every
     * rate in the movement code is `k * dt`, and a clamped approach with rate
     * `k * 24` is NOT two approaches with rate `k * 12` — it overshoots less.
     * Sub-stepping would be the more "correct" integrator and the wrong game.
     */
    dt = sim->dt_accum;
    if (dt > Q2_DT_MAX) {
        /*
         * Past the clamp the surplus is DISCARDED rather than carried, which is
         * how the original slows down instead of catching up: 0x800184C8 writes
         * the clamped value back over the accumulator.
         */
        dt = Q2_DT_MAX;
        sim->dt_accum = 0;
    } else {
        sim->dt_accum -= dt;
    }

    q2_sim_tick(sim, input, dt);
    ticks = 1;

    return ticks;
}

void q2_sim_eye(const q2_sim *sim, s32 out_pos[3])
{
    if (!sim || !out_pos)
        return;

    out_pos[0] = sim->player[sim->cur_player].pos[0];
    /* World Y increases downward, so the eye sits at a smaller Y than the feet. */
    out_pos[1] = sim->player[sim->cur_player].pos[1] - sim->player[sim->cur_player].view_height;
    out_pos[2] = sim->player[sim->cur_player].pos[2];
}

/* ------------------------------------------------------------------------- */
/* 0x80038260 — the view angles                                               */
/* ------------------------------------------------------------------------- */
/*
 * How much of a kick is left, in 1.0.12.
 *
 * `(remaining << 12) / period`, and the shift comes FIRST: the original forms
 * the fixed-point numerator and then divides, so a kick with 7 of 30 ticks left
 * is 955/4096 rather than 0. Dividing first and scaling after — the obvious
 * way — quantises every kick to nothing.
 *
 * A deadline in the past yields nothing at all, and the original memsets the
 * amplitudes to zero when it does (0x80038328). This does not, because the
 * amplitudes are read nowhere else and clearing them would make the composer
 * mutate the player it is handed.
 */
static s32 kick_scale(s32 deadline, s32 now, s32 period)
{
    s32 remaining = deadline - now;

    if (remaining < 0)
        return 0;
    return (remaining << Q2_FRAC_12) / period;
}

void q2_sim_view_angles(const q2_sim *sim, s32 out[3])
{
    const q2_player *p;
    s32 s;

    if (!out)
        return;

    out[0] = out[1] = out[2] = 0;

    if (!sim)
        return;

    p = &sim->player[sim->cur_player];

    out[0] = p->pitch;
    out[1] = p->yaw;
    out[2] = p->roll;

    /* 0x800382A4 — the firing kick, all three axes, over 30 ticks. */
    s = kick_scale(p->kick_time, sim->level_time, Q2_VIEW_KICK_FIRE);
    if (s) {
        out[0] += ((s32)p->kick[0] * s) >> Q2_FRAC_12;
        out[1] += ((s32)p->kick[1] * s) >> Q2_FRAC_12;
        out[2] += ((s32)p->kick[2] * s) >> Q2_FRAC_12;
    }

    /* 0x80038334 — the damage kick, pitch and roll only, over 150. */
    s = kick_scale(p->pain_time, sim->level_time, Q2_VIEW_KICK_HURT);
    if (s) {
        out[0] += ((s32)p->hurt_kick[0] * s) >> Q2_FRAC_12;
        out[2] += ((s32)p->hurt_kick[1] * s) >> Q2_FRAC_12;
    }

    /* 0x800383A4 — the landing kick, pitch alone, over 90. */
    s = kick_scale(p->fall_time, sim->level_time, Q2_VIEW_KICK_FALL);
    if (s)
        out[0] += ((s32)p->fall_value * s) >> Q2_FRAC_12;
}
