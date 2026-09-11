/*
 * test_events.c — the event-script RUNTIME: one-shot latching, the executor
 * prologue, WAIT, the zone-gate abort, and what survives a zone change (the
 * carry and the EVE_ replay).
 *
 * Everything here is pinned to an address in the retail executable, because
 * every one of these behaviours was read out of it rather than guessed. The
 * fixtures are synthetic Events chunks: the disc's own scripts exercise most of
 * this too, but a synthetic record can be made to fail in exactly one way,
 * which a shipped script cannot.
 *
 * Where a behaviour has NO use on the disc — opcode 0x09 WAIT, which appears
 * zero times in 6,646 items — the test says so, because a test is the only
 * evidence that exists for it and the reader should know that.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events_rt.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_i(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* A synthetic Events chunk.
 *
 * The record area starts at 8, never at 0, so that offset 0 is what it is on
 * the disc: the chunk's u32 record count, not a record. Several tests turn on
 * that — see the CAT_C leave edge. */
#define FIX_FIRST_RECORD 8

/* 4 KB: the EVE_ cap test needs 130 records of 16 bytes. */
typedef struct fixture {
    u8        raw[4096];
    u32       size;
    q2_events ev;
} fixture;

static void fix_begin(fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->size = FIX_FIRST_RECORD;
}

/* Append a record header. Its size is patched by fix_end_record. */
static u32 fix_record(fixture *f, u8 flags)
{
    u32 at = f->size;

    f->raw[at + 0] = 0;
    f->raw[at + 1] = 0;
    f->raw[at + 2] = 0;         /* n_items, counted up by fix_item   */
    f->raw[at + 3] = flags;
    f->size += 4;
    return at;
}

/* Append an item to the record started at `rec`. Returns the item's offset. */
static u32 fix_item(fixture *f, u32 rec, u8 op, u8 len, const u8 *payload)
{
    u32 at = f->size;

    f->raw[at + 0] = op;
    f->raw[at + 1] = len;
    if (payload && len > 2)
        memcpy(f->raw + at + 2, payload, (size_t)len - 2);
    f->size += len;
    f->raw[rec + 2]++;
    return at;
}

static void fix_end_record(fixture *f, u32 rec)
{
    u32 size = f->size - rec;

    while (size & 3u) {         /* records are a multiple of four */
        f->raw[f->size++] = 0;
        size++;
    }
    f->raw[rec + 0] = (u8)size;
    f->raw[rec + 1] = (u8)(size >> 8);
}

static void fix_end(fixture *f, u32 record_count)
{
    memset(&f->ev, 0, sizeof(f->ev));
    f->ev.data         = f->raw;
    f->ev.size         = f->size;
    f->ev.record_count = record_count;
    f->ev.dir_offset   = 4;
    f->ev.first_record = FIX_FIRST_RECORD;
}

static s32 slot_of(const q2_event_rt *rt, u32 offset)
{
    u32 i;

    for (i = 0; i < rt->record_count; i++)
        if (rt->offsets[i] == offset)
            return (s32)i;
    return -1;
}

/* A CALL payload: the primitive index at item+2, rest zero. */
static void call_payload(u8 *p, u8 index)
{
    memset(p, 0, 10);
    p[0] = index;
}

/* A list payload for TRIGGER/ENABLE/DISABLE: s16 count then u16 offsets,
 * padded so that len == 4 + 2*(count + (count & 1)). */
static u8 list_len(u32 count)
{
    return (u8)(4 + 2 * (count + (count & 1u)));
}

static void list_payload(u8 *p, const u16 *targets, u32 count)
{
    u32 i;

    memset(p, 0, 32);
    p[0] = (u8)count;
    p[1] = (u8)(count >> 8);
    for (i = 0; i < count; i++) {
        p[2 + i * 2] = (u8)targets[i];
        p[3 + i * 2] = (u8)(targets[i] >> 8);
    }
}

/* ------------------------------------------------------------------------- */
/* 0x80027488..0x80027498: the dispatcher writes DISABLED := ONESHOT back into
 * the ITEM before it dispatches, so a one-shot item retires while the record
 * around it keeps running. */
static void test_item_oneshot_latch(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[10];
    u32 rec, k;

    printf("item one-shot latch (0x80027498)\n");

    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    fix_item(&f, rec, Q2_EVOP_MOVER_A, 24, NULL);
    call_payload(pay, 5);
    fix_item(&f, rec, (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT), 12, pay);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");

    for (k = 0; k < 3; k++) {
        q2_event_rt_trigger(&rt, rec);
        q2_event_rt_update(&rt);
    }

    /* The mover has no callback here, so it is counted as skipped — either
     * way it is REACHED on all three passes. */
    check_eq_i(rt.skipped_movers, 3,
               "the plain item runs on every pass");
    check_eq_i(rt.call_count, 1,
               "the ONESHOT item runs exactly once (0x80027498)");
    check_eq_i(rt.ran_count, 3,
               "the record itself is not one-shot and runs three times");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* The item latch is applied BEFORE the opcode bounds check at
 * 0x8002749C..0x800274A8 and before any handler's own length test, so a
 * one-shot item its handler will reject still retires.
 *
 * The vehicle is a ZONEGATE of length 20 — a length the chunk parser accepts
 * (item lengths are a multiple of four, so 14 is not even representable) and
 * 0x80027788 does not, because it demands EXACTLY 16. The handler refuses it;
 * 0x80027498 has already run. */
static void test_item_latch_precedes_validation(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[18];
    u32 rec, gate;

    printf("the item latch precedes validation (0x8002749C)\n");

    memset(pay, 0, sizeof(pay));
    memcpy(pay, "Zone9", 5);

    fix_begin(&f);
    rec  = fix_record(&f, Q2_EVREC_CAT_B);
    gate = fix_item(&f, rec, (u8)(Q2_EVOP_ZONEGATE | Q2_EVOP_ONESHOT), 20, pay);
    fix_item(&f, rec, Q2_EVOP_MOVER_A, 24, NULL);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.current_zone = 0;

    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_OK,
          "a 20-byte zone gate is refused: 0x80027788 wants exactly 16");
    check(!rt.has_zone_change,
          "and it asks for no zone");
    check_eq_i(q2_event_rt_item_flags(&rt, gate), Q2_EVOP_DISABLED,
               "yet the ONESHOT item has already retired (0x80027498)");
    check_eq_i(rt.skipped_movers, 1, "the record ran on past it");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* The item latch is stored at 0x80027498 and the handler is not entered until
 * the jump at 0x800274C4, so a handler that looks at its own op byte already
 * sees the item retired. A hook is the port's handler, and it can look. */
typedef struct latch_probe {
    q2_event_rt *rt;
    u32          record;
    s32          item_seen;    /* the CALL's item_flags, read in its hook */
    s32          record_seen;  /* the record's flags, read in the hook    */
} latch_probe;

static void latch_probe_hook(void *user, const q2_event_item *item, u8 index)
{
    latch_probe *p = (latch_probe *)user;

    (void)index;
    p->item_seen   = q2_event_rt_item_flags(p->rt, item->offset);
    p->record_seen = q2_event_rt_flags(p->rt, p->record);
}

static void test_item_latch_precedes_dispatch(void)
{
    fixture f;
    q2_event_rt rt;
    latch_probe probe;
    u8 pay[14];
    u32 rec, gate;

    printf("the item latch precedes the handler (0x80027498 / 0x800274C4)\n");

    /* Observed from inside: [CALL|ONESHOT] in a repeatable record. */
    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    call_payload(pay, 3);
    fix_item(&f, rec, (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT), 12, pay);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    probe.rt          = &rt;
    probe.record      = rec;
    probe.item_seen   = -1;
    probe.record_seen = -1;
    rt.on_call      = latch_probe_hook;
    rt.on_call_user = &probe;

    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(probe.item_seen, Q2_EVOP_DISABLED,
               "inside its own handler a one-shot item is already retired");
    q2_event_rt_free(&rt);

    /*
     * Observed from outside: an ACCEPTED one-shot gate stops the record
     * (0x8002783C returns 1), so the only place its latch can be written is
     * ahead of the handler. Latched after the abort, it would still be armed
     * on the second pass and abort again — and the door behind it, which is
     * the disc's usual shape (74 COMMON records are exactly [ZONEGATE,
     * MOVER_A]), would never open.
     */
    memset(pay, 0, sizeof(pay));
    memcpy(pay, "Zone1", 5);

    fix_begin(&f);
    rec  = fix_record(&f, Q2_EVREC_CAT_B);
    gate = fix_item(&f, rec, (u8)(Q2_EVOP_ZONEGATE | Q2_EVOP_ONESHOT), 16, pay);
    fix_item(&f, rec, Q2_EVOP_MOVER_A, 24, NULL);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture restarts");
    rt.current_zone = 0;

    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE,
          "the one-shot gate is accepted");
    check_eq_i(rt.skipped_movers, 0, "and aborts the record");
    check_eq_i(q2_event_rt_item_flags(&rt, gate), Q2_EVOP_DISABLED,
               "and is retired even though its record stopped on it");

    rt.has_zone_change = false;
    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_OK,
          "the second pass skips the retired gate");
    check(!rt.has_zone_change, "and asks for no zone");
    check_eq_i(rt.skipped_movers, 1, "so the item behind it runs");
    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* 0x8002799C: the record gate is the DISABLED bit and nothing else.
 * 0x800279A8..0x800279BC: the latch is DISABLED := ONESHOT plus HASRUN.
 * 0x800278B0: ENABLE clears bit 7 alone, so it genuinely re-arms. */
