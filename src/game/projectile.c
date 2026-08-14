#include "projectile.h"

#include <string.h>

void q2_projectiles_init(q2_projectiles *list)
{
    if (!list)
        return;
    memset(list, 0, sizeof(*list));
}

static q2_projectile *alloc_slot(q2_projectiles *list, s32 *out_index)
{
    u32 i;

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        if (!list->p[i].in_use) {
            memset(&list->p[i], 0, sizeof(list->p[i]));
            list->p[i].in_use = true;
            list->live++;
            if (out_index)
                *out_index = (s32)i;
            return &list->p[i];
        }
    }
    if (out_index)
        *out_index = -1;
    return NULL;
}

static void release(q2_projectiles *list, u32 index)
{
    if (index >= Q2_PROJ_MAX || !list->p[index].in_use)
        return;
    list->p[index].in_use = false;
    if (list->live)
        list->live--;
}

/* ------------------------------------------------------------------------- */
s32 q2_projectile_launch(q2_projectiles *list, const q2_fire_result_v2 *fire,
                         s32 owner, s32 now)
{
    q2_projectile *p;
    s32 index = -1;
    const q2_shot *s;
    s64 len2;
    s32 len, speed;
    int k;

    if (!list || !fire || !fire->fired || fire->shot_count == 0)
        return -1;

    switch (fire->kind) {
    case Q2_FK_BOLT:
    case Q2_FK_GRENADE:
    case Q2_FK_HAND_GRENADE:
    case Q2_FK_ROCKET:
    case Q2_FK_BFG:
        break;
    default:
        return -1;      /* hitscan and rail resolve without an entity */
    }

    p = alloc_slot(list, &index);
    if (!p)
        return -1;

    s = &fire->shot[0];
    memcpy(p->pos, s->origin, sizeof(p->pos));
    p->damage = s->damage;
    p->mod    = fire->mod;
    p->owner  = owner;
    p->node   = -1;      /* found on the first move, as a fresh entity's is */

    switch (fire->kind) {
    case Q2_FK_BOLT:
        /*
         * The bolt is the one projectile whose direction IS its velocity: the
         * spawner stores the argument straight into the entity's +0x52 and the
         * sweep at 0x80047D40 multiplies it by the frame's dt. So there is
         * nothing to normalise, and the hyperblaster's bolt really is half the
         * speed of the blaster's because its shift is one bit deeper.
         */
        p->kind = Q2_PROJ_BOLT;
        p->splash_radius = 0;
        p->expires = now + Q2_LIFETIME_BOLT;
        for (k = 0; k < 3; k++)
            p->vel[k] = s->dir[k] * 4096;      /* 1.0.12, as the mover wants */
        return index;
    case Q2_FK_GRENADE:
        p->kind = Q2_PROJ_GRENADE;
        p->splash_radius = Q2_SPLASH_RADIUS_GRENADE;
        p->expires = now + Q2_FUSE_GRENADE;
        speed = fire->projectile_speed ? fire->projectile_speed
                                       : Q2_GRENADE_LAUNCH_SPEED;
        break;
    case Q2_FK_HAND_GRENADE:
        p->kind = Q2_PROJ_HAND_GRENADE;
        p->splash_radius = Q2_SPLASH_RADIUS_GRENADE;
        p->expires = now + (fire->projectile_timer ? fire->projectile_timer
                                                   : Q2_FUSE_HAND_GRENADE);
        speed = fire->projectile_speed ? fire->projectile_speed
                                       : Q2_GRENADE_LAUNCH_SPEED;
        break;
    case Q2_FK_ROCKET:
        p->kind = Q2_PROJ_ROCKET;
        p->splash_radius = Q2_SPLASH_RADIUS_ROCKET;
        speed = fire->projectile_speed ? fire->projectile_speed
                                       : Q2_ROCKET_SPEED;
        break;
    default:
        p->kind = Q2_PROJ_BFG;
        p->splash_radius = Q2_SPLASH_RADIUS_BFG;
        speed = fire->projectile_speed ? fire->projectile_speed
                                       : Q2_BFG_SPEED_UNREAD;
        break;
    }

    /* The shot's direction carries the weapon's own scale, so normalise it and
     * re-apply the projectile's speed. Speeds are per second on the 300 Hz
     * level clock, so a tick's worth is speed/300 — kept as a 1.0.12 fraction
     * to avoid losing the slow ones to integer truncation. */
    len2 = (s64)s->dir[0] * s->dir[0] + (s64)s->dir[1] * s->dir[1] +
           (s64)s->dir[2] * s->dir[2];
    len = 0;
    if (len2 > 0) {
        s64 lo = 0, hi = 0x7FFFFFFF;
        while (lo <= hi) {
            s64 mid = lo + (hi - lo) / 2;
            if (mid * mid <= len2) { len = (s32)mid; lo = mid + 1; }
            else hi = mid - 1;
        }
    }
    if (len <= 0) {
        release(list, (u32)index);
        return -1;
    }

    for (k = 0; k < 3; k++)
        p->vel[k] = (s32)(((s64)s->dir[k] * speed * 4096) /
                          ((s64)len * Q2_TICKS_PER_SECOND));

    return index;
}

