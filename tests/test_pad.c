/*
 * test_pad.c — the pad, the nine control styles, and the parts of the player's
 *              frame that only exist once there is a pad to drive them.
 *
 * Three groups, and they are separate because they fail for different reasons:
 *
 *   the styles       what q2_pad_read produces for a given pad word
 *   the view kicks   what q2_sim_view_angles composes on top of the aim
 *   the recentre     the look-button chord, which is state across ticks
 */
#include <stdio.h>
#include <string.h>

#include "pad.h"
#include "sim.h"
#include "worldscale.h"

static int g_checks;
static int g_failures;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (condition) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_i(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: got %lld, want %lld\n", what, (long long)got,
               (long long)want);
        g_failures++;
    }
}

/* One frame of a pad, from nothing. */
static void press(q2_input *out, int style, u32 buttons)
{
    q2_pad_state pad;
    q2_pad_config cfg;

    memset(&pad, 0, sizeof(pad));
    pad.buttons = buttons;

    q2_pad_config_default(&cfg);
    cfg.style = style;

    q2_pad_read(&pad, &cfg, out);
}

/* And the same buttons a second frame later. */
static void hold(q2_input *out, int style, u32 buttons)
{
    q2_pad_state pad;
    q2_pad_config cfg;

    memset(&pad, 0, sizeof(pad));
    pad.buttons = buttons;
    pad.prev    = buttons;

    q2_pad_config_default(&cfg);
    cfg.style = style;

    q2_pad_read(&pad, &cfg, out);
}

/* ------------------------------------------------------------------------- */
static void test_defaults(void)
{
    q2_pad_config cfg;

    printf("defaults\n");

    q2_pad_config_default(&cfg);
    check_eq_i(cfg.style, Q2_PAD_STYLE_STANDARD_A,
               "0x8001BDA8 leaves every player on STANDARD A");
    check_eq_i(cfg.mouse_speed, 64, "and a look scale of 64");

    check_eq_i(Q2_PAD_FULL, 127, "full digital deflection is 127, not 128");

    /*
     * The consequence, which is the reason the constant matters at all: the
     * wish target is (maxspeed * axis) >> 7, so 127 falls 22 short of the
     * speed table's own figure and 128 would land exactly on it.
     */
    check_eq_i((Q2_SPEED_NORMAL * Q2_PAD_FULL) >> Q2_WISH_SHIFT, 2778,
               "so a running player asks for 2778, not 2800");
}

/* ------------------------------------------------------------------------- */
static void test_standard_a(void)
{
    q2_input in;

    printf("\nSTANDARD A — 0x800196E8\n");

    press(&in, Q2_PAD_STYLE_STANDARD_A, Q2_PAD_UP);
    check_eq_i(in.forward, Q2_PAD_FULL, "UP walks forward");
    check(in.buttons & Q2_BTN_MOVING, "and raises the moving bit");

    press(&in, Q2_PAD_STYLE_STANDARD_A, Q2_PAD_L2);
    check_eq_i(in.side, -Q2_PAD_FULL, "L2 strafes left");
    check(!(in.buttons & Q2_BTN_MOVING),
          "but strafing alone does NOT raise the moving bit");

    press(&in, Q2_PAD_STYLE_STANDARD_A, Q2_PAD_LEFT);
    check_eq_i(in.yaw, -Q2_PAD_FULL, "LEFT turns left");

    press(&in, Q2_PAD_STYLE_STANDARD_A, Q2_PAD_R1);
    check_eq_i(in.pitch, -Q2_PAD_FULL, "R1 looks down");
    check(in.buttons & Q2_BTN_LOOK_DOWN, "and flags the look-down button");

    press(&in, Q2_PAD_STYLE_STANDARD_A, Q2_PAD_L1);
    check(in.buttons & Q2_BTN_LOOK_UP, "L1 flags the look-up button");

    press(&in, Q2_PAD_STYLE_STANDARD_A, Q2_PAD_L1 | Q2_PAD_R1);
    check((in.buttons & (Q2_BTN_LOOK_UP | Q2_BTN_LOOK_DOWN)) ==
              (Q2_BTN_LOOK_UP | Q2_BTN_LOOK_DOWN),
          "and both together set both — the chord the recentre watches");
}

/* ------------------------------------------------------------------------- */
/*
 * The finding that changes a binding rather than a number: bit 22 is the jump
 * button's press EDGE and bit 21 is the same button HELD, so they are one
 * control and a port with two keys has invented a mechanic.
 */
