#include "mover.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Building movers from the script                                            */
/* ------------------------------------------------------------------------- */
static q2_mover *mover_push(q2_mover_set *set)
{
    if (set->count >= set->capacity) {
        u32 want = set->capacity ? set->capacity * 2 : 32;
        q2_mover *bigger = (q2_mover *)realloc(set->movers, want * sizeof(q2_mover));
        if (!bigger)
            return NULL;
        set->movers   = bigger;
        set->capacity = want;
    }

    {
        q2_mover *m = &set->movers[set->count++];
        memset(m, 0, sizeof(*m));
        m->portal_node = -1;
        m->partner     = -1;
        m->wait_timer  = Q2_MOVER_WAIT_NEVER;
        return m;
    }
}

/* Copy up to four Scene node indices, dropping the -1 holes. */
static void collect_nodes(q2_mover *m, const u8 *payload, u32 at)
{
    int i;

    m->part_count = 0;
    for (i = 0; i < Q2_MOVER_MAX_PARTS; i++) {
        s16 n = q2_rd_s16(payload + at + (u32)i * 2);
        if (n < 0)
            continue;
        m->node[m->part_count++] = n;
    }
}

q2_result q2_movers_build(q2_mover_set *out, const q2_events *events)
{
    q2_event_record rec;

    if (!out || !events)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (events->record_count == 0)
        return Q2_OK;

    if (!q2_events_first_record(events, &rec))
        return Q2_OK;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            const u8 *p;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;

            /* item.payload points past the two header bytes, so a documented
             * offset of +N is payload[N - 2]. Keep the disc offsets in the
             * code and subtract once, rather than pre-subtracting and losing
             * the correspondence with the header. */
            p = item.payload - 2;

            switch (item.opcode) {
            case Q2_EVOP_MOVER_A: {
                q2_mover *m;
                if (item.len < 24)
                    break;
                m = mover_push(out);
                if (!m)
                    return Q2_ERR_NO_MEMORY;

                m->axis        = 1;                          /* hard-wired */
                m->target      = (s16)-q2_rd_s16(p + 2);
                m->speed       = (s16)abs(q2_rd_s16(p + 4));
                m->portal_node = q2_rd_s16(p + 6);
                collect_nodes(m, p, 8);
                m->key_mask    = q2_rd_u16(p + 16);
                m->delay_timer = (u16)(p[18] * Q2_MOVER_TIMEBASE);
                m->wait_timer  = (p[19] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[19] * Q2_MOVER_TIMEBASE);
                m->touch_opens = q2_rd_s16(p + 20) != 0;
                /* A opens through obstructions but not closes. */
                m->block_flags = Q2_MV_BLK_IGNORE_OPENING;
                break;
            }

            case Q2_EVOP_MOVER_B: {
                q2_mover *m;
                s16 axis_field;
                if (item.len < 24)
                    break;
                m = mover_push(out);
                if (!m)
                    return Q2_ERR_NO_MEMORY;

                axis_field     = q2_rd_s16(p + 8);
                m->axis        = (u8)(axis_field & 3);
                m->target      = (axis_field == 0)
                                 ? q2_rd_s16(p + 2)
                                 : (s16)-q2_rd_s16(p + 2);
                m->speed       = (s16)abs(q2_rd_s16(p + 4));
                m->portal_node = q2_rd_s16(p + 6);
                collect_nodes(m, p, 10);
                m->key_mask    = q2_rd_u16(p + 18);
                m->delay_timer = (u16)(p[20] * Q2_MOVER_TIMEBASE);
                m->wait_timer  = (p[21] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[21] * Q2_MOVER_TIMEBASE);
                break;
            }

            case Q2_EVOP_MOVER_C: {
                q2_mover *leaf0, *leaf1;
                s16 travel, speed, portal, axis_field;
                u16 keys;
                u8  delay, wait;
                u32 i0;

                if (item.len < 32)
                    break;

                travel     = q2_rd_s16(p + 2);
                speed      = (s16)abs(q2_rd_s16(p + 4));
                portal     = q2_rd_s16(p + 6);
                axis_field = q2_rd_s16(p + 8);
                keys       = q2_rd_u16(p + 26);
                delay      = p[28];
                wait       = p[29];

                leaf0 = mover_push(out);
                if (!leaf0)
                    return Q2_ERR_NO_MEMORY;
                i0 = out->count - 1;

                leaf0->axis        = (u8)(axis_field & 3);
                leaf0->target      = travel;
                leaf0->speed       = speed;
                leaf0->portal_node = -1;      /* leaf 1 owns the portal */
                collect_nodes(leaf0, p, 10);
                leaf0->key_mask    = keys;
                leaf0->delay_timer = (u16)(delay * Q2_MOVER_TIMEBASE);
                leaf0->wait_timer  = (wait == 0xFF)
                                     ? Q2_MOVER_WAIT_NEVER
                                     : (u16)(wait * Q2_MOVER_TIMEBASE);

                leaf1 = mover_push(out);
                if (!leaf1)
                    return Q2_ERR_NO_MEMORY;

                /* mover_push may have reallocated, so re-take leaf0. */
                leaf0 = &out->movers[i0];

                leaf1->axis        = leaf0->axis;
                leaf1->target      = (s16)-travel;   /* opposite leaf */
                leaf1->speed       = speed;
                leaf1->portal_node = portal;
                collect_nodes(leaf1, p, 18);
                leaf1->key_mask    = keys;
                leaf1->delay_timer = leaf0->delay_timer;
                leaf1->wait_timer  = leaf0->wait_timer;
                leaf1->partner     = (s32)i0;
                leaf0->partner     = (s32)(out->count - 1);
                break;
            }

            default:
                break;
            }
        }
    } while (q2_events_next_record(events, &rec, &rec));

    return Q2_OK;
}

