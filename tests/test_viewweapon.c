/*
 * test_viewweapon.c — the weapon in the player's hands, as behaviour.
 *
 * `q2psx-inspect viewweapon <disc>` already reads the real animation bank off a
 * real executable and checks every constant against the instruction it came
 * from, so nothing here re-asserts a key offset. What this pins down is the
 * behaviour that no table can express: that the state machine cycles the way
 * the original's transitions say it does, that LOWER holds at the bottom of its
 * arc until the seventy-tick countdown expires rather than swapping early, that
 * a shot cannot be cancelled by switching weapons, that running dry does not
 * wedge the machine, that a long frame plays a short clip through instead of
 * skipping the events on it, and that the weapon is placed on the eye.
 *
 * The bank here is synthetic, with clip lengths chosen so that each transition
 * is unambiguous. That is deliberate: a test that used the real bank would be
 * testing the disc as much as the code.
 */
#include "viewweapon.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------------------- */
/* A bank with one key per state and a distinct duration each, so that "which  */
/* clip are we in" is readable from the clock alone.                          */
/* ------------------------------------------------------------------------- */
#define RAISE_TICKS  40
#define FIRE_TICKS   30
#define IDLE_TICKS   50
#define LOWER_TICKS  20

static q2_vm_key   g_keys[Q2_VM_SLOTS][Q2_VM_STATES];
static q2_vm_tables g_tab;

static void build_bank(void)
{
    static const s16 dur[Q2_VM_STATES] = {
        RAISE_TICKS, FIRE_TICKS, IDLE_TICKS, LOWER_TICKS
    };
    int w, s;

    memset(&g_tab, 0, sizeof(g_tab));
    memset(g_keys, 0, sizeof(g_keys));

    for (w = 0; w < Q2_VM_SLOTS; w++) {
        for (s = 0; s < Q2_VM_STATES; s++) {
            q2_vm_key *k = &g_keys[w][s];

            k->duration = dur[s];
            k->event    = Q2_VM_EVENT_NONE;
            /* A translation big enough that rotating it is measurable. */
            k->t[0] = (s16)(100 + w * 10);
            k->t[1] = (s16)(200 + s * 10);
            k->t[2] = (s16)300;

            g_tab.clip[w][s].count = 1;
            g_tab.clip[w][s].key   = k;
            g_tab.clip[w][s].addr  = 0x8009D000u + (u32)(w * 4 + s) * 20u;
        }
        snprintf(g_tab.model_name[w], sizeof(g_tab.model_name[w]),
                 "Weapon %d", w);
    }

    /* The fire clip of weapon 1 carries an event, so the event path is live. */
    g_keys[1][Q2_VM_FIRE].event = 7;
}

/* Run the machine until `state` is reached or the budget runs out. */
static int run_until(q2_viewweapon *vw, q2_vm_state state, bool fire,
                     int budget)
{
    int t;

    for (t = 0; t < budget; t++) {
        if (vw->state == state)
            return t;
        q2_vw_advance(vw, 10, fire, Q2_VW_FIRED);
    }
    return (vw->state == state) ? t : -1;
}

/* ------------------------------------------------------------------------- */
static void test_raise_to_idle(void)
{
    q2_viewweapon vw;
    int t;

    q2_vw_init(&vw, &g_tab, 1);
    CHECK(vw.state == Q2_VM_RAISE, "a fresh weapon starts in raise, got %s",
          q2_vm_state_name(vw.state));
    CHECK(vw.weapon == 1, "weapon %d", vw.weapon);

    t = run_until(&vw, Q2_VM_IDLE, false, 100);
    CHECK(t >= 0, "the raise never reached idle");
    CHECK(vw.state == Q2_VM_IDLE, "state %s", q2_vm_state_name(vw.state));

    /* Idle loops rather than running out. */
    {
        int i;
        for (i = 0; i < 200; i++)
            q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
        CHECK(vw.state == Q2_VM_IDLE, "idle should loop, got %s",
              q2_vm_state_name(vw.state));
    }
}

