#include "events_rt.h"

#include <stdlib.h>
#include <string.h>

/* How many records may run in one update before we stop. The original ran its
 * queue once per tick, so a self-triggering script simply ran again next tick
 * rather than locking up. */
#define Q2_EVENT_RT_MAX_PER_UPDATE 256

q2_result q2_event_rt_init(q2_event_rt *rt, const q2_events *events)
{
    q2_event_record rec;
    u32 n = 0;

    if (!rt || !events)
        return Q2_ERR_INVALID_ARG;

    memset(rt, 0, sizeof(*rt));
    rt->events = *events;

    if (events->record_count == 0)
        return Q2_OK;                 /* a real state on 36 chunks */

    rt->flags   = (u8 *)calloc(events->record_count, sizeof(u8));
    rt->offsets = (u32 *)calloc(events->record_count, sizeof(u32));
    if (!rt->flags || !rt->offsets) {
        q2_event_rt_free(rt);
        return Q2_ERR_NO_MEMORY;
    }

    /* Seed the shadow from the file. Bits 0-2 are runtime state and are clear
     * on disc, so the stored byte is a valid initial value as-is. */
    if (q2_events_first_record(&rt->events, &rec)) {
        do {
            if (n >= events->record_count)
                break;
            rt->offsets[n] = rec.offset;
            rt->flags[n]   = rec.flags;
            n++;
        } while (q2_events_next_record(&rt->events, &rec, &rec));
    }

    rt->record_count = n;
    return Q2_OK;
}

void q2_event_rt_free(q2_event_rt *rt)
{
    if (!rt)
        return;
    free(rt->flags);
    free(rt->offsets);
    memset(rt, 0, sizeof(*rt));
}

static s32 record_slot(const q2_event_rt *rt, u32 offset)
{
    u32 i;

    for (i = 0; i < rt->record_count; i++) {
        if (rt->offsets[i] == offset)
            return (s32)i;
    }
    return -1;
}

u8 q2_event_rt_flags(const q2_event_rt *rt, u32 offset)
{
    s32 slot;

    if (!rt)
        return 0;
    slot = record_slot(rt, offset);
    return (slot < 0) ? 0 : rt->flags[slot];
}

bool q2_event_rt_trigger(q2_event_rt *rt, u32 offset)
{
    u32 i;

    if (!rt || rt->pending_count >= Q2_EVENT_RT_PENDING_MAX)
        return false;
    if (record_slot(rt, offset) < 0)
        return false;

    /* Queueing the same record twice in one update would run it twice. */
    for (i = 0; i < rt->pending_count; i++) {
        if (rt->pending[i] == offset)
            return true;
    }

    rt->pending[rt->pending_count++] = offset;
    return true;
}

