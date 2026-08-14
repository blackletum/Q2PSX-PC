#include "combat.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Mod properties                                                             */
/* ------------------------------------------------------------------------- */
/*
 * The jump table at 0x800ACE1C, sixteen words indexed by mod-1. Each entry
 * lands on either 0x80058348 (`s0 = 1`) or 0x80058350 (`s0 = 0`), and `s0` is
 * the third argument to the armour routine — which reads the `+4` column when
 * it is set and the `+2` column when it is clear. So the flag is "this is
 * energy damage".
 *
 * Anything above 16 misses the table's `sltiu ..., 16` bound and falls through
 * with s0 already zero, so mods 17..21 — bullets among them — are ordinary.
 */
static const u8 k_mod_energy[Q2_MOD_COUNT] = {
    /*  0 */ 0,
    /*  1 */ 1, /*  2 */ 1, /*  3 */ 0, /*  4 */ 1,
    /*  5 */ 1, /*  6 */ 1, /*  7 */ 0, /*  8 */ 0,
    /*  9 */ 0, /* 10 */ 0, /* 11 */ 1, /* 12 */ 1,
    /* 13 */ 0, /* 14 */ 1, /* 15 */ 0, /* 16 */ 1,
    /* 17 */ 0, /* 18 */ 0, /* 19 */ 0, /* 20 */ 0, /* 21 */ 0
};

bool q2_mod_is_energy(s16 mod)
{
    if (mod < 0 || mod >= Q2_MOD_COUNT)
        return false;
    return k_mod_energy[mod] != 0;
}

bool q2_mod_knocks_back(s16 mod)
{
    /* 0x80057ED0..0x80057EE8, in the order the branches test them. */
    return mod == Q2_MOD_ROCKET || mod == Q2_MOD_GRENADE ||
           mod == Q2_MOD_RAIL   || mod == Q2_MOD_BULLET;
}

s16 q2_mod_effect_timer(s16 mod, int *slot)
{
    /* 0x800585A4..0x80058604. Four mods arm a timer, each in its own byte. */
    switch (mod) {
    case Q2_MOD_ENERGY_BOLT: if (slot) *slot = 1; return 3;
    case Q2_MOD_2:           if (slot) *slot = 0; return 15;
    case Q2_MOD_4:           if (slot) *slot = 2; return 30;
    case Q2_MOD_5:           if (slot) *slot = 4; return 5;
    default:                 if (slot) *slot = -1; return 0;
    }
}

void q2_combat_rules_default(q2_combat_rules *r)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->skill = 1;     /* not the lowest, so monster damage is not halved */
}

/* ------------------------------------------------------------------------- */
/* Actors                                                                     */
/* ------------------------------------------------------------------------- */
void q2_actor_init(q2_actor *a)
{
    if (!a)
        return;
    memset(a, 0, sizeof(*a));
    a->radius = 286;      /* the movement sweep's half extent, FORMATS §5 */
}

void q2_actor_from_monster(q2_actor *a, const q2_monster *m)
{
    if (!a || !m)
        return;
    q2_actor_init(a);
    a->origin[0] = m->pos[0];
    a->origin[1] = m->pos[1];
    a->origin[2] = m->pos[2];
    a->health     = m->health;
    a->gib_health = m->gib_health;
    a->has_client = false;
    /* Every spawned creature is driven by a relocated module, so the module
     * owns its health — see the header. The port's own creatures do not have
     * one yet, so this stays false until a module is bound. */
    a->ai_owned = false;
}

void q2_actor_to_monster(const q2_actor *a, q2_monster *m)
{
    if (!a || !m)
        return;
    m->health = a->health;
    if (m->health <= 0)
        m->dead = true;
}

void q2_actor_from_player(q2_actor *a, const q2_inventory *inv,
                          const s32 pos[3])
{
    if (!a)
        return;
    q2_actor_init(a);
    if (pos) {
        a->origin[0] = pos[0];
        a->origin[1] = pos[1];
        a->origin[2] = pos[2];
    }
    a->has_client = true;
    if (!inv)
        return;
    a->health       = inv->health;
    a->armour       = inv->armour;
    a->armour_class = 0;
    a->cells        = inv->ammo[Q2_AMMO_CELLS];
    a->gib_health   = -100;
}

