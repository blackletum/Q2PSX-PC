#include "mover.h"

#include <stdlib.h>
#include <string.h>

/* The bank keys, registered by name at 0x8002D4E0..0x8002D800. */
const char *const q2_mover_sound_name[Q2_MVSND_COUNT] = {
    "msc_keyuse",   /* 0x800B27CC */
    "msc_keytry",   /* 0x800B27D0 */
    "pt1__strt",    /* 0x800B27D8 */
    "amb_butn2"     /* 0x800B27E4 */
};

/* Ask for a sound, unless this family has none. A BUTTON has exactly one and
 * uses it for everything; PISTON and DISH have none at all. */
static void mover_sound(q2_mover *m, s8 which)
{
    if (m->silent)
        return;
    if (m->is_button)
        which = Q2_MVSND_BUTTON;
    m->sound_pending = which;
}

s8 q2_mover_take_sound(q2_mover_set *set, u32 index)
{
    s8 s;

    if (!set || index >= set->count)
        return Q2_MVSND_NONE;
    s = set->movers[index].sound_pending;
    set->movers[index].sound_pending = Q2_MVSND_NONE;
    return s;
}


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
        /* A mover starts fully closed, so its portal starts sealed. */
        m->sealed      = 1;
        m->sound_pending = Q2_MVSND_NONE;
        /* Not a primitive: the MOVER_A/B/C opcodes build these, and only the
         * CALL path overwrites it. Zero would read as the first table row. */
        m->prim        = Q2_MOVER_PRIM_OPCODE;
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

q2_result q2_movers_build(q2_mover_set *out, const q2_events *events,
                          const q2_uf_operands *ops)
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
            const u8 *p, *q;
            u32 first;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;

            /* item.payload points past the two header bytes, so a documented
             * offset of +N is payload[N - 2]. Keep the disc offsets in the
             * code and subtract once, rather than pre-subtracting and losing
             * the correspondence with the header. */
            p = item.payload - 2;

            /*
             * AND THE OBJECT SLOTS COME FROM THE OTHER BUFFER.
             *
             * `0x80029794` is explicit about it: it sets s2 = item+8 in
             * COMMON's copy, forms s3 = gp+376 + (s2 - gp+372) at
             * 0x80029824/0x80029828, WRITES -1 through s2 at 0x80029850 and
             * then READS the node index through s3 at 0x80029854. COMMON.DAT's
             * Events is a snapshot that agrees with ZONE0's only; every zone
             * above zero carries its own slot values, and reading COMMON's
             * built a door with no parts and animated whichever nodes ZONE0
             * happened to name.
             *
             * ONLY the slot cursor rebases. 0x80025D70-0x80025E30 takes
             * travel, speed, the key mask and the two timers from the record
             * it is walking; the pristine copy is consulted for the portal
             * node at +6 and the node array alone. `q` is that copy.
             */
            q = q2_uf_operand_at(ops, p, item.len);

            /* Where this item's movers start, so its offset can be stamped on
             * whatever the switch pushes — one for A and B, two for C. */
            first = out->count;

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
                m->portal_node = q2_rd_s16(q + 6);
                collect_nodes(m, q, 8);
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
                m->portal_node = q2_rd_s16(q + 6);
                collect_nodes(m, q, 10);
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
                portal     = q2_rd_s16(q + 6);
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
                collect_nodes(leaf0, q, 10);
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
                collect_nodes(leaf1, q, 18);
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

            for (; first < out->count; first++)
                out->movers[first].item_offset = item.offset;
        }
    } while (q2_events_next_record(events, &rec, &rec));

    return Q2_OK;
}

/* The one legal item length for each of the three, from the operand table. */
static u32 info_len(q2_uf_prim prim)
{
    const q2_uf_prim_info *i = q2_uf_info(prim);
    return i ? i->item_len : 0xFFFFu;
}

