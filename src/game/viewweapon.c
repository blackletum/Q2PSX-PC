#include "random.h"
#include "viewweapon.h"

#include "trig.h"

#include <stdlib.h>
#include <string.h>

/* 1.0.12 one — the hyperblaster's barrel at rest, 0x8004FCD8. */
#define VW_HYPER_RAMP_ONE  4096

/* The part 0x8004FED0 hands 0x8006D43C, in its `a2`. */
#define VW_HYPER_BARREL_PART 6

/* The four weapons the frame driver 0x8004FEE8 has an arm for. */
#define VW_MACHINEGUN    4
#define VW_CHAINGUN      5
#define VW_HAND_GRENADE  6
#define VW_HYPERBLASTER  9

/* ------------------------------------------------------------------------- */
/* Keys                                                                       */
/* ------------------------------------------------------------------------- */
static const q2_vm_key *clip_key(const q2_vm_clip *c, u32 frame)
{
    if (!c || frame >= c->count)
        return NULL;
    return &c->key[frame];
}

static const q2_vm_clip *current_clip(const q2_viewweapon *vw)
{
    return q2_vm_clip_get(vw->tab, vw->weapon, vw->state);
}

static const q2_vm_key *current_key(const q2_viewweapon *vw)
{
    return clip_key(current_clip(vw), vw->frame);
}

/*
 * The machine's whole "has the animation finished" test, 0x8004F93C and its
 * three siblings: `clip_start + 20*frame == clip_end`. The console compares for
 * EQUALITY, so a frame index that somehow walked past the end would never test
 * done; `>=` is the same answer for every index the retail bank can produce
 * and stops a malformed table from running off the end here.
 */
static bool clip_done(const q2_vm_clip *c, u32 frame)
{
    return !c || frame >= c->count;
}

/*
 * 0x8004F1F8. A key with a non-zero jitter takes `((rand() * duration) >> 15 +
 * 1) * 10`; every other key takes its duration as written. The multiply by ten
 * is spelled out in the original as a shift-add chain, and it is why a jittered
 * key is an order of magnitude longer than its stored value rather than a small
 * perturbation of it.
 */
static s32 key_duration(const q2_vm_key *k)
{
    if (!k)
        return 0;
    if (k->jitter == 0)
        return k->duration;

    {
        s32 r = (s32)(q2_random_next() & 0x7FFF);
        s32 n = ((r * (s32)k->duration) >> 15) + 1;
        return n * 10;
    }
}

/*
 * `base + (key - base) * (total - left) / total`, as one of the six blocks at
 * 0x8004EF80…0x8004F1C0 (and the display's at 0x8004F284…0x8004F3E0) computes
 * it: the difference is formed from two `lh` reads, `mult` keeps the low word
 * of the product, `div` truncates toward zero, and the sum goes back through
 * `lhu`/`addu`/`sh`, so it wraps as a halfword. There is no clamp — a key whose
 * remaining time exceeds its total (the cooked-off grenade's 150 at 0x8004AA1C)
 * extrapolates backwards exactly as the console does.
 *
 * A zero total would reach the `break 7` the compiler put after each `div`;
 * no retail key has one, and here it simply lands on the key.
 */
static s16 key_lerp(s16 base, s16 to, s32 total, s32 left)
{
    s32 diff = (s32)to - (s32)base;
    s32 prod;

    if (total == 0)
        return to;
    prod = (s32)(u32)(u64)((s64)diff * (s64)(total - left));
    return (s16)(u16)((u32)(u16)base + (u32)(prod / total));
}

/*
 * The KEY RELOAD — 0x8004EF5C…0x8004F264, which runs inside the substep loop
 * whenever the frame or the state differs from what it was at the top of the
 * substep, and at no other time.
 *
 * It does two things in a fixed order. First the six base halfwords (+224…+228
 * rotation, +236…+240 translation) take the fraction of the OLD key that was
 * actually played — `old` is the clip the substep began with and `old_frame`
 * its frame then, and the fraction uses the clock as the frame driver left it.
 * Then the new key is loaded from the clip and frame the machine and the arms
 * have settled on: its duration, jittered through rand() if it asks, into both
 * +48 and +80.
 *
 * Doing it here, after both drivers, is what makes a machinegun's held wrap
 * (frame 2 → 0 in the arm) load key 0's clock instead of playing key 2's
 * duration toward key 0's pose, and what makes the LOWER hold, which rewinds
 * the frame back to where the substep found it, load nothing at all.
 */
static void reload_key(q2_viewweapon *vw, const q2_vm_clip *old, u32 old_frame)
{
    const q2_vm_key *was = clip_key(old, old_frame);
    const q2_vm_key *k;
    int i;

    if (was) {
        for (i = 0; i < 3; i++) {
            vw->from_r[i] = key_lerp(vw->from_r[i], was->r[i], vw->total, vw->left);
            vw->from_t[i] = key_lerp(vw->from_t[i], was->t[i], vw->total, vw->left);
        }
    }

    k = current_key(vw);
    vw->total = key_duration(k);
    vw->left  = vw->total;

    if (k && k->event != Q2_VM_EVENT_NONE) {
        vw->event = k->event;
        vw->event_pending = true;
    }
}

/* 0x8004F280…0x8004F3E0: the display value, computed fresh after the substep
 * loop from the base and the current key without being written back. */
