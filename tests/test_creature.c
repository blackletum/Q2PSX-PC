/*
 * test_creature.c — the per-creature layer: binding a decoded module to code.
 *
 * The DECODER is checked against the disc by `q2psx-inspect creatures`, which
 * runs it over all fifteen module instances and reports what it found; there
 * is no point duplicating that here with a synthetic module image.
 *
 * What is checked here is the join: that a decoded creature's moves reach the
 * frame driver with their end callbacks resolved, that a move is found by its
 * first frame rather than its position, and that a creature with no hand
 * written implementation still animates instead of standing still.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "crebind.h"
#include "creworld.h"
#include "creature.h"
#include "spawn.h"

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

static void put_u16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* ------------------------------------------------------------------------- */
static int g_batch_stands;

static void batch_stand(q2_monster *m)
{
    g_batch_stands++;
    m->frame = 123;
}

static void test_summoned_batch_is_started(void)
{
    q2_creature_world w;
    q2_monster monster;
    u8 population[28];

    puts("a CREBATCH wake starts its formerly dormant monsters");

    memset(&w, 0, sizeof(w));
    memset(population, 0, sizeof(population));
    memcpy(population, "LiftRoom", 8);
    w.pop.data = population;
    w.pop.size = sizeof(population);
    w.pop.group_count = 1;
    w.pop_ready = true;

    q2_monster_init(&monster);
    monster.group = 0;
    monster.health = 20;
    monster.in_use = false;       /* held by q2_creature_world_hold_batches */
    monster.spawnflags |= Q2_SVFLAG_INUSE;
    monster.stand = batch_stand;
    w.set.monsters = &monster;
    w.set.count = 1;

    q2_level_reset();
    g_batch_stands = 0;
    check_eq_i(q2_creature_world_summon(&w, "LiftRoom"), 1,
               "the dormant record is selected");
    check(monster.in_use, "its port-side in-use latch is raised");
    check_eq_i(g_batch_stands, 1,
               "monster_start_go installs the creature's standing move");
    check(monster.think == q2_M_MoveFrame,
          "and installs the retail frame driver");
    check_eq_i(monster.next_think, 1,
               "with a live next-think deadline");
    check_eq_i(q2_creature_world_summon(&w, "LiftRoom"), 0,
               "a second selection does not restart an active creature");
}

/* ------------------------------------------------------------------------- */
/* A creature shaped like a decoded one, without needing a disc               */
/* ------------------------------------------------------------------------- */
#define TEST_CLASS 77
#define ADDR_STAND 0x80101000u
#define ADDR_RUN   0x80102000u
#define ADDR_FIRE  0x80103000u

static q2_creature g_cre;

static void build_creature(void)
{
    memset(&g_cre, 0, sizeof(g_cre));

    memcpy(g_cre.name, "Testbeast", 10);
    g_cre.base           = 0x80100000u;
    g_cre.class_byte[0]  = TEST_CLASS;
    g_cre.class_count    = 1;
    g_cre.speed_scale    = 14;
    g_cre.mass           = 250;

    g_cre.callback[0] = ADDR_STAND;     /* stand */
    g_cre.callback[4] = ADDR_RUN;       /* run   */

    g_cre.method[5]   = ADDR_RUN;       /* the run callback is also a think */
    g_cre.method[6]   = ADDR_FIRE;
    g_cre.method_count = 7;

    /* Two moves: a looping stand and a run whose end callback is the run
     * function, which is the shape every creature on the disc uses. */
    g_cre.move[0].addr         = 0x80104000u;
    g_cre.move[0].first_frame  = 146;
    g_cre.move[0].last_frame   = 149;
    g_cre.move[0].frame_index  = 0;
    g_cre.move[0].frame_count  = 4;
    g_cre.move[0].endfunc_addr = 0;
    g_cre.move[0].via          = 0;

    g_cre.move[1].addr         = 0x80104010u;
    g_cre.move[1].first_frame  = 99;
    g_cre.move[1].last_frame   = 101;
    g_cre.move[1].frame_index  = 4;
    g_cre.move[1].frame_count  = 3;
    g_cre.move[1].endfunc_addr = ADDR_RUN;
    g_cre.move[1].via          = 4;

    g_cre.move_count = 2;

    /* stand frames */
    g_cre.frames[0].ai = Q2_AI_STAND; g_cre.frames[0].dist = 0;
    g_cre.frames[0].think = 0;
    g_cre.frames[1] = g_cre.frames[0];
    g_cre.frames[2] = g_cre.frames[0];
    g_cre.frames[3] = g_cre.frames[0];
    /* run frames, the middle one firing */
    g_cre.frames[4].ai = Q2_AI_RUN; g_cre.frames[4].dist = 10;
    g_cre.frames[4].think = 0;
    g_cre.frames[5].ai = Q2_AI_RUN; g_cre.frames[5].dist = 10;
    g_cre.frames[5].think = 6;
    g_cre.frames[6] = g_cre.frames[4];

    g_cre.frame_count = 7;
}

/* ------------------------------------------------------------------------- */
static int g_stand_calls, g_run_calls, g_fire_calls;

static void t_stand(q2_monster *m) { g_stand_calls++; q2_cre_set_move(m, 146); }
static void t_run(q2_monster *m)   { g_run_calls++;   q2_cre_set_move(m, 99);  }
static void t_fire(q2_monster *m)  { (void)m; g_fire_calls++; }

