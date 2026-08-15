/*
 * events_rt.h — the event runtime: triggers, arming, and zone gates.
 *
 * The Events chunks are a compiled script. events.h parses them; this executes
 * them. Together they give the game its trigger graph, which is what turns a
 * walkable level into something with progression.
 *
 * ---------------------------------------------------------------------------
 * What works and what does not, and why
 * ---------------------------------------------------------------------------
 * WORKS: trigger, enable, disable, zone gates, and therefore level progression
 * and teleports. These handlers operate purely on record offsets, which are
 * on-disc data that events.h already decodes correctly.
 *
 * DOES NOT WORK: doors and lifts. Not because the opcodes are unknown — 0x03,
 * 0x04 and 0x05 are identified as movers — but because the s16 slots inside a
 * mover item hold a Scene NODE index on disc and a RUNTIME OBJECT index after
 * load. The engine rewrites those bytes in place during a load-time pre-pass
 * that has not been decoded. Executing a mover against the disc values would
 * move the wrong thing, so the runtime records the intent and does nothing.
 *
 * That is a deliberate choice. A mover that visibly moves the wrong geometry is
 * harder to notice, and harder to debug, than one that plainly does not run.
 *
 * ---------------------------------------------------------------------------
 * Mutable state
 * ---------------------------------------------------------------------------
 * A record's flags byte carries runtime state — has-run, one-shot, disabled —
 * and the engine writes it back. The chunk here is borrowed and read-only, so
 * this keeps a shadow array of flags indexed by record offset. Bits 0-2 are
 * runtime and are clear on disc, which is what makes the shadow safe to seed
 * straight from the file.
 */
#ifndef Q2PSX_EVENTS_RT_H
#define Q2PSX_EVENTS_RT_H

#include "events.h"
#include "q2psx.h"

/* The engine's own limit on simultaneously live script objects. */
#define Q2_EVENT_RT_PENDING_MAX 64

typedef enum q2_event_outcome {
    Q2_EVENT_OK = 0,
    Q2_EVENT_ZONE_CHANGE,   /* a zone gate fired; see pending_zone */
    Q2_EVENT_UNSUPPORTED    /* a mover was reached and skipped     */
} q2_event_outcome;

typedef struct q2_event_rt {
    q2_events events;

    u8  *flags;         /* shadow of each record's mutable flags byte      */
    u32 *offsets;       /* record offsets, parallel to flags               */
    u32  record_count;

    u32  pending[Q2_EVENT_RT_PENDING_MAX];
    u32  pending_count;

    /* Set when a zone gate fires. The caller is responsible for the load. */
    bool has_zone_change;
    u32  pending_zone;

    /* Counters, so a caller can tell "nothing happened" from "nothing is
     * implemented yet". */
    u32  resumed_count; /* deferred records that came due and finished */
    u32  ran_count;
    u32  skipped_movers;

    /*
     * Called for each Q2_EVOP_CALL item a running record reaches, with the
     * item and its UserFuncs index.
     *
     * The runtime deliberately knows nothing about primitives: which index is
     * SIMROT is a per-map question that `userfuncs.[ch]` answers and this
     * module has no map. So a CALL is reported rather than interpreted, and
     * the owner — who does hold the map's UserFuncs and its rotator set —
     * decides what it means. Without this a rotator never receives the step
     * request `q2_rotators_tick` waits for, and a level's turning geometry
     * stands still: `rot moved 0` on every map.
     */
    void (*on_call)(void *user, const q2_event_item *item, u8 call_index);
    void  *on_call_user;

    /*
     * A MOVER opcode, reported for the same reason a CALL is: the runtime
     * knows a door was asked to open and not WHICH door, because a mover's
     * identity is the item's chunk offset and the SET that maps it lives with
     * the owner (mover.h). Without this `q2_movers_build` and
     * `q2_mover_trigger` had no callers at all and every door and lift on the
     * disc stood still — 1,006 MOVER_A items, 20 MOVER_B, 292 MOVER_C.
     */
    void (*on_mover)(void *user, const q2_event_item *item);
    void  *on_mover_user;

    /*
     * ABORT THE REST OF THIS RECORD — the engine's own `gp[0x423C]`.
     *
     * `ONKEYDO` is a predicate: it tests the player's key bits and, when they
     * do not satisfy it, stops the record it sits in so the items AFTER it do
     * not run. The console expresses that as a flag the primitive sets and the
     * record executor reads, which is why this is a field rather than a return
     * value — a hook that only reports a CALL has nowhere to put an answer.
     *
     * Set it from an `on_call` hook; the executor clears it.
     */
    bool abort_record;

    /*
     * DEFER THE REST OF THIS RECORD — what `TIMER` means.
     *
     * `TIMER` is "a delayed continuation of the rest of the record": the items
     * before it run now and the ones after it run later. Expressed the same way
     * the abort is, as a field an `on_call` hook writes, because the hook
     * reports a CALL and has nowhere else to put an answer. Non-zero stops the
     * record HERE and queues its remainder for `clock + defer_ticks`.
     *
     * The delay is the primitive's: `(base + ((range * rand()) >> 15)) * 30`,
     * and the 30 is not the 300 everything else on this clock uses — which
     * `userfuncs.c` calls out and which is why the caller computes it.
     */
    s32 defer_ticks;

    /*
     * Records waiting to resume, and the clock they are waiting against.
     * `q2_event_rt_advance` moves the clock; `q2_event_rt_update` runs whatever
     * has come due before it takes anything new off the queue.
     */
    struct {
        u32 offset;
        u8  next_item;
        s32 due;
    }    deferred[Q2_EVENT_RT_PENDING_MAX];
    u32  deferred_count;
    s32  clock;

    u32  mover_count;   /* MOVER items reported                          */
    u32  call_count;    /* CALL items reported, for the same "did anything
                         * happen" reason as the counters above */
} q2_event_rt;

q2_result q2_event_rt_init(q2_event_rt *rt, const q2_events *events);

/* Move the runtime's own clock, in the simulation's tick units. Deferred
 * records come due against it. */
void q2_event_rt_advance(q2_event_rt *rt, s32 ticks);
void      q2_event_rt_free(q2_event_rt *rt);

/* Queue the record at `offset` to run on the next update. */
bool q2_event_rt_trigger(q2_event_rt *rt, u32 offset);

/* Queue a record by its name in the Events directory. */
bool q2_event_rt_trigger_named(q2_event_rt *rt, const char *name);

/*
 * Run everything queued, which may queue more. Returns what happened.
 *
 * Bounded rather than run-to-empty: a script that triggers itself would
 * otherwise spin forever, and the original ran its queue once per tick.
 */
q2_event_outcome q2_event_rt_update(q2_event_rt *rt);

/* Current flags of the record at `offset`, or 0 if unknown. */
u8 q2_event_rt_flags(const q2_event_rt *rt, u32 offset);

#endif /* Q2PSX_EVENTS_RT_H */