static void test_fire(void)
{
    q2_viewweapon vw;
    s16 ev = 0;
    int i;
    bool got_event = false;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);

    /* The trigger takes effect on the tick it is pressed, not at the next key
     * boundary — 0x8004FAF4 tests it every tick. */
    q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_FIRE, "the trigger should enter fire, got %s",
          q2_vm_state_name(vw.state));
    CHECK(vw.fire_latch, "the latch should be set while a shot is in flight");

    /* Holding must NOT restart the clip every tick. */
    for (i = 0; i < 200 && vw.state == Q2_VM_FIRE; i++) {
        q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
        if (q2_vw_take_event(&vw, &ev) && ev == 7)
            got_event = true;
    }
    CHECK(i < 200, "the fire clip never ended while the trigger was held");
    CHECK(got_event, "the fire clip's event was never raised");

    /* Releasing clears the latch so the next press fires again. */
    q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
    CHECK(!vw.fire_latch, "releasing the trigger should clear the latch");
}

static void test_fire_denied(void)
{
    q2_viewweapon vw;
    int i;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);

    /*
     * An empty weapon must not enter the fire clip and must not wedge: the
     * original clears the latch and recomputes the neighbours (0x8004FB5C), and
     * the machine keeps idling.
     */
    for (i = 0; i < 50; i++)
        q2_vw_advance(&vw, 10, true, Q2_VW_FIRE_DENIED);

    CHECK(vw.state == Q2_VM_IDLE, "a denied shot should leave it idle, got %s",
          q2_vm_state_name(vw.state));
    CHECK(!vw.fire_latch, "a denied shot should not latch");
}

static void test_switch(void)
{
    q2_viewweapon vw;
    int i;
    bool swapped = false;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);

    q2_vw_select(&vw, 5);
    CHECK(vw.switch_ticks == Q2_VW_SWITCH_TICKS,
          "selecting should arm the %d-tick countdown, got %d",
          Q2_VW_SWITCH_TICKS, vw.switch_ticks);

    /* It must LOWER first, and the model must not change while it does. */
    q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_LOWER, "selecting should lower, got %s",
          q2_vm_state_name(vw.state));
    CHECK(vw.weapon == 1, "the weapon must not change during the lower");

    /*
     * The countdown gates the swap. The lower clip is 20 ticks and the
     * countdown is 70, so the machine must still be holding the old weapon well
     * after the clip would otherwise have ended.
     */
    for (i = 0; i < 4; i++)
        q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
    CHECK(vw.weapon == 1,
          "the swap happened before the countdown expired (ticks left %d)",
          vw.switch_ticks);

    for (i = 0; i < 40 && !swapped; i++)
        swapped = q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);

    CHECK(swapped, "the weapon never swapped");
    CHECK(vw.weapon == 5, "swapped to %d, wanted 5", vw.weapon);
    CHECK(vw.state == Q2_VM_RAISE, "a swap should raise the new weapon, got %s",
          q2_vm_state_name(vw.state));
    CHECK(strcmp(q2_vw_model_name(&vw), "Weapon 5") == 0,
          "model name '%s'", q2_vw_model_name(&vw));
}

static void test_switch_cannot_cancel_a_shot(void)
{
    q2_viewweapon vw;

    q2_vw_init(&vw, &g_tab, 1);
    run_until(&vw, Q2_VM_IDLE, false, 100);
    q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_FIRE, "should be firing");

    /* 0x8004FAB4 excludes FIRE from the lower transition. */
    q2_vw_select(&vw, 3);
    q2_vw_advance(&vw, 5, false, Q2_VW_FIRED);
    CHECK(vw.state == Q2_VM_FIRE,
          "switching must not cancel a shot in flight, got %s",
          q2_vm_state_name(vw.state));
}

/*
 * A long host frame must play the clip through rather than skipping it: the
 * original consumes dt in chunks of min(left, dt) (0x8004EEA4), and the events
 * on the keys it steps over are what fire the shot.
 */
static void test_long_frame_consumes_keys(void)
{
    q2_viewweapon vw;
    u32 played_small, played_big;
    int i;

    q2_vw_init(&vw, &g_tab, 1);
    for (i = 0; i < 30; i++)
        q2_vw_advance(&vw, 10, false, Q2_VW_FIRED);
    played_small = vw.keys_played;

    q2_vw_init(&vw, &g_tab, 1);
    q2_vw_advance(&vw, 300, false, Q2_VW_FIRED);
    played_big = vw.keys_played;

    CHECK(played_big == played_small,
          "one 300-tick frame played %u keys, thirty 10-tick frames played %u",
          played_big, played_small);
}

