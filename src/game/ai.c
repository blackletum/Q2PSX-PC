#include "ai.h"

#include <stdlib.h>
#include <string.h>

#include "trig.h"

q2_range_band q2_ai_range(const q2_monster *m, const s32 target[3])
{
    s64 d2;

    if (!m || !target)
        return Q2_RANGE_FAR;

    d2 = q2_monster_dist_sq(m, target);

    if (d2 <= (s64)Q2_RANGE_MELEE_UNITS * Q2_RANGE_MELEE_UNITS)
        return Q2_RANGE_MELEE;
    if (d2 <= (s64)Q2_RANGE_NEAR_UNITS * Q2_RANGE_NEAR_UNITS)
        return Q2_RANGE_NEAR;
    if (d2 <= (s64)Q2_RANGE_MID_UNITS * Q2_RANGE_MID_UNITS)
        return Q2_RANGE_MID;

    return Q2_RANGE_FAR;
}

void q2_ai_face(q2_monster *m, const s32 target[3])
{
    s32 dx, dz;

    if (!m || !target)
        return;

    dx = target[0] - m->pos[0];
    dz = target[2] - m->pos[2];

    if (dx == 0 && dz == 0)
        return;

    /*
     * Yaw is measured so that 0 faces +Z and a quarter turn faces +X, matching
     * the convention the camera and the movement code already use. Deriving it
     * by search over the sine table keeps everything in fixed point and avoids
     * pulling in atan2 for the one place it would be needed.
     */
    {
        s32 best = 0;
        s64 best_dot = INT64_MIN;
        s32 a;

        for (a = 0; a < Q2_ANGLE_360; a += 16) {
            s32 sx = q2_sin12(a);
            s32 sz = q2_cos12(a);
            s64 dot = (s64)sx * dx + (s64)sz * dz;

            if (dot > best_dot) {
                best_dot = dot;
                best = a;
            }
        }

        /* Refine within the coarse step so the facing is not visibly quantised. */
        {
            s32 a2;
            for (a2 = best - 16; a2 <= best + 16; a2++) {
                s32 sx = q2_sin12(a2);
                s32 sz = q2_cos12(a2);
                s64 dot = (s64)sx * dx + (s64)sz * dz;
                if (dot > best_dot) {
                    best_dot = dot;
                    best = a2;
                }
            }
        }

        m->ideal_yaw = best;
    }
}

bool q2_ai_turn_toward(q2_monster *m, s32 max_step)
{
    s32 delta;

    if (!m || max_step <= 0)
        return false;

    /* Take the short way round: a creature that turns the long way looks like
     * it is confused rather than tracking. */
    delta = m->ideal_yaw - m->angles[1];
    while (delta > Q2_ANGLE_180)  delta -= Q2_ANGLE_360;
    while (delta < -Q2_ANGLE_180) delta += Q2_ANGLE_360;

    if (delta > max_step)       delta = max_step;
    else if (delta < -max_step) delta = -max_step;
    else {
        m->angles[1] = m->ideal_yaw;
        return true;
    }

    m->angles[1] += delta;

    while (m->angles[1] >= Q2_ANGLE_360) m->angles[1] -= Q2_ANGLE_360;
    while (m->angles[1] < 0)             m->angles[1] += Q2_ANGLE_360;

    return false;
}

