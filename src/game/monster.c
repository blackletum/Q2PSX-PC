#include "monster.h"

#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "aimove.h"
#include "trig.h"

const char *const q2_ai_verb_names[Q2_AI_VERB_COUNT] = {
    "none", "stand", "walk", "run", "charge", "move"
};

q2_level q2_level_state;

void q2_level_reset(void)
{
    memset(&q2_level_state, 0, sizeof(q2_level_state));
}

void q2_monster_init(q2_monster *m)
{
    if (!m)
        return;

    memset(m, 0, sizeof(*m));

    m->class_id    = 0;
    m->speed_scale = 10;      /* neutral: the frame scale is /10 */
    m->yaw_speed   = 200;     /* a shade under 18 degrees a tick */

    /* The movement hull the loader fills from SecondaryCol. A creature with a
     * zero box would pass through everything, so give it the player's. */
    m->mins[0] = m->mins[2] = -286;
    m->maxs[0] = m->maxs[2] =  286;
    m->mins[1] = -560;        /* Y points down, so mins is above  */
    m->maxs[1] =    0;
    m->view_height = -400;
}

s32 q2_monster_frame_dist(const q2_monster *m, const q2_mframe *frame)
{
    if (!m || !frame)
        return 0;

    /* A held frame plays its animation without advancing the creature, which
     * is how a wind-up or a recoil stays in place. 0x80061924. */
    if (m->aiflags & Q2_AI_HOLD_FRAME)
        return 0;

    return ((s32)frame->dist * (s32)m->speed_scale * 12) / 10;
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

/*
 * range() — 0x8005D560..0x8005D5C4, and again identically inside
 * ai_checkattack at 0x8005E194.
 *
 * The comparison is against the SQUARED distance, which is why the melee band
 * is a per-class number: one class id gets double reach and everything else
 * shares the short one.
 */
q2_range_band q2_range(const q2_monster *self, const q2_monster *other)
{
    s64 d2;
    s64 melee;

    if (!self || !other)
        return Q2_RANGE_FAR;

    d2 = q2_monster_dist_sq(self, other->pos);

    melee = (self->class_id == Q2_CLASS_LONG_MELEE)
          ? (s64)Q2_MELEE_DISTANCE_BIG * Q2_MELEE_DISTANCE_BIG
          : (s64)Q2_MELEE_DISTANCE * Q2_MELEE_DISTANCE;

    if (d2 <= melee - 1)
        return Q2_RANGE_MELEE;
    if (d2 <= (s64)Q2_RANGE_NEAR_DIST * Q2_RANGE_NEAR_DIST - 1)
        return Q2_RANGE_NEAR;
    if (d2 <= (s64)Q2_RANGE_MID_DIST * Q2_RANGE_MID_DIST - 1)
        return Q2_RANGE_MID;

    return Q2_RANGE_FAR;
}

/*
 * infront() — 0x8005D608..0x8005D774.
 *
 * AngleVectors, then normalise the offset, then a 1.12 dot product with
 * per-term rounding, compared against 1230. The original really does normalise
 * rather than compare squares, so this does too: the rounding of the
 * normalise is visible in the result at short range.
 */
bool q2_infront(const q2_monster *self, const q2_monster *other)
{
    s32 forward[3];
    s32 vec[3];
    s32 dot;

    if (!self || !other)
        return false;

    q2_angle_vectors(self->angles, forward, NULL, NULL);

    vec[0] = other->pos[0] - self->pos[0];
    vec[1] = other->pos[1] - self->pos[1];
    vec[2] = other->pos[2] - self->pos[2];

    q2_vector_normalize(vec);

    dot = (s32)(((s64)vec[0] * forward[0] + 4095) >> 12)
        + (s32)(((s64)vec[1] * forward[1] + 4095) >> 12)
        + (s32)(((s64)vec[2] * forward[2] + 4095) >> 12);

    return dot >= Q2_INFRONT_DOT;
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
/* The class method table                                                     */
/* ------------------------------------------------------------------------- */
/*
 * 0x80061D10 fills 256 slots with one shared inert table and lets a module
 * overwrite individual methods through 0x80061DE4. The original can do that
 * because every class's table lives in one fixed arena; here the tables are
 * allocated on first write, which is invisible from the outside — an
 * unregistered class still answers NULL for every method, and the frame
 * driver's `if (fn)` guard is what makes that inert rather than fatal.
 */
static q2_class_method *g_class_methods[Q2_CLASS_COUNT];

void q2_class_table_reset(void)
{
    u32 i;
    for (i = 0; i < Q2_CLASS_COUNT; i++) {
        free(g_class_methods[i]);
        g_class_methods[i] = NULL;
    }
}

void q2_class_method_set(u32 class_id, u32 index, q2_class_method fn)
{
    if (class_id >= Q2_CLASS_COUNT || index >= Q2_CLASS_METHOD_COUNT)
        return;

    if (!g_class_methods[class_id]) {
        g_class_methods[class_id] =
            (q2_class_method *)calloc(Q2_CLASS_METHOD_COUNT,
                                      sizeof(q2_class_method));
        if (!g_class_methods[class_id])
            return;
    }

    g_class_methods[class_id][index] = fn;
}

q2_class_method q2_class_method_get(u32 class_id, u32 index)
{
    if (class_id >= Q2_CLASS_COUNT || index >= Q2_CLASS_METHOD_COUNT)
        return NULL;
    if (!g_class_methods[class_id])
        return NULL;
    return g_class_methods[class_id][index];
}

void q2_class_think_set(u32 class_id, u32 think_index, q2_class_method fn)
{
    if (think_index >= Q2_CLASS_VERB_BASE)
        return;
    q2_class_method_set(class_id, think_index, fn);
}

void q2_class_verb_set(u32 class_id, u32 verb_index, q2_class_method fn)
{
    q2_class_method_set(class_id, Q2_CLASS_VERB_BASE + verb_index, fn);
}

/* ------------------------------------------------------------------------- */
/* M_MoveFrame — 0x8006175C                                                   */
/* ------------------------------------------------------------------------- */
void q2_M_MoveFrame(q2_monster *m)
{
    const q2_mmove *move;
    const q2_mframe *frame;
    s32 index;

    if (!m)
        return;

    move = m->currentmove;
    if (!move || !move->frames)
        return;

    /* The think re-arms itself every tick. Note this happens BEFORE anything
     * else, so a move that frees the entity still leaves a sane nextthink. */
    m->next_think = q2_level_state.framenum + 1;

    if (m->nextframe
        && (s32)m->nextframe >= move->first_frame
        && (s32)m->nextframe <= move->last_frame) {
        /* A think function asked for a specific frame; honour it and clear the
         * request rather than advancing. */
        m->frame     = (s16)m->nextframe;
        m->nextframe = 0;
    } else {
        if (m->frame == move->last_frame) {
            if (move->endfunc) {
                move->endfunc(m);

                /* The callback may have freed the entity — 0x800617F8 tests
                 * bit 1 of the svflags word and bails. */
                if (m->svflags & 0x2)
                    return;

                move = m->currentmove;
                if (!move || !move->frames)
                    return;
            }
        }

        if (m->frame < move->first_frame || m->frame > move->last_frame) {
            /* Landed outside the move, which is what happens the tick after a
             * new move is installed. Snap to its first frame and drop the hold,
             * because a hold belongs to the move that set it. */
            m->aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
            m->frame = (s16)move->first_frame;
        } else if (!(m->aiflags & Q2_AI_HOLD_FRAME)) {
            m->frame++;
            if (m->frame > move->last_frame)
                m->frame = (s16)move->first_frame;
        }
    }

    index = m->frame - move->first_frame;
    if (index < 0 || index > move->last_frame - move->first_frame)
        return;

    frame = &move->frames[index];

    if (frame->ai & Q2_AI_LOCAL_FLAG) {
        /* The creature's own verb. It gets no distance: a creature that steers
         * itself does not want the frame's. 0x800618B8. */
        q2_class_method fn =
            q2_class_method_get(m->class_id,
                                Q2_CLASS_VERB_BASE + (frame->ai & 0x7Fu));
        if (fn)
            fn(m);
    } else if (frame->ai < 8) {
        q2_ai_verb_fn fn = q2_ai_verbs[frame->ai];
        if (fn)
            fn(m, q2_monster_frame_dist(m, frame));
    }

    if (frame->think) {
        q2_class_method fn = q2_class_method_get(m->class_id, frame->think);
        if (fn)
            fn(m);
    }
}

/* ------------------------------------------------------------------------- */
/* monster_start_go — 0x80061BA4                                              */
/* ------------------------------------------------------------------------- */
/*
 * The dead-monster pause is 1e9 ticks rather than id's 1e8, because the clock
 * is ten times faster. It is not "forever" in either engine, just longer than
 * any level lasts, and reproducing the exact number matters for a save that
 * round-trips.
 */
#define Q2_PAUSE_FOREVER 1000000000

/* The class id the loader gives a path corner. Read as the equality at
 * 0x80061C10. */
#define Q2_CLASS_PATH_CORNER 114

/* Resolved by targetname; the port hands the resolver in rather than owning a
 * script namespace. */
static q2_monster *(*g_pick_target)(s16 targetname, void *user);
static void *g_pick_target_user;

void q2_ai_set_pick_target(q2_monster *(*fn)(s16, void *), void *user)
{
    g_pick_target      = fn;
    g_pick_target_user = user;
}

q2_monster *q2_pick_target(s16 targetname)
{
    if (!g_pick_target || targetname <= 0)
        return NULL;
    return g_pick_target(targetname, g_pick_target_user);
}

void q2_monster_start_go(q2_monster *m)
{
    if (!m || m->health <= 0)
        return;

    if (m->target > 0) {
        q2_monster *t = q2_pick_target(m->target);

        m->movetarget = t;
        m->goalentity = t;

        if (!t) {
            /* A target that does not resolve leaves the creature standing
             * where it was placed rather than walking to the world origin. */
            m->target    = 0;
            m->pausetime = Q2_PAUSE_FOREVER;
            if (m->stand)
                m->stand(m);
        } else if (t->class_id == Q2_CLASS_PATH_CORNER) {
            s32 v[3];
            q2_link_entity(m, 1);
            v[0] = t->pos[0] - m->pos[0];
            v[1] = t->pos[1] - m->pos[1];
            v[2] = t->pos[2] - m->pos[2];
            m->ideal_yaw  = q2_vectoyaw(v);
            m->angles[2]  = m->ideal_yaw;
            q2_link_entity(m, 4);
            if (m->walk)
                m->walk(m);
            m->target = 0;
        } else {
            /* Targeting something that is not a path corner is a level-design
             * mistake in the original too; it stands rather than pathing. */
            m->movetarget = NULL;
            m->goalentity = NULL;
            m->pausetime  = Q2_PAUSE_FOREVER;
            if (m->stand)
                m->stand(m);
        }
    } else {
        m->ideal_yaw = m->angles[2];
        q2_link_entity(m, 4);
        m->pausetime = Q2_PAUSE_FOREVER;
        if (m->stand)
            m->stand(m);
    }

    m->think      = q2_M_MoveFrame;
    m->next_think = q2_level_state.time + 1;
}

/* ------------------------------------------------------------------------- */
bool q2_mmove_read(const u8 *image, size_t size, u32 offset, q2_mmove *out)
{
    if (!image || !out)
        return false;
    if ((size_t)offset + Q2_MMOVE_SIZE > size)
        return false;

    memset(out, 0, sizeof(*out));

    out->image_offset   = offset;
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
