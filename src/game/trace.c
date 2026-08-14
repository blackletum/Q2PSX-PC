#include "trace.h"

#include <string.h>

#include "worldscale.h"

/* ------------------------------------------------------------------------- */
/* Helpers in the original's arithmetic                                       */
/* ------------------------------------------------------------------------- */

/* 0x8004545C: differences are taken between low halfwords, so they wrap. */
static s16 sub16(s32 a, s32 b)
{
    return (s16)(u16)((u16)((u32)a & 0xFFFFu) - (u16)((u32)b & 0xFFFFu));
}

/* ------------------------------------------------------------------------- */
/* 0x8005625C — the unstick nudge                                             */
/* ------------------------------------------------------------------------- */
/*
 * Six cases, chosen by a running maximum over +nx, -nx, +ny, -ny, +nz, -nz in
 * that order, each selecting a unit vector from the table at 0x800AEA64:
 *
 *     0x800AEA64  ( 1, 0, 0)      0x800AEA6C  (-1, 0, 0)
 *     0x800AEA74  ( 0, 1, 0)      0x800AEA7C  ( 0,-1, 0)
 *     0x800AEA84  ( 0, 0, 1)      0x800AEA8C  ( 0, 0,-1)
 *
 * The comparisons are strict (`slt`), so a tie keeps the earlier axis — which
 * is why an exactly diagonal normal nudges along X rather than splitting.
 */
void q2_move_push_vector(const s16 normal[3], s16 out[3])
{
    static const s16 axis[6][3] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1}
    };
    s32 best;
    int which = 0;

    if (!out)
        return;

    out[0] = out[1] = out[2] = 0;
    if (!normal)
        return;

    best = normal[0];
    if (best < -(s32)normal[0]) { best = -(s32)normal[0]; which = 1; }
    if (best <  (s32)normal[1]) { best =  (s32)normal[1]; which = 2; }
    if (best < -(s32)normal[1]) { best = -(s32)normal[1]; which = 3; }
    if (best <  (s32)normal[2]) { best =  (s32)normal[2]; which = 4; }
    if (best < -(s32)normal[2]) {                         which = 5; }

    out[0] = axis[which][0];
    out[1] = axis[which][1];
    out[2] = axis[which][2];
}

/* ------------------------------------------------------------------------- */
/* 0x80052C70 — swept point against one inflated box                          */
/* ------------------------------------------------------------------------- */
bool q2_move_box_overlap(const s32 move_min[3], const s32 move_max[3],
                         const s32 box_min[3], const s32 box_max[3])
{
    int i;

    if (!move_min || !move_max || !box_min || !box_max)
        return false;

    /* 0x80052CB8..0x80052D44, in the original's own axis order. */
    for (i = 0; i < 3; i++) {
        if (move_max[i] < box_min[i]) return false;
        if (box_max[i] < move_min[i]) return false;
    }

    return true;
}

