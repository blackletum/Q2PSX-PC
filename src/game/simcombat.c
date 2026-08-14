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

    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player.pos);
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

    sy = q2_sin12(sim->player.yaw);   cy = q2_cos12(sim->player.yaw);
    sp = q2_sin12(sim->player.pitch); cp = q2_cos12(sim->player.pitch);

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
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player.pos);

    r = q2_weapon_fire(&sim->combat.inv, &sim->combat.rng, NULL,
                       sim->combat.weapon_id, eye,
                       sim->player.yaw, sim->player.pitch, aim,
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

    switch (r.kind) {
    case Q2_FK_BULLET:
        /* Every pellet is its own trace, which is why a shotgun can catch two
         * creatures and a machinegun cannot. */
        for (i = 0; i < r.shot_count; i++) {
            const q2_shot *s = &r.shot[i];
            s32 frac = world_fraction_for(sim, s->origin, s->dir);
            q2_combat_fire_bullet(&sim->combat.self, s->origin, s->dir,
                                  s->damage, frac, Q2_HITSCAN_RADIUS,
                                  sim->combat.targets,
                                  sim->combat.target_count,
                                  &sim->combat.rules, NULL);
        }
        break;

    case Q2_FK_RAIL: {
        const q2_shot *s = &r.shot[0];
        s32 frac = world_fraction_for(sim, s->origin, s->dir);
        q2_combat_fire_rail(&sim->combat.self, s->origin, s->dir, s->damage,
                            frac, Q2_HITSCAN_RADIUS, sim->combat.targets,
                            sim->combat.target_count, &sim->combat.rules);
        break;
    }

    default:
        q2_projectile_launch(&sim->combat.projectiles, &r, -1, sim->level_time);
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
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player.pos);
    sim->combat.rules.level_time = sim->level_time;

    out = q2_combat_damage(attacker, &sim->combat.self, damage, mod, point,
                           &sim->combat.rules);

    q2_actor_to_player(&sim->combat.self, &sim->combat.inv);
    return out;
}

/* ------------------------------------------------------------------------- */
void q2_sim_combat_tick(q2_sim *sim)
{
    u32 i;

    if (!sim)
        return;

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        q2_projectile *p = &sim->combat.projectiles.p[i];
        q2_proj_step step;
        s32 hit_index;
        s32 dir[3];
        int k;

        if (!p->in_use)
            continue;

        q2_projectile_step(&sim->combat.projectiles, i, sim->gravity,
                           sim->level_time, &step);

        if (step.expired) {
            q2_projectile_detonate(&sim->combat.projectiles, i,
                                   &sim->combat.self, sim->combat.targets,
                                   sim->combat.target_count,
                                   &sim->combat.rules);
            continue;
        }

        /* A creature in the way takes it before the world does. */
        for (k = 0; k < 3; k++)
            dir[k] = step.to[k] - step.from[k];

        hit_index = q2_combat_nearest_on_segment(step.from, dir,
                                                 Q2_HITSCAN_RADIUS,
                                                 sim->combat.targets,
                                                 sim->combat.target_count);
        if (hit_index >= 0) {
            q2_projectile_impact(&sim->combat.projectiles, i, step.to, NULL,
                                 &sim->combat.self,
                                 sim->combat.targets[hit_index],
                                 sim->combat.targets,
                                 sim->combat.target_count,
                                 &sim->combat.rules);
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
                q2_projectile_impact(&sim->combat.projectiles, i, end, NULL,
                                     &sim->combat.self, NULL,
                                     sim->combat.targets,
                                     sim->combat.target_count,
                                     &sim->combat.rules);
                continue;
            }
        }

        q2_projectile_commit(&sim->combat.projectiles, i, step.to);
    }
}
