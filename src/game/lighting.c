/*
 * lighting.c — the three-light selector and the GTE state it produces.
 *
 * Transcribed from 0x8006AFE8 (the gather), 0x8006AD4C (one candidate),
 * 0x80076040 (the attenuation) and 0x8008A5E8 (libgte's VectorNormal), and
 * since the timed-light round from 0x80075D14 (AddTimedLight), 0x800741AC
 * (ProcessTimedLights) and the ramp inside 0x80074598 (ProcessMobileLights).
 * Where
 * the original's arithmetic is observable it is reproduced rather than tidied:
 * the 16-bit wrapping delta, the truncating divides, the order the three axes
 * are rejected in, and the fact that the ranking never actually sorts — it
 * rotates a four-entry list and lets the loser be overwritten.
 */
#include "lighting.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* 0x80076040 — distance attenuation                                          */
/* ------------------------------------------------------------------------- */
s32 q2_light_attenuation(const q2_light *l, const s16 d[3], s32 scale)
{
    u32 outer, inner, num, den;
    u32 dist;

    if (!l || !d)
        return 0;

    /*
     * `mult` then `srl 6`. The multiply is allowed to overflow into the low
     * word and the shift is LOGICAL, which is load-bearing: the fallback
     * light's radiusSq of 0x3FFFFFFF times 64 wraps to 0xFFFFFFC0 and comes
     * back as 0x03FFFFFF. A port that widened this to 64 bits would give that
     * light a radius sixteen times too large.
     */
    outer = (l->radius_sq * (u32)scale) >> Q2_LIGHT_ATTEN_SHIFT;

    dist = (u32)((s32)d[0] * d[0] + (s32)d[1] * d[1] + (s32)d[2] * d[2]);
    if (dist >= outer)
        return 0;

    num   = outer - dist;
    inner = (l->inner_radius_sq * (u32)scale) >> Q2_LIGHT_ATTEN_SHIFT;
    den   = outer - inner;

    /* Make room for the << 12 without losing the top of the numerator. Both
     * sides shift together, so the ratio survives. */
    while (num & 0xFFF00000u) {
        num >>= 2;
        den >>= 2;
    }

    if (den == 0)
        return Q2_LIGHT_ONE;   /* inner == outer: a flat fill, not a falloff */

    return (s32)((num << 12) / den);
}

/* ------------------------------------------------------------------------- */
/* The 16-bit wrapping delta                                                  */
/* ------------------------------------------------------------------------- */
void q2_light_delta(const q2_light *l, const s32 origin[3], s16 out[3])
{
    const s32 lp[3] = { l->x, l->y, l->z };
    int i;

    for (i = 0; i < 3; i++)
        out[i] = (s16)(u16)((u16)(u32)lp[i] - (u16)(u32)origin[i]);
}

/* ------------------------------------------------------------------------- */
/* 0x8006AD4C — offer one light                                               */
/* ------------------------------------------------------------------------- */
static s32 abs_s32(s32 v) { return v < 0 ? -v : v; }

/* `mult` then `sra 8` with a +255 bias for negatives: a divide by 256 that
 * truncates toward zero rather than flooring. */
static s32 div256_trunc(s32 v)
{
    return (v < 0 ? v + 255 : v) >> 8;
}