void q2_movers_free(q2_mover_set *set)
{
    if (!set)
        return;
    free(set->movers);
    memset(set, 0, sizeof(*set));
}

/* ------------------------------------------------------------------------- */
void q2_mover_trigger(q2_mover_set *set, u32 index)
{
    q2_mover *m;

    if (!set || index >= set->count)
        return;

    m = &set->movers[index];
    m->triggered = 1;

    /* A closing door reverses at once rather than waiting for the next tick;
     * anything not fully shut ignores the trigger. */
    if (m->state == Q2_MV_CLOSING)
        m->state = Q2_MV_OPENING;
}

/* ------------------------------------------------------------------------- */
/* The state machine                                                          */
/* ------------------------------------------------------------------------- */
static void mover_move(q2_mover_set *set, q2_mover *m, s32 dt, int dir)
{
    s32 old = m->offset;
    int settled = 0;

    if (dir) {
        if (m->target > 0) {
            m->offset += m->speed * dt;
            if (m->offset > m->target) {
                m->offset = m->target;
                m->state  = Q2_MV_ARRIVED;
            }
        } else {
            m->offset -= m->speed * dt;
            if (m->offset < m->target) {
                m->offset = m->target;
                m->state  = Q2_MV_ARRIVED;
            }
        }

        if (m->state == Q2_MV_BLOCKED && --m->block_timer == 0)
            m->state = m->saved_state;
    } else {
        if (m->target > 0) {
            m->offset -= m->speed * dt;
            if (m->offset < 0) {
                m->offset = 0;
                settled   = 1;
                m->state  = Q2_MV_IDLE;
            }
        } else {
            m->offset += m->speed * dt;
            if (m->offset > 0) {
                m->offset = 0;
                settled   = 1;
                m->state  = Q2_MV_IDLE;
            }
        }
    }

    /*
     * The portal node's visibility bit follows the door, EXCEPT that a leaf
     * whose partner is still moving must not re-seal the opening. That is what
     * stops a double door going opaque the instant its first leaf shuts.
     */
    if (m->portal_node >= 0) {
        bool partner_busy = false;

        if (m->partner >= 0 && (u32)m->partner < set->count)
            partner_busy = settled && set->movers[m->partner].state != Q2_MV_IDLE;

        (void)partner_busy;   /* consumed by the renderer's visibility pass */
    }

    (void)old;
}

u32 q2_movers_tick(q2_mover_set *set, s32 dt, u16 player_keys)
{
    u32 moved = 0, i;

    if (!set || dt <= 0)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_mover *m = &set->movers[i];
        int trig = m->triggered;
        s32 before = m->offset;

        m->triggered = 0;

        switch (m->state) {
        case Q2_MV_IDLE:
            if (!trig) {
                m->announced = 0;
                break;
            }
            /* Locked doors complain once, not every tick. */
            if (m->key_mask && !(player_keys & m->key_mask)) {
                m->announced = 1;
                break;
            }
            m->state = Q2_MV_DELAY;
            if (m->key_mask)
                m->announced = 1;
            break;

        case Q2_MV_DELAY:
            if ((s16)(m->delay_timer - dt) > 0) {
                m->delay_timer = (u16)(m->delay_timer - dt);
                break;
            }
            m->state = Q2_MV_OPENING;
            break;

        case Q2_MV_ARRIVED:
            m->state = Q2_MV_OPEN;
            break;

        case Q2_MV_OPEN: {
            u16 t;
            if (m->wait_timer == Q2_MOVER_WAIT_NEVER)
                break;
            t = (u16)(m->wait_timer - dt);
            if ((s16)t > 0) {
                m->wait_timer = t;
                break;
            }
            /* Standing in the doorway holds it open. */
            if (trig) {
                m->wait_timer = 1;
                break;
            }
            m->state = Q2_MV_CLOSING;
            break;
        }

        case Q2_MV_OPENING:
            mover_move(set, m, dt, 1);
            break;

        case Q2_MV_CLOSING:
            mover_move(set, m, dt, 0);
            break;

        case Q2_MV_BLOCKED:
            /* A door blocked while closing reverses; one blocked opening
             * carries on in the direction it was already going. */
            mover_move(set, m, dt, m->saved_state == Q2_MV_CLOSING);
            break;

        default:
            m->state = Q2_MV_IDLE;
            break;
        }

        if (m->offset != before)
            moved++;
    }

    return moved;
}

void q2_movers_node_offset(const q2_mover_set *set, u32 scene_node, s32 out[3])
{
    u32 i, k;

    if (!out)
        return;

    out[0] = out[1] = out[2] = 0;

    if (!set)
        return;

    for (i = 0; i < set->count; i++) {
        const q2_mover *m = &set->movers[i];

        if (m->offset == 0)
            continue;

        for (k = 0; k < m->part_count; k++) {
            if ((u32)m->node[k] != scene_node)
                continue;
            out[m->axis] += m->offset;
            break;
        }
    }
}