static const q2_cre_impl g_impl = {
    "Testbeast",
    { t_stand, NULL, NULL, NULL, t_run, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL, t_run, t_fire, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    NULL
};

/* ------------------------------------------------------------------------- */
static void test_bind(void)
{
    q2_cre_bind bind;
    q2_monster m;

    printf("binding a decoded creature\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();

    check(q2_creature_bind(&bind, &g_cre, &g_impl), "a creature binds");
    check_eq_i(bind.move_count, 2, "with both its moves");

    /* The think handlers reach the engine's class table, which is what the
     * module's own init does through import +0x118. */
    check(q2_class_method_get(TEST_CLASS, 6) == t_fire,
          "think handlers are registered for the class byte");

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    check_eq_i(m.class_byte, TEST_CLASS, "spawning takes the class byte");
    check_eq_i(m.speed_scale, 14, "and the module's animation speed scale");
    check(m.stand == t_stand, "and the stand callback");
    check(m.run == t_run, "and the run callback");
    check((m.svflags & Q2_SVF_MONSTER) != 0, "and is flagged a monster");
    check(q2_ent_inuse(&m), "and is in use");
}

/* ------------------------------------------------------------------------- */
/* The shared population-to-entity flag hand-off, before module spawn runs. */
static void test_population_spawn_flags(void)
{
    enum { SPAWN_LIST = Q2_POP_GROUP_SIZE + 4,
           SIZE = SPAWN_LIST + Q2_POP_SPAWN_SIZE + 4 };
    u8 bytes[SIZE];
    q2_population pop;
    q2_monster_set set;
    q2_monster *m;

    printf("population spawn flags\n");

    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, "Zone0", 5);
    put_u32(bytes + 0x0C, SPAWN_LIST);
    /* bytes + 24 is the group's four-byte terminator. */

    put_u32(bytes + SPAWN_LIST, 34);  /* Insane class id: module reads flags */
    put_u16(bytes + SPAWN_LIST + 0x12, 0xFFFEu);
    put_u16(bytes + SPAWN_LIST + 0x14, 0x81D8u);
    /* One zero class-id word after this record terminates its spawn list. */

    memset(&pop, 0, sizeof(pop));
    pop.data        = bytes;
    pop.size        = sizeof(bytes);
    pop.group_count = 1;

    memset(&set, 0, sizeof(set));
    q2_monster_set_register(&set, 34);
    check_eq_i(q2_spawn_from_population(&set, &pop, NULL, NULL), Q2_OK,
               "one hand-built record spawns");
    check_eq_i(set.count, 1, "the record produced one monster");

    m = set.count ? &set.monsters[0] : NULL;
    check(m != NULL, "the spawned monster exists");
    if (m) {
        check_eq_i(m->target, -2,
                   "the link halfword reaches the entity target without a sentinel rewrite");
        check_eq_i((m->spawnflags >> Q2_POP_SPAWN_FLAGS_SHIFT) &
                       Q2_POP_SPAWN_FLAGS_MASK,
                   0x1D8,
                   "only the record's low nine flag bits reach entity bits 18..26");
        check((m->spawnflags & Q2_SVFLAG_INUSE) != 0,
              "the shared copy preserves the independent in-use bit");
    }

    q2_monster_set_free(&set);
}

static void test_move_lookup(void)
{
    q2_cre_bind bind;
    q2_monster m;

    printf("moves are found by first frame, not by position\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_creature_bind(&bind, &g_cre, &g_impl);
    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    check(q2_cre_find_move(&m, 146) != NULL, "the stand move is found at 146");
    check(q2_cre_find_move(&m, 99) != NULL, "the run move is found at 99");
    check(q2_cre_find_move(&m, 4242) == NULL,
          "a frame the disc does not have is not found");

    check(q2_cre_set_move(&m, 99), "installing by first frame works");
    check_eq_i(m.currentmove->first_frame, 99, "and installs that move");
    check_eq_i(m.currentmove->last_frame, 101, "with its own range");

    check(!q2_cre_set_move(&m, 4242),
          "and a missing animation fails visibly rather than substituting");
}

static void test_endfunc_resolution(void)
{
    q2_cre_bind bind;
    q2_monster m;

    printf("end callbacks resolve through the implementation\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_creature_bind(&bind, &g_cre, &g_impl);
    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    /* The run move's end callback is the module's run function, which the
     * implementation supplies — so it must come out as t_run. */
    q2_cre_set_move(&m, 99);
    check(m.currentmove->endfunc == (q2_endfunc)t_run,
          "an endfunc that is also a callback resolves to it");

    /* The stand move has none, and must loop rather than pick one up. */
    q2_cre_set_move(&m, 146);
    check(m.currentmove->endfunc == NULL, "a move with no endfunc loops");
}

static void test_frames_drive(void)
{
    q2_cre_bind bind;
    q2_monster m;
    int i;

    printf("the decoded frames actually drive the creature\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_ai_set_world(NULL);
    q2_level_reset();
    q2_creature_bind(&bind, &g_cre, &g_impl);

    g_stand_calls = g_run_calls = g_fire_calls = 0;

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    /*
     * A creature with no enemy does not run — ai_checkattack stands it down
     * on the first tick, which is correct and would make this a test of
     * nothing. So give it something to chase.
     */
    {
        static q2_monster target;
        q2_monster_init(&target);
        target.in_use      = true;
        target.spawnflags |= Q2_SVFLAG_INUSE;
        target.client      = true;
        target.health      = 100;
        target.pos[2]      = 4000;
        m.enemy      = &target;
        m.goalentity = &target;
        m.yaw_speed  = 4096;
        m.health     = 100;
    }

    q2_cre_set_move(&m, 99);
    m.frame = 99;

    /* Five ticks over a three-frame move: enough to reach the last frame and
     * then run the end callback on the tick after it. */
    for (i = 0; i < 5; i++) {
        q2_level_state.framenum++;
        q2_level_state.time++;
        q2_M_MoveFrame(&m);
    }

    check(g_fire_calls > 0,
          "a frame's think index reaches the creature's own handler");
    check(g_run_calls > 0,
          "and the move's end callback hands control back to run");

    /* The run frames carry a distance, so the creature must have advanced. */
    check(m.pos[0] != 0 || m.pos[2] != 0,
          "and the per-frame distances move it");
}

/* Records what the fire hook was handed, for the decoded-fire check below. */
static int g_test_fire_count;
static int g_test_fire_flash;

static void test_fire_hook(q2_monster *m, int flash, void *user)
{
    (void)m; (void)user;
    g_test_fire_count++;
    g_test_fire_flash = flash;
}

static void test_generic_fallback(void)
{
    q2_cre_bind bind;
    q2_monster m;
    const q2_cre_impl *impl;

    printf("a creature with no transcription still animates\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();

    impl = q2_cre_impl_find("Nosuchbeast");
    check(impl != NULL, "an unknown module gets the generic implementation");
    check(impl->name == NULL, "which is not pretending to be that creature");

    q2_creature_bind(&bind, &g_cre, impl);
    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    check(m.stand != NULL, "it still has a stand callback");
    m.stand(&m);
    check(m.currentmove != NULL, "which installs a real decoded move");
    check_eq_i(m.currentmove->first_frame, 146,
               "the one the module's own stand callback installs");

    m.run(&m);
    check_eq_i(m.currentmove->first_frame, 99, "and likewise for run");

    /* But it performs no per-frame action, which is the whole of what is
     * missing and must not be papered over. */
    /* Its think handlers are the decoded-action trampolines, not NULL: the
     * creature acts from what was read off the disc. */
    check(q2_class_method_get(TEST_CLASS, 6) != NULL,
          "and a think handler that runs the decoded action");

    /*
     * And a decoded FIRE reaches the fire hook. The five import slots the
     * projectile spawners live in were named out of the loader at 0x8007DA00
     * and confirmed by what each calls — the rocket at +0x98 goes through
     * 0x8004AF28, which combat.h already records as being called from that very
     * address. Before this, a CALL step of any kind did nothing, which is why
     * six of the seven modules hunted the player and never shot.
     */
    {
        static q2_cre_think think[8];

        memset(think, 0, sizeof(think));
        think[6].step_count      = 1;
        think[6].step[0].op      = Q2_CRE_OP_CALL;
        think[6].step[0].import_ofs = 0x98;          /* the rocket */

        q2_creature_bind_thinks(&bind, think, 8);

        q2_cre_set_fire_hook(test_fire_hook, NULL);
        g_test_fire_count = 0;
        g_test_fire_flash = -1;

        /* No enemy: a decoded creature must not shoot at nothing. */
        m.enemy = NULL;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_test_fire_count, 0, "no enemy, no shot");

        /* An enemy that is alive: the shot goes. */
        {
            static q2_monster foe;

            q2_monster_init(&foe);
            foe.health = 100;
            m.enemy = &foe;
            q2_cre_run_think(&m, 6);
            check_eq_i(g_test_fire_count, 1, "a decoded fire reaches the hook");
            check_eq_i(g_test_fire_flash, 0x98,
                       "carrying the import slot it came from");

            /* A dead enemy stops it, the same guard every refire has. */
            foe.health = 0;
            q2_cre_run_think(&m, 6);
            check_eq_i(g_test_fire_count, 1, "and a dead enemy stops it");
        }

        /* The vector-maths imports are NOT shots: 40 of the disc's 107 call
         * steps are these, and treating them as fire would have every creature
         * shoot three times an animation frame. */
        m.enemy = NULL;
        {
            static q2_monster foe2;

            q2_monster_init(&foe2);
            foe2.health = 100;
            m.enemy = &foe2;
            think[6].step[0].import_ofs = 0xC0;
            q2_cre_run_think(&m, 6);
            check_eq_i(g_test_fire_count, 1,
                       "muzzle arithmetic is not a shot");
        }

        q2_cre_set_fire_hook(NULL, NULL);
    }
}

static void test_soldier_present(void)
{
    const q2_cre_impl *impl;

    printf("the transcribed creatures are registered\n");

    impl = q2_cre_impl_find("Soldier");
    check(impl != NULL && impl->name != NULL, "the Soldier is transcribed");

    if (impl && impl->name) {
        /* The fourteen think indices its animations actually use. */
        static const int used[] = { 2, 3, 4, 6, 7, 8, 9, 10, 11,
                                    16, 17, 18, 20, 21 };
        u32 i;
        int have = 0;
        for (i = 0; i < sizeof(used) / sizeof(used[0]); i++)
            if (impl->method[used[i]])
                have++;
        check_eq_i(have, 14, "with all fourteen of its think indices covered");

        check(impl->callback[7] == NULL,
              "and no melee, which is what its module declares");
        check(impl->callback[0] != NULL, "but a stand callback");
        check(impl->callback[11] != NULL, "a pain callback");
        check(impl->callback[12] != NULL, "and a die callback");
    }
}

/* ------------------------------------------------------------------------- */
/* The decoded-action executor                                                */
/* ------------------------------------------------------------------------- */
static int g_snd_calls; static int g_snd_last;
static int g_mel_calls; static s32 g_mel_dmg, g_mel_kick, g_mel_aim0;

static void spy_sound(q2_monster *m, int which, void *u)
{
    (void)m; (void)u; g_snd_calls++; g_snd_last = which;
}

static void spy_melee(q2_monster *m, const s32 aim[3], s32 dmg, s32 kick,
                      void *u)
{
    (void)m; (void)u;
    g_mel_calls++; g_mel_dmg = dmg; g_mel_kick = kick; g_mel_aim0 = aim[0];
}

static void test_actions(void)
{
    q2_cre_bind bind;
    q2_monster m;
    static q2_cre_think th[Q2_CLASS_METHOD_COUNT];

    printf("running a think function that was decoded rather than written\n");

    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_creature_bind(&bind, &g_cre, q2_cre_impl_find("Nosuchbeast"));

    memset(th, 0, sizeof(th));

    /* Index 6, shaped like the Berserk's club frame: a grunt and a swing. */
    th[6].step_count          = 2;
    th[6].step[0].op          = Q2_CRE_OP_SOUND;
    th[6].step[0].addr        = 0x80101758u;
    th[6].step[1].op          = Q2_CRE_OP_MELEE;
    th[6].step[1].aim[0]      = 1020;
    th[6].step[1].damage_base = 5;
    th[6].step[1].damage_rand = 3;
    th[6].step[1].kick        = 400;

    /* Index 7, a refire — gated, as every refire on the disc is. */
    th[7].step_count    = 1;
    th[7].step[0].op    = Q2_CRE_OP_NEXTFRAME;
    th[7].step[0].frame = 42;
    th[7].step[0].gated = true;

    /* Index 8, the duck helpers' aiflags store. */
    th[8].step_count       = 1;
    th[8].step[0].op       = Q2_CRE_OP_AIFLAG;
    th[8].step[0].flag_set = Q2_AI_HOLD_FRAME;

    q2_creature_bind_thinks(&bind, th, Q2_CLASS_METHOD_COUNT);

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);

    q2_cre_set_sound_hook(spy_sound, NULL);
    q2_cre_set_melee_hook(spy_melee, NULL);
    g_snd_calls = g_mel_calls = 0;
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));

    /*
     * SOMETHING TO SWING AT, in reach. `fire_hit` (0x80061118) measures
     * `enemy->origin - self->origin` before it does anything else, so a melee
     * step with no enemy or one past `aim[0]` is refused — which is what the
     * two arms below pin.
     */
    {
        static q2_monster victim;

        q2_monster_init(&victim);
        victim.health = 100;
        victim.pos[0] = m.pos[0];
        victim.pos[1] = m.pos[1];
        victim.pos[2] = m.pos[2] + 900;    /* inside the 1020 reach */
        m.enemy = &victim;

        q2_cre_run_think(&m, 6);
        check_eq_i(g_snd_calls, 1, "a decoded sound step plays a sound");
        check_eq_i(g_snd_last, (int)0x80101758,
                   "identified by the module address of its handle");
        check_eq_i(g_mel_calls, 1, "and a decoded melee step swings");
        check_eq_i(g_mel_aim0, 1020, "with the module's own aim distance");
        check_eq_i(g_mel_kick, 400, "and its own kick");
        check(g_mel_dmg >= 5 && g_mel_dmg < 8,
              "and damage in the module's own base-plus-spread range");

        /*
         * 0x80061198 `slt v1, aim[0], range` and the jump to the epilogue at
         * 0x800614B4: past the reach, the swing does nothing. This is the
         * player who backs off during the wind-up, which used to be clubbed
         * from any distance at all.
         */
        victim.pos[2] = m.pos[2] + 5000;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_mel_calls, 1, "a swing past the aim's reach lands nothing");
        check_eq_i((int)q2_cre_actions.melee_short, 1,
                   "and is counted as short rather than lost");

        /*
         * STRICTLY greater: `range == aim[0]` still hits, because the console
         * compares `aim[0] < range` and not `<=`.
         */
        victim.pos[2] = m.pos[2] + 1020;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_mel_calls, 2, "a swing at exactly the reach still lands");

        /* And the four counters partition the calls. */
        check_eq_i((int)q2_cre_actions.melee_calls,
                   (int)(q2_cre_actions.melee_sent +
                         q2_cre_actions.melee_no_hook +
                         q2_cre_actions.melee_no_enemy +
                         q2_cre_actions.melee_short),
                   "and every swing is counted exactly once");

        m.enemy = NULL;
        q2_cre_run_think(&m, 6);
        check_eq_i(g_mel_calls, 2, "a swing with no enemy swings at nothing");
    }

    /*
     * A gated refire must not fire at a corpse. Every refire on the disc opens
     * with that guard, so it is read rather than invented — and without it a
     * creature jumps back into its firing frame forever.
     */
    m.nextframe = 0;
    m.enemy     = NULL;
    q2_cre_run_think(&m, 7);
    check_eq_i(m.nextframe, 0, "a gated refire does nothing with no enemy");

    {
        static q2_monster live;
        q2_monster_init(&live);
        live.health = 50;
        m.enemy = &live;
        q2_cre_run_think(&m, 7);
        check_eq_i(m.nextframe, 42, "and does fire at a live one");

        live.health = 0;
        m.nextframe = 0;
        q2_cre_run_think(&m, 7);
        check_eq_i(m.nextframe, 0, "but not once it is dead");
    }

    m.aiflags = 0;
    q2_cre_run_think(&m, 8);
    check((m.aiflags & Q2_AI_HOLD_FRAME) != 0,
          "a decoded aiflags step reaches the flags word");

    g_snd_calls = 0;
    q2_cre_run_think(&m, 20);
    check_eq_i(g_snd_calls, 0, "an index that decoded to nothing does nothing");

    q2_cre_set_sound_hook(NULL, NULL);
    q2_cre_set_melee_hook(NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* Module sound fallbacks — Soldier module+0xE50, Tankcomm module+0x808       */
/* ------------------------------------------------------------------------- */
#define TEST_MOD_BASE 0x80100000u

static const q2_cre_sound_bind k_soldier_binds[] = {
    { 0x801032A4u, "sol_sght1" },
    { 0x801032ACu, "sol_pain1" },
    { 0x801032B0u, "sol_pain2" },
    { 0x801032B4u, "sol_pain3" },
    { 0x801032B8u, "sol_deth1" },
    { 0x801032BCu, "sol_deth2" },
    { 0x801032C0u, "sol_deth3" }
};

static const q2_cre_sound_bind k_tank_binds[] = {
    { 0x80102100u, "tnk_idle1" },
    { 0x80102110u, "tnk_step" },
    { 0x80102104u, "tnk_pain" }
};

/* The bank, as a NULL-terminated list of the names it carries. */
static bool stub_bank_has(const char *name, void *user)
{
    const char *const *bank = (const char *const *)user;
    u32 i;

    for (i = 0; bank && bank[i]; i++)
        if (strcmp(bank[i], name) == 0)
            return true;

    return false;
}

static void check_name(const char *got, const char *want, const char *what)
{
    g_checks++;
    if ((got == NULL) != (want == NULL) ||
        (got && want && strcmp(got, want) != 0)) {
        printf("  FAIL  %s: got %s, want %s\n", what,
               got ? got : "(null)", want ? want : "(null)");
        g_failures++;
    }
}

static void test_sound_fallback(void)
{
    static const char *bank_pain1[] = { "sol_pain1", "sol_deth2", NULL };
    static const char *bank_both[]  = { "sol_pain1", "sol_pain2", NULL };
    static const char *bank_own[]   = { "sol_pain2", "sol_pain3", NULL };
    static const char *bank_empty[] = { NULL };
    static const char *bank_step[]  = { "tnk_step", "tnk_pain", NULL };
    static const char *bank_idle[]  = { "tnk_idle1", "tnk_step", NULL };
    static const char *bank_d12[]   = { "sol_deth1", "sol_deth2", NULL };
    static const char *bank_d13[]   = { "sol_deth1", "sol_deth3", NULL };
    static const char *bank_d23[]   = { "sol_deth2", "sol_deth3", NULL };
    static const char *bank_p13[]   = { "sol_pain1", "sol_pain3", NULL };
    const u32 nsol = (u32)(sizeof(k_soldier_binds) /
                           sizeof(k_soldier_binds[0]));
    const u32 ntnk = (u32)(sizeof(k_tank_binds) / sizeof(k_tank_binds[0]));
    u32 nfb = 0;

    printf("module sound fallbacks\n");

    check(q2_creature_sound_fallbacks(&nfb) != NULL && nfb == 7,
          "seven fallback slots: the Soldier's two trios and the Tank's one");

    /*
     * soldier_pain plays +0x32B4 and sol_pain3 is in NO bank on the disc, so
     * this address is the one every Soldier's flinch goes through. Before this
     * it resolved to "sol_pain3", the bank missed, and every Soldier on every
     * map that places one was silent when hurt.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_pain1),
               "sol_pain1",
               "the pain slot falls back to sol_pain1 on a COMMAND-like bank");

    /* The die handler plays +0x32C0, sol_deth3 — absent only on COMMAND. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032C0u,
                                         stub_bank_has, (void *)bank_pain1),
               "sol_deth2",
               "and the death slot falls back to sol_deth2");

    /* pain2 is preferred: 0x80100E50 loads +0x32B0 first. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_both),
               "sol_pain2",
               "with both present the module's own preference wins");

    /*
     * ...but a slot whose OWN name resolved is never overwritten: 0x80100EAC
     * `bne v0, zero` guards the store into +0x32B4.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_own),
               "sol_pain3",
               "a slot the bank does carry keeps its own name");

    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         stub_bank_has, (void *)bank_empty),
               NULL,
               "a bank with none of the trio is silence, not a wrong name");

    /* Not in any group: the registration stands, bank or no bank. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032A4u,
                                         stub_bank_has, (void *)bank_empty),
               "sol_sght1",
               "an ungrouped address still resolves to its registration");

    /*
     * The Tank Commander: tnk_idle1 is in ZERO banks and tnk_step in the
     * thirteen that carry a Tank, so this substitution fires on every map.
     */
    check_name(q2_creature_sound_resolve("Tankcomm", k_tank_binds, ntnk,
                                         TEST_MOD_BASE, 0x80102100u,
                                         stub_bank_has, (void *)bank_step),
               "tnk_step",
               "the Tank's idle handle is overwritten with its footstep");
    check_name(q2_creature_sound_resolve("Tankcomm", k_tank_binds, ntnk,
                                         TEST_MOD_BASE, 0x80102100u,
                                         stub_bank_has, (void *)bank_idle),
               "tnk_idle1",
               "and would keep tnk_idle1 if any bank carried it");

    /*
     * The four names that really ARE silent by design must stay silent: they
     * have no group, so an absent one resolves to its registration and the
     * caller's bank lookup misses, exactly as before.
     */
    check_name(q2_creature_sound_resolve("Tankcomm", k_tank_binds, ntnk,
                                         TEST_MOD_BASE, 0x80102104u,
                                         stub_bank_has, (void *)bank_empty),
               "tnk_pain",
               "an ungrouped Tank sound is not substituted");

    /* A NULL bank means "carries everything", so nothing is substituted. */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B4u,
                                         NULL, NULL),
               "sol_pain3",
               "with no bank predicate the registration stands");

    /*
     * BY NAME, which is how the host actually meets the Soldier. soldier_pain
     * sends index 4 through the sound hook, not an address, and the host turns
     * that into "sol_pain3" through the transcribed table — so the address
     * form above can never see it. Every Soldier on every map that places one
     * was silent when hurt because that name went straight to the bank.
     */
    check_name(q2_creature_sound_fallback("Soldier",
                                          q2_cre_soldier_sound_name(4),
                                          stub_bank_has, (void *)bank_pain1),
               "sol_pain1",
               "and by name it falls back to sol_pain1 on a COMMAND-like bank");
    check_name(q2_creature_sound_fallback("Soldier",
                                          q2_cre_soldier_sound_name(4),
                                          stub_bank_has, (void *)bank_both),
               "sol_pain2", "or to the preferred sol_pain2 where it exists");
    check_name(q2_creature_sound_fallback("Soldier",
                                          q2_cre_soldier_sound_name(7),
                                          stub_bank_has, (void *)bank_pain1),
               "sol_deth2", "the die index falls back to sol_deth2");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain3",
                                          stub_bank_has, (void *)bank_empty),
               NULL, "and a bank with none of the trio is silence");
    check_name(q2_creature_sound_fallback("Tankcomm", "tnk_idle1",
                                          stub_bank_has, (void *)bank_step),
               "tnk_step", "the Tank's idle name becomes its footstep");
    check_name(q2_creature_sound_fallback("Tankcomm", "tnk_pain",
                                          stub_bank_has, (void *)bank_empty),
               "tnk_pain", "an ungrouped name passes through untouched");
    check_name(q2_creature_sound_fallback("Arachner", "sol_pain3",
                                          stub_bank_has, (void *)bank_pain1),
               "sol_pain3",
               "and a group belongs to its own module, not to the name");

    /*
     * THE SLOT KEEPS ITS OWN NAME BY NAME TOO. The name form is the one the
     * host is told to use, and every store in the fill pass is guarded on its
     * DESTINATION — 0x80100F1C `bne v0, zero` skips the store into +0x32C0,
     * 0x80100EAC the one into +0x32B4 — so a slot the bank carries is never
     * overwritten, even by a higher preference the bank also carries. Each
     * bank below carries the slot's own name AND a name the module prefers.
     */
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth3",
                                          stub_bank_has, (void *)bank_d23),
               "sol_deth3",
               "by name, sol_deth3 stays sol_deth3 beside the preferred "
               "sol_deth2 (0x80100F1C)");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain3",
                                          stub_bank_has, (void *)bank_own),
               "sol_pain3",
               "and sol_pain3 stays sol_pain3 beside the preferred sol_pain2 "
               "(0x80100EAC)");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain1",
                                          stub_bank_has, (void *)bank_both),
               "sol_pain1",
               "and sol_pain1 stays sol_pain1 beside the preferred sol_pain2 "
               "(0x80100E7C)");

    /*
     * THE DEATH TRIO'S ORDER, deth2 then deth1 then deth3: 0x80100EB8 loads
     * [+0x32BC], 0x80100ECC [+0x32B8] if that was zero, 0x80100EDC [+0x32C0]
     * if that was too. Each pair is pinned with a bank that carries exactly
     * those two and not the slot asked about, so every other order gives a
     * different answer to at least one line — by address and by name.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032C0u,
                                         stub_bank_has, (void *)bank_d12),
               "sol_deth2",
               "deth2 before deth1: the +0x32C0 slot falls to sol_deth2");
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth3",
                                          stub_bank_has, (void *)bank_d12),
               "sol_deth2", "and by name");
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032BCu,
                                         stub_bank_has, (void *)bank_d13),
               "sol_deth1",
               "deth1 before deth3: the +0x32BC slot falls to sol_deth1");
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth2",
                                          stub_bank_has, (void *)bank_d13),
               "sol_deth1", "and by name");
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B8u,
                                         stub_bank_has, (void *)bank_d23),
               "sol_deth2",
               "deth2 before deth3: the +0x32B8 slot falls to sol_deth2, not "
               "sol_deth3");
    check_name(q2_creature_sound_fallback("Soldier", "sol_deth1",
                                          stub_bank_has, (void *)bank_d23),
               "sol_deth2", "and by name");

    /*
     * AND THE PAIN TRIO'S, pain2 then pain1 then pain3 (0x80100E50, 0x80100E60,
     * 0x80100E70). pain2-before-pain1 is the bank_both line above; these are
     * the two pairs that involve pain3, which no line pinned.
     */
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032B0u,
                                         stub_bank_has, (void *)bank_p13),
               "sol_pain1",
               "pain1 before pain3: the +0x32B0 slot falls to sol_pain1");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain2",
                                          stub_bank_has, (void *)bank_p13),
               "sol_pain1", "and by name");
    check_name(q2_creature_sound_resolve("Soldier", k_soldier_binds, nsol,
                                         TEST_MOD_BASE, 0x801032ACu,
                                         stub_bank_has, (void *)bank_own),
               "sol_pain2",
               "pain2 before pain3: the +0x32AC slot falls to sol_pain2");
    check_name(q2_creature_sound_fallback("Soldier", "sol_pain1",
                                          stub_bank_has, (void *)bank_own),
               "sol_pain2", "and by name");

    /*
     * AND THROUGH THE BIND, which is the call the host makes: it has the
     * creature and the registered name, and the module comes from the class
     * byte. A monster of another module is not substituted.
     */
    {
        static q2_creature sol;
        q2_cre_bind sb, tb;
        q2_monster s, t;

        memset(&sol, 0, sizeof(sol));
        memcpy(sol.name, "Soldier", 8);
        sol.base          = TEST_MOD_BASE;
        sol.class_byte[0] = 87;
        sol.class_count   = 1;

        build_creature();
        q2_cre_bind_reset();
        q2_class_table_reset();
        if (!q2_creature_bind(&sb, &sol, &g_impl) ||
            !q2_creature_bind(&tb, &g_cre, &g_impl)) {
            check(false, "the two test binds could not be made");
            return;
        }

        q2_monster_init(&s);
        s.class_byte = 87;
        q2_monster_init(&t);
        t.class_byte = TEST_CLASS;

        check_name(q2_cre_sound_resolve(&s, "sol_pain3", stub_bank_has,
                                        (void *)bank_pain1),
                   "sol_pain1", "a Soldier's pain resolves to what plays");
        check_name(q2_cre_sound_resolve(&t, "sol_pain3", stub_bank_has,
                                        (void *)bank_pain1),
                   "sol_pain3", "a Testbeast's is left alone");
        check_name(q2_cre_sound_resolve(NULL, "sol_pain3", stub_bank_has,
                                        (void *)bank_pain1),
                   "sol_pain3", "and no creature at all is no substitution");

        q2_cre_bind_reset();
        q2_class_table_reset();
    }
}

