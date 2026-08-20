/*
 * test_playerdeath.c — the player death chain's behaviour.
 *
 * Everything asserted here was read out of five functions in SLES_015.34 —
 * 0x800396AC, 0x80039550, 0x8003E238, 0x8005B358 and 0x8003CE14 — and the
 * comments in playerdeath.h carry the addresses. What the tests are for is the
 * behaviour that is easy to get subtly wrong and impossible to see in a
 * screenshot: which deaths make a sound, which one-shots really are one-shots,
 * and the two different endings single player and deathmatch give a body.
 */
#include "playerdeath.h"

#include "pad.h"   /* Q2_PAD_FULL */
#include "sim.h"

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

/* The frame step the sim runs at, in the 300-to-the-second clock. */
#define DT 10

/* ------------------------------------------------------------------------- */
/* The moves                                                                  */
/* ------------------------------------------------------------------------- */

static void test_move_names(void)
{
    /* The order is the order 0x8003C5F8..0x8003CBFC looks them up in, which is
     * also their order in the pool at 0x800AC554, twelve bytes apart. */
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_STAND),  "Stand")   == 0, "0");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_RUN),    "Run")     == 0, "1");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_ATTAK),  "Attak")   == 0, "2");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_DEATH1), "Death 1") == 0, "3");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_DEATH2), "Death 2") == 0, "4");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_DEATH3), "Death 3") == 0, "5");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_JUMP),   "Jump")    == 0, "6");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_PAIN1),  "Pain 1")  == 0, "7");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_PAIN2),  "Pain 2")  == 0, "8");
    CHECK(strcmp(q2_player_move_name(Q2_PMOVE_PAIN3),  "Pain 3")  == 0, "9");
}

static void test_anim_choice(void)
{
    q2_player_move m;

    /* The three death moves are a `rand() % 3` (0x8003CEEC). */
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 0);
    CHECK(m == Q2_PMOVE_DEATH1, "roll 0 is Death 1, got %d", (int)m);
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 1);
    CHECK(m == Q2_PMOVE_DEATH2, "roll 1 is Death 2, got %d", (int)m);
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 2);
    CHECK(m == Q2_PMOVE_DEATH3, "roll 2 is Death 3, got %d", (int)m);
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_STAND, 0, 3);
    CHECK(m == Q2_PMOVE_DEATH1, "roll 3 wraps to Death 1, got %d", (int)m);

    /* And so are the pain moves (0x8003CF74). */
    m = q2_player_anim_pick(Q2_PANIM_PAIN, Q2_PMOVE_STAND, 0, 1);
    CHECK(m == Q2_PMOVE_PAIN2, "roll 1 is Pain 2, got %d", (int)m);
}

static void test_a_death_move_is_never_replaced(void)
{
    /* 0x8003CEB4 refuses to choose over one and 0x8003D158 refuses to install
     * over one. Both say the same thing, so the test asks for everything. */
    q2_player_anim a;

    for (a = Q2_PANIM_STAND; a <= Q2_PANIM_ATTACK; a++) {
        q2_player_move m = q2_player_anim_pick(a, Q2_PMOVE_DEATH2,
                                               Q2_PDEATH_ANIM_WRAPPED, 0);
        CHECK(m == Q2_PMOVE_NONE,
              "anim %d displaced a death move with %d", (int)a, (int)m);
    }
}

static void test_pain_holds_but_death_cuts_in(void)
{
    q2_player_move m;

    /* 0x8003D188: a pain move that has not wrapped holds. */
    m = q2_player_anim_pick(Q2_PANIM_RUN, Q2_PMOVE_PAIN1, 0, 0);
    CHECK(m == Q2_PMOVE_NONE, "run interrupted an unfinished pain, got %d",
          (int)m);

    /* 0x8003D1D8: DEATH is the exception. */
    m = q2_player_anim_pick(Q2_PANIM_DEATH, Q2_PMOVE_PAIN1, 0, 0);
    CHECK(m == Q2_PMOVE_DEATH1, "death did not cut into pain, got %d", (int)m);

    /* Once it has wrapped, anything may follow it. */
    m = q2_player_anim_pick(Q2_PANIM_RUN, Q2_PMOVE_PAIN1,
                            Q2_PDEATH_ANIM_WRAPPED, 0);
    CHECK(m == Q2_PMOVE_RUN, "a finished pain did not release, got %d", (int)m);
}

