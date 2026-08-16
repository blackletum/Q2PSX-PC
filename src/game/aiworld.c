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
     * `ignore` and `mask` are still dropped, and that is a real gap rather
     * than an oversight: when `mask & 0x02000000` — and SV_movestep always
     * passes 0x02020003, at 0x8005FE7C and 0x80060014 — the original clips the
     * same sweep against the entity list at 0x800544EC and reports what it hit
     * in the result's +52/+56. This port has no entity list to clip against
     * here, so creatures walk through movers, bodies and each other.
     */
    (void)ignore; (void)mask;

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
    if (pos[0] == b3[0] && pos[1] == b3[1] && pos[2] == b3[2])
        return true;
    b->stats.los_blocked++;
    return false;
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

void q2_ai_world_bind_install(q2_ai_world_bind *bind)
{
    if (!bind || !bind->coll) {
        q2_ai_set_world(NULL);
        return;
    }
    q2_ai_set_world(&bind->world);
}