void q2_actor_to_player(const q2_actor *a, q2_inventory *inv)
{
    if (!a || !inv)
        return;
    inv->health = a->health;
    inv->armour = a->armour;
    inv->ammo[Q2_AMMO_CELLS] = a->cells;
}

/* ------------------------------------------------------------------------- */
/* Armour                                                                     */
/* ------------------------------------------------------------------------- */
s16 q2_combat_power_armour_absorb(q2_actor *a, s16 damage)
{
    s32 save, cap;

    if (!a || damage <= 0 || !a->has_client)
        return 0;
    /* 0x80057AC0: both power items live in one bit pair of the powerup word. */
    if (!(a->powerups & Q2_POWERUP_POWER_ARMOUR))
        return 0;
    if (a->cells <= 0)
        return 0;

    /* 0x80057AE0: `(damage * 2) / 3`, signed, truncating. */
    save = ((s32)damage * Q2_POWER_ARMOUR_NUM) / Q2_POWER_ARMOUR_DEN;

    /* 0x80057AF4: capped at twice the cells held. */
    cap = (s32)a->cells * 2;
    if (save >= cap)
        save = cap;
    save = (s16)save;
    if (save <= 0)
        return 0;

    /* 0x80057B8C: one cell per two points absorbed, truncating. */
    a->cells = (s16)(a->cells - (save / Q2_POWER_ARMOUR_CELLS));
    if (a->cells < 0)
        a->cells = 0;

    return (s16)save;
}

s16 q2_combat_armour_absorb(q2_actor *a, s16 damage, bool energy,
                            bool power_armour_fired,
                            const q2_combat_rules *rules)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    const q2_wt_armour *cls;
    s32 bias, factor, save;

    (void)power_armour_fired;   /* only suppresses the hit sound, not the maths */

    if (!a || damage <= 0 || !a->has_client)
        return 0;
    if (a->armour <= 0)
        return 0;
    if (a->armour_class >= Q2_WT_ARMOUR_CLASSES)
        return 0;

    /* 0x80057C0C: the bias is chosen by the same global the skill check uses.
     * 4095 rounds every non-zero fraction up; 2048 rounds to nearest. */
    bias = (rules && rules->deathmatch) ? Q2_ARMOUR_BIAS_DM : Q2_ARMOUR_BIAS_SP;

    cls    = &t->armour[a->armour_class];
    factor = energy ? cls->energy_protection : cls->normal_protection;

    /* 0x80057C7C: `(bias + factor * damage) >> 12`, a LOGICAL shift of a value
     * that cannot be negative here because both terms are non-negative. */
    save = (bias + factor * (s32)damage) >> 12;
    if (save > a->armour)
        save = a->armour;
    save = (s16)save;
    if (save <= 0)
        return 0;

    a->armour = (s16)(a->armour - save);
    return (s16)save;
}