/* ------------------------------------------------------------------------- */
static void test_placement(void)
{
    q2_viewweapon vw;
    s32 feet[3] = { 1000, 2000, 3000 };
    s32 stand[3], crouch[3], ang[3];
    s16 zero[3] = { 0, 0, 0 };

    q2_vw_init(&vw, &g_tab, 1);
    q2_vw_advance(&vw, 20, false, Q2_VW_FIRED);

    q2_vw_place(&vw, feet, 576, zero, zero, stand, ang);
    q2_vw_place(&vw, feet, 286, zero, zero, crouch, ang);

    /* FORMATS §9.12: eye.y = pos.y + 286 - viewOffset, and +Y is down, so a
     * smaller view offset puts the weapon LOWER (a larger y). */
    CHECK(crouch[1] - stand[1] == 290,
          "crouching should drop the weapon by 290, got %d",
          crouch[1] - stand[1]);

    /* The aim's x component is negated at the sum (0x8004F41C), so a positive
     * pitch input must produce a negative contribution to the angle. */
    {
        s16 pitch_up[3] = { 512, 0, 0 };
        s32 a0[3], a1[3], o[3];

        q2_vw_place(&vw, feet, 576, zero, zero, o, a0);
        q2_vw_place(&vw, feet, 576, pitch_up, zero, o, a1);
        CHECK(a1[0] - a0[0] == -512,
              "pitch must enter negated: delta %d", a1[0] - a0[0]);
        CHECK(a1[1] == a0[1] && a1[2] == a0[2],
              "pitch must not disturb yaw or roll");
    }

    /* Kick adds to aim rather than replacing it. */
    {
        s16 aim[3]  = { 0, 100, 0 };
        s16 kick[3] = { 0,  25, 0 };
        s32 a0[3], a1[3], o[3];

        q2_vw_place(&vw, feet, 576, aim, zero, o, a0);
        q2_vw_place(&vw, feet, 576, aim, kick, o, a1);
        CHECK(a1[1] - a0[1] == 25, "kick must add: delta %d", a1[1] - a0[1]);
    }

    /* Rigidity: a turn swings the weapon around the eye without stretching it. */
    {
        s16 y1[3] = { 0, 1024, 0 };
        s32 o0[3], o1[3];
        s32 eye_y = feet[1] + Q2_VW_EYE_BASE - 576;
        s64 d0 = 0, d1 = 0;
        int i;

        q2_vw_place(&vw, feet, 576, zero, zero, o0, ang);
        q2_vw_place(&vw, feet, 576, y1,   zero, o1, ang);

        for (i = 0; i < 3; i++) {
            s32 a = (i == 1) ? o0[i] - eye_y : o0[i] - feet[i];
            s32 b = (i == 1) ? o1[i] - eye_y : o1[i] - feet[i];
            d0 += (s64)a * a;
            d1 += (s64)b * b;
        }

        CHECK(o0[0] != o1[0] || o0[1] != o1[1] || o0[2] != o1[2],
              "a quarter turn should move the weapon");
        CHECK(d0 - d1 < 4096 * 4096 && d1 - d0 < 4096 * 4096,
              "a turn must not stretch it: %lld vs %lld",
              (long long)d0, (long long)d1);
    }
}

/* Weapon 0 is a live state, not an error: the clip table aliases slot 0 to
 * slot 1 exactly as the fire-function table does. */
static void test_no_weapon(void)
{
    q2_viewweapon vw;
    int i;

    q2_vw_init(&vw, &g_tab, 0);
    for (i = 0; i < 100; i++)
        q2_vw_advance(&vw, 10, true, Q2_VW_FIRED);
    CHECK(vw.weapon == 0, "weapon 0 should stay 0");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    build_bank();

    test_raise_to_idle();
    test_fire();
    test_fire_denied();
    test_switch();
    test_switch_cannot_cancel_a_shot();
    test_long_frame_consumes_keys();
    test_placement();
    test_no_weapon();

    if (g_fail == 0)
        printf("test_viewweapon: all checks passed\n");
    else
        printf("test_viewweapon: %d check%s failed\n",
               g_fail, g_fail == 1 ? "" : "s");

    return g_fail ? 1 : 0;
}