/* ------------------------------------------------------------------------- */
/* The enemy-alive guards belong to the refire callbacks, not to the shot     */
/* ------------------------------------------------------------------------- */
static int g_shot_calls;

static void spy_shot(q2_monster *m, const q2_cre_shot *shot, void *user)
{
    (void)m; (void)shot; (void)user;
    g_shot_calls++;
}

static void test_fire_shot_has_no_enemy_guard(void)
{
    static const q2_cre_shot shot = { 0x98, 0x98, 50, 550, 0, 0, 0, 1 };
    q2_monster m, foe;
    u32 i;

    printf("a transcribed fire think shoots with no enemy\n");

    q2_monster_init(&m);
    q2_cre_set_shot_hook(spy_shot, NULL);

    /*
     * The Tank's machine-gun think loads the enemy at 0x80101074 and, on
     * `beq a3, zero`, writes a literal zero pitch at 0x801010F4 and falls
     * through to the fire at 0x80101160. It fires at nothing, aimed flat.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    m.enemy = NULL;
    q2_cre_fire_shot(&m, &shot);
    check_eq_i(g_shot_calls, 1, "a shot with no enemy still reaches the hook");
    check_eq_i(q2_cre_actions.fire_sent, 1, "and counts as sent");
    check(q2_cre_actions.shot_no_enemy == 1 && g_shot_calls == 1,
          "with shot_no_enemy an observation beside the shot, not a refusal");

    /*
     * AND NOT AS A REFUSAL AS WELL. fire_no_enemy is the decoded path's refusal
     * count (cre_actions.c), and the census prints it beside fire_sent as a
     * share of fire_calls. A sent shot counted in it too would be counted twice.
     */
    check(q2_cre_actions.fire_no_enemy == 0 &&
              q2_cre_actions.fire_calls ==
                  q2_cre_actions.fire_sent + q2_cre_actions.fire_no_hook +
                  q2_cre_actions.fire_no_enemy +
                  q2_cre_actions.fire_dead_enemy,
          "fire_no_enemy stays a refusal count, so sent + no_hook + no_enemy + "
          "dead_enemy is still fire_calls");

    /* And nowhere in the think is obj+0x108, the health, loaded at all. */
    q2_monster_init(&foe);
    foe.health = -10;
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    m.enemy = &foe;
    q2_cre_fire_shot(&m, &shot);
    check_eq_i(g_shot_calls, 1, "and a shot at a corpse reaches it too");
    check(q2_cre_actions.shot_dead_enemy == 1 && q2_cre_actions.fire_sent == 1,
          "observed in shot_dead_enemy and still sent");
    check(q2_cre_actions.fire_dead_enemy == 0 &&
              q2_cre_actions.fire_calls ==
                  q2_cre_actions.fire_sent + q2_cre_actions.fire_no_hook +
                  q2_cre_actions.fire_no_enemy +
                  q2_cre_actions.fire_dead_enemy,
          "and not in fire_dead_enemy, the refusal count, as well");
    q2_cre_set_shot_hook(NULL, NULL);

    /*
     * The guard used to be the only thing between a NULL enemy and a
     * dereference, so every transcribed think is run with one to prove none of
     * them needs it. A crash here is the finding, not a flake.
     */
    {
        const q2_cre_impl *const *impls = q2_cre_impls();
        u32 n_run = 0;

        q2_cre_set_shot_hook(spy_shot, NULL);
        g_shot_calls = 0;
        for (i = 0; impls[i]; i++) {
            u32 j;

            for (j = 0; j < Q2_CLASS_METHOD_COUNT; j++) {
                q2_monster t;

                if (!impls[i]->method[j])
                    continue;

                q2_monster_init(&t);
                t.health     = 100;
                t.max_health = 100;
                t.enemy      = NULL;
                impls[i]->method[j](&t);
                n_run++;
            }
        }
        q2_cre_set_shot_hook(NULL, NULL);

        check(n_run > 0 && g_shot_calls > 0,
              "every transcribed think ran with a NULL enemy without "
              "crashing, and at least one of them fired anyway — reaching "
              "the hook with no enemy is what the guard prevented");
    }
}