/* ------------------------------------------------------------------------- */
void q2_projectile_step(q2_projectiles *list, u32 index, s32 gravity, s32 now,
                        q2_proj_step *out)
{
    q2_projectile *p;
    int k;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!list || index >= Q2_PROJ_MAX || !out)
        return;

    p = &list->p[index];
    if (!p->in_use)
        return;

    /* Only the grenades fall; the bolt, the rocket and the BFG blast fly
     * straight, which is why nothing but the grenades has a launch arc. */
    if (p->kind == Q2_PROJ_GRENADE || p->kind == Q2_PROJ_HAND_GRENADE)
        p->vel[1] += gravity;

    memcpy(out->from, p->pos, sizeof(out->from));
    for (k = 0; k < 3; k++)
        out->to[k] = p->pos[k] + (p->vel[k] >> 12);

    if (p->expires && now >= p->expires)
        out->expired = true;
}

void q2_projectile_commit(q2_projectiles *list, u32 index, const s32 to[3])
{
    if (!list || index >= Q2_PROJ_MAX || !to)
        return;
    if (!list->p[index].in_use)
        return;
    memcpy(list->p[index].pos, to, sizeof(list->p[index].pos));
}

/* ------------------------------------------------------------------------- */
u32 q2_projectile_detonate(q2_projectiles *list, u32 index,
                           q2_actor *attacker, q2_actor **targets, u32 count,
                           const q2_combat_rules *rules)
{
    q2_projectile *p;
    u32 hurt = 0;

    if (!list || index >= Q2_PROJ_MAX)
        return 0;
    p = &list->p[index];
    if (!p->in_use)
        return 0;

    if (p->splash_radius > 0 && p->damage > 0)
        hurt = q2_combat_radius_damage(attacker, NULL, p->pos, p->damage,
                                       p->splash_radius, p->mod,
                                       targets, count, rules);

    release(list, index);
    return hurt;
}

bool q2_projectile_impact(q2_projectiles *list, u32 index,
                          const s32 point[3], const s32 normal[3],
                          q2_actor *attacker, q2_actor *hit,
                          q2_actor **targets, u32 target_count,
                          const q2_combat_rules *rules)
{
    q2_projectile *p;

    if (!list || index >= Q2_PROJ_MAX || !point)
        return false;
    p = &list->p[index];
    if (!p->in_use)
        return false;

    memcpy(p->pos, point, sizeof(p->pos));

    /*
     * A grenade that meets the world bounces; a grenade that meets a target
     * goes off in its face. 0x8004A2B0's spawner installs a bounce handler and
     * the bounce sound (wep_grenlb1b) is one of the twenty-two, so the bounce
     * is certainly there — the coefficient below is not, and is modelled.
     */
    if ((p->kind == Q2_PROJ_GRENADE || p->kind == Q2_PROJ_HAND_GRENADE) &&
        !hit && normal) {
        s64 dot = (s64)p->vel[0] * normal[0] + (s64)p->vel[1] * normal[1] +
                  (s64)p->vel[2] * normal[2];
        int k;

        dot >>= 12;                       /* normal is 1.3.12 */
        for (k = 0; k < 3; k++) {
            s64 v = p->vel[k] - 2 * ((dot * normal[k]) >> 12);
            v = v * Q2_GRENADE_BOUNCE_NUM / Q2_GRENADE_BOUNCE_DEN;
            p->vel[k] = (s32)v;
        }
        p->bounced = true;
        return false;
    }

    /*
     * The rocket applies its direct hit at full damage BEFORE the blast
     * (0x8004AE14 then 0x8004AE54), so a target that eats a rocket takes both.
     * The bolt has no blast at all and is only ever the direct hit.
     */
    if (hit && p->damage > 0)
        q2_combat_damage(attacker, hit, p->damage, p->mod, point, rules);

    if (p->splash_radius > 0 && p->damage > 0)
        q2_combat_radius_damage(attacker, hit, p->pos, p->damage,
                                p->splash_radius, p->mod,
                                targets, target_count, rules);

    release(list, index);
    return true;
}