static void test_stand_does_not_cut_an_attack(void)
{
    /* 0x8003D008. */
    q2_player_move m = q2_player_anim_pick(Q2_PANIM_STAND, Q2_PMOVE_ATTAK,
                                           0, 0);
    CHECK(m == Q2_PMOVE_NONE, "stand cut an unwrapped attack, got %d", (int)m);

    m = q2_player_anim_pick(Q2_PANIM_STAND, Q2_PMOVE_ATTAK,
                            Q2_PDEATH_ANIM_WRAPPED, 0);
    CHECK(m == Q2_PMOVE_STAND, "a wrapped attack did not release, got %d",
          (int)m);
}

/* ------------------------------------------------------------------------- */
/* The gate                                                                   */
/* ------------------------------------------------------------------------- */

static void test_the_gate(void)
{
    /* 0x8003ADC0 is `bgtz`, so zero health is dead. */
    CHECK(!q2_player_should_die(1, 0), "1 health is alive");
    CHECK(q2_player_should_die(0, 0), "0 health is dead");
    CHECK(q2_player_should_die(-353, 0), "negative health is dead");

    /* 0x8003ADD4: once the corpse think has raised the bit, the gate is shut —
     * which is what stops the handler running twice. */
    CHECK(!q2_player_should_die(-1, Q2_PDEATH_DEAD_BIT),
          "the DEAD bit did not shut the gate");
}

/* ------------------------------------------------------------------------- */
/* The handler                                                                */
/* ------------------------------------------------------------------------- */

static void test_only_a_death_with_no_killer_cries_out(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    /* 0x80039728: shot by player 2, no sound at all. */
    q2_player_death_init(&d);
    q2_player_die(&d, 2, Q2_MP_MOD_SELF_LAST + 8 /* an ordinary weapon */,
                  0, true, false, &ev);
    CHECK(!ev.cried_out, "a death with a killer cried out");

    /* Killed by the world: the voice is raised. */
    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, true, false, &ev);
    CHECK(ev.cried_out, "a world kill did not cry out");
    CHECK(!ev.drowned, "it should be the ordinary voice");

    /* client+0x84 picks the second one. */
    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, true, true, &ev);
    CHECK(ev.drowned, "drowning did not pick the drowning voice");
}

static void test_lava_erases_the_killer_on_the_entity(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    /* 0x800396CC. Player 3 pushed you in; the lava killed you, and the lava is
     * nobody. Because the correction lands on the entity, the death also cries
     * out — which it would not have done had player 3 kept the credit. */
    q2_player_death_init(&d);
    q2_player_die(&d, 3, Q2_MP_MOD_SELF_LAST /* lava */, 0, true, false, &ev);
    CHECK(d.killer == -1, "lava left a killer of %d", (int)d.killer);
    CHECK(ev.cried_out, "an unattributed death did not cry out");
    CHECK(ev.frag_hook, "the hook is still called, with -1");
    CHECK(ev.frag_killer == -1, "the hook got killer %d", ev.frag_killer);

    /* And an ordinary means of death leaves the attacker alone. */
    q2_player_death_init(&d);
    q2_player_die(&d, 3, Q2_MP_MOD_SELF_LAST + 8, 0, true, false, &ev);
    CHECK(d.killer == 3, "an ordinary kill lost its killer, got %d",
          (int)d.killer);
}

static void test_a_fresh_spawn_owes_nobody_a_frag(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    /* 0x8003DE34 writes 4, not -1, and the hook's own `killer < 4` rejects it.
     * So a player who dies without ever being hit — walking into a crusher on
     * the first tick — scores for nobody rather than for player -1. */
    q2_player_death_init(&d);
    CHECK(d.killer == Q2_PDEATH_NO_KILLER, "a fresh spawn starts at %d",
          (int)d.killer);

    q2_player_die(&d, (s8)Q2_PDEATH_NO_KILLER, 0, 0, true, false, &ev);
    CHECK(d.killer == -1, "the sentinel should attribute to nobody");
    CHECK(ev.frag_hook, "a world kill still reaches the hook");
    CHECK(ev.frag_victim == 0, "the victim is the player who died");
}

static void test_the_handler_runs_once(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, &ev);
    CHECK(ev.frag_hook, "the first death scored");
    CHECK(d.stage == Q2_PDEATH_DYING, "the body is not dying, stage %d",
          (int)d.stage);

    /* The original swaps +0x3C, so the player think that got here is gone. A
     * second call must do nothing — not score a second frag. */
    q2_player_die(&d, 1, 18, 0, true, false, &ev);
    CHECK(!ev.frag_hook, "the handler scored twice");
    CHECK(!ev.death_page, "the handler opened a page twice");
}