static void test_record_latch_and_enable_rearm(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[32];
    u16 targets[1];
    u32 a, b;

    printf("record latch and ENABLE re-arm (0x800279BC / 0x800278B0)\n");

    fix_begin(&f);
    a = fix_record(&f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
    call_payload(pay, 5);
    fix_item(&f, a, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, a);

    b = fix_record(&f, Q2_EVREC_CAT_B);
    targets[0] = (u16)a;
    list_payload(pay, targets, 1);
    fix_item(&f, b, Q2_EVOP_ENABLE, list_len(1), pay);
    fix_end_record(&f, b);
    fix_end(&f, 2);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");

    q2_event_rt_trigger(&rt, a);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 1, "the one-shot record runs once");
    check_eq_i(q2_event_rt_flags(&rt, a), 0xC9,
               "its flags become CAT_A|ONESHOT|DISABLED|HASRUN (0x800279BC)");

    q2_event_rt_trigger(&rt, a);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 1, "and it refuses to run again");

    q2_event_rt_trigger(&rt, b);
    q2_event_rt_update(&rt);
    check_eq_i(q2_event_rt_flags(&rt, a), 0x49,
               "ENABLE clears bit 7 and nothing else — HASRUN survives");

    q2_event_rt_trigger(&rt, a);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 2,
               "the re-armed one-shot record runs a second time");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* The two latches have to land together. 447 of the disc's 457 one-shot ITEMS
 * sit inside one-shot RECORDS, where the record latch used to be the only
 * thing that stopped them repeating. Once ENABLE genuinely re-arms a spent
 * record, only the item latch keeps its one-shot items spent: the re-armed
 * record runs again and its one-shot items do not. LAB COMMON 0x6d4 and 0x990
 * are the disc's instance — every item one-shot, so re-armed, they run
 * nothing. */
typedef struct call_tally {
    u32 by_index[8];
} call_tally;

static void tally_hook(void *user, const q2_event_item *item, u8 index)
{
    call_tally *t = (call_tally *)user;

    (void)item;
    if (index < 8)
        t->by_index[index]++;
}

