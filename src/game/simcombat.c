/*
 * simcombat.c — where the reconstructed combat meets the reconstructed world.
 *
 * Everything here is glue, and deliberately so: weapon.c holds what the fire
 * functions do, combat.c holds what the damage function does, projectile.c
 * holds what a rocket is, and this file is the only place that knows all three
 * plus the collision hull. Keeping it separate from sim.c also keeps the
 * movement code readable, which is the harder of the two to follow.
 *
 * The one decision made here rather than read: a shot is traced against the
 * SecondaryCol hull the player moves in, because that is the only hull the port
 * can trace a segment through today. The console traces bullets against
 * PrimaryColl (0x80053974 takes the primary context) and only movement against
 * the secondary. The difference is the player's own 286-unit erosion, so a
 * bullet fired flat along a wall stops 286 units early. It is called out here
 * rather than hidden because it is a real divergence, not a rounding one.
 */
#include <string.h>

#include "sim.h"
#include "trig.h"

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */
void q2_sim_attach_effects(q2_sim *sim, const q2_fx_tables *tab, u32 seed)
{
    if (!sim)
        return;

    q2_fx_world_init(&sim->fx, tab);
    q2_rng_seed(&sim->fx_rng, seed);
    sim->fx_ready = (tab != NULL && tab->loaded);
}

bool q2_sim_attach_glint(q2_sim *sim, const q2_common_file *common)
{
    const dat_chunk *chunk;

    if (!sim)
        return false;

    memset(&sim->glint, 0, sizeof(sim->glint));

    if (!common)
        return false;

    chunk = common->chunk[Q2_COMMON_GLINT_MOD];
    if (!chunk || !chunk->data)
        return false;

    sim->glint.ready = q2_fx_glint_mesh_decode(&sim->glint.mesh,
                                               chunk->data, chunk->size);
    if (!sim->glint.ready)
        return false;

    /*
     * ASK THE LEVEL SCRIPT whether it turns a glint on, and take its numbers.
     *
     * The flag, the band count and the phase are all written by `LevelBin`, and
     * this port does not execute one — but it can read it, which is enough. A
     * map whose script raises no glint gets none; a map whose script uses
     * different numbers gets those rather than BIGGUN's.
     */
    {
        const dat_chunk *lb = common->chunk[Q2_COMMON_LEVEL_BIN];
        q2_fx_glint_script script;
        u32 i;

        if (!lb || !lb->data ||
            !q2_fx_glint_scan(&script, lb->data, lb->size)) {
            /* The mesh is loaded and drawable, but nothing turns it on. */
            return true;
        }

        sim->glint.raised     = true;
        sim->glint.band_count = script.band_count ? script.band_count
                                                  : Q2_FX_GLINT_BANDS;
        sim->glint.phase      = script.phase ? script.phase
                                             : Q2_FX_GLINT_PHASE_START;

        /*
         * The band records themselves are the one thing not readable: the
         * script writes them through its import table (effect.h), into memory
         * rather than into any chunk. The port lays them out evenly and says
         * so — it is the only invented quantity left in the effect system, and
         * it moves where the highlights sit, not whether or how they sweep.
         */
        sim->glint.tint[0] = 255;
        sim->glint.tint[1] = 220;
        sim->glint.tint[2] = 160;

        for (i = 0; i < sim->glint.band_count &&
                    i < Q2_FX_GLINT_BANDS_MAX; i++) {
            sim->glint.band[i].angle[1] =
                (s16)((s32)i * Q2_ONE_12 / (s32)sim->glint.band_count);
            sim->glint.band[i].phase  = (u8)(sim->glint.phase - (i & 3u));
            sim->glint.band[i].colour = 0x3AA0DCFFu;
        }
    }

    return true;
}

/* A spawn that costs nothing when no tables are attached. */
static void fx_at(q2_sim *sim, q2_fx_preset_id id, const s32 at[3])
{
    if (!sim->fx_ready || !at)
        return;
    q2_fx_spawn(&sim->fx, &sim->fx_rng, id, at, 0);
}