static void test_single_player_opens_the_page_and_arms_the_walk_back(void)
{
    q2_player_death       d;
    q2_player_death_event ev;

    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, false /* single player */, false, &ev);
    CHECK(ev.death_page, "single player did not open page 41");
    CHECK(ev.abandon_armed, "single player did not arm the walk-back");
    CHECK(!ev.frag_hook, "single player called the frag hook");
    CHECK(!ev.body_recorded, "single player pushed a body record");

    /* And deathmatch does neither of the first two. */
    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, true, false, &ev);
    CHECK(!ev.death_page, "deathmatch opened the death page");
    CHECK(!ev.abandon_armed, "deathmatch armed the walk-back");
    CHECK(ev.body_recorded, "deathmatch did not push a body record");
}

static void test_the_handler_reuses_the_weapon_model_field(void)
{
    q2_player_death d;

    /* 0x800397F8: the linked weapon model is released and the same word
     * becomes the gib threshold. */
    q2_player_death_init(&d);
    CHECK(d.linked_weapon, "a live player has a linked weapon model");
    q2_player_die(&d, 1, 18, 0, false, false, NULL);
    CHECK(!d.linked_weapon, "the weapon model was not released");
    CHECK(d.gib_health == Q2_PDEATH_GIB_HEALTH, "gib_health is %d",
          (int)d.gib_health);
    CHECK(d.corpse_ticks == Q2_PDEATH_CORPSE_TICKS, "corpse timer is %d",
          (int)d.corpse_ticks);
    CHECK(!d.has_body, "the player record still points at a body");
}

/* ------------------------------------------------------------------------- */
/* The body                                                                   */
/* ------------------------------------------------------------------------- */

static void test_single_player_body_lies_where_it_fell(void)
{
    q2_player_death d;
    int             i;

    q2_player_death_init(&d);
    q2_player_die(&d, -1, 0, 0, false, false, NULL);

    /* 0x80039610 sends single player straight to `ent2 |= DEAD` and nowhere
     * else: respawn_think is only reached through the deathmatch branch, so no
     * amount of time fades the body. */
    for (i = 0; i < 1000; i++)
        CHECK(q2_player_death_tick(&d, -10, DT, false, 0),
              "the single-player body left the world on tick %d", i);

    CHECK(d.stage == Q2_PDEATH_DYING, "stage is %d", (int)d.stage);
    CHECK((d.ent2 & Q2_PDEATH_DEAD_BIT) != 0, "the DEAD bit was never raised");
    CHECK(d.scale == Q2_PDEATH_SCALE_ONE, "the body shrank in single player");
}

static void test_the_dead_bit_waits_for_the_animation_in_deathmatch(void)
{
    q2_player_death d;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);

    /* 0x80039618: while +0x102 is clear the body is still falling. */
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK(d.move == Q2_PMOVE_DEATH1, "no death move was installed, got %d",
          (int)d.move);
    CHECK((d.ent2 & Q2_PDEATH_DEAD_BIT) == 0,
          "DEAD was raised before the animation ended");
    CHECK(d.stage == Q2_PDEATH_DYING, "stage is %d", (int)d.stage);

    /* Asking again while it plays does not restart it. */
    q2_player_death_tick(&d, -10, DT, true, 2);
    CHECK(d.move == Q2_PMOVE_DEATH1, "the death move was restarted as %d",
          (int)d.move);

    /* 0x8003DF90 raises bit 0 when the cursor walks past the end. */
    q2_player_death_anim_ended(&d);
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK((d.ent2 & Q2_PDEATH_DEAD_BIT) != 0, "DEAD was not raised");
    CHECK((d.ent2 & Q2_PDEATH_SETTLED_BIT) != 0, "0x8000 was not raised");
    CHECK(d.box_y == Q2_PDEATH_BODY_BOX_Y, "the box was not flattened, %d",
          (int)d.box_y);
    CHECK(d.stage == Q2_PDEATH_DOWN, "stage is %d", (int)d.stage);
}

static void test_the_body_dissolves_after_its_five_seconds(void)
{
    q2_player_death d;
    int             i;
    int             fade_started = -1;
    int             gone         = -1;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    q2_player_death_anim_ended(&d);

    for (i = 0; i < 400; i++) {
        bool alive = q2_player_death_tick(&d, -10, DT, true, 0);

        if (fade_started < 0 && d.stage == Q2_PDEATH_FADING)
            fade_started = i;
        if (!alive) {
            gone = i;
            break;
        }
    }

    /* Tick 0 is the one that finds the animation over and moves the body to
     * DOWN; ticks 1..150 spend the 1500. */
    CHECK(fade_started == Q2_PDEATH_CORPSE_TICKS / DT,
          "the fade started on tick %d, expected %d", fade_started,
          Q2_PDEATH_CORPSE_TICKS / DT);

    /* 4096 of scale at dt*16 a tick — 25.6 ticks, so 26 of them. */
    {
        const int step = DT * Q2_PDEATH_FADE_RATE;
        const int fade = (Q2_PDEATH_SCALE_ONE + step - 1) / step;

        CHECK(gone == fade_started + fade,
              "the body left on tick %d, expected %d", gone,
              fade_started + fade);
    }
    CHECK(d.stage == Q2_PDEATH_GONE, "stage is %d", (int)d.stage);
}