static void test_rearmed_record_keeps_its_spent_items(void)
{
    fixture f;
    q2_event_rt rt;
    call_tally tally;
    u8 pay[32];
    u16 targets[1];
    u32 a, b;

    printf("a re-armed record keeps its one-shot items spent\n");

    fix_begin(&f);
    a = fix_record(&f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
    call_payload(pay, 1);
    fix_item(&f, a, (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT), 12, pay);
    call_payload(pay, 2);
    fix_item(&f, a, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, a);

    b = fix_record(&f, Q2_EVREC_CAT_B);
    targets[0] = (u16)a;
    list_payload(pay, targets, 1);
    fix_item(&f, b, Q2_EVOP_ENABLE, list_len(1), pay);
    fix_end_record(&f, b);
    fix_end(&f, 2);

    memset(&tally, 0, sizeof(tally));
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.on_call      = tally_hook;
    rt.on_call_user = &tally;

    q2_event_rt_trigger(&rt, a);
    q2_event_rt_update(&rt);
    q2_event_rt_trigger(&rt, b);            /* ENABLE a */
    q2_event_rt_update(&rt);
    q2_event_rt_trigger(&rt, a);
    q2_event_rt_update(&rt);

    check_eq_i(tally.by_index[2], 2,
               "the re-armed record runs its plain item a second time");
    check_eq_i(tally.by_index[1], 1,
               "but its one-shot item stays retired (0x80027498)");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* The latch is applied BEFORE the item loop (store at 0x800279BC, first
 * dispatch at 0x80027A10), so a record that aborts on its first item is latched
 * exactly as one that runs to the end. */
static void test_record_latch_precedes_items(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[14];
    u32 rec;

    printf("the record latch precedes the item loop (0x800279BC)\n");

    memset(pay, 0, sizeof(pay));
    memcpy(pay, "Zone1", 5);

    fix_begin(&f);
    rec = fix_record(&f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
    fix_item(&f, rec, Q2_EVOP_ZONEGATE, 16, pay);
    fix_item(&f, rec, Q2_EVOP_MOVER_A, 24, NULL);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.current_zone = 0;

    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE,
          "the gate is accepted");
    check_eq_i(rt.skipped_movers, 0,
               "and the record stops there (0x8002783C returns 1)");
    check_eq_i(rt.ran_count, 1, "the aborted record still counts as run");
    check_eq_i(q2_event_rt_flags(&rt, rec), 0xC9,
               "and is latched, exactly as one that finished");

    q2_event_rt_free(&rt);
}

/*
 * What pins the ORDER, rather than only the result. Everything above would
 * still pass with the latch applied after the item loop, as long as it always
 * ran. The order shows when a record's own items change its flags byte:
 *
 *   [DISABLE <itself>, CALL] in a record that is NOT one-shot. Latched first
 *   (retail), the DISABLE lands on top of the latch and stays. Latched after,
 *   the latch's (f & 0x7F) takes bit 7 straight back off, and the record the
 *   script just disabled runs again next time.
 *
 * And a handler can simply look: the CALL's hook reads the record's flags and
 * finds HASRUN already on.
 */
static void test_record_latch_keeps_a_self_disable(void)
{
    fixture f;
    q2_event_rt rt;
    latch_probe probe;
    u8 pay[32];
    u16 targets[1];
    u32 rec;

    printf("a record that disables itself stays disabled (0x800279BC first)\n");

    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    targets[0] = (u16)FIX_FIRST_RECORD;          /* itself — the first record */
    list_payload(pay, targets, 1);
    fix_item(&f, rec, Q2_EVOP_DISABLE, list_len(1), pay);
    call_payload(pay, 4);
    fix_item(&f, rec, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check_eq_i(rec, FIX_FIRST_RECORD,
               "the record sits where its DISABLE points");
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    probe.rt          = &rt;
    probe.record      = rec;
    probe.item_seen   = -1;
    probe.record_seen = -1;
    rt.on_call      = latch_probe_hook;
    rt.on_call_user = &probe;

    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 1, "the record runs through its CALL once");
    check_eq_i(probe.record_seen,
               Q2_EVREC_DISABLED | Q2_EVREC_CAT_B | Q2_EVREC_HASRUN,
               "its CALL finds it latched AND disabled: the latch came first");
    check_eq_i(q2_event_rt_flags(&rt, rec) & Q2_EVREC_DISABLED,
               Q2_EVREC_DISABLED,
               "and the DISABLE survives the record's own run");

    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 1,
               "so the second trigger is refused at the gate");
    check_eq_i(rt.ran_count, 1, "and it is not latched again");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* Every resume path re-runs the prologue: 0x80027174..0x800271C0 in the TIMER
 * continuation, 0x80027994..0x800279BC in the record executor. A record
 * disabled between the defer and the due tick does not resume. */
static void defer_hook(void *user, const q2_event_item *item, u8 index)
{
    q2_event_rt *rt = (q2_event_rt *)user;

    (void)item;
    if (index == 1)             /* stand-in for a TIMER primitive */
        rt->defer_ticks = 10;
}

/* [CALL 1 (defers), CALL 2] under the given record flags. */
static u32 fix_deferring_record(fixture *f, u8 flags)
{
    u8 pay[10];
    u32 rec;

    fix_begin(f);
    rec = fix_record(f, flags);
    call_payload(pay, 1);
    fix_item(f, rec, Q2_EVOP_CALL, 12, pay);
    call_payload(pay, 2);
    fix_item(f, rec, Q2_EVOP_CALL, 12, pay);
    fix_end_record(f, rec);
    fix_end(f, 1);
    return rec;
}

static void test_deferred_resume_runs_the_prologue(void)
{
    fixture f;
    q2_event_rt rt;
    u32 rec;
    s32 slot;

    printf("a deferred resume re-runs the prologue (0x80027174)\n");

    /* A plain, repeatable record — what every one of the disc's TIMER records
     * is. Disabled by hand while it waits, standing in for a DISABLE item or a
     * DISABLEME firing between the timer and the due tick. */
    rec = fix_deferring_record(&f, Q2_EVREC_CAT_B);
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.on_call      = defer_hook;
    rt.on_call_user = &rt;

    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.deferred_count, 1, "the record deferred");
    check_eq_i(rt.call_count, 1, "having run its first item");
    check_eq_i(rt.ran_count, 1,
               "and was latched as run BEFORE its items (0x800279BC), "
               "deferral or not");
    check_eq_i(q2_event_rt_flags(&rt, rec), Q2_EVREC_CAT_B | Q2_EVREC_HASRUN,
               "HASRUN is already on at the moment of deferral");

    slot = slot_of(&rt, rec);
    check(slot >= 0, "the record has a slot");
    rt.flags[slot] |= Q2_EVREC_DISABLED;

    q2_event_rt_advance(&rt, 20);
    q2_event_rt_update(&rt);
    check_eq_i(rt.resumed_count, 1, "the entry came due and was spent");
    /* Only for a fire count of 1: 0x800273E0 frees the slot when the count
     * reaches zero, and 0x800273E4..0x800273EC re-arm any other — BOSS1 0x394's
     * fire count of 0 keeps its slot forever. This port models count 1. */
    check_eq_i(rt.deferred_count, 0,
               "and is gone, as retail's slot is for a fire count of 1");
    check_eq_i(rt.call_count, 1,
               "but the resume was refused at the gate (0x80027180)");
    check_eq_i(rt.ran_count, 1, "so it was not latched a second time");
    q2_event_rt_free(&rt);

    /* The control: left enabled, the same record resumes — so the refusal
     * above is the gate and not a resume that never happens — and the resume
     * re-applies the latch (0x800271A8..0x800271C0) before its first item. */
    rec = fix_deferring_record(&f, Q2_EVREC_CAT_B);
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture restarts");
    rt.on_call      = defer_hook;
    rt.on_call_user = &rt;

    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    q2_event_rt_advance(&rt, 20);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 2, "an enabled record resumes past its timer");
    check_eq_i(rt.ran_count, 2,
               "and the resume passed through the prologue as a second run");
    q2_event_rt_free(&rt);

    /*
     * A ONE-SHOT record that defers is refused by ITS OWN latch: the store at
     * 0x800279BC set DISABLED before the TIMER item was reached, and
     * 0x80027174 tests exactly that bit when the slot comes due. So on the
     * console the items after a TIMER in a one-shot record never run unless an
     * ENABLE re-arms the record while it waits.
     *
     * The instructions force this and the disc never meets it: of the 18
     * TIMER CALL items in the 49 COMMON scripts, not one sits in a ONESHOT
     * record (checked against each map's UserFuncs names).
     */
    rec = fix_deferring_record(&f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture restarts again");
    rt.on_call      = defer_hook;
    rt.on_call_user = &rt;

    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(q2_event_rt_flags(&rt, rec), 0xC9,
               "a one-shot record is ALREADY retired at the moment it defers");
    q2_event_rt_advance(&rt, 20);
    q2_event_rt_update(&rt);
    check_eq_i(rt.resumed_count, 1, "its entry comes due");
    check_eq_i(rt.call_count, 1,
               "and its own latch refuses the rest of the record");
    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* 0x80027784 + 0x80079178: an accepted gate returns 1, which every executor
 * site reads as "stop this record". A refused one returns 0 and the record
 * carries on. The fixture is the disc's own shape — all 107 non-final gates in
 * the COMMON scripts sit in CAT_B records, 74 of them as exactly [ZONEGATE,
 * MOVER_A]. */
static void test_zonegate_abort(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[14];
    u32 rec;

    printf("zone gates abort the record (0x8002783C)\n");

    memset(pay, 0, sizeof(pay));
    memcpy(pay, "Zone1", 5);

    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    fix_item(&f, rec, Q2_EVOP_ZONEGATE, 16, pay);
    fix_item(&f, rec, Q2_EVOP_MOVER_A, 24, NULL);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    /* Standing in zone 0: the gate names a different zone, so it fires. */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.current_zone = 0;
    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE,
          "a gate naming another zone fires");
    check_eq_i(rt.pending_zone, 1, "and names zone 1");
    check_eq_i(rt.skipped_movers, 0,
               "the mover after it does NOT run");
    q2_event_rt_free(&rt);

    /* Standing in zone 1 already: 0x800791E0's name compare refuses. */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture restarts");
    rt.current_zone = 1;
    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_OK,
          "a gate naming the resident zone does not fire (0x800791E8)");
    check(!rt.has_zone_change, "and raises no request");
    check_eq_i(rt.skipped_movers, 1,
               "so the mover after it DOES run");
    q2_event_rt_free(&rt);

    /* Deathmatch: 0x8007917C refuses before anything else. */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture restarts again");
    rt.current_zone = 0;
    rt.multiplayer  = true;
    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_OK,
          "in deathmatch the gate is a no-op (0x8007917C)");
    check(!rt.has_zone_change, "and raises no request");
    check_eq_i(rt.skipped_movers, 1, "the mover after it runs");
    q2_event_rt_free(&rt);

    /*
     * The owner has not said which zone is resident — -1, the init default,
     * which an owner has to overwrite after every init (the playable client
     * does, in client_load_zone). The gate is accepted
     * and the record runs ON: the port's fallback, not the console's, because
     * without the same-zone refusal an abort would shut every door or lift
     * that waits behind a gate (105 records in the COMMON scripts) for good.
     * This case exists to fail against an unconditional abort; it matches the
     * pre-abort behaviour on purpose.
     */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture restarts once more");
    check_eq_i(rt.current_zone, -1, "init leaves the resident zone unknown");
    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE,
          "with the zone unknown the gate is accepted");
    check(rt.has_zone_change, "and raises its request");
    check_eq_i(rt.skipped_movers, 1,
               "but does not abort: the door behind it still opens");
    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* 0x8002808C: the CAT_C leave pass queues the TRIGGER SENTINEL's event_offset,
 * which is 0 on 49 of 49 maps — so on the console the leave edge runs nothing.
 * This is RETAIL'S bug; the port reproduces it deterministically. */