/*
 * Where a hitscan shot leaves its mark.
 *
 * The port DOES have a contact point: `world_fraction_for` already runs the
 * pellet through the hull and hands back the 1.0.12 fraction at which the world
 * stopped it, and the direction carries the range, so `origin + dir * frac` is
 * the impact. The original's own hitscan (0x8004874C) does the same thing — it
 * forms the trace end as origin + dir, takes the clipped point back, and either
 * sprays blood there (0x80048980 -> 0x80048B64) or throws a spark (0x800486EC).
 *
 * A pellet that hit nothing and was stopped by nothing marks nothing: a frac of
 * 4096 with no victim means the shot ran out of range in open air.
 */
static void fx_hitscan_impact(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3], s32 frac, s32 victim,
                              const q2_damage_result *dr)
{
    s32 at[3];
    int k;

    if (!sim->fx_ready)
        return;

    for (k = 0; k < 3; k++)
        at[k] = origin[k] + (s32)(((s64)dir[k] * frac) >> Q2_FRAC_12);

    if (victim >= 0) {
        /* Flesh only: armour taking the whole hit is the case the HUD's damage
         * flash also distinguishes. */
        if (!dr || dr->taken > 0)
            fx_at(sim, Q2_FX_BLOOD, at);
        if (dr && dr->killed)
            q2_fx_gib(&sim->fx, &sim->fx_rng, at, 0, Q2_FX_BLOOD_RED);
        return;
    }

    if (frac < 4096)
        fx_at(sim, Q2_FX_SPARK, at);
}

/*
 * Which burst a projectile leaves behind.
 *
 * A bolt is the odd one out: 0x8004D74C reaches the small blue spark rather
 * than the fireball, which is why blaster hits read as a flash and everything
 * else reads as an explosion. The BFG has a burst of its own, four times the
 * size of any other (0x8004BDBC).
 */
static q2_fx_preset_id fx_for_projectile(q2_proj_kind kind)
{
    switch (kind) {
    case Q2_PROJ_BOLT: return Q2_FX_SPARK;
    case Q2_PROJ_BFG:  return Q2_FX_BFG_BURST;
    default:           return Q2_FX_EXPLOSION;
    }
}

/* ------------------------------------------------------------------------- */
void q2_sim_combat_init(q2_sim *sim)
{
    if (!sim)
        return;

    memset(&sim->combat, 0, sizeof(sim->combat));

    q2_inventory_init(&sim->combat.inv);
    q2_combat_rules_default(&sim->combat.rules);
    q2_projectiles_init(&sim->combat.projectiles);
    q2_rng_seed(&sim->combat.rng, 0x51ED2701u);

    /* A fresh player has the blaster and nothing else, which is what the
     * spawn path at 0x8003D4FC leaves in the weapon fields. */
    sim->combat.weapon_id        = Q2_WID_BLASTER;
    sim->combat.inv.weapons      = q2_weapon_tables_builtin()->owned_bit[Q2_WID_BLASTER];
    sim->combat.chaingun_bullets = 1;

    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);
}

/* ------------------------------------------------------------------------- */
void q2_sim_aim(const q2_sim *sim, s16 out[3])
{
    s32 sy, cy, sp, cp;

    if (!out)
        return;
    if (!sim) {
        out[0] = out[1] = out[2] = 0;
        return;
    }

    sy = q2_sin12(sim->player[sim->cur_player].yaw);   cy = q2_cos12(sim->player[sim->cur_player].yaw);
    sp = q2_sin12(sim->player[sim->cur_player].pitch); cp = q2_cos12(sim->player[sim->cur_player].pitch);

    /* The engine keeps this triple at player+0x3C..0x40 as a 1.3.12 unit
     * vector; every fire function then scales it for itself — >> 6 for a
     * blaster bolt, << 2 for a bullet — so the port has to hand over the same
     * magnitude or every weapon's range and spread come out wrong together. */
    out[0] = (s16)(((s64)cp * sy) >> 12);
    out[1] = (s16)(-sp);          /* world Y grows downward */
    out[2] = (s16)(((s64)cp * cy) >> 12);
}

void q2_sim_set_targets(q2_sim *sim, q2_actor **targets, u32 count)
{
    if (!sim)
        return;
    sim->combat.targets      = targets;
    sim->combat.target_count = targets ? count : 0;
}