/* ------------------------------------------------------------------------- */
/* The Tank Commander's two halves of the rule, on its own code               */
/* ------------------------------------------------------------------------- */
/*
 * A Tankcomm-shaped creature with just the three blaster moves, so the refire
 * callback has somewhere to go: 55..70 attack_blast (module+0x1D18, think 8 on
 * 64, 67 and 70, ending in tank_reattack_blaster), 65..70 reattack_blast
 * (module+0x1D3C) and 71..76 post_blast (module+0x1D60). Addresses and ranges
 * as `q2psx-inspect creatures` prints them for COMMAND.
 */
static q2_creature g_tank;

static void build_tank(void)
{
    static const struct { u32 addr; s32 first, last; } k[3] = {
        { 0x80101D18u, 55, 70 },
        { 0x80101D3Cu, 65, 70 },
        { 0x80101D60u, 71, 76 }
    };
    u32 i, f = 0;

    memset(&g_tank, 0, sizeof(g_tank));
    memcpy(g_tank.name, "Tankcomm", 9);
    g_tank.base          = TEST_MOD_BASE;
    g_tank.class_byte[0] = 91;
    g_tank.class_count   = 1;

    for (i = 0; i < 3; i++) {
        u32 n = (u32)(k[i].last - k[i].first + 1);

        g_tank.move[i].addr        = k[i].addr;
        g_tank.move[i].first_frame = k[i].first;
        g_tank.move[i].last_frame  = k[i].last;
        g_tank.move[i].frame_index = f;
        g_tank.move[i].frame_count = n;
        f += n;
    }
    g_tank.move_count  = 3;
    g_tank.frame_count = f;
}