static void test_catc_leave_edge_runs_nothing(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[10];
    u32 a, b, c;

    printf("the CAT_C leave edge runs nothing (0x8002808C)\n");

    fix_begin(&f);
    a = fix_record(&f, Q2_EVREC_CAT_A);
    call_payload(pay, 1);
    fix_item(&f, a, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, a);

    b = fix_record(&f, Q2_EVREC_CAT_B);
    call_payload(pay, 2);
    fix_item(&f, b, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, b);

    c = fix_record(&f, Q2_EVREC_CAT_C);
    call_payload(pay, 3);
    fix_item(&f, c, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, c);
    fix_end(&f, 3);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");

    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contact(&rt, a);
    q2_event_rt_contact(&rt, b);
    q2_event_rt_contact(&rt, c);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.ran_count, 2, "inside runs enter and stay, not leave");

    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.catc_leave_edges, 1, "the leave edge is detected");
    check_eq_i(rt.ran_count, 2,
               "and, as on the console, runs nothing");
    check_eq_i(rt.call_count, 2, "the CAT_C record's CALL never fires");

    /* The offset is the knob, not a deletion: point it at the leaving record
     * and the edge does run. An owner that reads a non-zero sentinel out of a
     * modified TrigBounds would get exactly this. */
    rt.catc_leave_offset = c;
    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contact(&rt, c);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.catc_leave_edges, 2, "a second leave edge");
    check_eq_i(rt.call_count, 3,
               "which, with the offset set, does run the record");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* Opcode 0x09 WAIT, 0x800276C4.
 *
 * NOTHING ON THE DISC REACHES THIS. Zero 0x09 items in 6,646, across all 164
 * containers, so these assertions are the only evidence the implementation has:
 * they pin what the instructions say, not what the console was observed doing.
 */
static void test_wait_counts_non_consecutive_arrivals(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[10];
    u32 rec, k;

    printf("WAIT counts non-consecutive arrivals (0x800276E8)\n");

    /* item+4 the stamp, item+8 the counter, item+10 the reload — payload is
     * item+2 onward, so the reload lives at payload[8]. */
    memset(pay, 0, sizeof(pay));
    pay[8] = 3;

    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    fix_item(&f, rec, Q2_EVOP_WAIT, 12, pay);
    call_payload(pay, 5);
    fix_item(&f, rec, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");

    q2_event_rt_trigger(&rt, rec);          /* tick 0 */
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 0, "the first arrival stops the record");

    q2_event_rt_advance(&rt, 1);
    q2_event_rt_advance(&rt, 1);            /* tick 2 */
    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 0, "the second arrival stops it too");

    q2_event_rt_advance(&rt, 1);
    q2_event_rt_advance(&rt, 1);            /* tick 4 */
    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 1,
               "the third lets the record through (counter reached 0)");

    /* 0x80027724/0x8002772C: at zero the counter is reloaded from item+10, so
     * the gate re-arms and the next three arrivals count down again. */
    for (k = 0; k < 2; k++) {
        q2_event_rt_advance(&rt, 1);
        q2_event_rt_advance(&rt, 1);        /* ticks 6, 8 */
        q2_event_rt_trigger(&rt, rec);
        q2_event_rt_update(&rt);
    }
    check_eq_i(rt.call_count, 1, "the reloaded counter holds it again");

    q2_event_rt_advance(&rt, 1);
    q2_event_rt_advance(&rt, 1);            /* tick 10 */
    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 2, "until its third arrival after the reload");

    q2_event_rt_free(&rt);
}