/*
 * PLATFORM's target: the distance from the node's BOX CENTRE to the `origin`
 * operand, which is where the script says the platform ends up.
 *
 * `0x8002CC98`..`0x8002CCE8` reads the node record's `+16/+28`, `+20/+32` and
 * `+24/+36` — the two bbox corners — halves each pair, subtracts it from the
 * item's VEC3 at `+4`, squares and sums; then `(sum >> 2)` goes through the
 * integer square root at `0x80055CBC` and the result is doubled
 * (`sll s4, v0, 1`). `isqrt(n/4) * 2` is `isqrt(n)`: the halving is to keep the
 * intermediate in range, not part of the answer.
 *
 * The magnitude is what the console stores — `sh s4, 68(s0)` with s4 positive —
 * so the DIRECTION is not in this operand and is transcribed literally rather
 * than guessed at. A positive target moves +Y, which is down in this engine.
 */
static s16 platform_target(const q2_scene *scene, s16 node, const u8 *item)
{
    q2_scene_node n;
    s64 sum = 0;
    int k;

    if (!scene || node < 0 || !q2_scene_get_node(scene, (u32)node, &n))
        return 0;

    for (k = 0; k < 3; k++) {
        s64 centre = ((s64)n.bbox_min[k] + n.bbox_max[k]) / 2;
        s64 d      = (s64)q2_rd_s32(item + 4 + 4 * k) - centre;

        sum += d * d;
    }

    {
        /* isqrt, by the same Newton step the console's is. */
        s64 x = sum, r = 0, bit = (s64)1 << 62;

        while (bit > x)
            bit >>= 2;
        while (bit) {
            if (x >= r + bit) {
                x -= r + bit;
                r = (r >> 1) + bit;
            } else {
                r >>= 1;
            }
            bit >>= 2;
        }
        if (r > 32767)
            r = 32767;
        return (s16)r;
    }
}

