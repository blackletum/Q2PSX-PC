#include "aiworld.h"

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

static void bound_trace(void *user, const s32 start[3], const s16 mins[3],
                        const s16 maxs[3], const s32 end[3],
                        const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 pos[3];
    s32 node = -1;

    (void)mins; (void)maxs; (void)ignore; (void)mask;

    memset(out, 0, sizeof(*out));

    if (!b || !b->coll) {
        out->fraction  = Q2_TRACE_ONE;
        out->endpos[0] = end[0];
        out->endpos[1] = end[1];
        out->endpos[2] = end[2];
        return;
    }

    if (!q2_coll_move(b->coll, start, end, -1, pos, &node)) {
        /*
         * The walk could not even place the start point. Treat that as solid
         * rather than clear: a creature that has fallen out of the hull must
         * not be handed a free move through the level.
         */
        out->fraction   = 0;
        out->startsolid = true;
        out->allsolid   = true;
        out->endpos[0]  = start[0];
        out->endpos[1]  = start[1];
        out->endpos[2]  = start[2];
        return;
    }

    out->endpos[0] = pos[0];
    out->endpos[1] = pos[1];
    out->endpos[2] = pos[2];
    out->fraction  = travelled_fraction(start, end, pos);
    out->ent       = NULL;
}

static bool bound_los(void *user, const s32 a[3], const s32 b3[3])
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 pos[3];
    s32 node = -1;

    if (!b || !b->coll)
        return true;

    if (!q2_coll_move(b->coll, a, b3, -1, pos, &node))
        return false;

    /* Sight is all-or-nothing in the original: `visible` tests the fraction
     * against 1.0 and takes anything less as blocked. */
    return pos[0] == b3[0] && pos[1] == b3[1] && pos[2] == b3[2];
}

static bool bound_bottom(void *user, const q2_monster *m)
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 start[3], end[3], pos[3];
    s32 node = -1;

    if (!b || !b->coll || !m)
        return true;

    start[0] = m->pos[0];
    start[1] = m->pos[1];
    start[2] = m->pos[2];

    /* Y points down, so probing for ground adds. */
    end[0] = start[0];
    end[1] = start[1] + b->bottom_reach;
    end[2] = start[2];

    if (!q2_coll_move(b->coll, start, end, -1, pos, &node))
        return false;

    /* Ground is anything that stopped the probe short. A probe that ran the
     * whole way means the creature is over a drop. */
    return pos[1] < end[1];
}

void q2_ai_world_bind_init(q2_ai_world_bind *bind, q2_collision *coll)
{
    if (!bind)
        return;

    memset(bind, 0, sizeof(*bind));

    bind->coll         = coll;
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
