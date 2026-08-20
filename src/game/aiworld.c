#include "aiworld.h"

#include "worldscale.h"

#include <string.h>

#include "aimove.h"

/*
 * How much of the requested move actually happened, as a 1.12 fraction.
 *
 * The collision walk reports where it stopped, not how far it got, so the
 * fraction is recovered from the distance travelled along the dominant axis.
 * That is the same axis the walk itself clips on, so the answer agrees with
 * the original's to within the rounding of one divide — and the AI only ever
 * compares it against 4096 or against another fraction from the same source.
 */
static s32 travelled_fraction(const s32 start[3], const s32 end[3],
                              const s32 hit[3])
{
    s32 axis = 0;
    s32 best = 0;
    int i;
    s64 want, got;

    for (i = 0; i < 3; i++) {
        s32 d = end[i] - start[i];
        if (d < 0)
            d = -d;
        if (d > best) {
            best = d;
            axis = i;
        }
    }

    if (best == 0)
        return Q2_TRACE_ONE;

    want = (s64)end[axis] - start[axis];
    got  = (s64)hit[axis] - start[axis];

    if (want < 0) { want = -want; got = -got; }
    if (got <= 0)
        return 0;
    if (got >= want)
        return Q2_TRACE_ONE;

    return (s32)((got * Q2_TRACE_ONE) / want);
}

/* Is the caller's box a real one? 0x8005BDA8..0x8005BE30 compares all six
 * components against the shared zero vector at 0x8009FBE4; anything non-zero
 * takes the eroded hull. A NULL pointer is the same as a zero box. */
static bool box_is_real(const s16 mins[3], const s16 maxs[3])
{
    int i;

    if (!mins || !maxs)
        return false;
    for (i = 0; i < 3; i++) {
        if (mins[i] || maxs[i])
            return true;
    }
    return false;
}

static void bound_trace(void *user, const s32 start[3], const s16 mins[3],
                        const s16 maxs[3], const s32 end[3],
                        const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    q2_collision *hull;
    s32 pos[3];
    s32 node = -1;

    /*
     * `ignore` is still dropped, and CREATURES AND BODIES still do not clip:
     * the port has no per-creature box list to hand this. `mask` is now read
     * for the one bit that matters — 0x02000000, which SV_movestep sets
     * (0x02020003 at 0x8005FE7C and 0x80060014) and which is what turns the
     * entity clip at 0x800544EC on. The entity list it clips against here is
     * the mover set: the doors and lifts, which are the entities a walker
     * actually has to be stopped by.
     */
    (void)ignore;

    memset(out, 0, sizeof(*out));

    if (b)
        b->stats.traces++;

    if (!b || !b->coll) {
        out->fraction  = Q2_TRACE_ONE;
        out->endpos[0] = end[0];
        out->endpos[1] = end[1];
        out->endpos[2] = end[2];
        return;
    }

    /*
     * THE HULL SELECT — 0x8005BD3C. A real box goes to SecondaryCol, which is
     * already eroded by the body's half-extent, so the swept point IS the box.
     * A degenerate one goes to PrimaryColl, the un-eroded query hull.
     */
    hull = b->coll;
    if (box_is_real(mins, maxs) && b->move_hull) {
        hull = b->move_hull;
        b->stats.trace_boxed++;
    }

    /*
     * `q2_coll_move` returns false when the move was STOPPED, not when it could
     * not begin — collision.h §0x80044C44 — and a stopped move is the normal,
     * useful answer for a walker's step trace, which exists precisely to be
     * stopped by the floor. `out_pos` is filled in either way; `out_node` is -1
     * only when the walk never found a cell at all, which is the real "started
     * outside the hull".
     *
     * Reading the false as start-solid is what made every creature in the game
     * stand still: `SV_movestep` bails on `allsolid` before it ever looks at
     * the fraction, so a creature standing on a floor was told it was buried in
     * one. Measured on BASE1, 1052 of 1052 traces took that arm.
     */
    if (!q2_coll_move(hull, start, end, -1, pos, &node) && node < 0) {
        /*
         * The eroded hull could not place the START. The original does not
         * retry — 0x8005BD3C hard-selects — but a creature whose body is
         * bigger than the erosion assumes (JAIL2's Gladiator misses the
         * SecondaryCol floor by two units) would otherwise freeze where it
         * previously walked. Fall back to the un-eroded hull and COUNT it, so
         * the placement failures stay visible instead of being papered over.
         */
        if (hull != b->coll &&
            (q2_coll_move(b->coll, start, end, -1, pos, &node) || node >= 0)) {
            b->stats.trace_fallback++;
        } else {
            b->stats.trace_unplaced++;
            out->fraction   = 0;
            out->startsolid = true;
            out->allsolid   = true;
            out->endpos[0]  = start[0];
            out->endpos[1]  = start[1];
            out->endpos[2]  = start[2];
            return;
        }
    }

    /*
     * AND THE DOORS — the entity arm of the same sweep.
     *
     * Clipped from the START to wherever the hull left the move, so the nearer
     * of the two wins with no fractions to compare. The creature's own box
     * inflates the mover boxes, which is the Minkowski sum that makes a swept
     * point stand for a swept body: without it a Gunner's centre would stop
     * flush against a door and half the Gunner would be inside it.
     *
     * The mask gate is the console's: a query that does not ask for entities
     * does not get them.
     */
    if (b->ents && (mask & Q2_MASK_ENTITY_BIT)) {
        q2_move_seg_hit mh;
        s32 body[3];
        int k;

        for (k = 0; k < 3; k++) {
            s32 lo = mins ? (s32)mins[k] : 0;
            s32 hi = maxs ? (s32)maxs[k] : 0;

            if (lo < 0) lo = -lo;
            if (hi < 0) hi = -hi;
            body[k] = lo > hi ? lo : hi;
        }

        if (q2_move_clip_segment(b->ents, start, pos, body, &mh)) {
            pos[0] = mh.pos[0];
            pos[1] = mh.pos[1];
            pos[2] = mh.pos[2];
            b->stats.trace_blocked_ent++;
        }
    }

    out->endpos[0] = pos[0];
    out->endpos[1] = pos[1];
    out->endpos[2] = pos[2];
    out->fraction  = travelled_fraction(start, end, pos);
    out->ent       = NULL;
    if (out->fraction >= Q2_TRACE_ONE)
        b->stats.trace_clear++;
}