bool q2_event_rt_trigger_named(q2_event_rt *rt, const char *name)
{
    u32 i;

    if (!rt || !name)
        return false;

    for (i = 0; i < rt->events.dir_count; i++) {
        q2_event_dir_entry e;
        if (!q2_events_get_dir_entry(&rt->events, i, &e))
            continue;
        if (strcmp(e.name, name) == 0)
            return q2_event_rt_trigger(rt, e.offset);
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Item handlers                                                              */
/* ------------------------------------------------------------------------- */
static void set_disabled_on_list(q2_event_rt *rt, const q2_event_item *item,
                                 bool disabled)
{
    const u8 *offsets = NULL;
    u32 count = 0, i;

    if (!q2_events_get_list(item, &count, &offsets))
        return;

    for (i = 0; i < count; i++) {
        u32 target = q2_events_list_entry(offsets, i);
        s32 slot   = record_slot(rt, target);

        if (slot < 0)
            continue;

        if (disabled)
            rt->flags[slot] |=  Q2_EVREC_DISABLED;
        else
            rt->flags[slot] &= (u8)~Q2_EVREC_DISABLED;
    }
}

static q2_event_outcome run_item(q2_event_rt *rt, const q2_event_item *item)
{
    switch (item->opcode) {
    case Q2_EVOP_TRIGGER: {
        const u8 *offsets = NULL;
        u32 count = 0, i;

        if (q2_events_get_list(item, &count, &offsets)) {
            for (i = 0; i < count; i++)
                q2_event_rt_trigger(rt, q2_events_list_entry(offsets, i));
        }
        return Q2_EVENT_OK;
    }

    case Q2_EVOP_ENABLE:
        set_disabled_on_list(rt, item, false);
        return Q2_EVENT_OK;

    case Q2_EVOP_DISABLE:
        set_disabled_on_list(rt, item, true);
        return Q2_EVENT_OK;

    case Q2_EVOP_ZONEGATE:
        /* The destination is the first byte of the payload. Loading the zone is
         * the caller's business; the runtime only reports it. */
        if (item->payload && item->len > Q2_EVENT_ITEM_HEADER_SIZE) {
            rt->has_zone_change = true;
            rt->pending_zone    = item->payload[0];
        }
        return Q2_EVENT_ZONE_CHANGE;

    case Q2_EVOP_MOVER_A:
    case Q2_EVOP_MOVER_B:
    case Q2_EVOP_MOVER_C:
        /*
         * REPORTED, not interpreted — the same split as a CALL.
         *
         * The note below was right about the hazard and wrong about the
         * conclusion. The disc values ARE Scene node indices, and `mover.h`
         * deliberately reads them as such rather than reproducing the
         * console's load-time rewrite into runtime object indices — so acting
         * on them is correct. What this file cannot do is say WHICH mover,
         * because the set belongs to the owner.
         */
        if (rt->on_mover) {
            rt->on_mover(rt->on_mover_user, item);
            rt->mover_count++;
            return Q2_EVENT_OK;
        }
        /*
         * Deliberately inert. The s16 slots hold Scene node indices on disc and
         * runtime object indices after a load-time pre-pass that has not been
         * decoded, so acting on the disc values would move the wrong geometry.
         */
        rt->skipped_movers++;
        return Q2_EVENT_UNSUPPORTED;

    case Q2_EVOP_CALL: {
        u8 index;

        /* Reported, not interpreted — see the note on `on_call`. */
        if (q2_events_get_call_index(item, &index)) {
            rt->call_count++;
            if (rt->on_call)
                rt->on_call(rt->on_call_user, item, index);
        }
        return Q2_EVENT_OK;
    }

    default:
        /* FX, WAIT, FXGROUP and anything else: recognised but not yet
         * implemented. Counting them separately from movers would imply more
         * certainty about them than we have. */
        return Q2_EVENT_OK;
    }
}

/* ------------------------------------------------------------------------- */
q2_event_outcome q2_event_rt_update(q2_event_rt *rt)
{
    q2_event_outcome result = Q2_EVENT_OK;
    u32 guard = 0;

    if (!rt)
        return Q2_EVENT_OK;

    while (rt->pending_count > 0 && guard < Q2_EVENT_RT_MAX_PER_UPDATE) {
        u32 offset;
        s32 slot;
        q2_event_record rec;
        u32 i;

        /* Take from the front so triggers fire in the order they were queued. */
        offset = rt->pending[0];
        memmove(rt->pending, rt->pending + 1,
                (size_t)(rt->pending_count - 1) * sizeof(u32));
        rt->pending_count--;
        guard++;

        slot = record_slot(rt, offset);
        if (slot < 0)
            continue;

        if (rt->flags[slot] & Q2_EVREC_DISABLED)
            continue;

        /* A one-shot that has already run does not run again. */
        if ((rt->flags[slot] & Q2_EVOP_ONESHOT) &&
            (rt->flags[slot] & Q2_EVREC_HASRUN))
            continue;

        if (!q2_events_record_at(&rt->events, offset, &rec))
            continue;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_event_outcome r;

            if (!q2_events_get_item(&rt->events, &rec, i, &item))
                break;
            if (item.op & Q2_EVOP_DISABLED)
                continue;

            r = run_item(rt, &item);
            if (r == Q2_EVENT_ZONE_CHANGE)
                result = Q2_EVENT_ZONE_CHANGE;

            /* A predicate said no. Everything after it in this record is what
             * it was guarding, so the record stops here — and it is still
             * marked as having run, exactly as the console's does, or a locked
             * door would re-test on every touch. */
            if (rt->abort_record) {
                rt->abort_record = false;
                break;
            }
        }

        rt->flags[slot] |= Q2_EVREC_HASRUN;
        rt->ran_count++;
    }

    return result;
}
