#include "combat.h"

#include <string.h>

#include "trig.h"

/*
 * INFERRED weapon stats. Not read from this executable — see combat.h.
 *
 * Ranges are in world units, so a splash radius of 1200 is 120 PC units at the
 * scale established in worldscale.h. Refire is on the 10 Hz AI clock, so 5
 * means twice a second.
 */
const q2_weapon_stats q2_weapon_stats_inferred[Q2_WEAPON_COUNT] = {
    /* mode, damage, pellets, spread, refire, ammo, splash, radius, speed */
    { Q2_FIRE_PROJECTILE, 10,  1,   0, 4, 0,   0,    0, 1000 }, /* Blaster    */
    { Q2_FIRE_HITSCAN,     4, 12, 500, 8, 1,   0,    0,    0 }, /* Shotgun    */
    { Q2_FIRE_HITSCAN,     6, 20, 800, 9, 2,   0,    0,    0 }, /* SuperShot  */
    { Q2_FIRE_HITSCAN,     8,  1, 300, 1, 1,   0,    0,    0 }, /* Machinegun */
    { Q2_FIRE_HITSCAN,     6,  1, 300, 1, 1,   0,    0,    0 }, /* Chaingun   */
    { Q2_FIRE_PROJECTILE,  0,  1,   0, 8, 1, 120, 1600,  600 }, /* HandGren   */
    { Q2_FIRE_PROJECTILE,  0,  1,   0, 6, 1, 120, 1600,  700 }, /* GrenLaunch */
    { Q2_FIRE_PROJECTILE,  0,  1,   0, 8, 1, 120, 1200,  650 }, /* RockLaunch */
    { Q2_FIRE_PROJECTILE, 15,  1,   0, 1, 1,   0,    0, 1000 }, /* Hyperblast */
    { Q2_FIRE_HITSCAN,   100,  1,   0, 15, 1,  0,    0,    0 }, /* Railgun    */
    { Q2_FIRE_PROJECTILE, 200, 1,   0, 20, 50, 200, 1000, 400 }  /* BFG       */
};

void q2_combat_init(q2_combat *c)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->stats = q2_weapon_stats_inferred;
}

void q2_combat_set_stats(q2_combat *c, const q2_weapon_stats *stats)
{
    if (!c)
        return;
    c->stats = stats ? stats : q2_weapon_stats_inferred;
}

/* ------------------------------------------------------------------------- */
s64 q2_combat_ray_dist_sq(const s32 origin[3], const s32 dir[3],
                          const s32 point[3], s64 *out_along)
{
    s64 vx, vy, vz, along;
    s64 cx, cy, cz;

    if (!origin || !dir || !point)
        return 0;

    vx = (s64)point[0] - origin[0];
    vy = (s64)point[1] - origin[1];
    vz = (s64)point[2] - origin[2];

    /* dir is a 1.3.12 unit vector, so the projection carries a 4096 scale that
     * is divided out here rather than left to surprise the caller. */
    along = ((s64)dir[0] * vx + (s64)dir[1] * vy + (s64)dir[2] * vz) >> Q2_FRAC_12;

    if (out_along)
        *out_along = along;

    /* Closest approach: v - dir*along, again unscaling the direction. */
    cx = vx - (((s64)dir[0] * along) >> Q2_FRAC_12);
    cy = vy - (((s64)dir[1] * along) >> Q2_FRAC_12);
    cz = vz - (((s64)dir[2] * along) >> Q2_FRAC_12);

    return cx * cx + cy * cy + cz * cz;
}

/* Forward vector for a yaw/pitch pair, in 1.3.12. */
static void view_direction(s32 yaw, s32 pitch, s32 out[3])
{
    s32 sy = q2_sin12(yaw),   cy = q2_cos12(yaw);
    s32 sp = q2_sin12(pitch), cp = q2_cos12(pitch);

    out[0] = (s32)(((s64)cp * sy) >> Q2_FRAC_12);
    out[1] = -sp;                 /* world Y grows downward */
    out[2] = (s32)(((s64)cp * cy) >> Q2_FRAC_12);
}

