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
    u32  ran_count;
    u32  skipped_movers;
} q2_event_rt;

q2_result q2_event_rt_init(q2_event_rt *rt, const q2_events *events);
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
