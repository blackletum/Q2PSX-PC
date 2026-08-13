/*
 * test_sim.c — the simulation's timing and physics behaviour.
 *
 * The properties worth pinning are the ones that would change how the game
 * *feels* without ever crashing: the tick rate, the long-frame clamp, and the
 * fact that everything stays integer.
 */
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "trig.h"

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
static void test_tick_rate(void)
{
    q2_sim sim;
    q2_input in;
    u32 ticks;

    printf("tick rate\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);

    /* The logic step is 12 dt units out of 300 per second, i.e. 25 Hz. One
     * second of real time must therefore produce 25 ticks -- not 50, not 60.
     * Getting this wrong changes every jump arc in the game. */
    ticks = 0;
    {
        int i;
        for (i = 0; i < 50; i++)              /* 50 fields == one PAL second */
            ticks += q2_sim_advance(&sim, &in, 1.0 / 50.0);
    }
    check_eq_i(ticks, 25, "one second of PAL fields runs 25 logic ticks");

    /* A long frame is clamped rather than sub-stepped. The original slowed the
     * world down instead of taking a huge step, and that is the behaviour. */
    q2_sim_init(&sim, NULL, 50);
    ticks = q2_sim_advance(&sim, &in, 10.0);   /* an absurd stall */
    check(ticks <= 3, "a 10-second stall is clamped, not integrated in full");
    printf("  a 10s stall produced %u ticks (clamped at %d dt)\n", ticks, Q2_DT_MAX);
}

/* ------------------------------------------------------------------------- */
static void test_gravity(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;
    s32 last_y;

    printf("gravity\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);

    /* Put the ground far below so the fall is unobstructed. World Y increases
     * downward on this disc, so "far below" is a large positive Y. */
    sim.player.ground_y  = 100000;
    sim.player.on_ground = false;

    last_y = sim.player.pos[1];
    for (i = 0; i < 10; i++) {
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
        check(sim.player.pos[1] >= last_y, "falling moves the player downward");
        last_y = sim.player.pos[1];
    }
    check(sim.player.vel[1] > 0, "downward velocity accumulates");

    /*
     * Terminal velocity must actually bound the fall.
     *
     * The ground has to be effectively unreachable for this to mean anything.
     * An earlier version of this test used ground_y = 100000, the player landed
     * within a few ticks, velocity was zeroed on landing, and the assertion
     * "vel <= terminal" passed against a velocity of zero — green, and proving
     * nothing. Assert that terminal is REACHED as well as not exceeded, so the
     * test fails if the clamp is removed or if the player lands early.
     */
    sim.player.ground_y  = INT32_MAX;
    sim.player.on_ground = false;

    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    check(sim.player.vel[1] == Q2_TERMINAL_VY,
          "a long fall reaches exactly terminal velocity and stops there");
    printf("  velocity after 200 falling ticks: %d (terminal %d)\n",
           sim.player.vel[1], Q2_TERMINAL_VY);

    /* And it must not creep past it on further ticks. */
    for (i = 0; i < 50; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player.vel[1], Q2_TERMINAL_VY, "terminal velocity is stable");
}

/* ------------------------------------------------------------------------- */
static void test_ground_and_view(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    s32 eye[3];
    int i;

    printf("ground and view height\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player.ground_y = 0;

    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player.on_ground, "the player lands on the ground plane");
    check_eq_i(sim.player.pos[1], 0, "the player rests exactly on the ground");

    /* The eye is above the feet, which with Y-down means a smaller Y. */
    q2_sim_eye(&sim, eye);
    check(eye[1] < sim.player.pos[1], "the eye sits above the feet");
    check_eq_i(sim.player.pos[1] - eye[1], Q2_VIEW_STAND, "standing eye height");

    /* Crouching eases the view down rather than snapping. */
    in.crouch = true;
    q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player.view_height < Q2_VIEW_STAND, "crouching lowers the view");
    check(sim.player.view_height > Q2_VIEW_CROUCH, "the view eases rather than snapping");

    for (i = 0; i < 40; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player.view_height, Q2_VIEW_CROUCH, "the view settles at crouch height");

    in.crouch = false;
    for (i = 0; i < 40; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check_eq_i(sim.player.view_height, Q2_VIEW_STAND, "and returns to standing");
}

/* ------------------------------------------------------------------------- */
static void test_movement(void)
{
    q2_sim sim;
    q2_input in;
    s32 spawn[3] = { 0, 0, 0 };
    int i;

    printf("movement\n");

    memset(&in, 0, sizeof(in));
    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 0);
    sim.player.ground_y = 0;

    /* Facing yaw 0, forward is +Z. */
    in.forward = 1024;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

    check(sim.player.pos[2] > 0, "holding forward at yaw 0 moves along +Z");
    check_eq_i(sim.player.pos[0], 0, "and does not drift sideways");
    printf("  20 ticks of forward moved %d world units (%d PC units)\n",
           sim.player.pos[2], sim.player.pos[2] / Q2_WORLD_SCALE);

    /* Releasing input must bring the player to rest, not coast forever. */
    in.forward = 0;
    for (i = 0; i < 200; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player.vel[2] == 0 || sim.player.vel[2] < 16,
          "friction brings the player to rest");

    /* Turning right by a quarter circle must make forward run along +X. */
    q2_sim_spawn(&sim, spawn, Q2_ANGLE_90);
    sim.player.ground_y = 0;
    in.forward = 1024;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);
    check(sim.player.pos[0] > 0, "at yaw 90 forward moves along +X");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC simulation tests\n\n");

    test_tick_rate();
    test_gravity();
    test_ground_and_view();
    test_movement();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