bool q2_light_set_add(q2_light_set *set, const s32 origin[3], const q2_light *l)
{
    q2_light_slot *slot;
    s32 atten, total;
    int i;

    if (!set || !origin || !l)
        return false;

    slot = &set->slot[set->rank[3]];

    q2_light_delta(l, origin, slot->d);

    /*
     * Axis reject, and the order is the original's: x, then Z, then y
     * (0x8006ADB4, 0x8006ADCC, 0x8006ADEC). It only matters for which
     * comparison runs first, but transcribing it costs nothing and a later
     * reader comparing against the disassembly should not have to wonder.
     */
    if (abs_s32(slot->d[0]) >= (s32)l->radius) return false;
    if (abs_s32(slot->d[2]) >= (s32)l->radius) return false;
    if (abs_s32(slot->d[1]) >= (s32)l->radius) return false;

    atten = q2_light_attenuation(l, slot->d, Q2_LIGHT_ATTEN_SCALE);
    if (atten == 0)
        return false;
    if (atten > Q2_LIGHT_ONE)
        atten = Q2_LIGHT_ONE;

    {
        const u8 rgb[3] = { l->r, l->g, l->b };
        for (i = 0; i < 3; i++) {
            s32 c = div256_trunc((s32)rgb[i] * atten);
            if (c > Q2_LIGHT_ONE)
                c = Q2_LIGHT_ONE;
            slot->rgb[i] = (s16)c;
        }
    }

    total = slot->rgb[0] + slot->rgb[1] + slot->rgb[2];
    slot->total = (s16)total;

    /*
     * Insertion by rotation. There is no sort and no compaction: the candidate
     * already occupies the slot ranked last, so accepting it means moving its
     * rank up and pushing the others down. Rejecting it means doing nothing,
     * and the next candidate overwrites it.
     */
    if (set->slot[set->rank[0]].total < slot->total) {
        s16 cand = set->rank[3];
        set->rank[3] = set->rank[2];
        set->rank[2] = set->rank[1];
        set->rank[1] = set->rank[0];
        set->rank[0] = cand;
        set->count++;
        return true;
    }
    if (set->slot[set->rank[1]].total < slot->total) {
        s16 cand = set->rank[3];
        set->rank[3] = set->rank[2];
        set->rank[2] = set->rank[1];
        set->rank[1] = cand;
        set->count++;
        return true;
    }
    if (set->slot[set->rank[2]].total < slot->total) {
        s16 cand = set->rank[3];
        set->rank[3] = set->rank[2];
        set->rank[2] = cand;
        set->count++;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* 0x8006AFE8 — the gather                                                    */
/* ------------------------------------------------------------------------- */
void q2_light_set_begin(q2_light_set *set)
{
    int i;

    if (!set)
        return;

    memset(set, 0, sizeof(*set));
    for (i = 0; i < Q2_LIGHT_SLOTS; i++)
        set->rank[i] = (s16)i;
}

/* 0x8006B150 — the record the engine builds at 0x800A1C48 when an entity has no
 * collision node. Position from the entity, everything else fixed. */
static void fallback_light(q2_light *out, const s32 origin[3])
{
    memset(out, 0, sizeof(*out));

    out->x = origin[0] + Q2_LIGHT_FALLBACK_OFS_X;
    out->y = origin[1] + Q2_LIGHT_FALLBACK_OFS_Y;
    out->z = origin[2] + Q2_LIGHT_FALLBACK_OFS_Z;

    out->r = out->g = out->b = Q2_LIGHT_FALLBACK_GREY;
    out->always_255 = 0;      /* really 0 in this record, not 255 */
    out->type       = 0;      /* style 0: no flare                */
    out->radius     = 0x7FFF;
    out->inner_radius_sq = 0x3FFFFFFFu;
    out->radius_sq       = 0x3FFFFFFFu;
}

void q2_light_gather(q2_light_set *set, const q2_light_world *w,
                     const s32 origin[3], s32 coll_node, s16 entity_f4)
{
    u32 i;

    if (!set || !origin)
        return;

    q2_light_set_begin(set);

    if (!w)
        return;

    if (entity_f4 < 0) {
        /* 0x8006B040 is `bgez`: only a negative +0xF4 reaches the view list at
         * 0x8006B048, and then nothing else — no world or static lights. */
        for (i = 0; i < w->dynamic_view_count && i < Q2_DYNLIGHT_MAX; i++)
            q2_light_set_add(set, origin, &w->dynamic_view[i]);
        return;
    }

    /* 0x8006B08C: the world dynamic list first. */
    for (i = 0; i < w->dynamic_world_count && i < Q2_DYNLIGHT_MAX; i++)
        q2_light_set_add(set, origin, &w->dynamic_world[i]);

    if (coll_node < 0) {
        q2_light fb;
        fallback_light(&fb, origin);
        q2_light_set_add(set, origin, &fb);
        return;
    }

    /* 0x8006B0C8: the static lights of the node the entity stands in. */
    if (w->space && w->statics) {
        u32 first, count;

        if (q2_spacelights_range(w->space, (u32)coll_node, &first, &count)) {
            for (i = 0; i < count; i++) {
                u16 index;
                q2_light l;

                if (!q2_spacelights_entry(w->space, first + i, &index))
                    break;
                if (!q2_light_get(w->statics, index, &l))
                    continue;
                q2_light_set_add(set, origin, &l);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* FLKLIGHT                                                                   */
/* ------------------------------------------------------------------------- */
s32 q2_flklight_inner_radius(s32 rand_0_32767)
{
    /* 0x8002882C: r*500 >> 15, + 400 -> a2's low half. */
    return ((rand_0_32767 * 500) >> 15) + 400;
}

s32 q2_flklight_outer_radius(s32 rand_0_32767)
{
    /* 0x8002885C: the same chain, + 1000 -> a2's high half. */
    return ((rand_0_32767 * 500) >> 15) + 1000;
}

/* 0x80075C34 — append a runtime light                                        */
/* ------------------------------------------------------------------------- */
bool q2_light_add_dynamic(q2_light_world *w, const s32 pos[3],
                          const u8 rgb[3], s32 inner_radius, s32 outer_radius,
                          u32 style, u32 size_shift)
{
    q2_light *l;

    if (!w || !pos || !rgb)
        return false;

    /* The original's full test is a pointer compare against the base of the
     * VIEW array, which is what makes sixteen the limit. Past it the light is
     * dropped and the caller is told so; nothing is replaced. */
    if (w->dynamic_world_count >= Q2_DYNLIGHT_MAX)
        return false;

    l = &w->dynamic_world[w->dynamic_world_count++];
    memset(l, 0, sizeof(*l));

    l->x = pos[0];
    l->y = pos[1];
    l->z = pos[2];

    l->r = rgb[0];
    l->g = rgb[1];
    l->b = rgb[2];

    /*
     * The type byte, assembled the way the original assembles it: the style
     * into bits 3..5 and the size shift into 6..7, with bits 0..2 left alone.
     * On a zeroed array that means they stay clear, where every light on the
     * disc has them set — the clearest possible statement that nothing reads
     * them.
     */
    l->type = (u8)(((style & 7u) << 3) | ((size_shift & 3u) << 6));

    /* Radii in, squares out. */
    l->radius          = (u16)(s16)outer_radius;
    l->inner_radius_sq = (u32)((s16)inner_radius * (s16)inner_radius);
    l->radius_sq       = (u32)((s16)outer_radius * (s16)outer_radius);

    return true;
}

void q2_light_world_begin_frame(q2_light_world *w)
{
    if (!w)
        return;

    w->dynamic_world_count = 0;
    w->dynamic_view_count  = 0;

    /* The timed table is NOT cleared here. ProcessLights resets only the two
     * list pointers (0x80075BAC, 0x80075BC0); the counters at 0x800E1218 are
     * zeroed by InitLights and by nothing else. */
}

/* ------------------------------------------------------------------------- */
/* 0x80075D14 — AddTimedLight                                                 */
/* ------------------------------------------------------------------------- */
bool q2_light_add_timed(q2_light_world *w, const s32 pos[3], const u8 rgb[3],
                        s32 inner_radius, s32 outer_radius, s32 ticks)
{
    q2_timed_light *t;
    u32 i;

    if (!w || !pos || !rgb)
        return false;

    /*
     * 0x80075D28: the first slot whose REMAINING halfword is zero. There is no
     * age, no priority and no eviction — when all sixteen are counting down the
     * function falls out of its loop with a null slot pointer and returns 0
     * (0x80075D48..0x80075D58).
     */
    for (i = 0; i < Q2_TIMEDLIGHT_SLOTS; i++)
        if (w->timed[i].remaining == 0)
            break;

    if (i == Q2_TIMEDLIGHT_SLOTS)
        return false;

    t = &w->timed[i];

    t->pos[0] = pos[0];          /* 0x80075D64..0x80075D78 */
    t->pos[1] = pos[1];
    t->pos[2] = pos[2];

    t->rgb[0]    = rgb[0];       /* 0x80075D7C: the whole colour word, so the */
    t->rgb[1]    = rgb[1];       /* fourth byte travels with it even though   */
    t->rgb[2]    = rgb[2];       /* both presets on the disc have it zero.    */
    t->colour_hi = 0;

    /*
     * The radii arrive packed into one register — inner in the low halfword,
     * outer in the high — and the truncation to 16 bits is the store, not the
     * caller. ProcessTimedLights reads them back with `lh`, signed.
     */
    t->inner = (s16)inner_radius;
    t->outer = (s16)outer_radius;

    /* 0x80075DD8 / 0x80075DDC: the SAME value into both counters. */
    t->total     = (s16)ticks;
    t->remaining = (s16)ticks;

    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x800741AC — ProcessTimedLights                                            */
/* ------------------------------------------------------------------------- */
u32 q2_light_timed_tick(q2_light_world *w)
{
    u32 emitted = 0;
    u32 i;

    if (!w)
        return 0;

    for (i = 0; i < Q2_TIMEDLIGHT_SLOTS; i++) {
        q2_timed_light *t = &w->timed[i];
        s32 inner, outer;

        /* 0x800741E4: a zero counter is a free slot and is stepped over
         * without being decremented. */
        if (t->remaining == 0)
            continue;

        /*
         * 0x800741EC..0x80074270. `mult` then `div`, both 32-bit and both
         * truncating toward zero, and the multiply happens FIRST — scaling the
         * radius before the divide is what keeps a small light from collapsing
         * to nothing on its second tick.
         */
        inner = ((s32)t->inner * t->remaining) / t->total;
        outer = ((s32)t->outer * t->remaining) / t->total;

        /*
         * The append is spelled out inline in the original rather than calling
         * AddLight, but it is instruction-for-instruction the same store
         * sequence, and the style and size bits come from four bytes the
         * function memsets to zero first (0x80074294 -> 0x80089E18 with a2 = 4).
         * So: style 0, size 0, always.
         */
        if (q2_light_add_dynamic(w, t->pos, t->rgb, inner, outer, 0, 0))
            emitted++;

        /*
         * 0x80074358, and reached from BOTH arms: a slot whose append was
         * refused because the world list is full still burns a tick. The
         * decrement is after the emit, which is why a slot given N ticks is
         * emitted at N/N first and 1/N last, and never at zero.
         */
        t->remaining = (s16)(t->remaining - 1);
    }

    return emitted;
}

void q2_light_timed_reset(q2_light_world *w)
{
    u32 i;

    if (!w)
        return;

    /* 0x80074194 walks the sixteen slots backwards storing zero into slot+32
     * and touches nothing else. A slot's position, colour and radii survive a
     * level load; only its claim on the table does not. */
    for (i = 0; i < Q2_TIMEDLIGHT_SLOTS; i++)
        w->timed[i].remaining = 0;
}

u32 q2_light_timed_active(const q2_light_world *w)
{
    u32 n = 0;
    u32 i;

    if (!w)
        return 0;

    for (i = 0; i < Q2_TIMEDLIGHT_SLOTS; i++)
        if (w->timed[i].remaining != 0)
            n++;

    return n;
}

/* ------------------------------------------------------------------------- */
/* The four muzzle flashes                                                    */
/* ------------------------------------------------------------------------- */
bool q2_light_muzzle_preset(u8 weapon_id, q2_muzzle_light *out)
{
    q2_muzzle_light m;

    memset(&m, 0, sizeof(m));
    m.rgb[0] = Q2_MUZZLE_FLASH_R;      /* 0x800AE9D4, all four weapons */
    m.rgb[1] = Q2_MUZZLE_FLASH_G;
    m.rgb[2] = Q2_MUZZLE_FLASH_B;

    switch (weapon_id) {
    case Q2_WID_SHOTGUN:
        /* 0x8004C3B0 addiu a0, s3, 164; 0x8004C3BC addiu a3, zero, 3. */
        m.inner         = Q2_SHOTGUN_LIGHT_INNER;
        m.outer         = Q2_SHOTGUN_LIGHT_OUTER;
        m.ticks         = Q2_SHOTGUN_LIGHT_TICKS;
        m.origin_offset = 0xA4;
        break;

    case Q2_WID_SUPER_SHOTGUN:
        /* 0x8004C674 addiu a0, s3, 84; 0x8004C680 addiu a3, zero, 4. */
        m.inner         = Q2_SSHOTGUN_LIGHT_INNER;
        m.outer         = Q2_SSHOTGUN_LIGHT_OUTER;
        m.ticks         = Q2_SSHOTGUN_LIGHT_TICKS;
        m.origin_offset = 0x54;
        break;

    case Q2_WID_MACHINEGUN:
    case Q2_WID_CHAINGUN:
        /*
         * 0x8004C744 and 0x8004CA9C. One frame through AddLight, and the radii
         * are two rand() draws away, with a different scale and base for each
         * weapon — q2_weapon_muzzle_light computes them, so this record carries
         * no numbers, only the fact that it must.
         */
        m.ticks         = 0;
        m.origin_offset = 0x54;
        m.random_radii  = true;
        break;

    default:
        return false;
    }

    if (out)
        *out = m;

    return true;
}

/* ------------------------------------------------------------------------- */
/* 0x80074598 — ProcessMobileLights, the arithmetic half                      */
/* ------------------------------------------------------------------------- */

/* `bgez` then `addiu +3` then `sra 2`: a divide by four that truncates toward
 * zero rather than flooring (0x80074658..0x80074674). */
static s32 div4_trunc(s32 v)
{
    return (v < 0 ? v + 3 : v) >> 2;
}

void q2_light_ramp_begin(q2_light_ramp *out, const q2_light *l)
{
    s32 inner, outer;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    if (!l)
        return;

    /* 0x80074628/0x80074638/0x80074644: r, g and b, in that order, out of
     * light+12, +13 and +14. */
    out->rgb[0] = l->r;
    out->rgb[1] = l->g;
    out->rgb[2] = l->b;
    out->radius = (s32)l->radius;

    inner = (s32)l->inner_radius_sq;
    outer = (s32)l->radius_sq;

    /*
     * 0x80074648: while the OUTER square will not fit in 32767, quarter both
     * and count the step. The limit is not arbitrary — GetDist2Shift_ squares
     * through the GTE with lm = 1, so each axis' square saturates at 0x7FFF and
     * a comparison against an unscaled radiusSq would be against a number the
     * distance can never reach.
     */
    while (outer > 32767) {
        inner = div4_trunc(inner);
        outer = div4_trunc(outer);
        out->shift++;
    }

    out->inner_sq = inner;
    out->outer_sq = outer;

    /* 0x8007468C..0x800746E4: the difference, divided by 256, truncating. It
     * is computed once per light and reused for every point of every brush. */
    out->den = div256_trunc(outer - inner);
}

bool q2_light_ramp_reaches(const q2_light_ramp *r, const s32 light_pos[3],
                           const s32 box_min[3], const s32 box_max[3],
                           u32 obj_flags)
{
    s32 lo[3], hi[3];
    int i;

    if (!r || !light_pos || !box_min || !box_max)
        return false;

    /* 0x8007469C..0x800746FC: the light's own box, pos +/- the RADIUS — not the
     * radius squared, and not the scaled one. */
    for (i = 0; i < 3; i++) {
        lo[i] = light_pos[i] - r->radius;
        hi[i] = light_pos[i] + r->radius;
    }

    /*
     * Six strict compares in the order x, z, y (0x80074734, 0x80074764,
     * 0x80074794 and the three interleaved with them). Strict matters: a light
     * whose box exactly touches a brush's face misses it.
     */
    if (!(lo[0] < box_max[0])) return false;
    if (!(box_min[0] < hi[0])) return false;
    if (!(lo[2] < box_max[2])) return false;
    if (!(box_min[2] < hi[2])) return false;
    if (!(lo[1] < box_max[1])) return false;
    if (!(box_min[1] < hi[1])) return false;

    /* 0x800747C4: the object's own dword at +0x0C, tested against 0x00800000.
     * An object carrying it is skipped whole — it keeps its baked colours. */
    if (obj_flags & Q2_LIGHT_NO_MOBILE)
        return false;

    return true;
}

/*
 * 0x800B0FB4 GetDist2Shift_ — the squared distance, shifted.
 *
 * MTC2 into IR1..IR3 keeps the low halfword sign-extended, so the shifted delta
 * is truncated to 16 bits before it is ever squared; SQR runs with sf = 0 and
 * lm = 1, so each component's square is unshifted and saturates at 0x7FFF. The
 * sum of three saturating squares is what the ramp is measured against, and it
 * is why the /4 loop above exists.
 */
static s32 gte_sqr_ir(s32 v)
{
    s32 s = (s16)v;
    s32 q = s * s;

    return q > 32767 ? 32767 : q;
}

static s32 dist2_shift(const s16 point[3], const s32 light_local[3], s32 shift)
{
    s32 sum = 0;
    int i;

    for (i = 0; i < 3; i++) {
        /* `lh` the point, `lw` the light, subtract in 32 bits, `srav`. */
        s32 d = (s32)point[i] - light_local[i];
        sum += gte_sqr_ir(d >> shift);
    }

    return sum;
}

bool q2_light_ramp_point(const q2_light_ramp *r, const s16 point[3],
                         const s32 light_local[3], s32 out[3])
{
    s32 dist, t, s;
    int i;

    if (!r || !point || !light_local || !out)
        return false;

    dist = dist2_shift(point, light_local, r->shift);

    /* 0x800749BC: strictly outside the outer square and the point is skipped
     * altogether — no contribution, and the packet keeps its baked colour. */
    if (r->outer_sq < dist)
        return false;

    t = dist - r->inner_sq;

    if (t < 0 || r->den == 0) {
        /*
         * 0x800749C8: inside the INNER radius the ramp is not evaluated at all
         * and the light contributes its colour bytes whole. So a light's core
         * is flat, not peaked — the falloff only exists between the two radii.
         *
         * `den == 0` joins that arm rather than dividing. The original would
         * take the `break 0x1C00` at 0x800749F8 — a divide-by-zero trap — which
         * needs innerSq == outerSq after the /4 loop. No light on the disc and
         * none AddLight builds produces that, so this is a guard for a case the
         * console never reaches, not a difference in behaviour.
         */
        out[0] = r->rgb[0];
        out[1] = r->rgb[1];
        out[2] = r->rgb[2];
        return true;
    }

    /*
     * 0x800749EC then 0x80074A1C. `s` reaches 0 well before the outer radius
     * and goes NEGATIVE on the last shell, because `den` was truncated down:
     * 600/1200 gives den = 65, and a point exactly on the outer radius has
     * t = 16875, t / den = 259, s = -4. The three `addiu +255` biases at
     * 0x80074A30, 0x80074A4C and 0x80074A68 exist to make that final divide
     * truncate toward zero rather than floor.
     */
    s = 255 - t / r->den;

    for (i = 0; i < 3; i++)
        out[i] = div256_trunc(s * (s32)r->rgb[i]);

    return true;
}

void q2_light_ramp_add(u8 vertex_rgb[3], const s32 contrib[3])
{
    int i;

    if (!vertex_rgb || !contrib)
        return;

    /*
     * 0x80074AC0..0x80074B0C: `addu` in 32 bits, `slti 256`, `sb`. There is a
     * ceiling and no floor, so the negative contribution of the outermost shell
     * subtracts and, on an already-black vertex, wraps through the byte store.
     * The cast is that `sb`.
     */
    for (i = 0; i < 3; i++) {
        s32 v = (s32)vertex_rgb[i] + contrib[i];

        if (v > 255)
            v = 255;
        vertex_rgb[i] = (u8)v;
    }
}

bool q2_light_point_slot(u8 slot, u32 *block, u32 *poly, u32 *corner)
{
    /* 0x80074A84: 0xFF ends the point's slot list, and the remaining slots of
     * that point are not examined. */
    if (slot == 0xFF)
        return false;

    /* 0x80074A8C tests the sign of the byte shifted up 24, i.e. bit 7. */
    if (block)
        *block = (u32)(slot >> 7);
    if (poly)
        *poly = (u32)((slot >> 2) & 31);   /* InitLights 0x8007413C andi 0x1F */
    if (corner)
        *corner = (u32)(slot & 3);

    return true;
}

s32 q2_light_packet_offset(u8 slot)
{
    /* 0x8007413C..0x80074164 builds this halfword for every byte 0..255:
     * 52 bytes a POLY_GT4, a 4-byte tag, and 12 bytes a corner. */
    return 52 * (s32)((slot >> 2) & 31) + 4 + 12 * (s32)(slot & 3);
}

/* ------------------------------------------------------------------------- */
/* 0x8008A5E8 — VectorNormal                                                  */
/* ------------------------------------------------------------------------- */
static s16  g_rsqrt[Q2_LIGHT_RSQRT_ENTRIES];
static bool g_rsqrt_ready;

/*
 * The original's table at 0x800A9C54 is 192 entries indexed by the magnitude's
 * mantissa in [64,256) — a four-fold window rather than a two-fold one, because
 * the exponent is rounded down to even so that halving it is exact. Entry i is
 * floor(32768 / sqrt(i + 64)), so entry 0 is exactly 4096 and entry 191 is
 * 2052. Built here from the formula for the same reason trig.c builds its sine
 * table: the derivation is checkable and everything after it is pure integer
 * arithmetic.
 */
static void build_rsqrt(void)
{
    int i;

    if (g_rsqrt_ready)
        return;

    for (i = 0; i < Q2_LIGHT_RSQRT_ENTRIES; i++)
        g_rsqrt[i] = (s16)(32768.0 / sqrt((double)(i + 64)));

    g_rsqrt_ready = true;
}

s16 q2_light_rsqrt_table(u32 index)
{
    build_rsqrt();
    return index < Q2_LIGHT_RSQRT_ENTRIES ? g_rsqrt[index] : 0;
}

void q2_vector_normal(const s16 in[3], s16 out[3])
{
    s32 sum, lzc, shift, mant;
    s16 recip;
    int i;

    build_rsqrt();

    if (!in || !out)
        return;

    /* SQR with sf = 0 (opcode 0x0A00428, bit 19 clear): the squares are NOT
     * pre-divided, so this is the magnitude squared at full scale. */
    sum = (s32)in[0] * in[0] + (s32)in[1] * in[1] + (s32)in[2] * in[2];

    if (sum <= 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }

    /*
     * LZCR with the low bit cleared. Forcing the exponent even is what makes
     * halving it exact, and it is also why the mantissa window is [64,256)
     * rather than [64,128): the top bit can land in either of two places.
     */
    lzc = (s32)q2_count_leading_zeros_u32((u32)sum) & ~1;
    shift = (31 - lzc) >> 1;

    mant = (lzc < 24) ? (sum >> (24 - lzc)) : (sum << (lzc - 24));
    if (mant < 64)  mant = 64;
    if (mant > 255) mant = 255;

    recip = g_rsqrt[mant - 64];

    /* GPF with sf = 0 (opcode 0x190003D): MAC = IR0 * IR with no shift of its
     * own, so the only shift is the exponent going back out. */
    for (i = 0; i < 3; i++)
        out[i] = (s16)(((s32)recip * in[i]) >> shift);
}

/* ------------------------------------------------------------------------- */
/* 0x8006B184 — the gather becomes GTE state                                  */
/* ------------------------------------------------------------------------- */

/* `mult` then `sra 11` with the +2047 bias for negatives. */
static s32 scale_11(s32 v)
{
    return (v < 0 ? v + 2047 : v) >> 11;
}

void q2_light_env_build(q2_light_env *env, const q2_light_set *set,
                        s32 intensity_a, s32 intensity_b, const u8 glow[3])
{
    s32 intensity;
    u32 active;
    u32 j;

    if (!env)
        return;

    memset(env, 0, sizeof(*env));

    if (!set)
        return;

    /* 0x8006B18C: four or more accepted still means three used. */
    active = set->count;
    if (active > Q2_LIGHT_ACTIVE_MAX)
        active = Q2_LIGHT_ACTIVE_MAX;
    env->active = active;

    intensity = scale_11(intensity_a * intensity_b);

    /*
     * The colour matrix is filled by COLUMN: light j's red, green and blue go
     * to rows 0, 1 and 2 of column j (0x8006B1E0 onward writes +0, +6, +12 for
     * the first, +2, +8, +14 for the second, +4, +10, +16 for the third).
     * Columns past `active` stay zero, which is why the count==1 and count==2
     * paths explicitly clear them.
     */
    for (j = 0; j < active; j++) {
        const q2_light_slot *s = &set->slot[set->rank[j]];
        int c;

        for (c = 0; c < 3; c++)
            env->colour.m[c][j] = s->rgb[c];

        q2_vector_normal(s->d, env->dir[j]);

        for (c = 0; c < 3; c++)
            env->dir[j][c] = (s16)(((s32)env->dir[j][c] * intensity) >> Q2_FRAC_12);
    }

    /*
     * The back colour is the entity's own glow, scaled by the SAME product but
     * shifted 24 instead of 11 and with no rounding bias (0x8006B468). With
     * both intensities at 4096 that is the identity, which is what makes 4096
     * the neutral value for each.
     */
    if (glow) {
        s32 product = intensity_a * intensity_b;
        int c;
        for (c = 0; c < 3; c++)
            env->back[c] = ((s32)glow[c] * product) >> 24;
    }
}

void q2_light_env_apply(const q2_light_env *env, gte_state *g)
{
    gte_matrix light;
    u32 j;
    int c;

    if (!env || !g)
        return;

    memset(&light, 0, sizeof(light));
    for (j = 0; j < Q2_LIGHT_ACTIVE_MAX; j++)
        for (c = 0; c < 3; c++)
            light.m[j][c] = (j < env->active) ? env->dir[j][c] : 0;

    gte_set_light_matrix(g, &light);
    gte_set_colour_matrix(g, &env->colour);
    gte_set_back_colour(g, env->back[0], env->back[1], env->back[2]);
}

/* ------------------------------------------------------------------------- */
/* 0x80075E14 — the ambient fade                                              */
/* ------------------------------------------------------------------------- */
void q2_light_glow_fade(u8 current[3], const u8 target[3], s32 steps)
{
    int i;

    if (!current || !target)
        return;

    if (steps == 0)
        steps = 1;

    for (i = 0; i < 3; i++) {
        s32 cur  = current[i];
        s32 diff = (s32)target[i] - cur;

        /*
         * `slt` against the step count first: a gap smaller than one step is
         * closed outright rather than truncating to no movement at all.
         *
         * The comparison is signed and the difference can be negative, so a
         * glow that is FALLING arrives immediately while one that is rising
         * eases. That asymmetry is the original's, not an oversight here.
         */
        if (diff < steps)
            current[i] = (u8)(cur + diff);
        else
            current[i] = (u8)(cur + diff / steps);
    }
}