static void interpolate(q2_viewweapon *vw)
{
    const q2_vm_key *k = current_key(vw);
    int i;

    if (!k)
        return;

    for (i = 0; i < 3; i++) {
        vw->cur_t[i] = key_lerp(vw->from_t[i], k->t[i], vw->total, vw->left);
        vw->cur_r[i] = key_lerp(vw->from_r[i], k->r[i], vw->total, vw->left);
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
void q2_vw_init(q2_viewweapon *vw, const q2_vm_tables *tab, int weapon)
{
    if (!vw)
        return;

    memset(vw, 0, sizeof(*vw));
    /* The entity allocator at 0x8006C1B8..0x8006C1FC establishes these before
     * the view-model constructor writes its positive +0xF4 at 0x8004F750. */
    vw->scale      = Q2_ONE_12;
    vw->fade       = Q2_ONE_12;
    vw->light_selector = 1;
    vw->glow[0]    = 0x40;
    vw->glow[1]    = 0x40;
    vw->glow[2]    = 0x40;
    vw->frame_sound = -1;
    vw->anim_end    = -1;
    vw->hyper_ramp  = VW_HYPER_RAMP_ONE;
    vw->tab     = tab;
    vw->weapon  = (weapon >= 0 && weapon < Q2_VM_SLOTS) ? weapon : 0;
    vw->pending = vw->weapon;

    /*
     * A level start begins in LOWER, not RAISE — 0x8004F7A4, `sw 3, 72(s2)`,
     * with frame 0 (`sw zero, 76`), total 1 (`sw 1, 48`), left 0
     * (`sw zero, 80`) and the dry latch cleared (`sh zero, 216`).
     *
     * The note here used to cite 0x8004FA48, which is the state the machine
     * lands in AFTER A SWAP — a different moment. The difference is visible:
     * the first substep finds the key spent, so the lower plays out from its
     * second key and then goes through the swap arm, which is what resolves
     * the model, so the console shows no weapon at all for the first frames of
     * a level and then raises it. Starting in RAISE skips that and has the gun
     * present from frame zero.
     *
     * The constructor loads NO key — no clock from the clip, no jitter roll,
     * no event — and it writes two fields the driver's arms read before
     * anything else does: the chaingun's rate +44 is 1 (0x8004F7C0) and the
     * per-frame cache +218 is 1 (0x8004F7B0, `s1` from 0x8004F748), so a first
     * chaingun shot's frame 0 is a NEW frame and plays the spin-up.
     */
    vw->state = Q2_VM_LOWER;
    vw->frame = 0;
    vw->total = 1;
    vw->left  = 0;
    vw->spin_rate       = 1;
    vw->last_fire_frame = 1;
    interpolate(vw);
}

void q2_vw_select(q2_viewweapon *vw, int weapon)
{
    if (!vw || weapon < 0 || weapon >= Q2_VM_SLOTS)
        return;
    vw->pending = weapon;

    /*
     * 0x8004ECEC. Asking for a weapon arms the 70-tick countdown; LOWER holds
     * at the bottom of its arc until it expires, which is what stops a fast
     * double-tap from swapping the model twice in as many frames.
     *
     * The cycle routine that does this (0x8004ECB4, called at 0x8003A8E0)
     * runs before the driver (0x8003AD98) in the same player think, and on the
     * frame it arms the countdown it does NOT also count it down — that is the
     * other arm, 0x8004ED44. So the advance that follows a selection sees the
     * full seventy; `switch_armed` is how it knows.
     */
    vw->switch_ticks = Q2_VW_SWITCH_TICKS;
    vw->switch_armed = true;
}

void q2_vw_request(q2_viewweapon *vw, int weapon)
{
    if (!vw || weapon < 0 || weapon >= Q2_VM_SLOTS)
        return;
    vw->pending = weapon;
}

void q2_vw_arm_switch(q2_viewweapon *vw)
{
    if (!vw)
        return;
    vw->switch_ticks = Q2_VW_SWITCH_TICKS;
    vw->switch_armed = true;
}

void q2_vw_set_model(q2_viewweapon *vw, const q2_model *model)
{
    if (vw)
        vw->model = model;
}

const char *q2_vw_model_name(const q2_viewweapon *vw)
{
    if (!vw || !vw->tab)
        return NULL;
    return vw->tab->model_name[vw->weapon];
}

bool q2_vw_take_refire(q2_viewweapon *vw)
{
    bool r;

    if (!vw)
        return false;
    r = vw->refire;
    vw->refire = false;
    return r;
}

bool q2_vw_take_event(q2_viewweapon *vw, s16 *event_out)
{
    if (!vw || !vw->event_pending)
        return false;
    if (event_out)
        *event_out = vw->event;
    vw->event_pending = false;
    return true;
}

bool q2_vw_wants_fire(const q2_viewweapon *vw)
{
    return vw && vw->state == Q2_VM_IDLE && !vw->fire_latch &&
           vw->pending == vw->weapon;
}

/* ------------------------------------------------------------------------- */
/* One advance's fire plumbing                                                */
/* ------------------------------------------------------------------------- */
/*
 * What an advance knows about its fire function. `precomputed` is the one
 * report the caller made before the call (see q2_vw_advance_firing): it
 * answers the idle arm's first ask and nothing else.
 */
typedef struct vw_fire_ctx {
    bool              held;
    q2_vw_fire_result fired;
    bool              precomputed;
    q2_vw_fire_fn     fire;
    void             *user;
    bool              dry_seen;    /* a dry pass with no auto_select */
} vw_fire_ctx;

/* The console's own word for a fire function's answer: 1 for a shot (the
 * blaster's 0x8004C1A4), 2 for dry, 0 for neither. The port's enum numbers
 * them differently — a shot is 0 there — so the driver's return, which the
 * player think tests for nonzero, is kept in the console's encoding. */
static s16 native_fire_word(q2_vw_fire_result r)
{
    return r == Q2_VW_FIRED ? 1 : r == Q2_VW_FIRE_DENIED ? 2 : 0;
}

/* A fire function ran: a shot or a spawn that failed (the console's 1 or 0). */
static bool fire_ran(q2_vw_fire_result r)
{
    return r == Q2_VW_FIRED || r == Q2_VW_FIRE_FAILED;
}

/* The idle arm's `jalr` at 0x8004FB30. */
static q2_vw_fire_result idle_fire(q2_viewweapon *vw, vw_fire_ctx *cx)
{
    if (cx->precomputed) {
        cx->precomputed = false;
        return cx->fired;
    }
    /*
     * After a dry pass the console never asks again in the same call: its
     * auto-select has already changed the selection, so the next pass lowers
     * (0x8004FAB4) before it reaches this arm. A caller that reselects only
     * after the advance has not changed it yet, and asking it again would be
     * a call the console never makes; the trigger reads as up instead.
     */
    if (cx->dry_seen)
        return Q2_VW_FIRE_NONE;
    if (cx->fire)
        return cx->fire(cx->user, vw->weapon, 1);
    return cx->fired == Q2_VW_FIRE_ASK ? Q2_VW_FIRE_NONE : cx->fired;
}

/*
 * One of the arms' `jalr`s — 0x8004FFE0, 0x8005020C, 0x800502D8 — and the
 * `sh v0, 0(s6)` after each, which stores the answer over the substep's result,
 * a quiet zero included. Without a callback the round is queued for
 * q2_vw_take_frame_fires and its outcome deferred to q2_vw_frame_fire_result;
 * Q2_VW_FIRE_ASK is the "deferred" answer the arms then ignore.
 */
static q2_vw_fire_result frame_fire(q2_viewweapon *vw, int rounds,
                                    vw_fire_ctx *cx)
{
    q2_vw_fire_result r;

    if (cx->fire) {
        r = cx->fire(cx->user, vw->weapon, rounds);
        vw->result = native_fire_word(r);
        return r;
    }
    vw->frame_fires += (u32)rounds;
    vw->result = 1;
    return Q2_VW_FIRE_ASK;
}

/* ------------------------------------------------------------------------- */
/* The state machine — 0x8004F87C, called at the top of EVERY substep         */
/* ------------------------------------------------------------------------- */
/*
 * The dry pass — 0x8004FB5C…0x8004FB98, reached when the idle arm's fire
 * function answered 2 or when the dry latch was already up.
 *
 * `fire_latch` IS THE DRY LATCH — viewmodel+216. Inside the state machine the
 * halfword is only ever CLEARED, `sh zero` at 0x8004F968 and in this pass's
 * delay slot at 0x8004FB64; the only two non-zero writes in the image are
 * 0x80050230 and 0x800502FC, the chaingun and hyperblaster arms after a fire
 * function reported 2. An earlier version also SET it here, on an idle shot
 * that came back dry, and so swallowed the next pass's ask; the console asks
 * again on every pass the trigger is down (the native driver fixture pins it).
 *
 * The pass then calls 0x800506C4, which is not a neighbour search but the
 * AUTO-SELECT: it walks the priority list at 0x8009DB7C for the first owned
 * weapon with at least the 0x8009DB4C minimum of its ammo and writes it into
 * the selection (client+102, 0x80050744) AND this record's pending weapon
 * (+214, 0x8005074C). So the very next substep's pass sees `weapon !=
 * selection` and lowers — in the same driver call. `auto_select` lets a caller
 * answer it in place; one that cannot drains `refire` after the advance and
 * reselects a tick late. 0x80050758 (twice) then fills the next/previous
 * neighbours at client+100/+96, which this module has no counterpart for.
 */
static void dry_pass(q2_viewweapon *vw, vw_fire_ctx *cx)
{
    vw->fire_latch = false;
    vw->refire     = true;
    if (vw->auto_select) {
        int w = vw->auto_select(cx->user, vw->weapon);

        if (w >= 0 && w < Q2_VM_SLOTS)
            vw->pending = w;
    } else {
        cx->dry_seen = true;
    }
}

/*
 * One pass, in the console's order. `clip` is the clip the driver handed in —
 * the one for the weapon and state at the top of the substep (0x8004EEE0) —
 * and every "done" test below is against it, including the FIRE test at the
 * end, after the idle arm may already have changed the state.
 *
 * Returns the halt flag, the halfword at 0x800B2B7C that the driver turns into
 * "consume no more of this frame" (0x8004EF14). Only the LOWER hold raises it.
 */
static bool machine_pass(q2_viewweapon *vw, const q2_vm_clip *clip,
                         vw_fire_ctx *cx, bool *swapped)
{
    s16 result = 0;       /* s4 — 0x8004F8DC */

    /* 0x8004F8F4…0x8004F910: a spent key advances the frame HERE, before any
     * state is looked at, and only here. */
    if (vw->left == 0) {
        vw->frame++;
        vw->keys_played++;
    }

    if (vw->state == Q2_VM_LOWER) {
        if (clip_done(clip, vw->frame)) {
            /*
             * 0x8004F944. The countdown (client+222) gates the swap: while it
             * runs the frame is rewound to the key the substep began on, the
             * halt flag is raised and the driver stops consuming this frame
             * (0x8004FA48…0x8004FA58). The frame is then unchanged, so no key
             * is reloaded: the weapon hangs at the bottom of its arc with the
             * last key's clock spent, rather than replaying it.
             */
            if (vw->switch_ticks > 0) {
                vw->frame--;
                vw->result = 0;
                return true;
            }

            vw->weapon     = vw->pending;     /* 0x8004F96C */
            vw->state      = Q2_VM_RAISE;     /* 0x8004F960 */
            vw->frame      = 0;               /* 0x8004F964 */
            vw->fire_latch = false;           /* 0x8004F968 */
            *swapped       = true;
            /*
             * 0x8004FA44. A new model in the hands starts at position 0 of its
             * own timeline — the rest pose. (0x8004FA3C is the hyperblaster's
             * exception, and it stores the PLAYER INDEX times ten, which is
             * zero for the only player this port draws a weapon for.)
             *
             * And that is ALL the swap writes. The move record +736 and the
             * flags +258 are left as the old weapon had them — nothing
             * between 0x8004F954 and 0x8004FA44 touches either — so a move
             * still on the record when the weapon went down resumes from
             * position 0 on the new weapon's next fire clip, if the new weapon
             * is one whose clips move it at all. This used to clear both.
             */
            vw->anim_pos   = 0;
        }
        vw->result = result;
        return false;
    }

    /* 0x8004FA5C: the raise ends in IDLE, which is where the fire function is
     * bound for the weapon now in hand (0x8004FAA4). */
    if (vw->state == Q2_VM_RAISE && clip_done(clip, vw->frame)) {
        vw->state = Q2_VM_IDLE;
        vw->frame = 0;
        vw->result = result;
        return false;
    }

    /*
     * 0x8004FAB4. A selection that differs from what is in hand starts the
     * lower from RAISE or IDLE — never from FIRE, which is why you cannot
     * cancel a shot by switching — and it is tested BEFORE the idle arm, so a
     * held trigger cannot postpone it.
     */
    if (vw->pending != vw->weapon && vw->state != Q2_VM_FIRE) {
        vw->state = Q2_VM_LOWER;
        vw->frame = 0;
        vw->result = result;
        return false;
    }

    if (vw->state == Q2_VM_IDLE) {
        /* 0x8004FAF4: the trigger, or a latch to release. */
        if (cx->held || vw->fire_latch) {
            u32 was = vw->frame;
            q2_vw_fire_result r = Q2_VW_FIRE_NONE;

            vw->frame = 0;                     /* 0x8004FB2C, before the call */
            if (!vw->fire_latch) {
                r = idle_fire(vw, cx);
                if (r == Q2_VW_FIRE_NONE) {
                    /*
                     * The port's own outcome: the trigger is down, the weapon
                     * is fed, and the sim's refire gate said no. The console
                     * cannot reach this — its idle arm always fires — so the
                     * pass behaves as though the trigger were up: the frame is
                     * put back and the idle loop below decides. Treating it as
                     * a shot would play a fire clip that never happened;
                     * treating it as dry would switch off a loaded gun.
                     */
                    vw->frame = was;
                    goto idle_loop;
                }
                result = native_fire_word(r);
            }

            /* 0x8004FB44 / 0x8004FB54: dry, or the latch was up. */
            if (result == 2 || vw->fire_latch) {
                dry_pass(vw, cx);
                goto fire_check;
            }

            /*
             * 0x8004FB9C…0x8004FBD4. Anything else — a shot, or a fire
             * function that spawned nothing (its 0) — plays the fire clip, and
             * the quad's sound belongs to the call rather than the clip.
             */
            if (vw->quad_active)
                vw->quad_sound = true;
            vw->state = Q2_VM_FIRE;
            vw->cook  = false;
            vw->fires_started++;
            /* QPlayerFireGrenade 0x8004EBDC creates the held grenade through
             * AnyoneFireGrenade 0x8004AA6C — on a shot, not on a spawn that
             * failed — and from this frame its think owns +740. */
            if (vw->weapon == VW_HAND_GRENADE && r == Q2_VW_FIRED) {
                vw->hand_live       = true;
                vw->hand_cook_ticks = 0;
                vw->hand_release    = false;
            }
            goto fire_check;
        }

idle_loop:
        /* 0x8004FBD8: idle loops. */
        if (clip_done(clip, vw->frame)) {
            vw->frame = 0;
            vw->result = result;
            return false;
        }
    }

fire_check:
    /*
     * 0x8004FC04: the fire clip returns to idle when it runs out — and it does
     * NOT clear the latch, which is what lets a dry chaingun's latch reach the
     * idle pass that releases it.
     */
    if (vw->state == Q2_VM_FIRE && clip_done(clip, vw->frame)) {
        vw->state = Q2_VM_IDLE;
        vw->frame = 0;
    }

    vw->result = result;              /* 0x8004FC3C / 0x8004FC40 */
    return false;
}

/* ------------------------------------------------------------------------- */
/* The per-weapon FIRE-state driver — 0x8004FEE8                              */
/* ------------------------------------------------------------------------- */
/*
 * Called from inside the key loop on every substep (0x8004EF4C), after the
 * state machine's pass and the model-move driver, gated on `state == FIRE`
 * (0x8004FF38), then a switch on the weapon id. Only four weapons have an arm;
 * every other weapon's fire clip simply plays out.
 *
 * Because it runs after the pass, it sees the frame the pass has just
 * advanced to with that key's clock still spent: a wrap here replaces the
 * frame before the reload, so the reload loads the key the arm chose.
 *
 * The shots these arms take are not the idle check's. On the console each arm
 * `jalr`s the weapon's fire function directly, which is how a chaingun fires
 * once per animation frame for as long as its loop runs.
 */

/* +52 accumulates the step and fires each time it passes 30 (0x8004FFB8). */
#define VW_SPIN_THRESHOLD 30

static void fire_state_step(q2_viewweapon *vw, s32 step, vw_fire_ctx *cx)
{
    q2_vw_fire_result r;

    if (vw->state != Q2_VM_FIRE)
        return;

    switch (vw->weapon) {
    case VW_MACHINEGUN:
        /*
         * 0x8004FF84. Held: frame 2 wraps back to 0 so the three-key cycle
         * repeats, and the accumulator fires once each time it passes 30.
         *
         * The wrap happens on the substep that ENDS key 1 — the pass has just
         * advanced the frame to 2 — so key 2 is never played while the trigger
         * is down: the reload after this arm loads key 0's clock. The port
         * used to wrap a substep later, after key 2's clock had been loaded,
         * and so played key 2's duration toward key 0's pose.
         *
         * The accumulator is RESET, not decremented (0x80050038's
         * `sw zero, 52`), so a long substep cannot bank a burst. 0x8004FFD0
         * re-tests frame 2 before the call; after the wrap it cannot be.
         */
        if (cx->held) {
            if (vw->frame == 2)
                vw->frame = 0;

            vw->spin_accum += step;
            if (vw->spin_accum < VW_SPIN_THRESHOLD)
                break;

            r = frame_fire(vw, 1, cx);
            if (r == Q2_VW_FIRE_DENIED) {
                /* 0x8004FFF4: `j 0x80050034` with `sw v0, 72` in its delay
                 * slot — IDLE on the spot, frame untouched; the driver's
                 * reload sees the state change and loads the idle key. */
                vw->state = Q2_VM_IDLE;
            } else if (fire_ran(r) && vw->quad_active) {
                vw->quad_sound = true;          /* 0x80050004 */
            }
            vw->spin_accum = 0;                 /* 0x80050038 */
        } else {
            /*
             * 0x8005003C. Releasing clears the accumulator, and a release on
             * the MIDDLE key jumps to the last one — so the clip can end
             * instead of hanging on a key the wrap keeps returning to.
             */
            vw->spin_accum = 0;
            if (vw->frame == 1)
                vw->frame = 2;
        }
        break;

    case VW_CHAINGUN:
        /*
         * 0x80050058. Three bands in one 28-key clip: spin-up 0..8, LOOP
         * 9..17, spin-down 18..27.
         *
         *   held     and frame == 17 -> frame = 9   (stay in the loop)
         *   released and frame ==  9 -> frame = 27
         *
         * A DRY chaingun skips the wrap entirely (0x80050074 jumps straight to
         * the sounds), which is what lets the loop fall out into the spin-down
         * instead of grinding on an empty gun.
         */
        if (cx->held) {
            if (!vw->fire_latch && vw->frame == 17)
                vw->frame = 9;
        } else {
            /* 0x800500A0 stores the STATE (1) into the rate field from the
             * delay slot of the frame-9 test, so EVERY released substep writes
             * it, not only the one at frame 9. Any frame that reaches the band
             * table below overwrites it before the shot. */
            vw->spin_accum = 0;
            vw->spin_rate  = 1;
            if (vw->frame == 9)
                vw->frame = 27;
        }

        /* Everything below is once per NEW frame — the halfword cache at
         * +218, which nothing but this arm and the constructor writes: a new
         * fire clip does not reset it. */
        if ((s32)(s16)vw->frame == vw->last_fire_frame)
            break;
        vw->last_fire_frame = (s32)(s16)vw->frame;

        /* The three band boundaries each play their own clip. */
        if (vw->frame == 0)       vw->frame_sound = Q2_WSND_CHAINGUN_UP;
        else if (vw->frame == 10) vw->frame_sound = Q2_WSND_CHAINGUN_LOOP;
        else if (vw->frame == 18) vw->frame_sound = Q2_WSND_CHAINGUN_DOWN;

        if (vw->fire_latch) {
            /* 0x800503C4: a dry gun leaves the FIRE state when the clip comes
             * back round to its first key, rather than at the clip's end. */
            if (vw->frame == 0)
                vw->state = Q2_VM_IDLE;
            break;
        }

        /* 0x8005015C: no shot on the band boundaries or the whole spin-down. */
        if (vw->frame == 0 || vw->frame == 9 || vw->frame == 17 ||
            vw->frame >= 18)
            break;

        /*
         * THE BULLETS PER FRAME, which is openquestions #39c and was defaulted
         * to one here. 0x80050180 reads three bands off the frame —
         *
         *     frame < 5           1
         *     5 <= frame < 9      2 while the trigger is held, else 1
         *     frame >= 10         3
         *
         * — so a chaingun wound all the way up throws three rounds per
         * animation frame and a tapped one throws a single round. Frame 9 is
         * filtered out above and is the one frame that writes no rate at all.
         * 0x800501E8 then clamps the rate to the ammo the selection holds; the
         * fire callback owns the ammo, so it clamps this single burst itself.
         */
        if (vw->frame < 5)
            vw->spin_rate = 1;
        else if (vw->frame < 9)
            vw->spin_rate = cx->held ? 2 : 1;
        else if (vw->frame >= 10)
            vw->spin_rate = 3;

        r = frame_fire(vw, vw->spin_rate < 1 ? 1 : vw->spin_rate, cx);
        if (r == Q2_VW_FIRE_DENIED)
            vw->fire_latch = true;              /* 0x80050230 */
        else if (fire_ran(r) && vw->quad_active)
            vw->quad_sound = true;              /* 0x80050234 */
        break;

    case VW_HYPERBLASTER:
        /*
         * 0x8005026C. Frames 1..5 are the loop; a held trigger wraps 5 back to
         * 1, and frame 6 is the tail the loop must not fire on. A dry gun goes
         * straight to the sounds, which is what lets it reach frame 6 and stop.
         *
         * The accumulator is cleared by a shot (0x80050330) and by a release,
         * but NOT when it passes 30 on frame 6 — 0x800502C8 branches past the
         * clear, so it stands until the trigger lets go.
         */
        if (cx->held && !vw->fire_latch) {
            if (vw->frame == 5)
                vw->frame = 1;

            vw->spin_accum += step;
            if (vw->spin_accum >= VW_SPIN_THRESHOLD && vw->frame != 6) {
                r = frame_fire(vw, 1, cx);
                if (r == Q2_VW_FIRE_DENIED)
                    vw->fire_latch = true;      /* 0x800502FC */
                else if (fire_ran(r) && vw->quad_active)
                    vw->quad_sound = true;      /* 0x80050300 */
                vw->spin_accum = 0;
            }
        } else if (!cx->held) {
            vw->spin_accum = 0;
        }

        if ((s32)(s16)vw->frame == vw->last_fire_frame)
            break;
        vw->last_fire_frame = (s32)(s16)vw->frame;

        /*
         * AND IT LOOPS THE CHAINGUN'S SAMPLE. 0x80050378 loads `0x800B2B3C`,
         * which `0x8004E248` fills from `wep_chngnl1a` — the same handle the
         * chaingun's own loop plays. The hyperblaster's `wep_hyprbf1a` lives at
         * `0x800B2B58` and belongs to the FIRE function, not to this arm. The
         * name-to-slot order was read twice and agrees, so this is the retail
         * build's behaviour rather than a mis-decode.
         */
        if (vw->frame == 0)
            vw->frame_sound = Q2_WSND_CHAINGUN_LOOP;
        else if (vw->frame == 6)
            vw->frame_sound = Q2_WSND_HYPERBLAST_DOWN;

        /* 0x800503C4 again, shared with the chaingun. */
        if (vw->fire_latch && vw->frame == 0)
            vw->state = Q2_VM_IDLE;
        break;

    case VW_HAND_GRENADE:
        /*
         * 0x800503DC — the COOK, and it is the one arm that does not fire.
         *
         * IT IS GATED ON THE MODEL'S OWN ANIMATION, which is the part that was
         * missing: 0x800503F0 refuses to prime until viewmodel+256 has reached
         * 380, and 380 sits inside the `Set` move's 0..470 span. So the arm has
         * to finish bringing the grenade up out of the belt before the hold
         * begins; until then the clip runs on and the throw happens on time
         * whatever the trigger is doing.
         *
         * Once primed the position is PINNED at 380, the frame at 1, and the
         * key's remaining time GROWS by the step, so the clip cannot advance
         * and the grenade is held for as long as the trigger is down.
         */
        /*
         * AND ON THE GRENADE'S MARK: 0x80050404 also requires +740 — the held
         * grenade's previous position minus one — to be below 380. Pinned, the
         * mark is 379 and the hold continues; released past 380 and pressed
         * again, the mark is past it and the arm keeps going. The sounds and
         * the throw are the grenade's own think, at the end of the advance.
         */
        if (cx->held && vw->anim_pos >= Q2_VW_COOK_POSITION &&
            vw->hand_prev_anim < Q2_VW_COOK_POSITION) {
            vw->anim_pos = Q2_VW_COOK_POSITION;     /* 0x8005041C */
            vw->frame    = 1;                       /* 0x80050420 */
            vw->left    += step;                    /* 0x80050428 */
            vw->cook     = true;
        } else {
            vw->cook = false;
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* The model's own named move — 0x80050454                                    */
/* ------------------------------------------------------------------------- */
/*
 * The jump table at 0x800ACD34, indexed by `weapon - 3` over nine slots, with
 * the outer test at 0x800504D4 admitting only 3, 6, 7, 8 and 11. The three arms
 * it reaches load a 12-byte name and hand it to 0x8006D330, which is
 * `q2_model_move_by_name` — the engine never indexes block D.
 */
static const char *anim_move_name(int weapon)
{
    switch (weapon) {
    case 3:  return "Fire";     /* Supershot G   — 0x800505EC */
    case 6:  return "Set";      /* HandGren G    — 0x800505AC */
    case 7:  return "Spin";     /* GrenLaunch G  — 0x800505CC */
    case 8:  return "Fire";     /* RockLaunch G  — 0x800505EC */
    case 11: return "Fire";     /* Bfg G         — 0x800505EC */
    default: return NULL;
    }
}

/*
 * 0x80050454's second half, run on every substep of the key loop.
 *
 * A move already playing advances by the step and ends when the position walks
 * past the record's last (0x8005053C's `slt end, pos`); otherwise a key
 * carrying event 1 in the FIRE state starts one. The state test is
 * 0x80050508's `state != 1`, which is FIRE — an idle clip's event, if one ever
 * carried this value, would not reach the lookup.
 *
 * `event` is what the driver hands it in `a1` (0x8004EF1C…0x8004EF34): the
 * event of the key at the frame the state machine has just left, read through
 * the clip the substep BEGAN with. On the substep that enters FIRE that is the
 * idle clip's key 0, so a fire clip's first-key event starts its move one
 * substep later, from the fire clip itself.
 */
static void anim_step(q2_viewweapon *vw, s32 step, s16 event)
{
    const char      *name;
    q2_model_move    mv;

    /*
     * BOTH HALVES ARE GATED, and on two things. 0x800504D4…0x800504F8 returns
     * for any weapon in hand (client+98) but 3, 6, 7, 8 and 11, and 0x80050508
     * returns for any state but FIRE — before the record at +736 is even
     * looked at. So a move only ever advances inside a fire clip: one still
     * playing when its clip runs out stands where it is through IDLE, LOWER
     * and RAISE, and picks up again on the next fire clip. This used to
     * advance a playing move in any state, which let it run out in idle.
     */
    name = anim_move_name(vw->weapon);
    if (!name || vw->state != Q2_VM_FIRE)
        return;

    if (vw->anim_end >= 0) {
        s32 pos = (s32)vw->anim_pos + step;

        if (pos > (s32)vw->anim_end) {
            /* 0x80050548: the record is dropped, the position rewinds to the
             * rest pose, and the two flag bits swap over. */
            vw->anim_pos   = 0;
            vw->anim_end   = -1;
            vw->anim_flags = (u16)((vw->anim_flags | 1u) & ~2u);
            return;
        }
        vw->anim_pos = (s16)pos;
        return;
    }

    if (event != 1)                       /* 0x80050564 */
        return;
    if (vw->anim_flags & 2u)              /* 0x80050574 */
        return;
    if (!vw->model)
        return;
    if (!q2_model_move_by_name(vw->model, name, &mv))
        return;

    /*
     * BLOCK D TIMES FIVE, and the evidence is the GRENADE, not this call site.
     *
     * 0x80050530 stores the record's `+12` into viewmodel+256 with no scale of
     * any kind, and openquestions #51f swept the whole image for a load-time
     * multiply and found none — every `x5` shape is a 20-byte record stride. So
     * the arithmetic here has no instruction behind it, and taking the numbers
     * as written would be the defensible move if one constant did not rule it
     * out.
     *
     * That constant is the 380 at 0x800503F8. The cook arm refuses to prime the
     * hand grenade until viewmodel+256 has reached 380, and `HandGren G`'s `Set`
     * move runs 0..94 on the disc. Unscaled the position tops out at 94 and the
     * grenade could NEVER be cooked; the console cooks grenades. The threshold
     * has to sit inside the move's span, which puts the scale at 4.05 or more,
     * and five is the ratio the two documented unit systems already have — block
     * D counts two per animation frame (#51g), the animation position counts ten
     * (#51d). At five the span is 0..470 and 380 lands four fifths of the way
     * through the arm coming up, which is where a prime belongs.
     *
     * So: the five is inferred, the inference is forced, and the thing that
     * forces it is written down here so the next reader can attack it.
     */
    vw->anim_pos   = (s16)((s32)mv.start * 5);
    vw->anim_end   = (s16)((s32)mv.end   * 5);
    vw->anim_flags = (u16)((vw->anim_flags & ~1u) | 2u);
    vw->anims_started++;
}

/* ------------------------------------------------------------------------- */
/* The hyperblaster's barrel — 0x8004FC78                                     */
/* ------------------------------------------------------------------------- */
/*
 * A third driver, called once per substep beside the other two (0x8004F270)
 * and gated on weapon 9 and nothing else — 0x8004FCB8 returns immediately for
 * every other weapon.
 *
 * The four fire-frame arms behind the jump table at 0x800ACD1C all compute the
 * same shape, `base - 40 * (duration - left + 2)`, with a different base each:
 * 4096, 3296, 2096, 1296 for frames 1, 2, 3 and 4. Frame 6 is a straight
 * `(left << 12) / duration`, seeded to 4096 on the substep the key starts.
 * Frames 0, 5 and everything from 7 write nothing, and neither does any state
 * but FIRE and IDLE — so a raise or a lower holds whatever the last shot left
 * the barrel at, which is what makes the spin carry across the clip boundary.
 *
 * It runs AFTER the key reload, through the clip the reload used, so on the
 * substep a key starts it already sees that key's fresh clock.
 *
 * `duration` here is the key's STORED duration (`lh 12(v0)` reads the key, not
 * the viewmodel's clock), so a jittered key would divide by a different number
 * than it counts down — no fire clip carries jitter, so it never arises.
 */
static void hyper_ramp_step(q2_viewweapon *vw)
{
    const q2_vm_key *k;
    s32 dur, elapsed;

    if (vw->weapon != VW_HYPERBLASTER)
        return;

    if (vw->state == Q2_VM_IDLE) {
        vw->hyper_ramp = VW_HYPER_RAMP_ONE;      /* 0x8004FCD8 */
        return;
    }
    if (vw->state != Q2_VM_FIRE)
        return;                                   /* 0x8004FCD0 holds it */

    k = current_key(vw);
    if (!k)
        return;

    dur = k->duration;
    if (dur <= 0)
        return;

    /* 0x8004FD3C…0x8004FD50: the elapsed term is `duration - left + 2`, and
     * the two is the original's, not a rounding. */
    elapsed = dur - vw->left + 2;

    switch (vw->frame) {
    case 1: vw->hyper_ramp = (s16)(4096 - 40 * elapsed); break;
    case 2: vw->hyper_ramp = (s16)(3296 - 40 * elapsed); break;
    case 3: vw->hyper_ramp = (s16)(2096 - 40 * elapsed); break;
    case 4: vw->hyper_ramp = (s16)(1296 - 40 * elapsed); break;

    case 6:
        /* 0x8004FE44: the first substep of the key seeds it at one, and after
         * that it is the key's own remaining fraction. */
        if (vw->left == dur)
            vw->hyper_ramp = VW_HYPER_RAMP_ONE;
        vw->hyper_ramp = (s16)((vw->left << 12) / dur);
        break;

    default:
        break;                                    /* 0, 5 and 7 upward hold */
    }
}

u32 q2_vw_take_frame_fires(q2_viewweapon *vw)
{
    u32 n;

    if (!vw)
        return 0;
    n = vw->frame_fires;
    vw->frame_fires = 0;
    return n;
}

void q2_vw_frame_fire_result(q2_viewweapon *vw, bool dry)
{
    if (!vw)
        return;

    if (!dry) {
        if (vw->quad_active)
            vw->quad_sound = true;
        return;
    }

    switch (vw->weapon) {
    case VW_MACHINEGUN:
        /*
         * 0x8004FFF4: `bne v0, s1` against 2 falls through to `j 0x80050034`
         * with `sw v0, 72(s0)` in its delay slot, and 0x80050038 clears +52.
         * The frame is left where it was. In the driver the reload after the
         * arm loads the idle key; this deferred form, reached after the
         * advance, does that reload itself.
         */
        if (vw->state == Q2_VM_FIRE) {
            const q2_vm_clip *fire_clip = current_clip(vw);

            vw->state      = Q2_VM_IDLE;
            vw->spin_accum = 0;
            reload_key(vw, fire_clip, vw->frame);
            interpolate(vw);
        }
        break;

    case VW_CHAINGUN:
    case VW_HYPERBLASTER:
        vw->fire_latch = true;      /* 0x80050230 / 0x800502FC */
        break;

    default:
        break;
    }
}

bool q2_vw_take_quad_sound(q2_viewweapon *vw)
{
    bool q;

    if (!vw)
        return false;
    q = vw->quad_sound;
    vw->quad_sound = false;
    return q;
}

s16 q2_vw_take_frame_sound(q2_viewweapon *vw)
{
    s16 snd;

    if (!vw)
        return -1;
    snd = vw->frame_sound;
    vw->frame_sound = -1;
    return snd;
}

s32 q2_vw_take_hand_grenade_cook(q2_viewweapon *vw)
{
    s32 ticks;

    if (!vw)
        return 0;
    ticks = vw->hand_cook_ticks;
    vw->hand_cook_ticks = 0;
    return ticks;
}

bool q2_vw_take_hand_grenade_release(q2_viewweapon *vw)
{
    bool release;

    if (!vw)
        return false;
    release = vw->hand_release;
    vw->hand_release = false;
    return release;
}

void q2_vw_hand_grenade_expired(q2_viewweapon *vw)
{
    if (!vw || vw->weapon != VW_HAND_GRENADE || vw->state != Q2_VM_FIRE)
        return;

    /* 0x8004AA1C..0x8004AA40, after the held grenade detonates. It does not
     * begin a new key: it writes frame and remaining time directly, leaving
     * the current interpolation total exactly as the executable does. */
    vw->frame           = 2;
    vw->left            = 150;
    vw->anim_pos        = 0;
    vw->anim_end        = -1;
    vw->anim_flags      = (u16)((vw->anim_flags | 1u) & ~2u);
    vw->cook            = false;
    vw->hand_prev_anim       = 0;              /* 0x8004AA28 */
    vw->hand_live       = false;
    vw->hand_cook_ticks = 0;
    vw->hand_release    = false;
    vw->frame_sound     = -1;
}

/*
 * The held grenade's think, PrimaryGrenadeActive 0x8004A368, in its held state
 * (0x8004A414…0x8004A544) — run once per frame after the driver, as the
 * console's entity pass runs it after the player's. The fuse, the copy of the
 * weapon's position and the throw itself belong to the port's own grenade;
 * what is here is everything the think reads off and writes back to this
 * record.
 */
static void hand_grenade_think(q2_viewweapon *vw, s32 dt)
{
    s32 pos = vw->anim_pos;

    if (!vw->hand_live)
        return;

    /* 0x8004A42C: at the cook position the charge (+76) grows by dt*6. */
    if (pos == Q2_VW_COOK_POSITION)
        vw->hand_cook_ticks += dt;

    /* 0x8004A474: the prime sample, wep_hgrenc1b. */
    if (pos >= Q2_VW_HAND_PRIME_POSITION &&
        vw->hand_prev_anim < Q2_VW_HAND_PRIME_POSITION)
        vw->frame_sound = Q2_WSND_HANDGREN_PRIME;

    /* 0x8004A4A8…0x8004A4D0: thrown — state 2, and this think ends. */
    if (pos >= Q2_VW_HAND_RELEASE_POSITION &&
        vw->hand_prev_anim < Q2_VW_HAND_RELEASE_POSITION) {
        vw->hand_release = true;
        vw->frame_sound  = Q2_WSND_HANDGREN_THROW;
        vw->hand_live    = false;
    }

    vw->hand_prev_anim = (s16)(pos - 1);              /* 0x8004A534…0x8004A544 */
}

/* ------------------------------------------------------------------------- */
/* The driver — 0x8004EE0C                                                    */
/* ------------------------------------------------------------------------- */
bool q2_vw_advance(q2_viewweapon *vw, s32 dt, bool fire_held,
                   q2_vw_fire_result fired)
{
    return q2_vw_advance_firing(vw, dt, fire_held, fired, NULL, NULL);
}

bool q2_vw_advance_firing(q2_viewweapon *vw, s32 dt, bool fire_held,
                          q2_vw_fire_result fired, q2_vw_fire_fn fire,
                          void *user)
{
    vw_fire_ctx cx;
    bool swapped = false;
    int guard = 0;
    s32 frame_dt = dt;

    if (!vw || !vw->tab)
        return false;

    /*
     * The caller's report answers the idle arm's FIRST ask, and only when the
     * call begins where that ask is certain: idle, unlatched, nothing pending,
     * trigger down. The state machine is the first thing every substep runs
     * (0x8004EEFC), so in exactly that state the first substep's pass reaches
     * the `jalr` at 0x8004FB30 before anything else in the call has moved —
     * a caller that fires on `q2_vw_wants_fire` has made the console's call at
     * the console's moment. Every later ask in the same call goes to `fire`.
     */
    cx.held        = fire_held;
    cx.fired       = fired;
    cx.precomputed = fire_held && fired != Q2_VW_FIRE_ASK &&
                     q2_vw_wants_fire(vw);
    cx.fire        = fire;
    cx.user        = user;
    cx.dry_seen    = false;

    /* The cycle routine's half (0x8004ED44…0x8004ED70), which the console
     * runs before the driver on every frame that does not arm it. */
    if (vw->switch_armed) {
        vw->switch_armed = false;
    } else if (vw->switch_ticks > 0) {
        vw->switch_ticks = (s16)(vw->switch_ticks - dt);
        if (vw->switch_ticks < 0)
            vw->switch_ticks = 0;
    }

    /*
     * 0x8004EEA4…0x8004F278, one iteration per substep, in this order:
     *
     *   step = min(left, dt); left -= step        0x8004EEAC / 0x8004EED0
     *   the state machine's pass                  0x8004EEFC  (0x8004F87C)
     *   dt -= step, or 0 if it raised the halt    0x8004EF10 / 0x8004EF14
     *   the model's named move                    0x8004EF38  (0x80050454)
     *   the per-weapon frame driver               0x8004EF4C  (0x8004FEE8)
     *   the key reload, if frame or state moved   0x8004EF5C…0x8004F264
     *   the hyperblaster's barrel                 0x8004F270  (0x8004FC78)
     *
     * So the machine runs on EVERY substep, not only at key boundaries: the
     * frame advances inside the pass when the key's clock is spent, and the
     * idle arm's trigger test — and its fire call — happens on the first
     * substep of the call, before the step has done anything but shorten the
     * key. Every write the port makes falls in that order now; the
     * restructured loop it replaced ran the machine after the drivers, only at
     * key boundaries, plus an idle check and a selection test after the loop.
     *
     * The guard is a port's own. The console has two configurations that
     * would spin here forever: an idle clip of a single key whose clock is
     * spent (the pass rewinds the frame to the key it was on, so nothing
     * reloads and the step stays zero), and a dry trigger held with nothing
     * to switch to. The retail bank has no one-key clip and 0x800506C4 always
     * finds the blaster, so neither arises on the console; here a substep that
     * leaves everything as it found it reloads its key instead of repeating.
     */
    vw->substeps = 0;
    while (dt > 0 && guard++ < 512) {
        const q2_vm_clip *clip = current_clip(vw);       /* s4, 0x8004EEE0 */
        const q2_vm_key  *k;
        u32         old_frame   = vw->frame;              /* s1, 0x8004EEC8 */
        q2_vm_state old_state   = vw->state;              /* s2, 0x8004EEC0 */
        bool        old_latch   = vw->fire_latch;
        int         old_pending = vw->pending;
        s32         old_accum   = vw->spin_accum;
        s32         step        = (vw->left < dt) ? vw->left : dt;
        bool        halt;

        if (step < 0)
            step = 0;
        vw->left -= step;
        vw->substeps++;

        halt = machine_pass(vw, clip, &cx, &swapped);
        dt -= step;
        if (halt)
            dt = 0;

        k = clip_key(clip, vw->frame);
        anim_step(vw, step, k ? k->event : (s16)Q2_VM_EVENT_NONE);
        fire_state_step(vw, step, &cx);

        if (vw->frame != old_frame || vw->state != old_state)
            reload_key(vw, clip, old_frame);
        else if (step == 0 && !halt && vw->fire_latch == old_latch &&
                 vw->pending == old_pending && vw->spin_accum == old_accum)
            reload_key(vw, clip, old_frame);   /* the port's guard, above */

        hyper_ramp_step(vw);
    }

    /* 0x8004F280: the display value, which runs even when dt was zero and no
     * substep did (0x8004EE9C branches straight here). */
    interpolate(vw);

    hand_grenade_think(vw, frame_dt);
    return swapped;
}

s16 q2_vw_result(const q2_viewweapon *vw)
{
    return vw ? vw->result : 0;
}

/* ------------------------------------------------------------------------- */
/* The auto-aim assist — client+0x46                                          */
/* ------------------------------------------------------------------------- */
void q2_vw_set_aim_assist(q2_viewweapon *vw, s16 assist)
{
    if (vw)
        vw->aim_assist = assist;
}

/*
 * Seek — MISC.C, 0x8006FE3C, transcribed whole because every one of its four
 * arms matters at the extremes.
 *
 * The compare is on the SIGN-EXTENDED halfword (`lh a3` at 0x8006FE44) and the
 * arithmetic on the ZERO-EXTENDED one (`lhu t1` at 0x8006FE48): the step is
 * added to the unsigned copy and the result truncated by the `sh`, so a value
 * that walks off either end wraps the way a PSX angle should. Doing the
 * arithmetic on the signed copy instead would be the same number everywhere
 * except at the wrap, which is exactly where an angle lives.
 *
 * The overshoot test is `cur + step < target` rather than `<=`, so a step that
 * lands EXACTLY on the target takes the snap arm (0x8006FE90). That is a
 * DISTINCTION WITHOUT A DIFFERENCE and is written out so nobody straightens
 * it: both arms store the same halfword and both report arrival. The strict
 * `<` is kept because it is what the `slt` at 0x8006FE64 says.
 */
static bool seek_halfword(s16 *cur, s16 target, s32 step)
{
    s32 c = (s32)*cur;                  /* 0x8006FE44: lh   */
    u16 u = (u16)*cur;                  /* 0x8006FE48: lhu  */

    if (c < (s32)target) {              /* 0x8006FE4C: slt  */
        if (c + step < (s32)target)     /* 0x8006FE64: slt  */
            *cur = (s16)(u16)((u32)u + (u32)step);   /* 0x8006FE74 */
        else
            *cur = target;              /* 0x8006FE94 */
    } else {
        if ((s32)target < c - step)     /* 0x8006FE84: slt  */
            *cur = (s16)(u16)((u32)u - (u32)step);   /* 0x8006FE98 */
        else
            *cur = target;              /* 0x8006FE94 */
    }

    /* 0x8006FEAC: `xor` then `sltiu ..., 1` — one when it arrived. */
    return *cur == target;
}

bool q2_vw_seek_aim_assist(q2_viewweapon *vw, s16 target, s32 frame_dt)
{
    /*
     * 0x8003A45C / 0x8003A464: `sll a2, a2, 18` then `sra a2, a2, 16`. That is
     * the frame delta times four with the top fourteen bits shifted off the
     * end, and it is written out rather than as `* 4` because the discard is
     * the console's own — a delta over 16383 would wrap here on the console
     * too. At the PAL frame delta of 12 ([0x800B2DB4], twice the per-field
     * [0x800B2DD8]) the step is 48 of the 4096-unit circle a tick, so the
     * barrel covers a quarter turn in about twenty-one ticks.
     */
    s32 step = (s32)((u32)frame_dt << 18) >> 16;

    if (!vw)
        return false;

    return seek_halfword(&vw->aim_assist, target, step);
}

s16 q2_vw_clip_pitch(const q2_viewweapon *vw)
{
    if (!vw)
        return 0;

    /*
     * 0x8004F3E4…0x8004F3F8. Both operands are read with `lhu` and the sum is
     * stored back with `sh` before `RotMatrixYXZ` reads it, so the sum is a
     * SIXTEEN-BIT one and wrapping is the behaviour, not an accident: a clip
     * rotation near the top of the circle plus an upward assist comes out the
     * other side, which is the same angle.
     *
     * The store back into the entity is not reproduced, and the reason is that
     * it is dead. Nothing reads viewweapon+0xE6 again before the next tick
     * recomputes it from scratch at 0x8004F2F8 (`sh v0, 230(s3)`, the
     * interpolated display value written after the substep loop), and the two
     * reads that follow in this function — 0x8004F40C and 0x8004F438 — are off
     * `s6`, the PLAYER entity, not `s3`. Keeping `cur_r` clean instead means a
     * frame on which the interpolation happens not to run cannot accumulate
     * the assist twice.
     */
    return (s16)(u16)((u32)(u16)vw->cur_r[0] + (u32)(u16)vw->aim_assist);
}

/* ------------------------------------------------------------------------- */
/* Placement                                                                  */
/* ------------------------------------------------------------------------- */
void q2_vw_place(const q2_viewweapon *vw,
                 const s32 feet[3], s32 view_offset,
                 const s16 aim[3], const s16 kick[3],
                 s32 origin_out[3], s32 angles_out[3])
{
    s16 rot[3][3];
    s32 ang[3];
    s32 local[3];
    int i;

    if (!vw || !feet)
        return;

    /*
     * 0x8004F40C. The three angles are the player's aim plus the second triple
     * the original adds, and the x component is NEGATED at the sum. The
     * animation's own rotation rides on top, which is what makes a weapon dip
     * as it is raised rather than snapping upright.
     */
    /*
     * `ang` is the VIEW matrix and nothing else. The clip's own rotation is NOT
     * in it: the original builds `sp+40` from the player's aim and the second
     * angle triple only (0x8004F40C…0x8004F448) and hands that to RotMatrix, and
     * the clip's rotation reaches the model through the separate chain at
     * 0x8004F284. Folding the clip's angles in here rotates the offset by them
     * too, and since several clips carry a half-circle component that puts the
     * weapon behind the camera — where every face is rejected at the projection
     * plane and nothing draws at all.
     */
    ang[0] = -((aim ? aim[0] : 0) + (kick ? kick[0] : 0));
    ang[1] =  ((aim ? aim[1] : 0) + (kick ? kick[1] : 0));
    ang[2] =  ((aim ? aim[2] : 0) + (kick ? kick[2] : 0));

    if (angles_out) {
        /* The model's own orientation does carry the clip's rotation, composed
         * on top of the view — 0x8004F474's MulMatrix.
         *
         * AND THE X COMPONENT CARRIES ONE MORE TERM. 0x8004F3F0 adds the
         * owner's client+0x46 to the clip rotation X before the matrix is
         * built, so the barrel pitches onto the auto-aim target while the view
         * does not. This used to be plain `vw->cur_r[0]` on the strength of a
         * sweep that proved the halfword unwritten; it is written, indirectly,
         * and `q2_vw_clip_pitch` says where. */
        angles_out[0] = ang[0] + q2_vw_clip_pitch(vw);
        angles_out[1] = ang[1] + vw->cur_r[1];
        angles_out[2] = ang[2] + vw->cur_r[2];
    }

    if (!origin_out)
        return;

    /*
     * 0x8004F5E0. The animation's translation is rotated before the eye is
     * added — rotate then translate, not the other way round, or the weapon
     * slides across the view instead of swinging with it.
     *
     * The matrix is the camera's INVERSE, and that is not a detail. The view
     * model is authored in view space: `Blaster G` runs from z = 0 at the grip
     * to z = 482 at the muzzle, so its own +Z is "away from the eye", not a
     * world direction. Rotating the offset by the camera's forward matrix
     * instead sends it out along a world axis — which is what put the weapon
     * behind the camera and had every face rejected at the projection plane.
     *
     * A rotation matrix's inverse is its transpose, so this is the same matrix
     * read down its columns.
     */
    /*
     * All THREE angles, because `RotMatrix` takes all three.
     *
     * This used to build a yaw/pitch matrix and apply its transpose, which is
     * the same thing as `q2_rotation_euler(pitch, yaw, 0)` applied directly —
     * correct except that it silently drops the roll. `0x8004F464` hands
     * `RotMatrix` the whole SVECTOR at `sp+40`, and `sp+44` is the z component,
     * so the console rotates the offset by the roll as well. Dropping it means
     * the weapon does not lean with the view: the strafe lean rolls the camera
     * and the gun stays upright and slides, which is the one case where the
     * two visibly separate.
     *
     * `q2_rotation_euler` is `RotMatrix`'s own composition — checked at
     * `0x80089E38`, which writes `m[1][2] = -sin(x)` and
     * `m[0][2] = sin(y)cos(x)`, the signature of Ry·Rx with Z outermost.
     */
    /*
     * THE CAMERA'S OWN BUILDER, TRANSPOSED - and the transpose is the whole
     * correctness argument.
     *
     * This used to be `q2_rotation_euler(rot, -pitch, yaw, roll)` applied
     * directly, and every operand in it had been checked against the
     * executable and agreed. It was still wrong, and openquestions #46 spent
     * three passes re-reading operands, because the defect is not in any of
     * them: it is in the IDENTITY the arrangement rests on.
     *
     * Follow a part origin through modeldraw.c. With `local` zero at the grip,
     * `inst.origin = feet + R_place*t`, and `cam.pos` the eye - which is the
     * same `feet - view_offset` this function computes, deliberately -
     *
     *     camera_space = view * (inst.origin - cam.pos) = view * R_place * t
     *
     * so the weapon lands where the clip authored it, at `t` in view space,
     * only if **view * R_place is the identity**. Nothing asserted that, and it
     * is not: `q2_rotation_view(yaw, pitch, roll)` and
     * `q2_rotation_euler(-pitch, yaw, roll)` agree on yaw and disagree on
     * pitch, so their product is the identity when the player looks level and
     * is off by 3260/4096 at a 26-degree pitch. tests/test_viewweapon.c pins it
     * now.
     *
     * Building the placement matrix with the CAMERA'S OWN function and applying
     * its transpose makes `view * R_place == I` true by construction, for any
     * angles, because a rotation matrix's transpose is its inverse. That is the
     * same argument `q2_vw_build_ot` already makes for the ROTATION half; it
     * was never applied to the translation.
     *
     * The argument ORDER is the camera's too: `q2_rotation_view` takes
     * (yaw, pitch, roll), and `ang` is indexed (pitch, yaw, roll) - with
     * `ang[0]` already negated at the sum above, which is 0x8004F40C's own
     * negation and is why the pitch is passed back through un-negated here.
     */
    q2_rotation_view(rot, ang[1], -ang[0], ang[2]);

    for (i = 0; i < 3; i++) {
        local[i] = ((s32)rot[0][i] * vw->cur_t[0]
                  + (s32)rot[1][i] * vw->cur_t[1]
                  + (s32)rot[2][i] * vw->cur_t[2]) >> Q2_FRAC_12;
    }

    /*
     * 0x8004F5E8…0x8004F640, and the base is the ENTITY ORIGIN.
     *
     * The console's expression is `pos.y + 286 - viewOffset` where `pos` is
     * entity+0x58 — the point the mover works in, 286 above the feet. This
     * function is handed the FEET (that is what `q2_player.pos` holds), so the
     * conversion has to happen before the 286 is added, and then the two
     * cancel:
     *
     *     (feet - 286) + 286 - viewOffset  ==  feet - viewOffset
     *
     * which is exactly what q2_sim_eye computes. That identity is the whole
     * point: the weapon hangs off the eye, so the two must be the same
     * arithmetic, and writing it as `feet + 286 - viewOffset` — adding the
     * constant to a base that is already 286 low — put the weapon 286 units
     * below the camera and dropped it off the bottom of the screen.
     *
     * `Q2_VW_EYE_BASE` is kept for the two tests that pin the constant against
     * 0x8004F608, and deliberately not used in the sum: there is nothing here
     * to add it to that is not immediately subtracted again.
     */
    origin_out[0] = feet[0] + local[0];
    origin_out[1] = feet[1] + local[1] - view_offset;
    origin_out[2] = feet[2] + local[2];
}

u32 q2_vw_build_ot(const q2_viewweapon *vw,
                   const q2_model_instance *proto,
                   const s32 feet[3], s32 view_offset,
                   const s16 aim[3], const s16 kick[3],
                   const q2_camera *cam,
                   psx_ot *ot, gte_state *gte,
                   q2_model_draw_stats *stats)
{
    q2_model_instance inst;
    q2_model_pose     pose[Q2_VW_POSE_MAX];
    bool              posed = false;
    s32 origin[3];
    s32 angles[3];
    s16 rot_out[3][3];

    if (!vw || !vw->model || !cam || !ot || !gte)
        return 0;

    if (proto)
        inst = *proto;
    else
        q2_model_instance_init(&inst);

    q2_vw_place(vw, feet, view_offset, aim, kick, origin, angles);

    /* q2_vw_place fills `angles` for callers that want the composed
     * rotation; this path builds its own from the camera below. */
    (void)angles;

    /*
     * POSE IT, which nothing did — and an unposed model draws its vertices RAW.
     *
     * Every other model in the frame goes through this (entitydraw.c makes the
     * argument at length: a part's rest transform is not the identity, and the
     * medkits sat in the floor until it was applied). The weapon in the hands
     * was the one model still drawn from the vertex block, so `HandGren G` —
     * whose raw bounds are a 105-unit lump against a posed 666-unit arm — came
     * out as a fist-sized blob under the crosshair, and `Railgun G` and `Bfg G`
     * were short by their own rest translations.
     *
     * The position is the model's own timeline, held rather than failed off the
     * end (`_held` is the engine's own behaviour: 0x8006B924 has no end test).
     * With no named move playing it is zero, which is the rest pose.
     */
    if (vw->model->hdr.num_parts <= Q2_VW_POSE_MAX) {
        q2_model_anim clip;
        u32 within = 0;
        u32 at = vw->anim_pos > 0 ? (u32)vw->anim_pos : 0u;

        if (q2_model_anim_at_held(vw->model, at, &clip, &within, NULL) &&
            q2_model_pose_at(vw->model, &clip, within, pose) == Q2_OK)
            posed = true;

        /*
         * AND THE HYPERBLASTER'S BARREL, which is the pose plus one patched
         * field — 0x8006D43C.
         *
         * The console walks to part 6's key in the model's own animation and
         * overwrites the low eleven bits of its packed rotation word with
         * `(ramp & 0xFFF) >> 1`, leaving the other two Euler half-angles
         * alone (its `a2` and `a3` arrive as -1 and the two `bltz` tests skip
         * their stores). It can write there because the model is in RAM; here
         * the word is read back out and re-decoded into this instance's own
         * pose, which draws the same barrel without touching the bank.
         */
        if (posed && vw->weapon == VW_HYPERBLASTER &&
            VW_HYPER_BARREL_PART < vw->model->hdr.num_parts) {
            u32 rot;

            if (q2_model_part_rotation_word(vw->model, &clip, within,
                                            VW_HYPER_BARREL_PART, &rot)) {
                u32 half = ((u32)(u16)vw->hyper_ramp & 0xFFFu) >> 1;

                rot = (rot & ~0x7FFu) | half;
                q2_model_key_rotation(rot, pose[VW_HYPER_BARREL_PART].q);
            }
        }
    }

    inst.model     = vw->model;
    inst.pose      = posed ? pose : NULL;
    inst.anim_pos  = vw->anim_pos > 0 ? vw->anim_pos : 0;
    /* +0xFC/+0xFE scale the GTE light matrix and back colour, not the model
     * transform. The caller passes them to q2_light_env_build; geometry keeps
     * the allocator-neutral transform just like an ordinary entity. */
    inst.scale = Q2_ONE_12;
    /*
     * The view-weapon entity is permanently assigned screen area 1 at
     * 0x8004EE64 (`sb 1, entity+0x9E`).  It later reaches the ordinary model
     * renderer, whose 0x8006BEB0 call selects that area before projecting it.
     *
     * This is observable because 0x80065684 is stateful: its negative selector
     * is a no-op.  Leaving the prototype's default -1 here therefore inherited
     * whichever portal-local origin the last particle or projectile installed,
     * moving and clipping the gun as the visible area list changed.
     */
    inst.sort_area = 1;
    inst.origin[0] = origin[0];
    inst.origin[1] = origin[1];
    inst.origin[2] = origin[2];

    /*
     * The rotation the model is drawn with, and the reason it is a matrix
     * rather than three angles.
     *
     * `q2_model_build_ot` composes the CAMERA's rotation with the instance's,
     * because that is what the console does for anything standing in the world.
     * A view model is not standing in the world: it is authored in view space,
     * so what has to survive that composition is the clip's own wobble and
     * nothing else. The instance matrix is therefore the camera's inverse —
     * its transpose — with the clip's rotation on top:
     *
     *      camera * (camera^T * R_clip)  ==  R_clip
     *
     * which is the port's form of the original composing `RotMatrix(view
     * angles)` into the entity's own matrix at 0x8004F474 and letting the
     * camera undo it.
     */
    {
        s16 inv[3][3], clip[3][3];
        int r, c, k;

        /*
         * The matrix to cancel is the CAMERA's, so it is built from the camera
         * — with the camera's own builder — rather than reconstructed from the
         * aim and the kick.
         *
         * Reconstructing it was exact only by luck. `q2_model_build_ot` uses
         * `q2_rotation_view(cam->yaw, cam->pitch, cam->roll)`, and this built a
         * yaw/pitch matrix from the aim and the kick, so the two agreed only
         * while the roll was zero and the caller's aim + kick happened to
         * equal the camera's angles. Taking the camera directly makes
         * `camera * (camera^T * clip) == clip` true by construction for any
         * camera, which is what the cancellation is for.
         */
        q2_rotation_view(inv, cam->yaw, cam->pitch, cam->roll);
        /*
         * `q2_vw_clip_pitch` and not `cur_r[0]`: this call IS 0x8004F3F4's
         * `RotMatrixYXZ(s3+230, s3+704)`, and the halfword at s3+230 has had
         * the owner's client+0x46 added to it four instructions earlier. The
         * camera's matrix is built without that term, which is the whole point
         * — the gun tips onto the lock and the view stays put.
         */
        q2_rotation_euler(clip, q2_vw_clip_pitch(vw),
                          vw->cur_r[1], vw->cur_r[2]);

        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                s32 acc = 0;
                for (k = 0; k < 3; k++)
                    acc += (s32)inv[k][r] * (s32)clip[k][c];   /* inv^T * clip */
                rot_out[r][c] = (s16)(acc >> Q2_FRAC_12);
            }
        }
        inst.rot = (const s16 (*)[3])rot_out;
    }

    return q2_model_build_ot(&inst, cam, ot, gte, stats);
}