static void test_tank_burst_rule(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Tankcomm");
    q2_cre_bind bind;
    q2_monster m, foe;
    int i, extended;

    printf("the Tank finishes a burst and decides at the refire\n");

    if (!impl || !impl->method[8] || !impl->method[3] || !impl->method[13]) {
        check(false, "the Tank's blaster, reattack and machine-gun thinks");
        return;
    }

    build_tank();
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_ai_set_world(NULL);
    if (!q2_creature_bind(&bind, &g_tank, impl)) {
        check(false, "a Tankcomm-shaped creature could not be bound");
        return;
    }

    q2_monster_init(&m);
    q2_creature_spawn(&bind, &m, 0);
    m.health = 750;

    q2_monster_init(&foe);
    foe.in_use = true;
    foe.health = -10;                       /* died on the volley's first shot */
    m.enemy = &foe;

    q2_cre_set_skill(3);
    q2_cre_set_shot_hook(spy_shot, NULL);

    /*
     * FIRST HALF: the volley already under way finishes. TankBlaster
     * (module+0xC4C) builds its aim off entity+0xBC and never loads obj+0x108,
     * so frames 64, 67 and 70 all go out at a target that is already dead.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    q2_cre_set_move(&m, 55);
    for (i = 0; i < 3; i++)
        impl->method[8](&m);
    check_eq_i(g_shot_calls, 3,
               "all three blaster frames fire at a dead target");

    /*
     * BOTH HALVES AT ONCE, over thirty-two volleys at a corpse: each volley
     * sends its three shots, then tank_reattack_blaster decides — and its
     * `lh v0, 264(v0)` / `blez` at module+0x144C sends a dead enemy to
     * post_blast every time, even on nightmare with the target in view. A
     * refire that DID extend would play reattack_blast (65..70), whose think 8
     * on 67 and 70 fires twice more. So the console's rule is exactly three
     * shots a volley: never none (the old shared guard) and never five.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    for (i = 0; i < 32; i++) {
        int k;

        q2_cre_set_move(&m, 55);
        for (k = 0; k < 3; k++)
            impl->method[8](&m);            /* frames 64, 67, 70 */
        impl->method[3](&m);                /* the endfunc of 55..70 */
        if (m.currentmove && m.currentmove->first_frame == 65) {
            impl->method[8](&m);            /* frames 67 and 70 of 65..70 */
            impl->method[8](&m);
        }
    }
    check_eq_i(g_shot_calls, 32 * 3,
               "every volley at a corpse finishes and none is extended");
    check(q2_cre_actions.shot_dead_enemy == 32 * 3 &&
              q2_cre_actions.fire_dead_enemy == 0 &&
              q2_cre_actions.fire_calls == q2_cre_actions.fire_sent,
          "all 96 are observed in shot_dead_enemy and none is counted as "
          "refused, so the census's sent share is the whole of fire_calls");

    /*
     * The CONTROL that keeps "none is extended" from being vacuous: the same
     * loop against a live target does extend, on id's `random() <= 0.6`, so
     * the refire really is reachable from here with skill, sight and the move
     * table all in place. Thirty-two tries at 0.6 all missing has odds of
     * about 2e-13.
     */
    foe.health = 100;
    extended = 0;
    for (i = 0; i < 32; i++) {
        q2_cre_set_move(&m, 55);
        impl->method[3](&m);
        if (m.currentmove && m.currentmove->first_frame == 65)
            extended++;
    }
    check(extended > 0, "control: a live target can be re-attacked");

    /*
     * AND THE MACHINE GUN WITH NO TARGET AT ALL. TankMachineGun's `beq a3,
     * zero, 0x801010F4` (module+0x107C) writes a zero pitch and falls through
     * to the fire at module+0x1160.
     */
    memset(&q2_cre_actions, 0, sizeof(q2_cre_actions));
    g_shot_calls = 0;
    m.enemy = NULL;
    impl->method[13](&m);
    check(g_shot_calls == 1 && q2_cre_actions.shot_no_enemy == 1,
          "TankMachineGun fires with no enemy, and is counted doing it");

    q2_cre_set_shot_hook(NULL, NULL);
    q2_cre_set_skill(1);
    q2_cre_bind_reset();
    q2_class_table_reset();
}

