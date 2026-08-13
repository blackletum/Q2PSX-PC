#include "monster.h"

#include <string.h>

#include "trig.h"

const char *const q2_ai_verb_names[Q2_AI_VERB_COUNT] = {
    "none", "stand", "walk", "run", "charge", "move"
};

void q2_monster_init(q2_monster *m)
{
    if (!m)
        return;

    memset(m, 0, sizeof(*m));

    m->current_move = -1;
    m->enemy        = -1;
    m->class_id     = -1;
    m->speed_scale  = 10;      /* neutral: the frame scale is /10 */
}

s32 q2_monster_frame_dist(const q2_monster *m, const q2_mframe *frame)
{
    if (!m || !frame)
        return 0;

    /* A held frame plays its animation without advancing the creature, which
     * is how a wind-up or a recoil stays in place. */
    if (m->aiflags & Q2_AI_HOLD_FRAME)
        return 0;

    return ((s32)frame->dist * (s32)m->speed_scale * 12) / 10;
}

bool q2_monster_infront(const q2_monster *m, const s32 target[3])
{
    s32 dx, dz;
    s32 fx, fz;
    s64 dot;
    s64 len;

    if (!m || !target)
        return false;

    /* Yaw-only, on the horizontal plane: the creature's facing has no pitch. */
    fx = q2_sin12(m->angles[1]);
    fz = q2_cos12(m->angles[1]);

    dx = target[0] - m->pos[0];
    dz = target[2] - m->pos[2];

    /*
     * The facing vector is unit length in 1.3.12, so |f| == 4096 and
     *
     *     cos = dot / (4096 * |d|)
     *
     * The test cos > 1230/4096 therefore reduces to dot > 1230 * |d|, and
     * squaring both sides removes the square root entirely. Squaring is only
     * valid once dot is known positive, which the early return guarantees.
     */
    dot = (s64)fx * dx + (s64)fz * dz;
    if (dot <= 0)
        return false;

    len = (s64)dx * dx + (s64)dz * dz;
    if (len == 0)
        return true;          /* standing on top of it */

    return dot * dot > (s64)Q2_INFRONT_DOT * Q2_INFRONT_DOT * len;
}

s64 q2_monster_dist_sq(const q2_monster *m, const s32 target[3])
{
    s64 dx, dy, dz;

    if (!m || !target)
        return 0;

    dx = (s64)target[0] - m->pos[0];
    dy = (s64)target[1] - m->pos[1];
    dz = (s64)target[2] - m->pos[2];

    return dx * dx + dy * dy + dz * dz;
}

bool q2_monster_damage(q2_monster *m, s16 amount)
{
    if (!m || amount <= 0 || !m->in_use || m->dead)
        return false;

    m->health = (s16)(m->health - amount);

    if (m->health > 0)
        return false;

    m->dead = true;

    /* Below the gib threshold the body is destroyed outright rather than
     * playing a death animation. The threshold is negative and per-creature —
     * 240 health with -60 gib on one, 149 with -70 on another. */
    return true;
}

/* ------------------------------------------------------------------------- */
bool q2_mmove_read(const u8 *image, size_t size, u32 offset, q2_mmove *out)
{
    if (!image || !out)
        return false;
    if ((size_t)offset + Q2_MMOVE_SIZE > size)
        return false;

    out->first_frame    = q2_rd_s32(image + offset + 0);
    out->last_frame     = q2_rd_s32(image + offset + 4);
    out->frames_offset  = q2_rd_u32(image + offset + 8);
    out->endfunc_offset = q2_rd_u32(image + offset + 12);

    /* A move must cover at least one frame and run forwards. */
    if (out->last_frame < out->first_frame)
        return false;

    return true;
}

bool q2_mframe_read(const u8 *image, size_t size, u32 offset, q2_mframe *out)
{
    if (!image || !out)
        return false;

    /* Three bytes, unpadded. Stepping by four walks off the end of every
     * animation on the disc. */
    if ((size_t)offset + Q2_MFRAME_SIZE > size)
        return false;

    out->ai    = image[offset + 0];
    out->dist  = (s8)image[offset + 1];
    out->think = image[offset + 2];

    return true;
}