/* ------------------------------------------------------------------------- */
q2_fire_result q2_combat_fire(q2_combat *c, q2_inventory *inv,
                              const s32 origin[3], s32 yaw, s32 pitch,
                              s32 now,
                              q2_monster *monsters, u32 monster_count,
                              s32 hit_radius)
{
    q2_fire_result result;
    const q2_weapon_stats *w;
    s32 dir[3];

    memset(&result, 0, sizeof(result));
    result.killed = -1;

    if (!c || !inv || !origin || !c->stats)
        return result;
    if (inv->current_weapon < 0 || inv->current_weapon >= Q2_WEAPON_COUNT)
        return result;

    /* Rate limit before touching ammo, so a blocked shot costs nothing. */
    if (now < c->next_fire)
        return result;

    if (!q2_inventory_can_fire(inv))
        return result;

    w = &c->stats[inv->current_weapon];

    if (w->ammo_per_shot > 0 && !q2_inventory_consume(inv, w->ammo_per_shot))
        return result;

    c->next_fire = now + (w->refire > 0 ? w->refire : 1);
    result.fired = true;

    view_direction(yaw, pitch, dir);

    if (w->mode == Q2_FIRE_PROJECTILE) {
        result.spawn_projectile = true;
        result.projectile_dir[0] = dir[0];
        result.projectile_dir[1] = dir[1];
        result.projectile_dir[2] = dir[2];
        result.projectile_speed  = w->speed;
        return result;
    }

    /* Hitscan. Each pellet is resolved against every live monster; the spread
     * is applied as a widening of the acceptance radius with range rather than
     * by perturbing the ray, which keeps it deterministic and cheap. */
    {
        u32 i;
        s16 per_shot = (s16)(w->damage * (w->pellets > 0 ? w->pellets : 1));

        for (i = 0; i < monster_count; i++) {
            q2_monster *m = &monsters[i];
            s64 along = 0, dist_sq;
            s64 accept;

            if (!m->in_use || m->dead)
                continue;

            dist_sq = q2_combat_ray_dist_sq(origin, dir, m->pos, &along);

            if (along <= 0)
                continue;              /* behind the shooter */

            /* Spread widens the effective radius with distance. */
            accept = (s64)hit_radius + ((s64)w->spread * along) / 100000;
            if (dist_sq > accept * accept)
                continue;

            if (q2_monster_damage(m, per_shot))
                result.killed = (s32)i;

            result.hits++;
            result.damage_dealt = (s16)(result.damage_dealt + per_shot);

            /* A single hitscan bolt stops at the first thing it hits; a
             * shotgun's pellets are modelled as one spread cone that can catch
             * several targets, so only the single-pellet case stops here. */
            if (w->pellets <= 1)
                break;
        }
    }

    return result;
}

/* ------------------------------------------------------------------------- */
u32 q2_combat_splash(const s32 centre[3], s16 damage, s16 radius,
                     q2_monster *monsters, u32 monster_count)
{
    u32 hurt = 0, i;

    if (!centre || !monsters || damage <= 0 || radius <= 0)
        return 0;

    for (i = 0; i < monster_count; i++) {
        q2_monster *m = &monsters[i];
        s64 d2;
        s64 dist;
        s16 applied;

        if (!m->in_use || m->dead)
            continue;

        d2 = q2_monster_dist_sq(m, centre);
        if (d2 > (s64)radius * radius)
            continue;

        /* Linear falloff to zero at the radius. Integer square root avoided by
         * comparing squares above; here the distance is needed, so take it. */
        dist = 0;
        {
            s64 lo = 0, hi = radius;
            while (lo <= hi) {
                s64 mid = (lo + hi) / 2;
                if (mid * mid <= d2) { dist = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
        }

        applied = (s16)((s64)damage * (radius - dist) / radius);
        if (applied <= 0)
            continue;

        q2_monster_damage(m, applied);
        hurt++;
    }

    return hurt;
}