/* ------------------------------------------------------------------------- */
/* A prone Insane is a flyer — module+0x974, import +0x100                    */
/* ------------------------------------------------------------------------- */
static void test_insane_prone_flies(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Insane");
    q2_monster m;

    printf("a prone Insane is a flyer for life\n");

    if (!impl || !impl->spawn) {
        check(false, "the Insane has a spawn hook");
        return;
    }

    q2_cre_bind_reset();
    q2_level_reset();

    /*
     * Map flag 8 — bit 3 of the record's flags, spawnflags bit 21 after the
     * filler's `<< 18` — is the prone variant. Its arm ORs FL_NO_KNOCKBACK
     * and then calls import +0x100, flymonster_start, which raises FL_FLY
     * itself (0x800622B4). Before, the port set the knockback bit alone and
     * the crawler walked the stepping path with a walker's eye.
     */
    q2_monster_init(&m);
    m.spawnflags = 0x08u << 18;
    impl->spawn(&m);
    check((m.flags & (Q2_FL_NO_KNOCKBACK | Q2_FL_FLY)) ==
              (Q2_FL_NO_KNOCKBACK | Q2_FL_FLY),
          "a prone Insane takes no knockback AND is FL_FLY from spawn");
    check_eq_i(m.start_kind, Q2_CRE_START_FLY, "with the fly go-routine parked");

    q2_monster_start_go(&m);
    check_eq_i(m.view_height, -250, "so its eye is the flyer's flat -250");
    check_eq_i(m.yaw_speed, 114, "and it turns at the flyer's 114");
    check((m.flags & Q2_FL_FLY) != 0, "and is still flying once awake");

    /* The upright one calls +0xFC, walkmonster_start, and gets its model's
     * eye: ext2 304 on LAB is ~304 = -305. */
    q2_monster_init(&m);
    m.spawnflags = 0;
    impl->spawn(&m);
    m.model_ext2 = 304;
    q2_monster_start_go(&m);
    check(m.view_height == -305 && (m.flags & Q2_FL_FLY) == 0,
          "an upright Insane walks, with its eye from its model");
    check_eq_i(m.yaw_speed, 228, "and turns at the walker's 228");
}