bool q2_sim_give_weapon(q2_sim *sim, int weapon_id)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();

    if (!sim || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return false;
    if (sim->combat.inv.weapons & t->owned_bit[weapon_id])
        return false;

    sim->combat.inv.weapons |= t->owned_bit[weapon_id];

    /*
     * 0x80037E78: the pickup switches to the new weapon only when the blaster
     * is the one in hand. Picking up a shotgun while holding a railgun does not
     * take the railgun away.
     */
    if (sim->combat.weapon_id == Q2_WID_BLASTER)
        sim->combat.weapon_id = weapon_id;

    return true;
}

bool q2_sim_cycle_weapon(q2_sim *sim, int dir)
{
    int next;

    if (!sim)
        return false;

    next = q2_weapon_cycle(&sim->combat.inv, sim->combat.weapon_id, dir);
    if (next == Q2_WID_NONE)
        return false;

    sim->combat.weapon_id = next;
    return true;
}

/* ------------------------------------------------------------------------- */
/*
 * How far along a shot's direction the world lets it travel, as a 1.0.12
 * fraction. The direction carries the range — 0x80048790 forms the trace end as
 * origin + dir — so the fraction the hull returns is exactly what the entity
 * pass needs to bound itself with.
 */
static s32 world_fraction_for(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3])
{
    q2_trace tr;
    s32 end[3];
    int k;

    if (!sim->coll_ready)
        return 4096;

    for (k = 0; k < 3; k++)
        end[k] = origin[k] + dir[k];

    q2_sim_trace(sim, origin, end, &tr);
    return tr.hit ? tr.fraction : 4096;
}

q2_fire_result_v2 q2_sim_fire(q2_sim *sim)
{
    q2_fire_result_v2 r;
    s32 eye[3];
    s16 aim[3];
    u32 i;

    memset(&r, 0, sizeof(r));
    r.sound = -1;
    if (!sim)
        return r;

    q2_sim_eye(sim, eye);
    q2_sim_aim(sim, aim);

    sim->combat.rules.level_time = sim->level_time;
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);

    r = q2_weapon_fire(&sim->combat.inv, &sim->combat.rng, NULL,
                       sim->combat.weapon_id, eye,
                       sim->player[sim->cur_player].yaw, sim->player[sim->cur_player].pitch, aim,
                       sim->level_time, sim->combat.next_fire,
                       false, sim->combat.rules.deathmatch,
                       sim->combat.chaingun_bullets);

    sim->combat.last_shot = r;
    if (!r.fired)
        return r;

    sim->combat.next_fire = r.next_fire;
    sim->combat.kick[0] = r.kick[0];
    sim->combat.kick[1] = r.kick[1];
    sim->combat.kick[2] = r.kick[2];

    /*
     * And on to the view, which is where a kick was always going: the weapon
     * table's figure has had a home in `combat.kick` for a while and nothing
     * read it. `q2_sim_view_angles` composes it over 30 ticks (FORMATS.md
     * §9.12.11a), so the deadline is what turns one number into recoil.
     */
    sim->player[sim->cur_player].kick[0]  = r.kick[0];
    sim->player[sim->cur_player].kick[1]  = r.kick[1];
    sim->player[sim->cur_player].kick[2]  = r.kick[2];
    sim->player[sim->cur_player].kick_time = sim->level_time + Q2_VIEW_KICK_FIRE;

    switch (r.kind) {
    case Q2_FK_BULLET:
        /* Every pellet is its own trace, which is why a shotgun can catch two
         * creatures and a machinegun cannot. */
        for (i = 0; i < r.shot_count; i++) {
            const q2_shot *s = &r.shot[i];
            s32 frac = world_fraction_for(sim, s->origin, s->dir);
            q2_damage_result dr;
            s32 victim;

            victim = q2_combat_fire_bullet(&sim->combat.self, s->origin,
                                           s->dir, s->damage, frac,
                                           Q2_HITSCAN_RADIUS,
                                           sim->combat.targets,
                                           sim->combat.target_count,
                                           &sim->combat.rules, &dr);
            fx_hitscan_impact(sim, s->origin, s->dir, frac, victim, &dr);
        }
        break;

    case Q2_FK_RAIL: {
        const q2_shot *s = &r.shot[0];
        s32 frac = world_fraction_for(sim, s->origin, s->dir);
        u32 hits = q2_combat_fire_rail(&sim->combat.self, s->origin, s->dir,
                                       s->damage, frac, Q2_HITSCAN_RADIUS,
                                       sim->combat.targets,
                                       sim->combat.target_count,
                                       &sim->combat.rules);

        /* The rail does not stop at the first target, so it always marks the
         * world where the beam ends and blood is left to the per-target pass
         * this port does not get back from `fire_rail`. */
        fx_hitscan_impact(sim, s->origin, s->dir, frac, -1, NULL);
        (void)hits;
        break;
    }

    default:
        /* WHO fired it. The -1 here meant "the world", so a bolt could not say
         * who to credit and a kill by one had no killer. */
        q2_sim_proj_scan.launched++;
        q2_projectile_launch(&sim->combat.projectiles, &r,
                             sim->cur_player, sim->level_time);
        break;
    }

    return r;
}