static void test_wait_locks_out_consecutive_ticks(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[10];
    u32 rec, k;

    printf("WAIT never decrements on consecutive ticks (0x800276E8)\n");

    memset(pay, 0, sizeof(pay));
    pay[8] = 3;

    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    fix_item(&f, rec, Q2_EVOP_WAIT, 12, pay);
    call_payload(pay, 5);
    fix_item(&f, rec, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");

    /* Ticks 0, 1, 2, 3: the store at 0x800276E8 is the branch's DELAY SLOT, so
     * the stamp is rewritten to tick+1 on the refusing path too and the
     * counter can never move again. */
    for (k = 0; k < 4; k++) {
        q2_event_rt_trigger(&rt, rec);
        q2_event_rt_update(&rt);
        q2_event_rt_advance(&rt, 1);
    }
    check_eq_i(rt.call_count, 0,
               "four consecutive arrivals decrement the counter once, not four times");

    q2_event_rt_free(&rt);
}

/* [WAIT of `len` bytes, reload 2, CALL]. */
static u32 fix_wait_record(fixture *f, u8 len)
{
    u8 pay[10];
    u32 rec;

    memset(pay, 0, sizeof(pay));
    pay[8] = 2;                 /* item+10, read only when len == 12 */

    fix_begin(f);
    rec = fix_record(f, Q2_EVREC_CAT_B);
    fix_item(f, rec, Q2_EVOP_WAIT, len, pay);
    call_payload(pay, 5);
    fix_item(f, rec, Q2_EVOP_CALL, 12, pay);
    fix_end_record(f, rec);
    fix_end(f, 1);
    return rec;
}

static void test_wait_length_gate(void)
{
    fixture f;
    q2_event_rt rt;
    u32 rec;

    printf("WAIT acts on a length of exactly 12 (0x800276CC)\n");

    /* The shape the constructor at 0x80026F10 and the handler both accept:
     * armed, so its first arrival stops the record. */
    rec = fix_wait_record(&f, 12);
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 0, "a 12-byte WAIT holds the record");
    q2_event_rt_free(&rt);

    /* Any other length is refused with a 0 — the delay slot at 0x800276D0 —
     * which does NOT stop the record: the item is simply inert. */
    rec = fix_wait_record(&f, 8);
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture restarts");
    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 1,
               "an 8-byte WAIT is refused and the record runs on");
    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* 0x80026E60..0x80026E74: at load time a record with neither CAT_A nor CAT_C
 * has CAT_B forced into its flags byte. Inert on all 4,179 disc records —
 * their eight distinct flags values all leave the `ori` a no-op — so only a
 * synthetic record can show it. */
static void test_loader_forces_cat_b(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[10];
    u32 none, cat_a;

    printf("the loader's default category (0x80026E68)\n");

    fix_begin(&f);
    none = fix_record(&f, 0);            /* no category bit at all */
    call_payload(pay, 1);
    fix_item(&f, none, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, none);

    cat_a = fix_record(&f, Q2_EVREC_CAT_A);
    call_payload(pay, 2);
    fix_item(&f, cat_a, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, cat_a);
    fix_end(&f, 2);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    check_eq_i(q2_event_rt_flags(&rt, none), Q2_EVREC_CAT_B,
               "a record with no category is loaded as CAT_B");
    check_eq_i(q2_event_rt_flags(&rt, cat_a), Q2_EVREC_CAT_A,
               "one that has a category is left alone");

    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contact(&rt, none);
    q2_event_rt_contact(&rt, cat_a);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 2, "both run on the first frame inside");

    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contact(&rt, none);
    q2_event_rt_contact(&rt, cat_a);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 3,
               "and on the second only the defaulted one repeats");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/* 0x80027794: while gp+16948 is up — the STARTLEV run, the EVE_ replay — a
 * zone gate answers 0 before it reads its name, and the record runs on. */
static void test_initial_pass_mutes_zone_gates(void)
{
    fixture f;
    q2_event_rt rt;
    u8 pay[14];
    u32 rec;

    printf("zone gates are inert during an initial-state pass (0x80027794)\n");

    memset(pay, 0, sizeof(pay));
    memcpy(pay, "Zone1", 5);

    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    fix_item(&f, rec, Q2_EVOP_ZONEGATE, 16, pay);
    fix_item(&f, rec, Q2_EVOP_MOVER_A, 24, NULL);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    check(!rt.initial_pass, "init leaves the pass flag down");
    rt.current_zone = 0;             /* a gate that WOULD fire and abort */
    rt.initial_pass = true;

    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_OK,
          "with the flag up the gate answers 0");
    check(!rt.has_zone_change, "and raises no request");
    check_eq_i(rt.skipped_movers, 1, "and the record runs on to its mover");

    rt.initial_pass = false;
    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE,
          "the same gate fires once the flag is down again");
    check_eq_i(rt.skipped_movers, 1, "and aborts, as it does in play");

    q2_event_rt_free(&rt);
}

/* ------------------------------------------------------------------------- */
/*
 * ACROSS A ZONE CHANGE. The console never reloads COMMON's Events on a zone
 * change — gp+372 is written only by the level load (0x8007AD54) — so every
 * latched flags byte and item bit is still there in the next zone. The port
 * re-initialises the runtime; q2_event_carry is what it keeps across that.
 */
typedef struct carry_world {
    u32 oneshot;     /* CAT_A|ONESHOT [CALL 1]                        */
    u32 mixed;       /* CAT_B [CALL|ONESHOT 2, CALL 3]                */
    u32 enter;       /* CAT_A [CALL 4] — the volume the player is in  */
} carry_world;