/* ------------------------------------------------------------------------- */
/* The Insane's draws, and its place in the monster count                     */
/* ------------------------------------------------------------------------- */
/*
 * An Insane-shaped creature carrying just "Stand N", module+0x1388 {59, 64} —
 * the move its spawn installs at 0x80100960 (`sw v0, 216(s0)`) before the
 * prone/upright fork — and the Insane's own class byte, 94, so that its spawn
 * hook finds the move through the bind.
 */
static q2_creature g_insane;

static void build_insane(void)
{
    u32 i;

    memset(&g_insane, 0, sizeof(g_insane));
    memcpy(g_insane.name, "Insane", 7);
    g_insane.base          = TEST_MOD_BASE;
    g_insane.class_byte[0] = 94;
    g_insane.class_count   = 1;

    g_insane.move[0].addr        = 0x80101388u;
    g_insane.move[0].first_frame = 59;
    g_insane.move[0].last_frame  = 64;
    g_insane.move[0].frame_index = 0;
    g_insane.move[0].frame_count = 6;
    g_insane.move_count          = 1;

    for (i = 0; i < 6; i++)
        g_insane.frames[i].ai = Q2_AI_STAND;
    g_insane.frame_count = 6;
}

static void test_insane_start_draws(void)
{
    const q2_cre_impl *impl = q2_cre_impl_find("Insane");
    q2_cre_bind bind, beast;
    q2_monster m, t;
    int r1, r2, r3, next;

    printf("the Insane's start call draws, and it is not counted\n");

    if (!impl || !impl->spawn) {
        check(false, "the Insane has a spawn hook");
        return;
    }

    build_insane();
    build_creature();
    q2_cre_bind_reset();
    q2_class_table_reset();
    if (!q2_creature_bind(&bind, &g_insane, impl) ||
        !q2_creature_bind(&beast, &g_cre, &g_impl)) {
        check(false, "the Insane and Testbeast binds could not be made");
        return;
    }
    q2_level_reset();

    /* The stream the spawn will read, taken out in advance. */
    srand(4321);
    r1 = rand();
    r2 = rand();
    r3 = rand();

    /*
     * PRONE: flymonster_start through import +0x100 (0x80100994), whose
     * monster_start draws at 0x80061B2C for the start frame — and then
     * 0x8010099C jumps past the skin. ONE draw, and the frame it makes.
     */
    srand(4321);
    q2_monster_init(&m);
    m.spawnflags = 0x08u << 18;
    q2_creature_spawn(&bind, &m, 0);
    next = rand();
    check(r1 != r2 && next == r2,
          "a prone Insane's spawn draws exactly once: the start frame inside "
          "flymonster_start (0x800622B8 -> 0x80061B2C)");
    check_eq_i(m.frame, 59 + (r1 & 0x7FFF) % 6,
               "and that draw is Stand N's start frame, 59 + r % 6 "
               "(0x80061B8C)");

    /*
     * UPRIGHT: walkmonster_start (0x801009AC) draws the start frame first,
     * THEN the module draws the skin at 0x801009BC. Two draws, in that order.
     */
    srand(4321);
    q2_monster_init(&m);
    m.spawnflags = 0;
    q2_creature_spawn(&bind, &m, 0);
    next = rand();
    check(r2 != r3 && next == r3,
          "an upright Insane's spawn draws exactly twice");
    check(m.frame == 59 + (r1 & 0x7FFF) % 6 &&
              m.skinnum == (u8)((r2 & 0x7FFF) % 3),
          "the start frame first, then the skin — `rand() % 3` on the SECOND "
          "draw, as 0x801009B4 follows the wrapper call");

    /*
     * AND NEITHER IS COUNTED. The Insane raises AI_GOOD_GUY at 0x80100904,
     * before either start call, so monster_start's 0x80061A50 `andi v0, v0,
     * 0x100` skips the total_monsters increment. The Testbeast, which sets no
     * such bit, is the control: the count is live, it just leaves these out.
     */
    q2_monster_init(&t);
    q2_creature_spawn(&beast, &t, 0);
    check_eq_i(q2_level_state.total_monsters, 1,
               "two Insanes and one Testbeast make a level total of one");

    q2_cre_bind_reset();
    q2_class_table_reset();
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC creature tests\n\n");

    test_summoned_batch_is_started();
    test_bind();
    test_population_spawn_flags();
    test_move_lookup();
    test_endfunc_resolution();
    test_frames_drive();
    test_generic_fallback();
    test_soldier_present();
    test_actions();
    test_sound_fallback();
    test_fire_shot_has_no_enemy_guard();
    test_tank_burst_rule();
    test_insane_prone_flies();
    test_insane_start_draws();

    q2_cre_bind_reset();
    q2_class_table_reset();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