q2_damage_result q2_sim_hurt_player(q2_sim *sim, q2_actor *attacker,
                                    s16 damage, s16 mod, const s32 point[3])
{
    q2_damage_result out;

    memset(&out, 0, sizeof(out));
    if (!sim)
        return out;

    /* Health and armour live in the inventory, everything else in the actor, so
     * the two are synchronised around the call rather than duplicated. */
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);
    sim->combat.rules.level_time = sim->level_time;

    out = q2_combat_damage(attacker, &sim->combat.self, damage, mod, point,
                           &sim->combat.rules);

    q2_actor_to_player(&sim->combat.self, &sim->combat.inv);

    /* Blood only when flesh actually took some of it: armour absorbing the
     * whole hit is the case the HUD's damage flash also distinguishes. */
    if (out.taken > 0)
        fx_at(sim, Q2_FX_BLOOD, point);

    /*
     * The flinch. Amplitude scaled by how much got through, capped at the same
     * 40 degrees the fall kick is capped at, and pitched UP and rolled with the
     * sign of the knockback so a hit from the left throws the view right.
     *
     * The amplitude is the port's: 0x80038334 reads client+0x9C and +0x9E and
     * this is the only path that could write them, but the write itself sits
     * behind the damage callback rather than in T_Damage, so what the console
     * puts there is not established. The DECAY is the console's — 150 ticks
     * against the pain deadline — and that is the part that is felt.
     */
    if (out.taken > 0) {
        s32 amp = out.taken * 8;
        s32 side = 0;

        if (amp > Q2_FALL_KICK_MAX)
            amp = Q2_FALL_KICK_MAX;

        /* Which side it came from: the hit point against the view's own right
         * vector, so a shot from the left rolls the view right. */
        if (point) {
            s32 sy = q2_sin12(sim->player[sim->cur_player].yaw), cy = q2_cos12(sim->player[sim->cur_player].yaw);
            s32 dx = point[0] - sim->player[sim->cur_player].pos[0];
            s32 dz = point[2] - sim->player[sim->cur_player].pos[2];

            side = (cy * dx - sy * dz) >> Q2_FRAC_12;
        }

        sim->player[sim->cur_player].hurt_kick[0] = (s16)(-amp);
        sim->player[sim->cur_player].hurt_kick[1] = (s16)((side >= 0) ? -amp / 2 : amp / 2);
    }

    return out;
}

/*
 * The actor that fired a projectile, from the owner index the launch recorded.
 *
 * The step runs on player 0's tick — projectiles are the world's — so
 * `combat.self` at step time is player 0 whoever fired. Passing that as the
 * attacker credited every bolt in the air to player 0 and gave a
 * player-versus-player kill the wrong killer, or the shooter their own bolt.
 */
/* Where a projectile got to, per the note on q2_combat_scan: "it missed" has
 * several causes and a total cannot tell them apart. */
q2_sim_proj_stats q2_sim_proj_scan;

void q2_sim_set_world_targets(q2_sim *sim, q2_actor **targets, u32 count)
{
    if (!sim)
        return;
    sim->world_targets      = targets;
    sim->world_target_count = count;
}

static q2_actor *attacker_for(q2_sim *sim, s32 owner)
{
    if (owner < 0 || owner >= Q2_SIM_MAX_PLAYERS)
        return &sim->combat.self;
    if (owner == sim->cur_player)
        return &sim->combat.self;
    return &sim->pcombat[owner].self;
}