/* ------------------------------------------------------------------------- */
/* Knockback                                                                  */
/* ------------------------------------------------------------------------- */
static s32 isqrt64(s64 v)
{
    s64 lo = 0, hi = 0x7FFFFFFF, best = 0;

    if (v <= 0)
        return 0;
    while (lo <= hi) {
        s64 mid = lo + (hi - lo) / 2;
        if (mid * mid <= v) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (s32)best;
}

static void apply_knockback(q2_actor *attacker, q2_actor *target,
                            s16 damage, const s32 point[3],
                            const q2_combat_rules *rules)
{
    s64 dir[3];
    s32 len;
    s32 scale;
    s32 mass = rules ? rules->knockback_mass : 0;
    bool self = (attacker == target) && target->has_client;
    int i;

    dir[0] = (s64)target->origin[0] - point[0];
    dir[1] = (s64)target->origin[1] - point[1];
    dir[2] = (s64)target->origin[2] - point[2];

    /* 0x80057F28 normalises through 0x8008A588; the port does the same in
     * 1.3.12 so the multiply below keeps its scale. */
    len = isqrt64(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len <= 0)
        return;
    for (i = 0; i < 3; i++)
        dir[i] = (dir[i] * 4096) / len;

    /*
     * 0x80057F80: `125 * (mass + 64) / 64` for an ordinary hit, and
     * 0x80057FA8: `25 * (mass + 64) / 4` when a player hurt themselves.
     * The second is 3.2 times the first — the rocket jump.
     */
    if (self)
        scale = (25 * (mass + 64)) >> 2;
    else
        scale = (125 * (mass + 64)) >> 6;

    for (i = 0; i < 3; i++) {
        /*
         * 0x80057FC8..0x800580C4: `scale * unit[i] * damage / 2400 >> 4`, and
         * NOTHING divides the 1.3.12 scale back out — the unit vector's 4096 is
         * part of the impulse's magnitude. That is why the s16 clamps below are
         * reachable: anything past about 123 points of damage saturates.
         */
        s64 v = (s64)scale * dir[i];
        v = (v * damage) / Q2_KNOCKBACK_DIVISOR;
        v >>= Q2_KNOCKBACK_SHIFT;

        /* 0x800580E8: only the vertical component is floored, and only outside
         * deathmatch. World Y grows downward, so -3072 is a ceiling on how far
         * a blast underfoot can throw you — the single-player rocket jump has a
         * limit the deathmatch one does not. */
        if (i == 1 && !(rules && rules->deathmatch) && v < -3072)
            v = -3072;

        /* 0x800580F8..0x80058188: each component is clamped into s16. */
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;

        /* 0x80058188: a living target accumulates, a dead one is overwritten. */
        if (target->health > 0)
            target->knockback[i] += (s32)v;
        else
            target->knockback[i]  = (s32)v;
    }

    if (target->health > 0)
        target->knocked = true;
}

/* ------------------------------------------------------------------------- */
/* The damage function                                                        */
/* ------------------------------------------------------------------------- */
q2_damage_result q2_combat_damage(q2_actor *attacker, q2_actor *target,
                                  s16 damage, s16 mod, const s32 point[3],
                                  const q2_combat_rules *rules)
{
    q2_damage_result out;
    q2_combat_rules local;
    s32 amount = damage;
    s16 saved_power = 0, saved_armour = 0;
    bool was_alive;

    memset(&out, 0, sizeof(out));

    if (!target)
        return out;
    if (!rules) {
        q2_combat_rules_default(&local);
        rules = &local;
    }

    was_alive = target->health > 0;
    target->last_mod = mod;

    if (amount <= 0)
        return out;

    /* Knockback comes first and does not care about armour: 0x80057EC0 runs
     * before any absorption, and only when a point was supplied. */
    if (point && q2_mod_knocks_back(mod))
        apply_knockback(attacker, target, damage, point, rules);

    /*
     * 0x800582C8: at skill 0, a monster hitting a player does half. The test is
     * on the ATTACKER having no client block, which is what makes it "a monster
     * hit you" rather than "you were hurt".
     */
    if (rules->skill == 0 && target->has_client &&
        attacker && !attacker->has_client)
        amount = (amount + 1) >> 1;

    /* Invulnerability and the second protection powerup: 0x80058244 and
     * 0x80058230 both return outright while the clock has not passed. */
    if (target->has_client) {
        if (rules->level_time < target->invuln_until ||
            rules->level_time < target->protect_until) {
            out.blocked = true;
            return out;
        }
    }

    /* The two environmental mods are throttled per target rather than per hit:
     * 0x80058268 sets the next allowed time 400 ticks out, 0x800582AC 100. */
    if (mod == Q2_MOD_ACID || mod == Q2_MOD_LAVA) {
        s32 gap = (mod == Q2_MOD_ACID) ? Q2_ENV_THROTTLE_ACID
                                       : Q2_ENV_THROTTLE_LAVA;
        if (target->has_client) {
            if (rules->level_time < target->env_next) {
                out.blocked = true;
                return out;
            }
            target->env_next = rules->level_time + gap;
        }
    }

    /* Armour. Mod 8 is the one class that skips both stages (0x80058358). */
    if (mod != Q2_MOD_NO_ARMOUR) {
        saved_power = q2_combat_power_armour_absorb(target, (s16)amount);
        amount -= saved_power;

        saved_armour = q2_combat_armour_absorb(target, (s16)amount,
                                               q2_mod_is_energy(mod),
                                               saved_power != 0, rules);
        amount -= saved_armour;
    }

    out.absorbed_power  = saved_power;
    out.absorbed_armour = saved_armour;

    if (amount <= 0)
        return out;

    /*
     * A creature with a module posts rather than subtracts: 0x800584B4 hands
     * the amount to 0x800627F8 and jumps past the health store. Only brainless
     * entities and players are decremented here.
     */
    if (target->ai_owned && !target->has_client) {
        out.posted_to_ai = true;
        out.taken = (s16)amount;
        return out;
    }

    target->health = (s16)(target->health - amount);
    out.taken = (s16)amount;

    if (was_alive && target->health <= 0) {
        out.killed = true;
        out.gibbed = target->health <= target->gib_health;
    }

    {
        int slot;
        s16 v = q2_mod_effect_timer(mod, &slot);
        if (slot >= 0 && slot < (int)(sizeof(target->effect)))
            target->effect[slot] = (u8)v;
    }

    return out;
}

/* ------------------------------------------------------------------------- */
/* Radius damage                                                              */
/* ------------------------------------------------------------------------- */
s16 q2_combat_splash_at(s16 damage, s32 dist)
{
    s32 loss = (s32)(((s64)dist * Q2_SPLASH_FALLOFF_NUM) >>
                     Q2_SPLASH_FALLOFF_SHIFT);
    s32 v = (s32)damage - loss;
    return v > 0 ? (s16)v : 0;
}

u32 q2_combat_radius_damage(q2_actor *attacker, q2_actor *ignore,
                            const s32 point[3], s16 damage, s16 radius,
                            s16 mod, q2_actor **targets, u32 count,
                            const q2_combat_rules *rules)
{
    u32 hurt = 0, i;

    if (!point || !targets || damage <= 0 || radius <= 0)
        return 0;

    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 dx, dy, dz, d2;
        s64 reach;
        s32 dist;
        s16 points;

        if (!t || t == ignore)
            continue;

        dx = (s64)t->origin[0] - point[0];
        dy = (s64)t->origin[1] - point[1];
        dz = (s64)t->origin[2] - point[2];
        d2 = dx * dx + dy * dy + dz * dz;

        /* 0x800509AC: the comparison radius is the blast plus the target's own,
         * so a large creature is caught by a blast that misses its centre. */
        reach = (s64)radius + t->radius;
        if (d2 > reach * reach)
            continue;

        dist   = isqrt64(d2);
        points = q2_combat_splash_at(damage, dist);
        if (points <= 0)
            continue;

        q2_combat_damage(attacker, t, points, mod, point, rules);
        hurt++;
    }

    return hurt;
}