static bool bound_los(void *user, const s32 a[3], const s32 b3[3])
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 pos[3];
    s32 node = -1;

    if (!b || !b->coll)
        return true;

    b->stats.los_calls++;

    if (!q2_coll_move(b->coll, a, b3, -1, pos, &node)) {
        b->stats.los_blocked++;
        return false;
    }

    /* Sight is all-or-nothing in the original: `visible` tests the fraction
     * against 1.0 and takes anything less as blocked. */
    if (pos[0] != b3[0] || pos[1] != b3[1] || pos[2] != b3[2]) {
        b->stats.los_blocked++;
        return false;
    }

    /*
     * AND THE DOORS. `visible` (0x8005B950) runs its sweep through the entity
     * list too, and a closed door is an entity — so a creature that could see
     * you through one was not being generous, it was asking the hull a
     * question the hull cannot answer.
     *
     * This is the line that fixes "creatures attack through doors": every
     * creature's fire hook gates each shot on `q2_visible`, so a sight line
     * that stops at the door stops the shooting with it. A POINT, not a box:
     * sight is the query the original hands a zero box, which is what sends it
     * to PrimaryColl in the first place.
     */
    if (b->ents) {
        q2_move_seg_hit mh;

        if (q2_move_clip_segment(b->ents, a, b3, NULL, &mh)) {
            b->stats.los_blocked++;
            b->stats.los_blocked_ent++;
            return false;
        }
    }

    return true;
}

/*
 * M_CheckBottom — 0x8005FB24, and it is FOUR corners, not one centre.
 *
 * 0x8005FB44..0x8005FC00 writes four stack triples,
 *
 *     (x - 143, y + 502, z - 143)   (x - 143, y + 502, z + 143)
 *     (x + 143, y + 502, z + 143)   (x + 143, y + 502, z - 143)
 *
 * and sweeps each of them FROM THE ENTITY ORIGIN — a0 = 0x800C8E90,
 * PrimaryColl, a1 = the entity, loop bound `slti v0, s2, 4` at 0x8005FC48. The
 * sweeps are DIAGONAL: the start stays at the centre and only the end moves to
 * the corner, so a wall between the two stops the sweep and counts as ground.
 * Four vertical drops at the corners would miss that, which is why the corner
 * offsets go on the END only.
 *
 * The two constants: 143 is half the body's 286 half-extent, and 502 is
 * Q2_EYE_BASE + Q2_STEPSIZE — one step below the FEET, measured from the
 * origin. That 502 is independent proof of the origin convention, since it
 * only lands on the floor if the origin is 286 above the feet.
 *
 * APPROXIMATION, stated as one: in the original a corner whose sweep COMPLETES
 * is then passed to 0x80053974, which clips it against the entity list, and
 * only that failing fails the function. This port has no entity list to give
 * it, so a completed corner is taken as no ground. That is stricter than the
 * console, not looser, and it is not the console's rule.
 */