static void test_jump_is_swim(void)
{
    int style;

    printf("\njump and swim-up are one button — 0x80019A9C\n");

    for (style = 0; style < Q2_PAD_STYLE_COUNT; style++) {
        q2_input a, b;
        char what[96];

        /* Every button at once, so whichever mask this style uses is pressed. */
        press(&a, style, 0xFFFFu);
        hold(&b, style, 0xFFFFu);

        snprintf(what, sizeof(what), "%-14s tap jumps, hold swims",
                 q2_pad_style_name(style));
        check((a.buttons & Q2_BTN_JUMP) && (a.buttons & Q2_BTN_SWIM_UP) &&
              !(b.buttons & Q2_BTN_JUMP) && (b.buttons & Q2_BTN_SWIM_UP),
              what);
    }
}

/* ------------------------------------------------------------------------- */
static void test_fire_bits(void)
{
    q2_input a, b;

    printf("\nthe fire button's three bits — 0x80019A04\n");

    press(&a, Q2_PAD_STYLE_STANDARD_A, Q2_PAD_CROSS);
    hold(&b,  Q2_PAD_STYLE_STANDARD_A, Q2_PAD_CROSS);

    check(a.buttons & Q2_BTN_ATTACK_PRESS, "frame one has the press edge");
    check(a.buttons & Q2_BTN_ATTACK,       "frame one is held");
    check(!(a.buttons & Q2_BTN_ATTACK_REPEAT),
          "frame one is not yet a repeat");

    check(!(b.buttons & Q2_BTN_ATTACK_PRESS), "frame two has no edge");
    check(b.buttons & Q2_BTN_ATTACK,          "frame two is still held");
    check(b.buttons & Q2_BTN_ATTACK_REPEAT,   "frame two IS a repeat");

    check(a.attack && b.attack, "and `attack` tracks the held bit, not the edge");
}

/* ------------------------------------------------------------------------- */
/*
 * The mouse and stick styles scale their look axes by the MOUSE SPEED setting;
 * the two that read a stick pair raw do not. Both are worth pinning because the
 * two groups are otherwise indistinguishable from the outside.
 */
static void test_look_scaling(void)
{
    q2_pad_state pad;
    q2_pad_config cfg;
    q2_input in;

    printf("\nlook scaling — 0x80019230\n");

    memset(&pad, 0, sizeof(pad));
    pad.lx = 100;

    q2_pad_config_default(&cfg);
    cfg.style       = Q2_PAD_STYLE_RIGHT_MOUSE;
    cfg.mouse_speed = 64;
    q2_pad_read(&pad, &cfg, &in);
    check_eq_i(in.yaw, (100 * (64 + 32)) >> 4,
               "a mouse style scales by (speed + 32) >> 4");

    cfg.style = Q2_PAD_STYLE_LEFT_STICK;
    q2_pad_read(&pad, &cfg, &in);
    check_eq_i(in.yaw, 100, "LEFT STICK takes the byte raw");

    /* SWAP Y AXIS, and the two polarities that are genuinely opposite. */
    memset(&pad, 0, sizeof(pad));
    pad.ly = 100;

    cfg.style  = Q2_PAD_STYLE_RIGHT_MOUSE;
    cfg.swap_y = 0;
    q2_pad_read(&pad, &cfg, &in);
    check(in.pitch > 0, "a mouse style is not inverted by default");
    cfg.swap_y = 1;
    q2_pad_read(&pad, &cfg, &in);
    check(in.pitch < 0, "and SWAP Y AXIS inverts it");

    cfg.style  = Q2_PAD_STYLE_LEFT_STICK;
    cfg.swap_y = 0;
    q2_pad_read(&pad, &cfg, &in);
    check(in.pitch < 0,
          "LEFT STICK is inverted by default — 0x80019568 is `bne` where "
          "0x8001924C is `beq`");
    cfg.swap_y = 1;
    q2_pad_read(&pad, &cfg, &in);
    check(in.pitch > 0, "and SWAP Y AXIS makes it upright");
}

/* ------------------------------------------------------------------------- */
static void test_out_of_range(void)
{
    q2_input in;

    printf("\nan unconfigured style\n");

    press(&in, Q2_PAD_STYLE_COUNT + 3, 0xFFFFu);
    check_eq_i(in.forward, 0, "a style past the jump table moves nothing");
    check_eq_i(in.side, 0, "on either axis");
    check(!(in.buttons & Q2_BTN_JUMP),
          "and binds no button — 0x800191FC falls straight through to the tail");
}