static void fix_carry_world(fixture *f, carry_world *w, u8 plain_index)
{
    u8 pay[10];

    fix_begin(f);
    w->oneshot = fix_record(f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
    call_payload(pay, 1);
    fix_item(f, w->oneshot, Q2_EVOP_CALL, 12, pay);
    fix_end_record(f, w->oneshot);

    w->mixed = fix_record(f, Q2_EVREC_CAT_B);
    call_payload(pay, 2);
    fix_item(f, w->mixed, (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT), 12, pay);
    call_payload(pay, plain_index);
    fix_item(f, w->mixed, Q2_EVOP_CALL, 12, pay);
    fix_end_record(f, w->mixed);

    w->enter = fix_record(f, Q2_EVREC_CAT_A);
    call_payload(pay, 4);
    fix_item(f, w->enter, Q2_EVOP_CALL, 12, pay);
    fix_end_record(f, w->enter);
    fix_end(f, 3);
}

/* Everything a zone's worth of play leaves behind in the fixture. */
static void play_carry_world(q2_event_rt *rt, const carry_world *w)
{
    q2_event_rt_trigger(rt, w->oneshot);
    q2_event_rt_update(rt);
    q2_event_rt_trigger(rt, w->mixed);
    q2_event_rt_update(rt);

    q2_event_rt_contacts_begin(rt);
    q2_event_rt_contact(rt, w->enter);
    q2_event_rt_contacts_end(rt);
    q2_event_rt_update(rt);
}

static void test_carry_keeps_the_latches(void)
{
    fixture f;
    q2_event_rt rt;
    q2_event_carry *carry;
    call_tally tally;
    carry_world w;
    u32 item2;

    printf("the latches survive a zone change (0x8007901C)\n");

    carry = (q2_event_carry *)calloc(1, sizeof(*carry));
    check(carry != NULL, "a carry to hold them");
    if (!carry)
        return;

    fix_carry_world(&f, &w, 3);
    item2 = w.mixed + 4;             /* the first item follows the header */

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    memset(&tally, 0, sizeof(tally));
    rt.on_call      = tally_hook;
    rt.on_call_user = &tally;
    play_carry_world(&rt, &w);

    check_eq_i(q2_event_rt_flags(&rt, w.oneshot), 0xC9,
               "the one-shot record is spent in the old zone");
    check_eq_i(q2_event_rt_item_flags(&rt, item2), Q2_EVOP_DISABLED,
               "and so is the one-shot item");
    check_eq_i(q2_event_rt_flags(&rt, w.enter) & Q2_EVREC_RT2, Q2_EVREC_RT2,
               "and the player is inside the CAT_A volume");

    q2_event_rt_carry_out(&rt, carry);
    check_eq_i(carry->size, f.size, "the carry names the chunk it came from");
    check_eq_i(carry->record_count, 3, "and its record count");
    q2_event_rt_free(&rt);

    /* The zone load: a fresh runtime over the same COMMON chunk. */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the next zone's runtime");
    check_eq_i(q2_event_rt_flags(&rt, w.oneshot), 0x48,
               "which on its own has put the spent record back on the shelf");

    q2_event_rt_carry_in(&rt, carry);
    check_eq_i(q2_event_rt_flags(&rt, w.oneshot), 0xC9,
               "the carry brings the spent record back spent");
    check_eq_i(q2_event_rt_item_flags(&rt, item2), Q2_EVOP_DISABLED,
               "and the spent item back retired");
    check_eq_i(q2_event_rt_flags(&rt, w.enter),
               Q2_EVREC_CAT_A | Q2_EVREC_RT2 | Q2_EVREC_HASRUN,
               "and the contact bit with the rest of the byte");

    memset(&tally, 0, sizeof(tally));
    rt.on_call      = tally_hook;
    rt.on_call_user = &tally;

    q2_event_rt_trigger(&rt, w.oneshot);
    q2_event_rt_update(&rt);
    check_eq_i(tally.by_index[1], 0,
               "so the one-shot record does not run again");

    q2_event_rt_trigger(&rt, w.mixed);
    q2_event_rt_update(&rt);
    check_eq_i(tally.by_index[2], 0, "nor the one-shot item");
    check_eq_i(tally.by_index[3], 1, "while its repeatable neighbour does");

    /* Still standing in the volume the zone change happened in: with RT2
     * carried this is not an entry, so a CAT_A record does not fire twice. */
    q2_event_rt_contacts_begin(&rt);
    q2_event_rt_contact(&rt, w.enter);
    q2_event_rt_contacts_end(&rt);
    q2_event_rt_update(&rt);
    check_eq_i(tally.by_index[4], 0,
               "standing in the same CAT_A volume is not a new entry");

    q2_event_rt_free(&rt);
    free(carry);
}

/* A carry from one script must not land on another. Same size, same record
 * count, one operand byte different: the identity check is what refuses it. */
static void test_carry_refuses_another_script(void)
{
    fixture a, b;
    q2_event_rt rt;
    q2_event_carry *carry;
    carry_world wa, wb;

    printf("a carry from another script is refused\n");

    carry = (q2_event_carry *)calloc(1, sizeof(*carry));
    check(carry != NULL, "a carry to hold them");
    if (!carry)
        return;

    fix_carry_world(&a, &wa, 3);
    fix_carry_world(&b, &wb, 5);
    check_eq_i(a.size, b.size, "the two chunks are the same size");

    check(q2_event_rt_init(&rt, &a.ev) == Q2_OK, "script A runs");
    play_carry_world(&rt, &wa);
    q2_event_rt_carry_out(&rt, carry);
    q2_event_rt_free(&rt);

    check(q2_event_rt_init(&rt, &b.ev) == Q2_OK, "script B loads");
    q2_event_rt_carry_in(&rt, carry);
    check_eq_i(q2_event_rt_flags(&rt, wb.oneshot), 0x48,
               "A's latches are not applied to B");
    check_eq_i(q2_event_rt_item_flags(&rt, wb.mixed + 4), 0,
               "item bits included");
    q2_event_rt_replay(&rt, carry);
    check_eq_i(rt.replayed_count, 0, "and B is not replayed from A's bits");
    q2_event_rt_free(&rt);

    /* A runtime with nothing in it carries nothing. */
    memset(&rt, 0, sizeof(rt));
    q2_event_rt_carry_out(&rt, carry);
    check_eq_i(carry->size, 0, "an empty runtime leaves the carry empty");

    free(carry);
}

/* ------------------------------------------------------------------------- */
/*
 * THE EVE_ REPLAY, 0x8002936C. After the zone load the console re-dispatches
 * every item of every spent record, and every spent one-shot item elsewhere,
 * with gp+16948 up — which is what puts the new zone's rebuilt doors back the
 * way the script left them.
 */
typedef struct replay_log {
    const q2_event_rt *rt;      /* set to watch `initial_pass` from inside */
    u32 movers;
    u32 calls_in_pass;          /* CALLs that found the pass flag up       */
    u32 by_index[256];
} replay_log;

static void replay_mover_hook(void *user, const q2_event_item *item)
{
    (void)item;
    ((replay_log *)user)->movers++;
}

static void replay_call_hook(void *user, const q2_event_item *item, u8 index)
{
    replay_log *log = (replay_log *)user;

    (void)item;
    log->by_index[index]++;
    if (log->rt && log->rt->initial_pass)
        log->calls_in_pass++;
}

/* On the first run only: CALL 5 is a predicate that says no. */
static void abort_on_5(void *user, const q2_event_item *item, u8 index)
{
    q2_event_rt *rt = (q2_event_rt *)user;

    (void)item;
    if (index == 5)
        rt->abort_record = true;
}

static void test_replay_reopens_a_spent_gate_door(void)
{
    fixture f;
    q2_event_rt rt;
    q2_event_carry *carry;
    replay_log log;
    u8 pay[14];
    u32 rec;

    printf("the replay opens the door behind a spent zone gate (0x8002936C)\n");

    carry = (q2_event_carry *)calloc(1, sizeof(*carry));
    check(carry != NULL, "a carry to hold them");
    if (!carry)
        return;

    memset(pay, 0, sizeof(pay));
    memcpy(pay, "Zone1", 5);

    fix_begin(&f);
    rec = fix_record(&f, (u8)(Q2_EVREC_CAT_B | Q2_EVREC_ONESHOT));
    fix_item(&f, rec, Q2_EVOP_ZONEGATE, 16, pay);
    fix_item(&f, rec, Q2_EVOP_MOVER_A, 24, NULL);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    /* Zone 0: the gate fires and stops the record before its door. */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.current_zone = 0;
    q2_event_rt_trigger(&rt, rec);
    check(q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE, "the gate fires");
    check_eq_i(rt.skipped_movers, 0, "and the door behind it never ran");
    check_eq_i(q2_event_rt_flags(&rt, rec), 0xD1, "the record is spent");
    q2_event_rt_carry_out(&rt, carry);
    q2_event_rt_free(&rt);

    /* Zone 1: carried, then replayed once the owner's hooks are in. */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "zone 1's runtime");
    rt.current_zone = 1;
    q2_event_rt_carry_in(&rt, carry);
    memset(&log, 0, sizeof(log));
    rt.on_mover      = replay_mover_hook;
    rt.on_mover_user = &log;

    q2_event_rt_replay(&rt, carry);
    check_eq_i(log.movers, 1,
               "the replay reaches the door the original run never did");
    check(!rt.has_zone_change,
          "and its gate asks for no zone: gp+16948 is up (0x800294B4)");
    check(!rt.initial_pass, "the flag is down again afterwards (0x80029754)");
    check_eq_i(rt.replayed_count, 2, "both items of the spent record replayed");
    check_eq_i(q2_event_rt_flags(&rt, rec), 0xD1, "and the record stays spent");

    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(log.movers, 1, "a spent record still refuses a real trigger");
    q2_event_rt_free(&rt);

    /*
     * In zone 1 the same-zone refusal would have stopped that gate on its own
     * (0x800791E0). In a zone the gate does NOT name, only the pass flag
     * does: without it the replayed gate would be accepted and ask for Zone1
     * again — a zone load requested by a state restore. (The door would open
     * either way: the replay discards what a handler answers, 0x800295D4.)
     */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "zone 2's runtime");
    rt.current_zone = 2;
    q2_event_rt_carry_in(&rt, carry);
    memset(&log, 0, sizeof(log));
    rt.on_mover      = replay_mover_hook;
    rt.on_mover_user = &log;

    q2_event_rt_replay(&rt, carry);
    check(!rt.has_zone_change,
          "in another zone the replayed gate still asks for nothing");
    check_eq_i(log.movers, 1, "and the door behind it still opens");

    q2_event_rt_free(&rt);
    free(carry);
}