static void test_a_body_can_still_be_gibbed(void)
{
    q2_player_death d;

    /* 0x8003E270 repeats corpse_think's own test, so a rocket into a body that
     * is already down still takes it apart. */
    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    q2_player_death_anim_ended(&d);
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK(d.stage == Q2_PDEATH_DOWN, "stage is %d", (int)d.stage);

    CHECK(!q2_player_death_tick(&d, Q2_PDEATH_GIB_HEALTH, DT, true, 0),
          "the body survived being gibbed");
    CHECK(d.stage == Q2_PDEATH_GIBBED, "stage is %d", (int)d.stage);

    /* -40 exactly is gibbed: `gib_health < health` is what the original calls
     * alive, so the boundary belongs to the gib. */
    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    q2_player_death_tick(&d, Q2_PDEATH_GIB_HEALTH + 1, DT, true, 0);
    CHECK(d.stage == Q2_PDEATH_DYING, "-39 should not gib, stage %d",
          (int)d.stage);
}

static void test_the_corpse_slows_down(void)
{
    q2_player_death d;

    q2_player_death_init(&d);
    q2_player_die(&d, 1, 18, 0, true, false, NULL);
    d.velocity[0] = 300;
    d.velocity[1] = -20;
    d.velocity[2] = 0;

    /* 0x800395A0: dt * 5 toward zero, clamped rather than overshot. */
    q2_player_death_tick(&d, -10, DT, true, 0);
    CHECK(d.velocity[0] == 300 - DT * Q2_PDEATH_FRICTION, "vx is %d",
          (int)d.velocity[0]);
    CHECK(d.velocity[1] == 0, "vy overshot zero to %d", (int)d.velocity[1]);
    CHECK(d.velocity[2] == 0, "vz moved off zero to %d", (int)d.velocity[2]);
}

/* ------------------------------------------------------------------------- */
/* The two endings                                                            */
/* ------------------------------------------------------------------------- */

static void test_the_walk_back_to_the_front_end(void)
{
    s32 ticks = 0;
    int i;

    /* 0x80041D38: an unarmed deadline never fires. */
    for (i = 0; i < 100; i++)
        CHECK(!q2_player_abandon_tick(&ticks, DT), "an unarmed deadline fired");

    ticks = Q2_PDEATH_ABANDON_TICKS;
    for (i = 0; i < Q2_PDEATH_ABANDON_TICKS / DT - 1; i++)
        CHECK(!q2_player_abandon_tick(&ticks, DT), "it fired early on %d", i);
    CHECK(q2_player_abandon_tick(&ticks, DT), "it never fired");

    /* 0x80041DC8 clears it, so it fires exactly once. */
    CHECK(!q2_player_abandon_tick(&ticks, DT), "it fired twice");
}

static void test_resupply_is_spent(void)
{
    int n = 2;

    /* 0x8001FF0C, and it is the only write to 0x800B335D in the executable. */
    CHECK(q2_player_spend_resupply(&n) && n == 1, "first spend left %d", n);
    CHECK(q2_player_spend_resupply(&n) && n == 0, "second spend left %d", n);

    /* The page greys the row at zero and takes it out of the navigation, so the
     * original cannot reach the decrement from empty. Clamping rather than
     * wrapping a byte to 255 is therefore not a behaviour change. */
    CHECK(!q2_player_spend_resupply(&n), "an empty resupply was spent");
    CHECK(n == 0, "an empty resupply wrapped to %d", n);
}


/* ------------------------------------------------------------------------- */
/* Nobody is driving                                                          */
/* ------------------------------------------------------------------------- */
/*
 * The two things a corpse must not do, and both of them come out of ONE fact:
 * `player_die` overwrites `entity+0x3C` with the corpse think (0x80039818), so
 * the player think at 0x8003A1C8 is not installed any more.
 *
 * 0x8003A4A4 is the pad read's only call site and 0x8003AD98 is the view
 * weapon driver's only call site, and BOTH are inside that function. So a
 * corpse is not steered and does not hold a gun, without either being a rule
 * anybody wrote down.
 */