/* ------------------------------------------------------------------------- */
void q2_sim_combat_tick(q2_sim *sim)
{
    u32 i;

    if (!sim)
        return;

    /*
     * The projectiles in flight are the WORLD's, not a player's — one list,
     * shared, exactly like the entity sweep and the effects. So they step once
     * a frame, not once a player: with four players a bolt was advancing four
     * times per frame and a rocket crossed an arena at four times its speed.
     *
     * This is the same class of bug the world-half gate in `q2_sim_tick`
     * exists for, and it was missed because it lives in another file.
     */
    if (sim->cur_player != 0)
        return;

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        q2_projectile *p = &sim->combat.projectiles.p[i];
        q2_proj_step step;
        q2_actor **hit_list;
        u32 hit_count;
        s32 hit_index;
        s32 dir[3];
        int k;

        if (!p->in_use)
            continue;

        q2_sim_proj_scan.stepped++;
        q2_projectile_step(&sim->combat.projectiles, i, sim->gravity,
                           sim->level_time, &step);

        if (step.expired) {
            q2_sim_proj_scan.expired++;
            /* The kind and the position have to be taken before the detonate,
             * because it frees the slot. */
            q2_fx_preset_id fx = fx_for_projectile(p->kind);
            s32 where[3];

            memcpy(where, p->pos, sizeof(where));
            q2_projectile_detonate(&sim->combat.projectiles, i,
                                   attacker_for(sim, p->owner),
                                   sim->combat.targets,
                                   sim->combat.target_count,
                                   &sim->combat.rules);
            fx_at(sim, fx, where);
            continue;
        }

        /*
         * The BFG's beams — the game's weapon trail.
         *
         * 0x8004BD04 calls the beam maintainer every tick while the ball flies,
         * and it holds a green beam on every target it can see, refreshing each
         * one rather than adding a second (effect.h). The beams outlive the
         * ball's passage by their own timer, which is what makes the BFG leave
         * a lattice behind it rather than a single line.
         *
         * The port's visibility test is the same segment sweep the projectile
         * itself uses, because it has no separate line-of-sight query; the
         * original calls 0x80051874. Called out as the one substitution.
         */
        if (p->kind == Q2_PROJ_BFG && sim->fx_ready) {
            u32 t;

            for (t = 0; t < sim->combat.target_count; t++) {
                const q2_actor *a = sim->combat.targets[t];
                if (!a || a->health <= 0)
                    continue;

                q2_fx_beam_timed(&sim->fx, (s32)i, (s32)t,
                                 p->pos, a->origin,
                                 Q2_FX_TIMED_BEAM_RADIUS,
                                 Q2_FX_TIMED_BEAM_STYLE,
                                 Q2_FX_TIMED_BEAM_LIFE);
            }
        }

        /* A creature in the way takes it before the world does. */
        for (k = 0; k < 3; k++)
            dir[k] = step.to[k] - step.from[k];

        hit_list  = sim->world_targets ? sim->world_targets
                                       : sim->combat.targets;
        hit_count = sim->world_targets ? sim->world_target_count
                                       : sim->combat.target_count;

        hit_index = q2_combat_nearest_on_segment(step.from, dir,
                                                 Q2_HITSCAN_RADIUS,
                                                 hit_list, hit_count);

        /* Never its own shooter: the world list holds everybody, including the
         * player who fired this. */
        if (hit_index >= 0 && hit_list[hit_index] == attacker_for(sim, p->owner))
            hit_index = -1;

        /*
         * How close it came, whether or not it counted. Measured against the
         * segment's LINE, so "near but past the end" separates a bolt that is
         * badly aimed from one that is aimed correctly and simply has not
         * arrived yet — which a short per-tick step makes the common case.
         */
        {
            u32 k;
            s64 dl = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] +
                     (s64)dir[2] * dir[2];

            /* The square is enough to compare; the length is only printed. */
            q2_sim_proj_scan.seg_len = (s32)dl;

            for (k = 0; k < hit_count; k++) {
                q2_actor *t = hit_list[k];
                s64 along = 0, d2;
                s64 reach;

                if (!t || t == attacker_for(sim, p->owner) || t->health <= 0)
                    continue;

                d2 = q2_combat_ray_dist_sq(step.from, dir, t->origin, &along);
                if (q2_sim_proj_scan.closest_sq == 0 ||
                    d2 < q2_sim_proj_scan.closest_sq)
                    q2_sim_proj_scan.closest_sq = d2;

                reach = (s64)Q2_HITSCAN_RADIUS + t->radius;
                if (d2 <= reach * reach) {
                    q2_sim_proj_scan.near_miss++;
                    if (along > 4096)
                        q2_sim_proj_scan.past_end++;
                }
            }
        }

        if (hit_index >= 0) {
            q2_actor *victim = hit_list[hit_index];

            q2_sim_proj_scan.hit++;
            q2_fx_preset_id fx = fx_for_projectile(p->kind);
            bool was_alive = victim && victim->health > 0;

            q2_projectile_impact(&sim->combat.projectiles, i, step.to, NULL,
                                 attacker_for(sim, p->owner), victim,
                                 hit_list, hit_count,
                                 &sim->combat.rules);

            /*
             * Three bursts can come out of one impact and they are separate
             * effects in the original too: the projectile's own, the victim's
             * blood, and the gib puff if that was the killing blow. The gib
             * takes the creature's blood colour, which is a class property
             * rather than an effect parameter (effect.h).
             */
            fx_at(sim, fx, step.to);
            if (victim) {
                fx_at(sim, Q2_FX_BLOOD, step.to);
                if (was_alive && victim->health <= 0 && sim->fx_ready) {
                    q2_fx_gib(&sim->fx, &sim->fx_rng, step.to, 0,
                              Q2_FX_BLOOD_RED);
                }
            }
            continue;
        }

        if (sim->coll_ready) {
            s32 end[3], node = p->node;
            bool complete;

            /*
             * Traced from the PROJECTILE's own cell, not the player's. A rocket
             * that has crossed the map is nowhere near the shooter, and asking
             * the hull to clip a segment starting in the shooter's cell answers
             * a different question — one whose answer is usually "no obstacle",
             * which is how a projectile ends up flying through walls.
             */
            complete = q2_coll_move(&sim->coll, step.from, step.to, node,
                                    end, &node);
            p->node = node;

            if (!complete) {
                /* The hull gives back the clipped point; the port has no
                 * surface normal here, so a grenade landing on geometry stops
                 * rather than bouncing. Called out because the bounce sound is
                 * one of the twenty-two and the behaviour certainly exists. */
                bool consumed;
                q2_fx_preset_id fx = fx_for_projectile(p->kind);

                consumed = q2_projectile_impact(&sim->combat.projectiles, i,
                                                end, NULL, &sim->combat.self,
                                                NULL, sim->combat.targets,
                                                sim->combat.target_count,
                                                &sim->combat.rules);
                /* A grenade that only bounced has not gone off, so it must not
                 * leave a fireball behind. */
                if (consumed)
                    fx_at(sim, fx, end);
                continue;
            }
        }

        q2_projectile_commit(&sim->combat.projectiles, i, step.to);
    }

    /*
     * Debris, through PRIMARY collision.
     *
     * 0x80046DA0 installs 0x800C8E90 as the hull and the entity's +0xA0 as its
     * cell, where the player's path (0x80046DDC) installs SecondaryCol and
     * +0xA2. Tracing shards against the player's eroded hull would leave every
     * one of them floating 286 units off the floor.
     */
    for (i = 0; i < Q2_FX_DEBRIS_MAX; i++) {
        q2_fx_debris *d = &sim->fx.debris[i];
        q2_fx_debris_step step;

        if (!d->in_use)
            continue;

        q2_fx_debris_step_one(&sim->fx, i, sim->gravity, &step);

        if (step.expired) {
            d->in_use = false;
            continue;
        }

        if (sim->coll_primary_ready) {
            s32 end[3], node = d->node;

            if (!q2_coll_move(&sim->coll_primary, step.from, step.to, node,
                              end, &node)) {
                d->node = node;
                q2_fx_debris_impact(&sim->fx, i, end);
                continue;
            }
            d->node = node;
        }

        q2_fx_debris_commit(&sim->fx, i, step.to);
    }
}

/* ------------------------------------------------------------------------- */
u32 q2_sim_debris_burst(q2_sim *sim, const s32 bmin[3], const s32 bmax[3],
                        const s32 *at, u32 count, u8 area)
{
    if (!sim || !sim->fx_ready)
        return 0;
    return q2_fx_debris_burst(&sim->fx, &sim->fx_rng, bmin, bmax, at, count,
                              area);
}