q2_result q2_movers_build_calls(q2_mover_set *out, const q2_events *events,
                                const q2_userfuncs *uf,
                                const q2_uf_operands *ops,
                                const q2_scene *scene)
{
    q2_event_record rec;

    if (!out || !events || !uf)
        return Q2_ERR_INVALID_ARG;

    if (events->record_count == 0 || !q2_events_first_record(events, &rec))
        return Q2_OK;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call    call;
            const u8     *p;
            q2_mover     *m;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;
            if ((item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                continue;
            if (q2_uf_decode_call(&call, uf, &item) != Q2_OK)
                continue;
            if (call.prim != Q2_UF_LIFT1 &&
                call.prim != Q2_UF_CAGELIFT1 &&
                call.prim != Q2_UF_BUTTON &&
                call.prim != Q2_UF_PISTON &&
                call.prim != Q2_UF_DISH &&
                call.prim != Q2_UF_PLATFORM)
                continue;
            if (item.len < info_len(call.prim))
                continue;

            /* The operands, rebased the way a rotation call's are: an item the
             * game has already run reads -1 in COMMON's copy and lives at the
             * same offset in the zone's (#56). */
            /* Ask the rebase for the item's OWN length, not a constant 20:
             * DISH's item is eight bytes and asking for twenty would refuse to
             * rebase it and read the -1 in COMMON's copy instead. */
            p = q2_uf_operand_at(ops, item.payload - 2, item.len);

            m = mover_push(out);
            if (!m)
                return Q2_ERR_NO_MEMORY;

            m->axis        = 1;      /* all three of these are vertical */
            m->prim        = (u8)call.prim;
            m->item_offset = item.offset;
            m->block_flags = Q2_MV_BLK_IGNORE_OPENING;

            switch (call.prim) {
            case Q2_UF_LIFT1:
                m->target      = (s16)-(s16)q2_rd_u16(p + 4);
                m->speed       = (s16)abs(q2_rd_s16(p + 6));
                collect_nodes(m, p, 8);
                m->delay_timer = (u16)(p[16] * Q2_MOVER_TIMEBASE);
                m->wait_timer  = (p[17] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[17] * Q2_MOVER_TIMEBASE);
                break;

            case Q2_UF_CAGELIFT1:
                /* LIFT1's constructor operand for operand — see the correction
                 * in userfuncs.c. Only the wait sits elsewhere. */
                m->target      = (s16)-(s16)q2_rd_u16(p + 4);
                m->speed       = (s16)abs(q2_rd_s16(p + 6));
                collect_nodes(m, p, 8);
                m->wait_timer  = (p[18] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[18] * Q2_MOVER_TIMEBASE);
                break;

            case Q2_UF_BUTTON: {
                /* Its handler 0x80029C28 plays amb_butn2 (0x80029E04) and
                 * references none of the other five handles. */
                m->is_button = 1;
                /*
                 * `travel`'s SIGN selects obj+0x3A = +1 or -1 and its magnitude
                 * goes to obj+0x44 — so a button's speed is literally one unit
                 * a tick and its target is the travel. `invert` negates the
                 * target. Both are userfuncs.c's wording, not a reading of it.
                 */
                s16 travel = q2_rd_s16(p + 14);
                s16 mag    = (s16)abs(travel);

                m->target     = (s8)p[4] ? (s16)-mag : mag;
                m->speed      = 1;
                m->node[0]    = q2_rd_s16(p + 12);
                m->part_count = (m->node[0] >= 0) ? 1 : 0;
                m->wait_timer = (u16)(q2_rd_u16(p + 8) * Q2_MOVER_TIMEBASE);
                break;
            }

            case Q2_UF_PLATFORM:
                /* Speed at +18 and the target computed from the node's own
                 * position — see platform_target and the correction in
                 * userfuncs.c. */
                m->node[0]     = q2_rd_s16(p + 20);
                m->part_count  = (m->node[0] >= 0) ? 1 : 0;
                m->target      = platform_target(scene, m->node[0], p);
                m->speed       = (s16)abs(q2_rd_s16(p + 18));
                m->delay_timer = p[28];          /* UNSCALED, per the table */
                m->wait_timer  = (p[29] == 0xFF)
                                 ? Q2_MOVER_WAIT_NEVER
                                 : (u16)(p[29] * Q2_MOVER_TIMEBASE);
                break;

            case Q2_UF_DISH:
                /* Neither DISH's constructor (0x8002A880) nor PISTON's
                 * (0x8002D114) installs 0x80025658, and neither references any
                 * of the six sound handles: silent, deliberately. */
                m->silent = 1;
                /*
                 * The speed is not authored: it is the immediate ONE, written
                 * by the exec at `0x8002E314` (`addiu v1, zero, 1; sh v1,
                 * 58(a0)`) exactly as a button's is. #81 read the operand table,
                 * found no speed in it and concluded the primitive needed more
                 * reading — which was right about the table and wrong about the
                 * conclusion. There is no operand because there is no choice.
                 *
                 * The rest is `0x8002E2A0`: the object slot is at +6, the travel
                 * is `(s8)item[+5] << 5` into obj+0x44, obj+0x52 is a one-shot
                 * latch tested before anything else, and obj+0x4E is set to the
                 * clock plus 300 — one second, which is the wait.
                 */
                m->node[0]     = q2_rd_s16(p + 6);
                m->part_count  = (m->node[0] >= 0) ? 1 : 0;
                m->target      = (s16)((s16)(s8)p[5] << 5);
                m->speed       = 1;
                m->wait_timer  = Q2_MOVER_TIMEBASE;
                break;

            case Q2_UF_PISTON:
                m->silent = 1;          /* see DISH above */
                /* A crusher. `time` is UNSCALED, which is the table's word for
                 * it and the reason it is not multiplied here. */
                m->speed      = p[5];
                m->target     = (s16)q2_rd_u16(p + 6);
                m->node[0]    = q2_rd_s16(p + 8);
                m->part_count = (m->node[0] >= 0) ? 1 : 0;
                m->wait_timer = q2_rd_u16(p + 16);
                /* A crusher does not stop for you: it is what a blocked mover
                 * ignoring both directions is for. */
                m->block_flags = Q2_MV_BLK_IGNORE_OPENING |
                                 Q2_MV_BLK_IGNORE_CLOSING;
                break;

            default:
                break;
            }

            /*
             * A mover with no speed never arrives and one with no node moves
             * nothing. Either is a decode this port should not keep — and
             * saying WHICH is dropped is the measurement, because an empty
             * object slot and a primitive this port cannot build look identical
             * in a count. #81 is the entry that made that point.
             */
            if (m->speed == 0 || m->part_count == 0) {
                const q2_uf_prim_info *pi = q2_uf_info(call.prim);

                /*
                 * Say WHICH of the two, and do not print `node[0]` when there
                 * are none: a dropped mover's node field is the zero the push
                 * left, and printing it reads as "node 0" — a real index — for
                 * a mover whose four object slots were all -1.
                 */
                /*
                 * "No object slot resolves" is USUALLY NOT A FAULT and used to
                 * be reported as one. A CALL-built lift belonging to another
                 * zone genuinely has four -1 slots in the copy this zone
                 * carries — BASE1's CAGELIFT1 at item offset 704 is one, and
                 * its nodes (215 and 216) are sitting in ZONE1's copy where
                 * they belong. Every zone load printed one of these per
                 * out-of-zone lift and it read as a decode failure.
                 *
                 * Speed 0 IS a fault: such a mover triggers, ticks, and never
                 * arrives, so it keeps the loud level.
                 */
                if (m->part_count == 0)
                    Q2_DEBUG("mover %s not in this zone: all four object slots "
                             "are -1 (speed %d, target %d)",
                             pi ? pi->name : "?", m->speed, m->target);
                else
                    Q2_INFO("mover dropped: %s — speed 0, it would never "
                            "arrive (target %d)",
                            pi ? pi->name : "?", m->target);
                out->count--;
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
u32 q2_movers_trigger_item(q2_mover_set *set, u32 item_offset)
{
    u32 i, n = 0;

    if (!set)
        return 0;

    for (i = 0; i < set->count; i++) {
        if (set->movers[i].item_offset != item_offset)
            continue;
        q2_mover_trigger(set, i);
        n++;
    }

    return n;
}

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
/*
 * The displacement `mover_move` is about to apply along the mover's own axis,
 * clamped to the travel exactly as the move itself clamps it. Split out so the
 * obstruction test can ask "would this step run into something" before the
 * step happens, which is the order 0x80025CBC works in.
 */
static s32 mover_step(const q2_mover *m, s32 dt, int dir)
{
    s32 step = m->speed * dt;
    s32 to;

    if (dir)
        to = (m->target > 0) ? (m->offset + step) : (m->offset - step);
    else
        to = (m->target > 0) ? (m->offset - step) : (m->offset + step);

    if (dir) {
        if (m->target > 0 && to > m->target) to = m->target;
        if (m->target < 0 && to < m->target) to = m->target;
    } else {
        if (m->target > 0 && to < 0) to = 0;
        if (m->target < 0 && to > 0) to = 0;
    }

    return to - m->offset;
}

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
     * The BLOCKED countdown, out of the `if (dir)` arm it used to sit in.
     *
     * 0x80025D08 reloads obj+0x56 with 16 on both arms and the decrement is
     * outside the direction test, so a door blocked while closing counted down
     * and one blocked while opening did not. And the decrement was `--` on a
     * u8 with no guard: unreachable while nothing ever assigned Q2_MV_BLOCKED,
     * and a 255-tick freeze the moment something did.
     */
    if (m->state == Q2_MV_BLOCKED) {
        if (m->block_timer)
            m->block_timer--;
        if (m->block_timer == 0)
            m->state = m->saved_state;
    }

    /*
     * The portal node's visibility bit follows the door, EXCEPT that a leaf
     * whose partner is still moving must not re-seal the opening. That is what
     * stops a double door going opaque the instant its first leaf shuts.
     *
     * 0x80025C5C-0x80025C74 writes bit 15 of the portal node's `flags08`: set
     * on the tick the leaf settles fully CLOSED, cleared otherwise, and the
     * write skipped entirely while the partner object's state byte at +0x52 is
     * non-zero (0x80025C24-0x80025C54).
     *
     * `sealed` is that bit. It is recorded on the mover rather than written
     * into the client's node-hidden array, because that array has another
     * writer — OBJDRAWOFF, which the script uses to hide nodes and which keeps
     * a count — and two writers fighting over one byte would make a door
     * un-hide whatever a script had hidden. The zone draw reads this instead.
     */
    if (m->portal_node >= 0) {
        bool partner_busy = false;

        if (m->partner >= 0 && (u32)m->partner < set->count)
            partner_busy = set->movers[m->partner].state != Q2_MV_IDLE;

        if (!partner_busy)
            m->sealed = (u8)(settled ? 1 : 0);
    }

    (void)old;
}

u32 q2_movers_tick(q2_mover_set *set, s32 dt, u16 player_keys)
{
    return q2_movers_tick_blocked(set, dt, player_keys, NULL, NULL);
}

/*
 * Would this mover's next step run into something, and is it allowed to?
 *
 * 0x80025CBC. The direction decides which bit of `block_flags` exempts it: bit
 * 0 while opening, bit 1 while closing. A PISTON has both — it is a crusher —
 * and everything else has neither, so an ordinary door stops.
 */
static bool mover_obstructed(q2_mover_set *set, u32 index, q2_mover *m,
                             s32 step, q2_mover_blocked_fn blocked, void *user)
{
    u8 exempt;

    if (!blocked || step == 0)
        return false;
    if (!blocked(index, step, user))
        return false;

    exempt = (m->state == Q2_MV_CLOSING ||
              (m->state == Q2_MV_BLOCKED && m->saved_state == Q2_MV_CLOSING))
           ? Q2_MV_BLK_IGNORE_CLOSING : Q2_MV_BLK_IGNORE_OPENING;

    /* 0x80025D08 reloads the timer on BOTH arms; only the flag decides whether
     * the state changes with it. */
    m->block_timer = Q2_MOVER_BLOCK_TICKS;

    if (m->block_flags & exempt)
        return false;               /* a crusher does not care */

    if (m->state != Q2_MV_BLOCKED) {
        m->saved_state = m->state;
        m->state       = Q2_MV_BLOCKED;
    }
    (void)set;
    return true;
}

u32 q2_movers_tick_blocked(q2_mover_set *set, s32 dt, u16 player_keys,
                           q2_mover_blocked_fn blocked, void *user)
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
            /* Locked doors complain once, not every tick — 0x8002585C is
             * behind the same latch, so the refusal sound is once too. */
            if (m->key_mask && !(player_keys & m->key_mask)) {
                if (!m->announced)
                    mover_sound(m, Q2_MVSND_KEY_TRY);
                m->announced = 1;
                break;
            }
            m->state = Q2_MV_DELAY;
            if (m->key_mask) {
                /* 0x800257A8: the key was accepted. */
                mover_sound(m, Q2_MVSND_KEY_USE);
                m->announced = 1;
            }
            break;

        case Q2_MV_DELAY:
            if ((s16)(m->delay_timer - dt) > 0) {
                m->delay_timer = (u16)(m->delay_timer - dt);
                break;
            }
            /* 0x800258F0 -> 0x80025A5C: the motion starts. */
            m->state = Q2_MV_OPENING;
            mover_sound(m, Q2_MVSND_START);
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
            /* 0x800259BC -> the same 0x80025A5C. A door closing plays the
             * same start sound; there is no separate close. */
            m->state = Q2_MV_CLOSING;
            mover_sound(m, Q2_MVSND_START);
            break;
        }

        case Q2_MV_OPENING:
            if (!mover_obstructed(set, i, m, mover_step(m, dt, 1),
                                  blocked, user))
                mover_move(set, m, dt, 1);
            break;

        case Q2_MV_CLOSING:
            if (!mover_obstructed(set, i, m, mover_step(m, dt, 0),
                                  blocked, user))
                mover_move(set, m, dt, 0);
            break;

        case Q2_MV_BLOCKED: {
            /* A door blocked while closing reverses; one blocked opening
             * carries on in the direction it was already going. */
            int dir = m->saved_state == Q2_MV_CLOSING;

            if (!mover_obstructed(set, i, m, mover_step(m, dt, dir),
                                  blocked, user))
                mover_move(set, m, dt, dir);
            else if (m->block_timer == 0)
                m->state = m->saved_state;
            break;
        }

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