/* ------------------------------------------------------------------------- */
/* Tracing                                                                    */
/* ------------------------------------------------------------------------- */
s64 q2_combat_ray_dist_sq(const s32 origin[3], const s32 dir[3],
                          const s32 point[3], s64 *out_along)
{
    s64 vx, vy, vz;
    s64 len2, dot, along;
    s64 cx, cy, cz;

    if (out_along)
        *out_along = 0;
    if (!origin || !dir || !point)
        return 0;

    vx = (s64)point[0] - origin[0];
    vy = (s64)point[1] - origin[1];
    vz = (s64)point[2] - origin[2];

    len2 = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] + (s64)dir[2] * dir[2];
    if (len2 <= 0)
        return vx * vx + vy * vy + vz * vz;

    dot = (s64)dir[0] * vx + (s64)dir[1] * vy + (s64)dir[2] * vz;

    /* Fraction along the ray, 1.0.12 — 4096 is the far end. Nothing is
     * normalised because the direction's LENGTH is the weapon's range. */
    along = (dot * 4096) / len2;
    if (out_along)
        *out_along = along;

    cx = vx - ((s64)dir[0] * along) / 4096;
    cy = vy - ((s64)dir[1] * along) / 4096;
    cz = vz - ((s64)dir[2] * along) / 4096;

    return cx * cx + cy * cy + cz * cz;
}

/* Shared scan: the nearest actor whose sphere the ray crosses within the
 * fraction the world allows. Returns its index, or -1. */