bool q2_move_sweep_box(const s32 pos[3], const s16 delta[3],
                       const s32 box_min[3], const s32 box_max[3],
                       s16 other_dy, s32 half_extent,
                       s32 out_pos[3], s16 out_normal[3])
{
    /*
     * axis, "use the low face", and the normal, in the original's test order.
     * The normals are the six axis vectors at 0x800AEA2C, 8 bytes apart.
     */
    static const struct { int a, b, c; int low; s16 n[3]; } face[6] = {
        { 1, 0, 2, 1, { 0,  4096,  0} },   /* 0x800AEA2C */
        { 1, 0, 2, 0, { 0, -4096,  0} },   /* 0x800AEA34 */
        { 2, 0, 1, 1, { 0,  0,  4096} },   /* 0x800AEA3C */
        { 2, 0, 1, 0, { 0,  0, -4096} },   /* 0x800AEA44 */
        { 0, 1, 2, 1, { 4096,  0,  0} },   /* 0x800AEA4C */
        { 0, 1, 2, 0, {-4096,  0,  0} }    /* 0x800AEA54 */
    };
    s32 lo[3], hi[3];
    s32 d[3];
    int i, f;

    if (!pos || !delta || !box_min || !box_max)
        return false;

    /* The Minkowski sum: the target's box grown by the mover's half-extent. */
    for (i = 0; i < 3; i++) {
        lo[i] = box_min[i] - half_extent;
        hi[i] = box_max[i] + half_extent;
        d[i]  = delta[i];
    }

    /* Only the vertical axis is made relative to the target's own motion. */
    d[1] -= other_dy;

    for (f = 0; f < 6; f++) {
        int a = face[f].a, b = face[f].b, c = face[f].c;
        s32 plane = face[f].low ? lo[a] : hi[a];
        s32 pa    = pos[a] + ((a == 1) ? (s32)other_dy : 0);
        s32 gap, tb, tc;

        if (face[f].low) {
            if (plane < pa) continue;   /* already past the face   */
            if (d[a] < 0)   continue;   /* heading away from it    */
        } else {
            if (pa < plane) continue;
            if (d[a] > 0)   continue;
        }

        gap = plane - pa;

        /*
         * Both offsets are `d[other] * gap / d[a]`, truncating toward zero. A
         * move exactly parallel to the face (d[a] == 0) takes the zero branch
         * and can still report a contact — the original does that too, and the
         * caller's AABB gate is what keeps it from mattering.
         */
        tb = (d[a] != 0) ? (s32)(((s64)d[b] * gap) / d[a]) : 0;
        if (tb < lo[b] - pos[b] || hi[b] - pos[b] < tb)
            continue;

        tc = (d[a] != 0) ? (s32)(((s64)d[c] * gap) / d[a]) : 0;
        if (tc < lo[c] - pos[c] || hi[c] - pos[c] < tc)
            continue;

        if (out_pos) {
            out_pos[a] = plane;
            out_pos[b] = pos[b] + tb;
            out_pos[c] = pos[c] + tc;
        }
        if (out_normal) {
            out_normal[0] = face[f].n[0];
            out_normal[1] = face[f].n[1];
            out_normal[2] = face[f].n[2];
        }

        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* 0x80053C58 — sweep a move against every target                             */
/* ------------------------------------------------------------------------- */
int q2_move_sweep_world(const q2_move_world *w, const s32 pos[3],
                        const s32 dest[3], q2_move_contact *out)
{
    s32 cur[3];
    int result = 1;
    u32 i;
    int k;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->kind = Q2_MOVE_KIND_WORLD;
        out->id   = -1;
    }

    if (!pos || !dest)
        return 1;

    cur[0] = dest[0]; cur[1] = dest[1]; cur[2] = dest[2];

    if (w && w->targets) {
        for (i = 0; i < w->count; i++) {
            const q2_move_target *t = &w->targets[i];
            s32 mmin[3], mmax[3];
            s32 hit_pos[3];
            s16 hit_n[3];
            s16 delta[3];

            if (!t->active)
                continue;               /* 0x80053E20, byte +54 of the slot */

            /* 0x80054050: a target that declares flags is only considered when
             * they intersect the mover's own mask. */
            if (t->mask && !(t->mask & w->mask))
                continue;

            /* The move's AABB, rebuilt each iteration because a contact moves
             * the destination (0x80053EA8 onward). */
            for (k = 0; k < 3; k++) {
                s32 lo = pos[k] < cur[k] ? pos[k] : cur[k];
                s32 hi = pos[k] < cur[k] ? cur[k] : pos[k];

                mmin[k] = lo - w->half_extent;
                mmax[k] = hi + w->half_extent;
                delta[k] = sub16(cur[k], pos[k]);
            }

            if (!q2_move_box_overlap(mmin, mmax, t->min, t->max))
                continue;

            if (!q2_move_sweep_box(pos, delta, t->min, t->max, t->dy,
                                   w->half_extent, hit_pos, hit_n))
                continue;

            result = 0;
            cur[0] = hit_pos[0];
            cur[1] = hit_pos[1];
            cur[2] = hit_pos[2];

            if (out) {
                out->hit = true;
                out->kind = t->kind;
                out->id   = t->id;
                out->normal[0] = hit_n[0];
                out->normal[1] = hit_n[1];
                out->normal[2] = hit_n[2];
            }
        }
    }

    if (out) {
        out->pos[0] = cur[0];
        out->pos[1] = cur[1];
        out->pos[2] = cur[2];
    }

    return result;
}

/* ------------------------------------------------------------------------- */
/* 0x80050CE0 — which volume is the entity in                                 */
/* ------------------------------------------------------------------------- */
u16 q2_move_contents(const q2_move_world *w, const s32 pos[3], u16 mask)
{
    s32 lo[3], hi[3];
    u32 i;

    if (!w || !w->targets || !pos || mask == 0)
        return 0;

    /* The vertical half-extent is 285, not 286 (0x80050CF8 / 0x80050D28). */
    lo[0] = pos[0] - Q2_CONTENTS_HALF_XZ;
    lo[1] = pos[1] - Q2_CONTENTS_HALF_Y;
    lo[2] = pos[2] - Q2_CONTENTS_HALF_XZ;
    hi[0] = pos[0] + Q2_CONTENTS_HALF_XZ;
    hi[1] = pos[1] + Q2_CONTENTS_HALF_Y;
    hi[2] = pos[2] + Q2_CONTENTS_HALF_XZ;

    for (i = 0; i < w->count; i++) {
        const q2_move_target *t = &w->targets[i];

        if (t->kind != Q2_MOVE_KIND_VOLUME)
            continue;
        if (!(t->mask & mask))
            continue;
        if (!q2_move_box_overlap(lo, hi, t->min, t->max))
            continue;

        return t->mask;     /* 0x80050F78 returns the volume's own flags */
    }

    return 0;
}

/* 0x80045888 — 216 is in the branch's DELAY SLOT, so it is the default. */
s32 q2_move_step_height(u32 flags)
{
    return (flags & Q2_ENT_LOW_STEP) ? Q2_STEP_HEIGHT_LOW : Q2_STEP_HEIGHT;
}

/*
 * 0x800454F0..0x80045628. The projection of `d` onto `n`, in world units:
 * a 64-bit product, rounded by adding 2^23, then shifted right by 24 — the
 * normal is 1.3.12 and `d` already carries one factor of 4096, so 2^24 is the
 * right divisor and the addend is a true round-to-nearest.
 */
static s16 project(s16 n, s32 d)
{
    s64 p = (s64)n * (s64)d + (s64)0x800000;

    /* An arithmetic shift on a signed value is what the console does; spell it
     * out so a compiler that defines >> as logical cannot change the answer. */
    if (p < 0)
        return (s16)(-(s32)((-p + 0xFFFFFF) >> 24));

    return (s16)(s32)(p >> 24);
}

/* ------------------------------------------------------------------------- */
/* 0x80045144 — move with sliding                                             */
/* ------------------------------------------------------------------------- */
int q2_move(q2_collision *coll, q2_move_ent *ent, const s16 delta_in[3],
            const q2_move_mode *mode)
{
    s16 delta[3];
    s32 iterations;
    int result = 1;
    int i;

    if (!coll || !ent || !delta_in || !mode)
        return 1;

    delta[0] = delta_in[0];
    delta[1] = delta_in[1];
    delta[2] = delta_in[2];

    iterations = mode->iterations;

    for (;;) {
        s32  want[3];
        s32  dest[3];
        s32  end_pos[3];
        s32  end_node = -1;
        s16  normal[3] = {0, 0, 0};
        s16  push[3]   = {0, 0, 0};
        bool have_normal = false;
        int  world_ok;
        int  ents_ok = 1;

        for (i = 0; i < 3; i++)
            want[i] = ent->pos[i] + (s32)delta[i];

        dest[0] = want[0]; dest[1] = want[1]; dest[2] = want[2];

        /*
         * 0x80045220: the entity sweep runs FIRST and only when a3.hi is set,
         * which on the player's path means only 0x800456B0's retry. When it
         * hits, its clipped position — not the requested one — is what the
         * world trace is then asked for.
         */
        if (mode->sweep_ents && mode->world) {
            q2_move_contact hit;

            ents_ok = q2_move_sweep_world(mode->world, ent->pos, want, &hit);

            if (!ents_ok) {
                dest[0] = hit.pos[0];
                dest[1] = hit.pos[1];
                dest[2] = hit.pos[2];

                normal[0] = hit.normal[0];
                normal[1] = hit.normal[1];
                normal[2] = hit.normal[2];
                have_normal = true;

                result = 0;

                /* 0x80045244: the same flattest-floor rule as the world half. */
                if (ent->ground_normal[1] < hit.normal[1]) {
                    ent->ground_normal[0] = hit.normal[0];
                    ent->ground_normal[1] = hit.normal[1];
                    ent->ground_normal[2] = hit.normal[2];
                }

                /* 0x800452A4: standing on an entity is its own flag, with the
                 * thing being stood on named in bits 18..23. */
                if (mode->ground_mode && ent->max_slope_ny < hit.normal[1]) {
                    if (hit.kind == Q2_MOVE_KIND_ENTITY) {
                        ent->flags |= Q2_ENT_ON_ENTITY;
                        ent->flags &= ~Q2_ENT_STANDON_MASK;
                        ent->flags |= ((u32)hit.id & 0x3Fu) << Q2_ENT_STANDON_SHIFT;
                    } else {
                        ent->flags |= Q2_ENT_ON_GROUND;
                    }
                }
            }
        }

        world_ok = q2_coll_move(coll, ent->pos, dest, ent->node,
                                end_pos, &end_node) ? 1 : 0;

        if (!world_ok && coll->hit_plane_index >= 0) {
            q2_coll_plane pl;

            if (q2_collision_get_plane(coll, (u32)coll->hit_plane_index, &pl)) {
                normal[0] = pl.nx;
                normal[1] = pl.ny;
                normal[2] = pl.nz;
                have_normal = true;

                /* 0x80045318: keep the flattest floor seen this frame. +Y is
                 * down, so "flattest floor" is the LARGEST ny. */
                if (ent->ground_normal[1] < pl.ny) {
                    ent->ground_normal[0] = pl.nx;
                    ent->ground_normal[1] = pl.ny;
                    ent->ground_normal[2] = pl.nz;
                }

                /* 0x8004534C: and only the ground-mode move may declare it. */
                if (mode->ground_mode && ent->max_slope_ny < pl.ny)
                    ent->flags |= Q2_ENT_ON_GROUND;
            }
        }

        /* 0x80045374: the cached cell and the position come back from the
         * trace whether it completed or not. */
        ent->node   = end_node;
        ent->pos[0] = end_pos[0];
        ent->pos[1] = end_pos[1];
        ent->pos[2] = end_pos[2];

        /* 0x800453A0: `world_ok & ents_ok` — the move is finished only when
         * BOTH halves let it through. */
        if (world_ok && ents_ok)
            return result;

        result = 0;

        /* 0x800453AC: a second slot, keyed on |ny| instead, so a ceiling
         * counts here and cannot count as ground above. */
        if (have_normal) {
            s32 a = ent->last_normal[1] < 0 ? -(s32)ent->last_normal[1]
                                            :  (s32)ent->last_normal[1];
            s32 b = normal[1] < 0 ? -(s32)normal[1] : (s32)normal[1];

            if (a < b) {
                ent->last_normal[0] = normal[0];
                ent->last_normal[1] = normal[1];
                ent->last_normal[2] = normal[2];
            }
        }

        /* 0x800453F8: back off one unit along the dominant axis. */
        if (mode->push_mode && have_normal) {
            q2_move_push_vector(normal, push);
            for (i = 0; i < 3; i++)
                ent->pos[i] -= (s32)push[i];
        }

        /* 0x80045444: out of attempts. */
        if (iterations == 0)
            return result;

        /* 0x8004545C: what is left of the move, and the slide. */
        {
            s16 rem[3];
            s32 d;

            for (i = 0; i < 3; i++)
                rem[i] = sub16(want[i], ent->pos[i]);

            d = (s32)normal[0] * rem[0]
              + (s32)normal[1] * rem[1]
              + (s32)normal[2] * rem[2];

            for (i = 0; i < 3; i++)
                delta[i] = (s16)(rem[i] - project(normal[i], d));

            if (mode->push_mode) {
                for (i = 0; i < 3; i++)
                    delta[i] = (s16)(delta[i] - push[i]);
            }
        }

        iterations--;
    }
}

/* ------------------------------------------------------------------------- */
/* 0x800456B0 — move, and retry if the result is invalid                      */
/* ------------------------------------------------------------------------- */
int q2_move_checked(q2_collision *coll, q2_move_ent *ent, const s16 delta[3],
                    s32 iterations, bool ground_mode, bool push_mode,
                    q2_move_stuck_fn stuck_test, void *user,
                    const q2_move_world *world)
{
    q2_move_ent saved;
    q2_move_mode mode;
    int rc;

    if (!coll || !ent || !delta)
        return 1;

    /* 0x800456E4: position, cached cell and both contact normals are saved,
     * and nothing else — the flags word is patched rather than restored. */
    saved = *ent;

    mode.ground_mode = ground_mode;
    mode.push_mode   = push_mode;
    mode.iterations  = iterations;
    mode.sweep_ents  = false;
    mode.world       = world;

    rc = q2_move(coll, ent, delta, &mode);

    if (!stuck_test || !stuck_test(ent, user))
        return rc;

    /* 0x8004577C: put everything back, clear the two contact flags this move
     * may have set, and go again with entity sweeping on. */
    ent->pos[0] = saved.pos[0];
    ent->pos[1] = saved.pos[1];
    ent->pos[2] = saved.pos[2];
    ent->node   = saved.node;
    memcpy(ent->ground_normal, saved.ground_normal, sizeof(ent->ground_normal));
    memcpy(ent->last_normal,   saved.last_normal,   sizeof(ent->last_normal));
    ent->flags &= ~(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY);

    mode.sweep_ents = true;

    return q2_move(coll, ent, delta, &mode);
}

/* ------------------------------------------------------------------------- */
/* 0x8004583C — the stepped frame move                                        */
/* ------------------------------------------------------------------------- */
bool q2_move_step(q2_collision *coll, q2_move_ent *ent, const s16 delta[3],
                  const q2_move_world *world)
{
    s32 step;
    s32 saved_y;
    s32 drop;
    s16 v[3];
    bool grounded;

    if (!coll || !ent || !delta)
        return false;

    step    = q2_move_step_height(ent->flags);
    saved_y = ent->pos[1];

    /*
     * 0x80045ADC — the step sequence is for an entity that is ALREADY STANDING
     * on something and is not submerged. Everything else takes the simple slide
     * below.
     *
     * This is not an optimisation. The step sequence ends with a drop of a whole
     * step height, which is far larger than one tick of a jump — a jump produces
     * about 100 units of upward delta against a 216-unit step down — so running
     * it while airborne cancels the jump completely and the player never leaves
     * the floor. The flags are read here, before they are cleared.
     */
    grounded = (ent->flags & Q2_ENT_GROUNDED_MASK) != 0 &&
               !(ent->flags & Q2_ENT_UNDERWATER);

    /* 0x80045B14: both contact flags are cleared before the frame's moves, so
     * ground is decided fresh every frame. */
    ent->flags &= ~(Q2_ENT_ON_GROUND | Q2_ENT_ON_ENTITY);

    if (!grounded) {
        /*
         * 0x80045CA4 — airborne, or swimming. One slide with the frame delta,
         * three further attempts, and BOTH modes on: this move may declare
         * ground (which is how a fall lands) and it applies the unstick nudge.
         * Table entry 0x800AE93C = {1, 1}.
         */
        q2_move_checked(coll, ent, delta, 3, true, true, NULL, NULL, world);

        /*
         * 0x80045CD0 — if that did not land but the running ground normal says
         * a floor was touched, clear it and probe 20 units down to commit.
         * Delta from 0x800AE940, mode {1, 0}.
         */
        if (!(ent->flags & Q2_ENT_GROUNDED_MASK) &&
            ent->max_slope_ny < ent->ground_normal[1]) {
            ent->ground_normal[0] = ent->ground_normal[1] =
                ent->ground_normal[2] = 0;

            v[0] = 0; v[1] = 20; v[2] = 0;
            q2_move_checked(coll, ent, v, 0, true, false, NULL, NULL, world);
        }

        return (ent->flags & Q2_ENT_GROUNDED_MASK) != 0;
    }

    /* Up. -Y is up. Table entry 0x800AE930 = {0, 0}, zero further attempts. */
    v[0] = 0; v[1] = (s16)(-step); v[2] = 0;
    q2_move_checked(coll, ent, v, 0, false, false, NULL, NULL, world);

    /*
     * How far the LIFT actually got, measured before the slide runs.
     *
     * 0x80045B90 reads pos.y here and 0x80045BA0 subtracts it from the value
     * saved at 0x80045B68, which was read before the lift — so the drop is
     * sized by the lift ALONE and any vertical motion the slide contributes is
     * deliberately not counted. Measuring it after the slide instead (which is
     * the natural way to write this, and what this function used to do) makes
     * an entity sliding down a slope drop twice as far each frame and jitter.
     */
    drop = saved_y - ent->pos[1];

    /* Slide. 0x800AE934 = {0, 1}: nudge on, ground detection off, and three
     * further attempts — enough for a corner of two walls and a floor. */
    q2_move_checked(coll, ent, delta, 3, false, true, NULL, NULL, world);

    /*
     * 0x80045BC0..0x80045BD8 clears the ground normal before the down move, so
     * the value the drop leaves is this frame's and never last frame's. The
     * jump reads it, so a stale one lets you jump in mid-air.
     */
    ent->ground_normal[0] = ent->ground_normal[1] = ent->ground_normal[2] = 0;

    /* Down, by however far the lift managed plus the step. 0x800AE938 = {1, 0}:
     * this move, and only this move, may declare ground. */
    v[0] = 0; v[1] = (s16)(drop + step); v[2] = 0;
    q2_move_checked(coll, ent, v, 0, true, false, NULL, NULL, world);

    return (ent->flags & Q2_ENT_GROUNDED_MASK) != 0;
}