#define AI_BOTTOM_CORNER  (Q2_EYE_BASE / 2)             /* 143 */

static bool bound_bottom(void *user, const q2_monster *m)
{
    static const s32 k_corner[4][2] = {
        { -AI_BOTTOM_CORNER, -AI_BOTTOM_CORNER },
        { -AI_BOTTOM_CORNER,  AI_BOTTOM_CORNER },
        {  AI_BOTTOM_CORNER,  AI_BOTTOM_CORNER },
        {  AI_BOTTOM_CORNER, -AI_BOTTOM_CORNER }
    };
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 start[3];
    int i;

    if (!b || !b->coll || !m)
        return true;

    b->stats.bottom_calls++;

    start[0] = m->pos[0];
    start[1] = m->pos[1];
    start[2] = m->pos[2];

    for (i = 0; i < 4; i++) {
        s32 end[3], pos[3];
        s32 node = -1;

        end[0] = start[0] + k_corner[i][0];
        /* Y points down, so probing for ground adds. From the ORIGIN, which is
         * why the reach is a step below the feet rather than a step. */
        end[1] = start[1] + Q2_EYE_BASE + b->bottom_reach;
        end[2] = start[2] + k_corner[i][1];

        /*
         * A probe that is STOPPED has found ground; one that runs the whole way
         * has found a drop. So the return of `q2_coll_move` is the answer,
         * inverted — the previous reading had it the right way round only by
         * accident, because it also treated "stopped" as failure and so
         * reported a drop under every creature standing on a floor.
         */
        if (!q2_coll_move(b->coll, start, end, -1, pos, &node)) {
            if (node < 0) {
                /* Never found a cell: the creature is outside the hull. */
                b->stats.bottom_fail++;
                return false;
            }
            continue;                   /* stopped short: this corner has ground */
        }

        /*
         * THE CORNER THAT REACHED THE END, and this is the approximation the
         * block above used to call out as unfixable.
         *
         * 0x8005FB24 does not fail on it either: it hands the completed sweep
         * to 0x80053974, the ENTITY clip, and only that finding nothing makes
         * it a ledge. The entity list the port can supply is the mover set, so
         * a creature standing on a LIFT — or in a doorway whose floor is the
         * door itself — is now standing on something instead of being told it
         * is at the edge of the world and refusing to walk.
         */
        if (b->ents) {
            q2_move_seg_hit mh;

            if (q2_move_clip_segment(b->ents, start, end, NULL, &mh)) {
                b->stats.bottom_on_ent++;
                continue;
            }
        }

        b->stats.bottom_fail++;
        return false;                   /* ran the whole way: a drop */
    }

    return true;
}

void q2_ai_world_bind_init(q2_ai_world_bind *bind, q2_collision *coll,
                           q2_collision *move_hull)
{
    if (!bind)
        return;

    memset(bind, 0, sizeof(*bind));

    bind->coll         = coll;
    bind->move_hull    = move_hull;
    bind->bottom_reach = Q2_STEPSIZE;

    bind->world.user          = bind;
    bind->world.trace         = bound_trace;
    bind->world.line_of_sight = bound_los;
    bind->world.check_bottom  = bound_bottom;
}

void q2_ai_world_bind_entities(q2_ai_world_bind *bind,
                               const q2_move_world *ents)
{
    if (!bind)
        return;

    /*
     * Stored as handed over, not tested for emptiness: `q2_sim_attach_movers`
     * REALLOCATES the target array and rewrites the count, so a binding that
     * decided at this moment that the world was empty would stay empty for the
     * rest of the level. The clip copes with a count of zero on its own.
     */
    bind->ents = ents;
}

void q2_ai_world_bind_install(q2_ai_world_bind *bind)
{
    if (!bind || !bind->coll) {
        q2_ai_set_world(NULL);
        return;
    }
    q2_ai_set_world(&bind->world);
}