static s32 nearest_hit(const s32 origin[3], const s32 dir[3],
                       s32 world_fraction, s32 hit_radius,
                       q2_actor **targets, u32 count, u32 skip_mask_index,
                       s64 *out_along)
{
    s32 best = -1;
    s64 best_along = 0;
    u32 i;

    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 along = 0, d2;
        s64 reach;

        if (!t || i == skip_mask_index)
            continue;
        if (t->health <= 0)
            continue;

        d2 = q2_combat_ray_dist_sq(origin, dir, t->origin, &along);
        if (along <= 0 || along > world_fraction)
            continue;

        reach = (s64)hit_radius + t->radius;
        if (d2 > reach * reach)
            continue;

        if (best < 0 || along < best_along) {
            best = (s32)i;
            best_along = along;
        }
    }

    if (out_along)
        *out_along = best_along;
    return best;
}

q2_damage_result q2_combat_melee(q2_actor *attacker, q2_actor *target,
                                 s16 damage, const q2_combat_rules *rules)
{
    q2_damage_result out;

    memset(&out, 0, sizeof(out));
    if (!attacker || !target)
        return out;

    /* 0x800612F0 passes the attacker's own origin as the damage point, so a
     * melee hit lands with mod 7 — which is not in the knockback set, and so
     * a creature's claws move nothing. */
    return q2_combat_damage(attacker, target, damage, Q2_MOD_MELEE,
                            attacker->origin, rules);
}

s32 q2_combat_nearest_on_segment(const s32 origin[3], const s32 dir[3],
                                 s32 hit_radius, q2_actor **targets,
                                 u32 count)
{
    if (!origin || !dir || !targets)
        return -1;
    return nearest_hit(origin, dir, 4096, hit_radius, targets, count,
                       (u32)-1, NULL);
}

s32 q2_combat_fire_bullet(q2_actor *attacker, const s32 origin[3],
                          const s32 dir[3], s16 damage, s32 world_fraction,
                          s32 hit_radius, q2_actor **targets, u32 count,
                          const q2_combat_rules *rules,
                          q2_damage_result *out)
{
    s32 idx;
    s64 along = 0;
    s32 point[3];
    int k;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!origin || !dir || !targets)
        return -1;
    if (world_fraction <= 0 || world_fraction > 4096)
        world_fraction = (world_fraction <= 0) ? 0 : 4096;

    idx = nearest_hit(origin, dir, world_fraction, hit_radius,
                      targets, count, (u32)-1, &along);
    if (idx < 0)
        return -1;

    for (k = 0; k < 3; k++)
        point[k] = origin[k] + (s32)(((s64)dir[k] * along) / 4096);

    {
        q2_damage_result r = q2_combat_damage(attacker, targets[idx], damage,
                                              Q2_MOD_BULLET, point, rules);
        if (out)
            *out = r;
    }
    return idx;
}

u32 q2_combat_fire_rail(q2_actor *attacker, const s32 origin[3],
                        const s32 dir[3], s16 damage, s32 world_fraction,
                        s32 hit_radius, q2_actor **targets, u32 count,
                        const q2_combat_rules *rules)
{
    u32 hit = 0, i;

    if (!origin || !dir || !targets)
        return 0;
    if (world_fraction <= 0 || world_fraction > 4096)
        world_fraction = (world_fraction <= 0) ? 0 : 4096;

    /* The rail does not stop: 0x800493A8 loops from each impact, so everything
     * on the beam takes the full amount. */
    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 along = 0, d2, reach;
        s32 point[3];
        int k;

        if (!t || t->health <= 0)
            continue;

        d2 = q2_combat_ray_dist_sq(origin, dir, t->origin, &along);
        if (along <= 0 || along > world_fraction)
            continue;

        reach = (s64)hit_radius + t->radius;
        if (d2 > reach * reach)
            continue;

        for (k = 0; k < 3; k++)
            point[k] = origin[k] + (s32)(((s64)dir[k] * along) / 4096);

        q2_combat_damage(attacker, t, damage, Q2_MOD_RAIL, point, rules);
        hit++;
    }

    return hit;
}