static void test_replay_picks_only_spent_items(void)
{
    fixture f;
    q2_event_rt rt;
    q2_event_carry *carry;
    replay_log log;
    u8 pay[10];
    u32 rec, os1, os2;

    printf("outside a spent record only spent one-shot items replay\n");

    carry = (q2_event_carry *)calloc(1, sizeof(*carry));
    check(carry != NULL, "a carry to hold them");
    if (!carry)
        return;

    /* [CALL|OS 1, CALL 5 (says no), CALL|OS 2, CALL 3] — a repeatable record
     * that stops halfway, so one of its one-shot items is spent and the other
     * never ran. */
    fix_begin(&f);
    rec = fix_record(&f, Q2_EVREC_CAT_B);
    call_payload(pay, 1);
    os1 = fix_item(&f, rec, (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT), 12, pay);
    call_payload(pay, 5);
    fix_item(&f, rec, Q2_EVOP_CALL, 12, pay);
    call_payload(pay, 2);
    os2 = fix_item(&f, rec, (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT), 12, pay);
    call_payload(pay, 3);
    fix_item(&f, rec, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, rec);
    fix_end(&f, 1);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.on_call      = abort_on_5;
    rt.on_call_user = &rt;
    q2_event_rt_trigger(&rt, rec);
    q2_event_rt_update(&rt);
    check_eq_i(q2_event_rt_item_flags(&rt, os1), Q2_EVOP_DISABLED,
               "the first one-shot item is spent");
    check_eq_i(q2_event_rt_item_flags(&rt, os2), 0,
               "the second never ran");
    check_eq_i(q2_event_rt_flags(&rt, rec) & Q2_EVREC_DISABLED, 0,
               "and the record itself is not spent");
    q2_event_rt_carry_out(&rt, carry);
    q2_event_rt_free(&rt);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the next zone's runtime");
    q2_event_rt_carry_in(&rt, carry);
    memset(&log, 0, sizeof(log));
    log.rt          = &rt;
    rt.on_call      = replay_call_hook;
    rt.on_call_user = &log;
    q2_event_rt_replay(&rt, carry);

    check_eq_i(log.calls_in_pass, 1,
               "the CALL hook sees the pass flag up: a STRING can keep quiet");
    check_eq_i(log.by_index[1], 1,
               "the spent one-shot item is dispatched again");
    check_eq_i(log.by_index[2], 0, "the one that never ran is not");
    check_eq_i(log.by_index[5] + log.by_index[3], 0,
               "nor is anything that is not one-shot");
    check_eq_i(rt.replayed_count, 1, "one item replayed in all");
    check_eq_i(q2_event_rt_item_flags(&rt, os1), Q2_EVOP_DISABLED,
               "and the dispatcher's latch retires it again (0x80027498)");
    check_eq_i(rt.ran_count, 0,
               "the replay is not a run of the record: nothing was latched");

    q2_event_rt_free(&rt);
    free(carry);
}