/* ------------------------------------------------------------------------- */
/* The view kicks. Three decays, three periods, one composer.                 */
/* ------------------------------------------------------------------------- */
static void test_view_kicks(void)
{
    q2_sim sim;
    s32 spawn[3] = { 0, 0, 0 };
    s32 view[3];

    printf("\nthe view kicks — 0x80038260\n");

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 512);

    q2_sim_view_angles(&sim, view);
    check_eq_i(view[1], 512, "with no kick, the view is the aim");
    check_eq_i(view[0], 0, "and the pitch with it");

    /* A firing kick at full strength: deadline exactly one period out. */
    sim.player[0].kick[0]  = 100;
    sim.player[0].kick[2]  = -40;
    sim.player[0].kick_time = sim.level_time + Q2_VIEW_KICK_FIRE;
    q2_sim_view_angles(&sim, view);
    check_eq_i(view[0], 100, "a fresh firing kick lands at full amplitude");
    check_eq_i(view[2], -40, "on the roll as well");

    /* Half way through, half the kick. */
    sim.player[0].kick_time = sim.level_time + Q2_VIEW_KICK_FIRE / 2;
    q2_sim_view_angles(&sim, view);
    check_eq_i(view[0], 50, "half the time left is half the kick");

    /* Expired. */
    sim.player[0].kick_time = sim.level_time - 1;
    q2_sim_view_angles(&sim, view);
    check_eq_i(view[0], 0, "and a deadline in the past contributes nothing");

    /* The landing kick is the same shape over 90 ticks and pitch only. */
    memset(sim.player[0].kick, 0, sizeof(sim.player[0].kick));
    sim.player[0].fall_value = 200;
    sim.player[0].fall_time  = sim.level_time + Q2_VIEW_KICK_FALL;
    q2_sim_view_angles(&sim, view);
    check_eq_i(view[0], 200, "a fresh landing kick lands at full amplitude");
    check_eq_i(view[2], 0, "and does not touch the roll");

    /*
     * The quirk worth having a test for, because it looks like a bug: the pain
     * deadline is 210 and the hurt kick's period is 150, so a fresh hurt kick is
     * scaled by 210/150 rather than by one. That is the console's arithmetic —
     * one field doing two jobs — and clamping it would be the port inventing a
     * fix.
     */
    sim.player[0].fall_value  = 0;
    sim.player[0].hurt_kick[0] = 100;
    sim.player[0].pain_time    = sim.level_time + 210;
    q2_sim_view_angles(&sim, view);
    check_eq_i(view[0], (100 * ((210 << 12) / 150)) >> 12,
               "a fresh hurt kick overshoots, because 210 > 150");
    check(view[0] > 100, "which is more than its own amplitude");

    q2_sim_free(&sim);
}

/* ------------------------------------------------------------------------- */
/* The recentre chord.                                                        */
/* ------------------------------------------------------------------------- */
static void test_recentre(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;
    s32 pitched;

    printf("\nthe view recentre — 0x8003A780\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 0;

    /* Look down for a while. */
    in.pitch = Q2_PAD_FULL;
    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    pitched = sim.player[0].pitch;
    check(pitched > 200, "looking down moves the pitch well off level");

    /* Let go: it stays where it is. */
    in.pitch = 0;
    for (i = 0; i < 60; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].pitch > 200,
          "and releasing leaves it there — there is no automatic levelling");

    /* One tick of the chord does nothing, because the shift register cancels
     * an arm on the first tick of any press. */
    in.buttons = Q2_BTN_LOOK_UP | Q2_BTN_LOOK_DOWN;
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(!sim.player[0].recentring, "one tick of the chord does not arm it");

    /* The second tick does. */
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player[0].recentring, "the second tick arms it");

    /* And then it walks all the way to level and disarms itself. */
    in.buttons = 0;
    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].pitch, 0, "and it reaches exactly level");
    check(!sim.player[0].recentring, "then disarms");

    q2_sim_free(&sim);
}

/* ------------------------------------------------------------------------- */
/* The fly bit: one flag, four behaviours.                                    */
/* ------------------------------------------------------------------------- */
static void test_fly(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s32 fell;
    int i;

    printf("\nentity+0x10C bit 12 — 0x8003A41C\n");

    memset(&in, 0, sizeof(in));

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y = 100000;      /* nothing to land on */
    for (i = 0; i < 30; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    fell = sim.player[0].pos[1] - spawn[1];
    check(fell > 0, "with the flag clear the player falls");
    q2_sim_free(&sim);

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player[0].ground_y        = 100000;
    sim.full_basis_movement    = true;
    for (i = 0; i < 30; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].pos[1] - spawn[1], 0,
               "with it set there is no gravity at all");
    check(sim.player[0].ent2_flags & Q2_ENT2_FLY,
          "and the frame mirrors the setting into the flag every tick");

    /* And the jump is off, because a weightless entity has nothing to leave. */
    in.buttons = Q2_BTN_JUMP;
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player[0].jump_hold, 0, "the jump is skipped while flying");

    q2_sim_free(&sim);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC pad and view tests\n\n");

    test_defaults();
    test_standard_a();
    test_jump_is_swim();
    test_fire_bits();
    test_look_scaling();
    test_out_of_range();
    test_view_kicks();
    test_recentre();
    test_fly();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