bool q2_ai_find_target(q2_monster *m, const s32 target[3], s32 index)
{
    s64 d2;

    if (!m || !target || !m->in_use || m->dead)
        return false;

    if (m->enemy >= 0)
        return false;               /* already has one */

    d2 = q2_monster_dist_sq(m, target);
    if (d2 > (s64)Q2_SIGHT_UNITS * Q2_SIGHT_UNITS)
        return false;

    /*
     * No line-of-sight trace yet, so a creature can notice the player through a
     * wall. That is a known gap rather than an oversight — see ai.h.
     */
    if (!q2_monster_infront(m, target))
        return false;

    m->enemy = index;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Movement verbs                                                             */
/* ------------------------------------------------------------------------- */
static void step_forward(q2_monster *m, s32 dist)
{
    s32 sx, sz;

    if (dist == 0)
        return;

    sx = q2_sin12(m->angles[1]);
    sz = q2_cos12(m->angles[1]);

    m->pos[0] += (s32)(((s64)sx * dist) >> Q2_FRAC_12);
    m->pos[2] += (s32)(((s64)sz * dist) >> Q2_FRAC_12);
}

void q2_ai_stand(q2_monster *m, s32 dist)
{
    /* Standing still still advances if the frame says so — some idle
     * animations drift. */
    if (m)
        step_forward(m, dist);
}

void q2_ai_walk(q2_monster *m, s32 dist)
{
    if (!m)
        return;
    q2_ai_turn_toward(m, 40);
    step_forward(m, dist);
}

void q2_ai_run(q2_monster *m, s32 dist, const s32 target[3])
{
    if (!m)
        return;

    if (target)
        q2_ai_face(m, target);

    /* Running turns faster than walking, so a creature can track a moving
     * player rather than orbiting it. */
    q2_ai_turn_toward(m, 80);
    step_forward(m, dist);
}

void q2_ai_charge(q2_monster *m, s32 dist, const s32 target[3])
{
    if (!m)
        return;

    /* Charging keeps facing the target but does not re-path: it commits to the
     * attack. */
    if (target)
        q2_ai_face(m, target);

    q2_ai_turn_toward(m, 120);
    step_forward(m, dist);
}

void q2_ai_move(q2_monster *m, s32 dist)
{
    /* Pure animation-driven movement, no turning: used by pain and death. */
    if (m)
        step_forward(m, dist);
}

/* ------------------------------------------------------------------------- */
q2_ai_action q2_ai_think(q2_monster *m, const s32 target[3], s32 target_index,
                         s32 now)
{
    q2_range_band band;
    bool just_sighted = false;

    if (!m || !m->in_use || m->dead)
        return Q2_AI_ACT_NONE;

    /* Dead creatures and those still in their refire delay do nothing, but the
     * refire check must not block target acquisition or a monster shot from
     * behind would never turn around. */
    if (m->enemy < 0) {
        if (q2_ai_find_target(m, target, target_index))
            just_sighted = true;
        else {
            q2_ai_stand(m, 0);
            return Q2_AI_ACT_NONE;
        }
    }

    band = q2_ai_range(m, target);

    /* Lose a target that has gone far outside sight range, so a creature does
     * not chase across the whole level forever. */
    if (band == Q2_RANGE_FAR) {
        s64 d2 = q2_monster_dist_sq(m, target);
        if (d2 > (s64)Q2_SIGHT_UNITS * Q2_SIGHT_UNITS * 2) {
            m->enemy = -1;
            return Q2_AI_ACT_NONE;
        }
    }

    if (just_sighted)
        return Q2_AI_ACT_SIGHTED;

    switch (band) {
    case Q2_RANGE_MELEE:
        /* Close enough to strike. Face the target and attack on the refire
         * cadence rather than every tick. */
        q2_ai_face(m, target);
        q2_ai_turn_toward(m, 120);

        if (now >= m->attack_finished) {
            m->attack_finished = now + 10;   /* one second at the AI clock */
            m->attack_state    = Q2_AS_MELEE;
            return Q2_AI_ACT_MELEE;
        }
        return Q2_AI_ACT_NONE;

    case Q2_RANGE_NEAR:
    case Q2_RANGE_MID:
        /* Close the distance. The step is a plain per-tick advance because the
         * per-frame distances live in the creature's own animation data, which
         * is not wired to the state machine yet. */
        q2_ai_run(m, 120, target);

        if (band == Q2_RANGE_MID && now >= m->attack_finished) {
            m->attack_finished = now + 15;
            m->attack_state    = Q2_AS_MISSILE;
            return Q2_AI_ACT_ATTACK;
        }
        return Q2_AI_ACT_NONE;

    case Q2_RANGE_FAR:
    default:
        q2_ai_walk(m, 60);
        return Q2_AI_ACT_NONE;
    }
}