/* A replayed TRIGGER is drained after its record, through the usual gate. */
static void test_replay_drains_through_the_gate(void)
{
    fixture f;
    q2_event_rt rt;
    q2_event_carry *carry;
    replay_log log;
    u8 pay[32];
    u16 targets[2];
    u32 src, open, shut;

    printf("the replay drains its queue through the gate (0x80029620)\n");

    carry = (q2_event_carry *)calloc(1, sizeof(*carry));
    check(carry != NULL, "a carry to hold them");
    if (!carry)
        return;

    /* Chunk order matters to the replay: src, then open, then shut. src is a
     * 4-byte header and one TRIGGER item, and each target is a header and a
     * 12-byte CALL, so their offsets are known before they are written. */
    fix_begin(&f);
    src = fix_record(&f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
    targets[0] = (u16)(src + 4 + list_len(2));   /* open */
    targets[1] = (u16)(targets[0] + 16);         /* shut */
    list_payload(pay, targets, 2);
    fix_item(&f, src, Q2_EVOP_TRIGGER, list_len(2), pay);
    fix_end_record(&f, src);

    open = fix_record(&f, Q2_EVREC_CAT_B);
    call_payload(pay, 7);
    fix_item(&f, open, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, open);

    shut = fix_record(&f, Q2_EVREC_CAT_B);
    call_payload(pay, 8);
    fix_item(&f, shut, Q2_EVOP_CALL, 12, pay);
    fix_end_record(&f, shut);
    fix_end(&f, 3);

    check_eq_i(open, targets[0], "the first target is the open record");
    check_eq_i(shut, targets[1], "the second is the disabled one");

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    rt.flags[slot_of(&rt, shut)] |= Q2_EVREC_DISABLED;  /* a DISABLE earlier */
    q2_event_rt_trigger(&rt, src);
    q2_event_rt_update(&rt);
    check_eq_i(rt.call_count, 1,
               "in play the TRIGGER runs the open record only");
    q2_event_rt_carry_out(&rt, carry);
    q2_event_rt_free(&rt);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the next zone's runtime");
    q2_event_rt_carry_in(&rt, carry);
    memset(&log, 0, sizeof(log));
    rt.on_call      = replay_call_hook;
    rt.on_call_user = &log;
    q2_event_rt_replay(&rt, carry);

    check_eq_i(log.by_index[7], 1, "the replayed TRIGGER runs the open record");
    check_eq_i(log.by_index[8], 0, "and the gate refuses the disabled one");
    check_eq_i(rt.pending_count, 0, "the queue is empty afterwards");

    q2_event_rt_free(&rt);
    free(carry);
}

/*
 * 128 bit positions (0x80029094; 0x800294DC). Past them the EVE_ block holds
 * nothing and the replay reaches nothing — but the latches themselves were
 * never in the block: they are in memory, and memory is what the carry
 * keeps. The disc never gets here (89 positions at most, LAB), so only a
 * synthetic chunk shows it.
 */
static void test_replay_stops_at_128_positions(void)
{
    enum { RECORDS = 130 };
    fixture f;
    q2_event_rt rt;
    q2_event_carry *carry;
    replay_log *log;
    u32 rec[RECORDS];
    u32 k, replayed = 0;

    printf("the replay's bits stop at 128 positions (0x80029094)\n");

    carry = (q2_event_carry *)calloc(1, sizeof(*carry));
    log   = (replay_log *)calloc(1, sizeof(*log));
    check(carry != NULL && log != NULL, "room for the carry and the log");
    if (!carry || !log) {
        free(carry);
        free(log);
        return;
    }

    fix_begin(&f);
    for (k = 0; k < RECORDS; k++) {
        u8 pay[10];

        rec[k] = fix_record(&f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
        call_payload(pay, (u8)k);
        fix_item(&f, rec[k], Q2_EVOP_CALL, 12, pay);
        fix_end_record(&f, rec[k]);
    }
    fix_end(&f, RECORDS);
    check(f.size <= sizeof(f.raw), "the chunk fits the fixture");

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    for (k = 0; k < RECORDS; k++) {
        q2_event_rt_trigger(&rt, rec[k]);
        q2_event_rt_update(&rt);
    }
    check_eq_i(rt.call_count, RECORDS, "every record has run once");
    q2_event_rt_carry_out(&rt, carry);
    q2_event_rt_free(&rt);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the next zone's runtime");
    q2_event_rt_carry_in(&rt, carry);
    rt.on_call      = replay_call_hook;
    rt.on_call_user = log;
    q2_event_rt_replay(&rt, carry);

    for (k = 0; k < RECORDS; k++)
        replayed += log->by_index[k];
    check_eq_i(replayed, 128, "records at positions 0..127 are replayed");
    check_eq_i(log->by_index[127], 1, "the 128th is the last");
    check_eq_i(log->by_index[128] + log->by_index[129], 0,
               "the 129th and 130th have no bit");
    check_eq_i(q2_event_rt_flags(&rt, rec[129]), 0xC9,
               "yet the 130th is still spent: the carry kept its byte");
    q2_event_rt_free(&rt);

    /* Without the carry — the replay on a runtime that did not keep its
     * bytes — spent-ness comes back only through the bits the block has. */
    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "a runtime without the carry");
    q2_event_rt_replay(&rt, carry);
    check_eq_i(q2_event_rt_flags(&rt, rec[0]) & 0x81, 0x81,
               "the replay ORs 0x81 back into a record it has a bit for");
    check_eq_i(q2_event_rt_flags(&rt, rec[129]), 0x48,
               "and cannot for one past the cap");
    q2_event_rt_free(&rt);

    free(carry);
    free(log);
}

/*
 * The other half of the position rule: a ONESHOT item takes a bit position of
 * its own (0x8002955C tests authored bit 6, 0x800295A8 increments), not only a
 * record (0x80029524). The test above uses plain CALLs, so it would pass if
 * items took none. Here the records alone fit — 127 of them — and three
 * one-shot items in the first record push the last two past the cap.
 */
static void test_oneshot_items_take_positions(void)
{
    enum { RECORDS = 127, ITEMS0 = 3 };
    fixture f;
    q2_event_rt rt;
    q2_event_carry *carry;
    replay_log *log;
    u32 rec[RECORDS];
    u32 k;

    printf("a one-shot item takes an EVE_ position too (0x800295A8)\n");

    carry = (q2_event_carry *)calloc(1, sizeof(*carry));
    log   = (replay_log *)calloc(1, sizeof(*log));
    check(carry != NULL && log != NULL, "room for the carry and the log");
    if (!carry || !log) {
        free(carry);
        free(log);
        return;
    }

    fix_begin(&f);
    for (k = 0; k < RECORDS; k++) {
        u8 pay[10];

        rec[k] = fix_record(&f, (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT));
        if (k == 0) {
            u32 i;

            /* Positions 1..3, after the record's own position 0. */
            for (i = 0; i < ITEMS0; i++) {
                call_payload(pay, (u8)(200 + i));
                fix_item(&f, rec[k], (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT), 12,
                         pay);
            }
        } else {
            call_payload(pay, (u8)k);
            fix_item(&f, rec[k], Q2_EVOP_CALL, 12, pay);
        }
        fix_end_record(&f, rec[k]);
    }
    fix_end(&f, RECORDS);
    check(f.size <= sizeof(f.raw), "the chunk fits the fixture");

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the fixture starts a runtime");
    for (k = 0; k < RECORDS; k++) {
        q2_event_rt_trigger(&rt, rec[k]);
        q2_event_rt_update(&rt);
    }
    check_eq_i(rt.call_count, RECORDS - 1 + ITEMS0, "every record has run once");
    q2_event_rt_carry_out(&rt, carry);
    q2_event_rt_free(&rt);

    check(q2_event_rt_init(&rt, &f.ev) == Q2_OK, "the next zone's runtime");
    q2_event_rt_carry_in(&rt, carry);
    rt.on_call      = replay_call_hook;
    rt.on_call_user = log;
    q2_event_rt_replay(&rt, carry);

    check_eq_i(log->by_index[200] + log->by_index[201] + log->by_index[202],
               ITEMS0, "the first record replays with its one-shot items");
    check_eq_i(log->by_index[124], 1,
               "record 124 sits at position 127, the last with a bit");
    check_eq_i(log->by_index[125] + log->by_index[126], 0,
               "records 125 and 126 sit at 128 and 129: the items used their bits");
    q2_event_rt_free(&rt);

    free(carry);
    free(log);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC event runtime tests\n\n");

    test_item_oneshot_latch();
    test_item_latch_precedes_validation();
    test_item_latch_precedes_dispatch();
    test_record_latch_and_enable_rearm();
    test_rearmed_record_keeps_its_spent_items();
    test_record_latch_precedes_items();
    test_record_latch_keeps_a_self_disable();
    test_deferred_resume_runs_the_prologue();
    test_zonegate_abort();
    test_initial_pass_mutes_zone_gates();
    test_catc_leave_edge_runs_nothing();
    test_wait_counts_non_consecutive_arrivals();
    test_wait_locks_out_consecutive_ticks();
    test_wait_length_gate();
    test_loader_forces_cat_b();
    test_carry_keeps_the_latches();
    test_carry_refuses_another_script();
    test_replay_reopens_a_spent_gate_door();
    test_replay_picks_only_spent_items();
    test_replay_drains_through_the_gate();
    test_replay_stops_at_128_positions();
    test_oneshot_items_take_positions();

    /*
     * NOT TESTED HERE, ON PURPOSE: the pending queue's de-duplication
     * (q2_event_rt_trigger). It is a documented divergence this round only
     * re-justified in a comment — BOSS1 COMMON record 0x374 reaching 0x1e0
     * twice is its whole cost on the disc — so there is no behaviour change
     * for a test to catch, and a test that passes before and after proves
     * nothing about this round.
     */

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