static void test_a_corpse_is_not_driven_by_the_pad(void)
{
    q2_sim   driven, still;
    q2_input full, none;
    s16      yaw;
    int      i;

    memset(&none, 0, sizeof(none));
    full          = none;
    full.forward  = Q2_PAD_FULL;
    full.yaw      = Q2_PAD_FULL;
    full.buttons  = Q2_BTN_MOVING;

    /* Alive, that input turns the player — otherwise the rest of this test
     * would pass on a sim that ignored input altogether. */
    q2_sim_init(&driven, NULL, 50);
    yaw = (s16)driven.player[0].yaw;
    for (i = 0; i < 20; i++)
        q2_sim_tick(&driven, &full, 12);
    CHECK(driven.player[0].yaw != yaw, "a live player did not turn");

    /*
     * Dead, it changes nothing at all. Asserted by DIFFERENCE rather than by
     * an absolute position, because the body is still falling: two identical
     * corpses, one handed the stick at full deflection and one handed nothing,
     * must end the same twenty ticks in exactly the same state. Any difference
     * is input reaching a player who has none.
     */
    q2_sim_init(&driven, NULL, 50);
    q2_sim_init(&still,  NULL, 50);
    driven.player[0].ent2_flags |= Q2_ENT2_DEAD;
    still.player[0].ent2_flags  |= Q2_ENT2_DEAD;

    for (i = 0; i < 20; i++) {
        q2_sim_tick(&driven, &full, 12);
        q2_sim_tick(&still,  &none, 12);
    }

    CHECK(driven.player[0].yaw == still.player[0].yaw,
          "the stick turned a corpse: %d against %d",
          (int)driven.player[0].yaw, (int)still.player[0].yaw);
    CHECK(driven.player[0].pos[0] == still.player[0].pos[0] &&
          driven.player[0].pos[2] == still.player[0].pos[2],
          "the stick walked a corpse: (%d,%d) against (%d,%d)",
          (int)driven.player[0].pos[0], (int)driven.player[0].pos[2],
          (int)still.player[0].pos[0],  (int)still.player[0].pos[2]);
    CHECK(driven.player[0].wish[0] == 0 && driven.player[0].wish[2] == 0,
          "a corpse still had a wish velocity");
    CHECK(driven.player[0].jump_hold == 0, "a corpse was holding a jump");
}

static void test_the_view_weapon_is_freed_with_the_player(void)
{
    q2_player_death d;

    /*
     * `entity+0x44` is a pointer to the view weapon ENTITY — 0x8004EE0C opens
     * `s6 = self->[68]; s7 = s6->[12]`. 0x800397F8 hands it to 0x8006D280,
     * which pushes it back onto the free stack at 0x800B2BAC, and then the
     * same word becomes -40. So "there is a gun" and "the gib threshold" are
     * the same field either side of the death.
     */
    q2_player_death_init(&d);
    CHECK(d.linked_weapon, "a live player holds one");
    CHECK(d.gib_health == 0, "and the field is not a threshold yet");

    q2_player_die(&d, 1, 18, 0, false, false, NULL);
    CHECK(!d.linked_weapon, "the view weapon was not freed");
    CHECK(d.gib_health == Q2_PDEATH_GIB_HEALTH,
          "the same word did not become -40, it is %d", (int)d.gib_health);

    /* And a respawn hands one back, because 0x8003B250 builds a new entity. */
    q2_player_death_init(&d);
    CHECK(d.linked_weapon, "a respawned player has no gun");
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    test_move_names();
    test_anim_choice();
    test_a_death_move_is_never_replaced();
    test_pain_holds_but_death_cuts_in();
    test_stand_does_not_cut_an_attack();
    test_the_gate();
    test_only_a_death_with_no_killer_cries_out();
    test_lava_erases_the_killer_on_the_entity();
    test_a_fresh_spawn_owes_nobody_a_frag();
    test_the_handler_runs_once();
    test_single_player_opens_the_page_and_arms_the_walk_back();
    test_the_handler_reuses_the_weapon_model_field();
    test_single_player_body_lies_where_it_fell();
    test_the_dead_bit_waits_for_the_animation_in_deathmatch();
    test_the_body_dissolves_after_its_five_seconds();
    test_a_body_can_still_be_gibbed();
    test_the_corpse_slows_down();
    test_the_walk_back_to_the_front_end();
    test_resupply_is_spent();
    test_a_corpse_is_not_driven_by_the_pad();
    test_the_view_weapon_is_freed_with_the_player();

    if (g_fail) {
        printf("\n%d player-death check%s failed\n", g_fail,
               g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("player death: all checks passed\n");
    return 0;
}
