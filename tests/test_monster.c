/*
 * test_monster.c — the creature framework's arithmetic.
 *
 * The interesting things to pin here are the ones that are easy to get subtly
 * wrong and hard to notice in play: the unpadded frame stride, the distance
 * scaling, and the width of the forward cone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "crebind.h"
#include "monster.h"
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
static void test_frame_stride(void)
{
    /* Three frames packed with no padding. Reading these at a stride of four
     * would return the wrong ai/dist/think on every frame after the first. */
    static const u8 image[] = {
        1, 10, 0,      /* ai_stand,  dist 10, think 0 */
        3, 21, 2,      /* ai_run,    dist 21, think 2 */
        4, (u8)-5, 6   /* ai_charge, dist -5, think 6 */
    };
    q2_mframe f;

    printf("frame stride\n");

    check_eq_i(Q2_MFRAME_SIZE, 3, "a frame is three bytes, not four");

    check(q2_mframe_read(image, sizeof(image), 0, &f), "reads frame 0");
    check_eq_i(f.ai, Q2_AI_STAND, "frame 0 verb");
    check_eq_i(f.dist, 10, "frame 0 distance");

    check(q2_mframe_read(image, sizeof(image), 3, &f), "reads frame 1 at +3");
    check_eq_i(f.ai, Q2_AI_RUN, "frame 1 verb");
    check_eq_i(f.dist, 21, "frame 1 distance");
    check_eq_i(f.think, 2, "frame 1 think");

    check(q2_mframe_read(image, sizeof(image), 6, &f), "reads frame 2 at +6");
    check_eq_i(f.ai, Q2_AI_CHARGE, "frame 2 verb");
    check_eq_i(f.dist, -5, "distance is signed");

    /* Bounds: a partial frame at the end must be refused, not read. */
    check(!q2_mframe_read(image, sizeof(image), 7, &f), "refuses a partial frame");
    check(!q2_mframe_read(image, sizeof(image), 99, &f), "refuses past the end");
}

/* ------------------------------------------------------------------------- */
static void test_move_record(void)
{
    u8 image[Q2_MMOVE_SIZE * 2];
    q2_mmove mv;

    printf("move record\n");
    memset(image, 0, sizeof(image));

    /* first 4, last 11, frames at 0x40, no end callback. */
    image[0] = 4;
    image[4] = 11;
    image[8] = 0x40;

    check(q2_mmove_read(image, sizeof(image), 0, &mv), "reads a move");
    check_eq_i(mv.first_frame, 4, "first frame");
    check_eq_i(mv.last_frame, 11, "last frame");
    check_eq_i(mv.frames_offset, 0x40, "frame array offset");
    check_eq_i(mv.endfunc_offset, 0, "no end callback");

    /* A move running backwards is malformed and must be refused rather than
     * producing a negative frame count later. */
    memset(image, 0, sizeof(image));
    image[0] = 20;
    image[4] = 5;
    check(!q2_mmove_read(image, sizeof(image), 0, &mv), "refuses last < first");
}

/* ------------------------------------------------------------------------- */
static void test_frame_distance(void)
{
    q2_monster m;
    q2_mframe f;

    printf("frame distance\n");

    q2_monster_init(&m);
    f.ai = Q2_AI_RUN;
    f.dist = 21;
    f.think = 0;

    /* dist * speed_scale * 12 / 10, with the neutral scale of 10. */
    check_eq_i(q2_monster_frame_dist(&m, &f), (21 * 10 * 12) / 10, "neutral scale");

    m.speed_scale = 20;
    check_eq_i(q2_monster_frame_dist(&m, &f), (21 * 20 * 12) / 10, "double scale");

    /* A held frame animates without advancing, which is what a wind-up needs. */
    m.speed_scale = 10;
    m.aiflags |= Q2_AI_HOLD_FRAME;
    check_eq_i(q2_monster_frame_dist(&m, &f), 0, "hold-frame freezes movement");

    m.aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
    check(q2_monster_frame_dist(&m, &f) != 0, "and releases it again");

    /*
     * A DYING CREATURE STILL TRANSLATES, AND A DETACHED CORPSE DOES NOT.
     *
     * The console's corpse handler (0x8007F71C) makes no position write at
     * all, which is why `corpse` stops it — but `corpse` is raised by the
     * module's own `*_dead`, at the END of the death move. A body between the
     * killing hit and that callback is still an edict and still steps, and it
     * has to: the Infantry's Death1 opens at dist -4 and carries non-zero
     * steps most of the way through, so suppressing them played the whole
     * death clip on the spot the creature was shot.
     */
    m.dead = true;
    check(q2_monster_frame_dist(&m, &f) != 0,
          "a dying creature still translates: the death move's own steps");
    m.dead   = false;
    m.corpse = true;
    check_eq_i(q2_monster_frame_dist(&m, &f), 0, "a detached corpse does not");
    m.corpse = false;
}

/* ------------------------------------------------------------------------- */
static void test_infront(void)
{
    q2_monster m, t;

    printf("forward cone\n");

    q2_monster_init(&m);
    q2_monster_init(&t);
    m.pos[0] = 0; m.pos[1] = 0; m.pos[2] = 0;
    m.angles[2] = 0;                     /* facing +Z: yaw is angles[2] */

    t.pos[0] = 0; t.pos[1] = 0; t.pos[2] = 1000;
    check(q2_infront(&m, &t), "sees straight ahead");

    t.pos[0] = 0; t.pos[2] = -1000;
    check(!q2_infront(&m, &t), "does not see behind");

    t.pos[0] = 1000; t.pos[2] = 0;
    check(!q2_infront(&m, &t), "does not see exactly sideways");

    /* The cone is wide: a dot threshold of 1230/4096 is about 0.30, so roughly
     * 72 degrees off-axis is still visible. Check a point well off centre. */
    t.pos[0] = 900;  t.pos[2] = 1000;
    check(q2_infront(&m, &t), "the cone is wide, not narrow");

    /* Turning around must reverse the answers. */
    m.angles[2] = Q2_ANGLE_180;
    t.pos[0] = 0; t.pos[2] = 1000;
    check(!q2_infront(&m, &t), "turning around loses the target");
    t.pos[2] = -1000;
    check(q2_infront(&m, &t), "and acquires what was behind");
}

/* ------------------------------------------------------------------------- */
static void test_damage(void)
{
    q2_monster m;

    printf("damage and death\n");

    q2_monster_init(&m);
    m.in_use     = true;
    m.health     = 240;
    m.max_health = 240;
    m.gib_health = -60;

    check(!q2_monster_damage(&m, 100), "survives 100");
    check_eq_i(m.health, 140, "health drops");
    check(!m.dead, "still alive");

    check(q2_monster_damage(&m, 200), "dies when health passes zero");
    check(m.dead, "marked dead");

    /* A dead creature absorbs no further damage. */
    check(!q2_monster_damage(&m, 50), "further damage is ignored");
}

/* ------------------------------------------------------------------------- */
/*
 * M_ReactToDamage (0x80062654) and the tail of T_Damage (0x80062940..0x80062B54).
 *
 * These are behavioural rather than arithmetic, and every one of them pins a
 * thing that was silently not happening before: `oldenemy` had no writer at all
 * in the whole tree, the kill counter did not exist, and a creature shot from
 * behind never turned round.
 */
static int g_pain_calls;
static int g_die_calls;

static s16 g_pain_damage;
static void stub_pain(q2_monster *m, s16 damage)
{
    (void)m;
    g_pain_damage = damage;
    g_pain_calls++;
}
static s16 g_die_damage;
static void stub_die(q2_monster *m, s16 damage)
{
    g_die_damage = damage;
    g_die_calls++;
    m->dead = true;
}

static void make_monster(q2_monster *m, u8 class_byte, s16 health)
{
    q2_monster_init(m);
    m->in_use      = true;
    m->spawnflags |= Q2_SVFLAG_INUSE;
    m->svflags    |= Q2_SVF_MONSTER;
    m->class_id    = class_byte;
    m->health      = health;
    m->max_health  = health;
    m->gib_health  = (s16)(-health);
    m->pain        = stub_pain;
    m->die         = stub_die;
}

static void make_player(q2_monster *m)
{
    q2_monster_init(m);
    m->in_use      = true;
    m->spawnflags |= Q2_SVFLAG_INUSE;
    m->client      = true;
    m->health      = 100;
    m->max_health  = 100;
}

static void test_react_to_damage(void)
{
    q2_monster targ, player, other, ally;

    printf("M_ReactToDamage\n");

    /* A player shooting a creature that has no enemy becomes its enemy. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    q2_m_react_to_damage(&targ, &player);
    check(targ.enemy == &player, "an unengaged creature turns on its attacker");

    /* Shot by the thing it is already fighting: nothing is remembered. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.enemy    = &player;
    targ.oldenemy = NULL;
    q2_m_react_to_damage(&targ, &player);
    check(targ.oldenemy == NULL,
          "the current enemy shooting again changes nothing");

    /*
     * Hit by a DIFFERENT kind of creature that moves the same way: fight back.
     * 0x80062728 — same base type, different class byte, and not one of the
     * four the original refuses to take offence at.
     */
    q2_level_reset();
    make_monster(&targ, 87, 100);            /* Soldier */
    make_monster(&other, 79, 100);           /* Gunner  */
    q2_m_react_to_damage(&targ, &other);
    check(targ.enemy == &other,
          "a creature fights back at another kind of creature");

    /* Its own kind does not start a fight — id's classname test, and here it is
     * a class-byte equality. It takes up that one's target instead. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_monster(&other, 87, 100);
    make_player(&player);
    other.enemy = &player;
    q2_m_react_to_damage(&targ, &other);
    check(targ.enemy == &player,
          "hit by its own kind, it takes up that one's enemy instead");

    /*
     * THE FOUR EXCLUSIONS, from the class-byte column of the descriptor table
     * at 0x800A3518: Tankcomm 91, Boss1 90, Rider 83, Jorg 82 — id's tank,
     * supertank, makron and jorg. Splash from one of these makes a creature
     * take up ITS target rather than turn on it.
     */
    {
        const u8 excluded[4] = {
            Q2_CLASS_TANKCOMM, Q2_CLASS_BOSS1, Q2_CLASS_RIDER, Q2_CLASS_JORG
        };
        int i;

        for (i = 0; i < 4; i++) {
            q2_level_reset();
            make_monster(&targ, 87, 100);
            make_monster(&other, excluded[i], 100);
            make_player(&player);
            other.enemy = &player;
            q2_m_react_to_damage(&targ, &other);
            check(targ.enemy == &player,
                  "a big monster's crossfire does not start a fight");
        }
    }

    /* A good guy does not get angry at a player. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.aiflags |= Q2_AI_GOOD_GUY;
    q2_m_react_to_damage(&targ, &player);
    check(targ.enemy == NULL, "a good guy ignores a player's fire");

    /* Nor at another good guy. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_monster(&ally, 79, 100);
    targ.aiflags |= Q2_AI_GOOD_GUY;
    ally.aiflags |= Q2_AI_GOOD_GUY;
    q2_m_react_to_damage(&targ, &ally);
    check(targ.enemy == NULL, "a good guy ignores another good guy");

    /*
     * Already fighting a VISIBLE player: it remembers the new attacker rather
     * than switching to them. The stand-in world has no obstruction, so
     * `q2_visible` is true and this takes the first arm. 0x800626F4.
     */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    make_player(&other);
    other.pos[0] = 4000;
    targ.enemy = &player;
    q2_m_react_to_damage(&targ, &other);
    check(targ.enemy == &player, "it keeps the player it can see");
    check(targ.oldenemy == &other, "and remembers the one that shot it");
}

/* ------------------------------------------------------------------------- */
static void test_damage_reaction(void)
{
    q2_monster targ, player;

    printf("T_Damage tail\n");

    /* A survivable hit flinches, and turns the creature on the shooter first. */
    q2_level_reset();
    g_pain_calls = 0;
    g_die_calls  = 0;
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.health = 60;
    q2_monster_damage_reaction(&targ, &player, 40);
    check_eq_i(g_pain_calls, 1, "a survivable hit calls pain once");
    check_eq_i(g_pain_damage, 40, "and it is told how much landed");
    check_eq_i(g_die_calls, 0, "and does not call die");
    check(targ.enemy == &player, "and the creature has turned on the shooter");

    /* A creature mid-duck absorbs the hit without flinching — 0x80062AD0. */
    q2_level_reset();
    g_pain_calls = 0;
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.health   = 60;
    targ.aiflags |= Q2_AI_DUCKED;
    q2_monster_damage_reaction(&targ, &player, 40);
    check_eq_i(g_pain_calls, 0, "a ducked creature does not flinch");

    /* The lethal hit calls die exactly once. */
    q2_level_reset();
    g_pain_calls = 0;
    g_die_calls  = 0;
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.health = -10;
    q2_monster_damage_reaction(&targ, &player, 110);
    check_eq_i(g_die_calls, 1, "the lethal hit calls die");
    check_eq_i(g_die_damage, 110, "and it is told how much landed");
    check(targ.dead, "and the creature is dead");
    check_eq_i(targ.deadflag, Q2_DEAD_DEAD,
               "with deadflag and dead agreeing");
    check_eq_i(q2_level_state.killed_monsters, 1, "the kill is counted");
    check((targ.flags & Q2_FL_NO_KNOCKBACK) != 0,
          "a body takes no more knockback");

    /*
     * SHOOTING THE BODY AGAIN CALLS `die` AGAIN, and that is the point: it is
     * the only route to a module's gib arm, which tests `health <= gib_health`
     * BEFORE its own already-dead guard. The console's T_Damage has no guard on
     * that call at all — every path from 0x80062978 reaches `jalr v0` at
     * 0x80062A9C. What the second hit must NOT do is count a second kill or run
     * `monster_death_use` again; those are what 0x800629E8 and 0x80062A70 skip.
     */
    targ.health = -200;
    q2_monster_damage_reaction(&targ, &player, 190);
    check_eq_i(g_die_calls, 2, "shooting the body again re-enters die");
    check_eq_i(g_die_damage, 190, "with the new hit's damage");
    check_eq_i(q2_level_state.killed_monsters, 1, "but the kill is not counted twice");

    /* The floor, 0x800629B4. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.health = -30000;
    q2_monster_damage_reaction(&targ, &player, 30100);
    check_eq_i(targ.health, -9999, "health floors at -9999");

    /* A good guy's death is not on the scoreboard. 0x800629F8. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.aiflags |= Q2_AI_GOOD_GUY;
    targ.health   = -5;
    q2_monster_damage_reaction(&targ, &player, 105);
    check_eq_i(q2_level_state.killed_monsters, 0, "a good guy is not counted");

    /* monster_death_use, 0x800622E8. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.flags   |= Q2_FL_FLY | Q2_FL_SWIM;
    targ.aiflags |= Q2_AI_GOOD_GUY | Q2_AI_STAND_GROUND | Q2_AI_DUCKED
                  | Q2_AI_HOLD_FRAME;
    targ.health   = -5;
    q2_monster_damage_reaction(&targ, &player, 105);
    check_eq_i(targ.aiflags, Q2_AI_GOOD_GUY,
               "death clears every ai flag but AI_GOOD_GUY");
    check((targ.flags & (Q2_FL_FLY | Q2_FL_SWIM)) == 0,
          "a dead flyer stops flying");

    /* And it does NOT run a second time on a body already down — 0x80062A70
     * skips exactly this and the die call below it. */
    targ.aiflags |= Q2_AI_STAND_GROUND;
    targ.health   = -400;
    q2_monster_damage_reaction(&targ, &player, 395);
    check_eq_i(targ.aiflags, Q2_AI_GOOD_GUY | Q2_AI_STAND_GROUND,
               "a second hit does not re-run monster_death_use");

    /* The nightmare debounce, 0x80062B20: five seconds rather than the pain
     * handler's own three. */
    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.health = 60;
    q2_cre_set_skill(3);
    q2_monster_damage_reaction(&targ, &player, 40);
    check_eq_i(targ.pain_debounce, Q2_AI_SECONDS(5),
               "skill 3 pushes the pain debounce out to five seconds");
    q2_cre_set_skill(1);
}

/* ------------------------------------------------------------------------- */
/*
 * Corpses — the detach at 0x8007F098, the volume rescale at 0x8007F77C and the
 * handler at 0x8007F71C. None of this existed on this side, which is why a body
 * stayed a full-height creature that the AI still owned.
 */
static void test_corpse(void)
{
    q2_monster targ, player;

    printf("corpses\n");

    q2_level_reset();
    make_monster(&targ, 87, 100);
    make_player(&player);
    targ.health = -10;
    q2_monster_damage_reaction(&targ, &player, 110);

    check(!targ.corpse, "a body is not a corpse until its module says so");

    /* Every `*_dead` raises this; it is the detach's own trigger. */
    targ.svflags |= Q2_SVF_DEADMONSTER;
    q2_monster_corpse_detach(&targ);

    check(targ.corpse, "SVF_DEADMONSTER detaches the body");
    check_eq_i(targ.class_id, Q2_CLASS_CORPSE, "and it becomes class 47");
    check_eq_i(targ.corpse_was_class, 87, "with what it was kept");
    check(targ.think == NULL, "the think is gone");
    check(targ.enemy == NULL && targ.run == NULL,
          "and so is everything that made it a creature");
    check((targ.svflags & Q2_SVF_MONSTER) == 0, "it is no longer a monster");
    check(targ.in_use, "but it is still in the world");

    /*
     * The volume: a quarter as tall and half again as wide, from a ±286 cube.
     * 286/4 = 71 and (286*3)/2 = 429, with the console's own rounding.
     */
    check_eq_i(targ.maxs[1], 71,  "a corpse is a quarter as tall");
    check_eq_i(targ.mins[1], -71, "on both sides");
    check_eq_i(targ.maxs[0], 429, "and half again as wide");
    check_eq_i(targ.maxs[2], 429, "on both horizontal axes");

    /* Detaching twice must not rescale twice. */
    q2_monster_corpse_detach(&targ);
    check_eq_i(targ.maxs[1], 71, "detaching again changes nothing");
    check_eq_i(targ.corpse_was_class, 87, "and does not lose the old class");

    /* The handler gibs when health passes the threshold it kept. */
    check(!q2_monster_corpse_tick(&targ), "an intact corpse is not destroyed");
    check(!targ.gibbed, "and is not marked gibbed");

    targ.health = targ.gib_health;
    check(q2_monster_corpse_tick(&targ),
          "a corpse at exactly gib_health is destroyed — the boundary is "
          "inclusive, because the console keeps the body on gib < health");
    check(targ.gibbed, "and is marked gibbed");
    check(!q2_monster_corpse_tick(&targ), "and is destroyed only once");
}

/* ------------------------------------------------------------------------- */
/*
 * The dissolve gate, 0x8005B2A8 — the corpse handler's FIRST call (0x8007F728
 * `jal`, then 0x8007F730 `bne v0, zero` straight to the return). effect[2]
 * (+0x2F2) or else effect[0] (+0x2F0) swaps the body to a handler that drains
 * +0xF4 from 4096 by `dt << 6` a run and then frees it through 0x8006D280.
 */
static void make_corpse(q2_monster *m, s16 health)
{
    q2_monster player;

    q2_level_reset();
    make_monster(m, 87, 100);
    make_player(&player);
    m->health = health;
    q2_monster_damage_reaction(m, &player, 110);

    /* What a module's normal death arm leaves (DAMAGE_YES; combat.c,
     * nearest_hit): the body stays shootable so that it can still be
     * gibbed. */
    m->takedamage = Q2_DAMAGE_YES;
    m->svflags   |= Q2_SVF_DEADMONSTER;
    q2_monster_corpse_detach(m);
}

static void test_corpse_dissolve(void)
{
    q2_monster m;
    int i;
    bool gone, alive;

    printf("the corpse dissolve gate\n");

    /*
     * effect[0] SET: the gate takes the body on its first corpse tick. It is
     * not destroyed that tick; it is handed to 0x8005B39C with 4096 in +0xF4,
     * and it drops off the actor list, so no sweep can find it.
     */
    make_corpse(&m, -10);
    m.effect[0] = 15;                      /* Q2_MOD_2's timer, 0x800585A4 */
    alive = !q2_monster_corpse_tick(&m) && m.in_use;
    check(alive && m.dissolve_arm == Q2_CORPSE_DISSOLVE_FX0,
          "effect[0]: the gate's tick installs 0x8005B39C and keeps the body");
    check_eq_i(m.dissolve, 4096, "the gate stores 4096 in +0xF4 (0x8005B2F4)");
    check_eq_i(m.takedamage, Q2_DAMAGE_NO,
               "and the body leaves the actor list: no 0x800552B4 any more");

    /*
     * One AI tick is Q2_AI_TICK_DT = 30 of the engine's dt, so a run takes
     * 30 << 6 = 1920: 4096 -> 2176 -> 256 -> gone on the third.
     */
    alive = !q2_monster_corpse_tick(&m);
    check(alive && m.dissolve == 4096 - 1920,
          "first drain: still there, drained by 30 << 6");
    alive = !q2_monster_corpse_tick(&m);
    check(alive && m.dissolve == 256, "second drain: still there, at 256");
    check(q2_monster_corpse_tick(&m),
          "third drain: +0xF4 is no longer positive and the body is freed");
    check(!m.in_use && !m.gibbed,
          "freed as 0x8006D280 frees the actor, and never thrown");
    check(!q2_monster_corpse_tick(&m) && !m.in_use,
          "and it is freed only once");

    /*
     * NO EFFECT: the body lingers as before, however long it lies there, and
     * stays shootable so that it can still be gibbed.
     */
    make_corpse(&m, -10);
    gone = false;
    for (i = 0; i < 300; i++)
        gone = gone || q2_monster_corpse_tick(&m);
    check(!gone && m.in_use, "no effect: the body lingers for 300 ticks");
    check_eq_i(m.dissolve_arm, Q2_CORPSE_DISSOLVE_NONE,
               "no effect: the gate never installs a handler");
    check_eq_i(m.dissolve, 0, "no effect: +0xF4 is not written");
    check_eq_i(m.takedamage, Q2_DAMAGE_YES, "no effect: it can still be hit");

    /*
     * The gate reads +0x2F2 and +0x2F0 and nothing else: effect[1], [3], [4]
     * and [5] set do not open it.
     */
    make_corpse(&m, -10);
    m.effect[1] = 3;
    m.effect[3] = 9;
    m.effect[4] = 5;
    m.effect[5] = 7;
    gone = false;
    for (i = 0; i < 300; i++)
        gone = gone || q2_monster_corpse_tick(&m);
    check(!gone && m.in_use &&
          m.dissolve_arm == Q2_CORPSE_DISSOLVE_NONE,
          "effect[1], [3], [4] and [5] leave the gate shut");

    /*
     * effect[2] TAKES PRECEDENCE: 0x8005B2B4 reads it first and branches
     * straight to the store, so with both set the handler is 0x8005B444.
     */
    make_corpse(&m, -10);
    m.effect[2] = 30;                      /* Q2_MOD_4's timer */
    alive = !q2_monster_corpse_tick(&m) && m.in_use;
    check(alive && m.dissolve_arm == Q2_CORPSE_DISSOLVE_FX2,
          "effect[2] alone installs 0x8005B444 and keeps the body");

    make_corpse(&m, -10);
    m.effect[0] = 15;
    m.effect[2] = 30;
    alive = !q2_monster_corpse_tick(&m) && m.in_use;
    check(alive && m.dissolve_arm == Q2_CORPSE_DISSOLVE_FX2,
          "with effect[0] and effect[2] both set, effect[2] wins");
    check_eq_i(m.dissolve, 4096, "and the level is the same 4096");

    /*
     * THE GATE COMES BEFORE THE GIB TEST (0x8007F728 against 0x8007F748). A
     * body at gib_health with effect[0] set is not thrown: it dissolves.
     */
    make_corpse(&m, -10);
    m.health    = m.gib_health;
    m.effect[0] = 15;
    check(!q2_monster_corpse_tick(&m),
          "at gib_health with effect[0] set, the gate's tick destroys nothing");
    check(!m.gibbed, "the body is not thrown");
    check_eq_i(m.dissolve_arm, Q2_CORPSE_DISSOLVE_FX0,
               "it is dissolving instead");
    for (i = 0, gone = false; i < 3 && !gone; i++)
        gone = q2_monster_corpse_tick(&m);
    check(gone && !m.gibbed && !m.in_use,
          "and it goes at the end of the drain, still not thrown");

    /*
     * A body that lay a while and was hit later: the gate is asked every
     * tick, so the next tick after the byte is set takes it.
     */
    make_corpse(&m, -10);
    for (i = 0; i < 20; i++)
        q2_monster_corpse_tick(&m);
    m.effect[0] = 15;
    q2_monster_corpse_tick(&m);
    check_eq_i(m.dissolve_arm, Q2_CORPSE_DISSOLVE_FX0,
               "a byte set on a settled body opens the gate on its next tick");
}

/* ------------------------------------------------------------------------- */
/* The go-routines — 0x8006241C, 0x800624BC, 0x8006255C                       */
/* ------------------------------------------------------------------------- */
static void test_start_go_kinds(void)
{
    q2_monster m;

    printf("the three go-routines\n");

    /*
     * The field starts UNSET. It used to be 200 for everything, which is what
     * the `bne` at 0x80062434 exists to allow a module to keep — and nothing
     * ever set it, so 200 was every creature's turn rate.
     */
    q2_monster_init(&m);
    check_eq_i(m.yaw_speed, 0, "q2_monster_init leaves yaw_speed unset");

    /* 228/4096 of a turn is 20.04 degrees a tick — id's 20. 0x80062438. */
    q2_monster_init(&m);
    m.health = 100;
    q2_monster_walk_start_go(&m, 251);
    check_eq_i(m.yaw_speed, 228, "a walker's default yaw_speed is 228");
    check_eq_i(m.view_height, -252,
               "and a Soldier's eye is ~251, which is -252 and not -251");

    q2_monster_init(&m);
    m.health = 100;
    q2_monster_walk_start_go(&m, 507);
    check_eq_i(m.view_height, -508, "a Tank Commander's eye, ext2 507");

    q2_monster_init(&m);
    m.health = 100;
    q2_monster_walk_start_go(&m, 217);
    check_eq_i(m.view_height, -218, "an Arachner's eye, ext2 217");

    q2_monster_init(&m);
    m.health = 100;
    q2_monster_walk_start_go(&m, 380);
    check_eq_i(m.view_height, -381, "a Gunner's eye, ext2 380");

    q2_monster_init(&m);
    m.health = 100;
    q2_monster_walk_start_go(&m, 304);
    check_eq_i(m.view_height, -305, "an upright Insane's eye, ext2 304");

    /* The store is an `sh` into a s16, so ~0 truncates to -1, not to 0. */
    q2_monster_init(&m);
    m.health = 100;
    q2_monster_walk_start_go(&m, 0);
    check_eq_i(m.view_height, -1, "ext2 0 complements to -1, not to 0");

    /* Only the STORE is conditional: a creature that already has one keeps it,
     * which is the whole reason 0x80062434 is a branch. */
    q2_monster_init(&m);
    m.health    = 100;
    m.yaw_speed = 300;
    q2_monster_walk_start_go(&m, 251);
    check_eq_i(m.yaw_speed, 300, "a pre-set yaw_speed survives the go-routine");

    /* 114/4096 is 10.02 degrees — id's 10. Flat eye, no model read. */
    q2_monster_init(&m);
    m.health = 100;
    q2_monster_fly_start_go(&m);
    check_eq_i(m.yaw_speed, 114, "a flyer's default yaw_speed is 114");
    check_eq_i(m.view_height, -250, "and its eye is a flat -250");

    q2_monster_init(&m);
    m.health = 100;
    q2_monster_swim_start_go(&m);
    check_eq_i(m.yaw_speed, 114, "a swimmer's default yaw_speed is 114");
    check_eq_i(m.view_height, -100, "and its eye is -100 on both paths");

    /*
     * The WRAPPER raises the flag, at spawn — 0x800622B4 `ori v1, v1, 0x1` in
     * flymonster_start — before the creature has ever thought, and parks the
     * go-routine the port's single wake entry then dispatches on.
     */
    q2_monster_init(&m);
    m.health = 100;
    q2_monster_fly_start(&m);
    check((m.flags & Q2_FL_FLY) != 0,
          "flymonster_start raises FL_FLY at spawn, before any go-routine");
    check_eq_i(m.start_kind, Q2_CRE_START_FLY, "and parks the fly go-routine");
    q2_monster_start_go(&m);
    check_eq_i(m.view_height, -250, "start_go dispatches a flyer");
    check_eq_i(m.yaw_speed, 114, "with the flyer's turn rate");
    check((m.flags & Q2_FL_FLY) != 0, "and it is still flying");

    q2_monster_init(&m);
    m.health = 100;
    q2_monster_swim_start(&m);
    check((m.flags & Q2_FL_SWIM) != 0,
          "swimmonster_start raises FL_SWIM, 0x80062280");
    q2_monster_start_go(&m);
    check_eq_i(m.view_height, -100, "start_go dispatches a swimmer");

    /* 0x80062240 overwrites whatever go-routine was parked and writes no
     * flag of its own. */
    q2_monster_init(&m);
    m.health     = 100;
    m.start_kind = Q2_CRE_START_SWIM;
    q2_monster_walk_start(&m);
    check(m.start_kind == Q2_CRE_START_WALK &&
              (m.flags & (Q2_FL_FLY | Q2_FL_SWIM)) == 0,
          "walkmonster_start parks the walk go-routine and raises no flag");

    q2_monster_init(&m);
    m.health     = 100;
    m.model_ext2 = 251;
    q2_monster_start_go(&m);
    check_eq_i(m.view_height, -252, "start_go dispatches a walker by its model");
    check(m.yaw_speed == 228 && (m.flags & (Q2_FL_FLY | Q2_FL_SWIM)) == 0,
          "with the walker's turn rate, and neither movement bit");

    /* The port guard, not the disc's: with no model resolved the eye keeps the
     * -290 stand-in rather than going to -1. */
    q2_monster_init(&m);
    m.health = 100;
    q2_monster_start_go(&m);
    check(m.view_height == -290 && m.yaw_speed == 228,
          "an unresolved model keeps the stand-in eye rather than ~0 = -1, "
          "but still gets the walker's turn rate");
}

/* ------------------------------------------------------------------------- */
/* monster_start's start-frame pick — 0x80061B1C..0x80061B8C                  */
/* ------------------------------------------------------------------------- */
/*
 * Each wrapper ends in `jal 0x800619E0`, and monster_start's last act is
 * `if (currentmove) frame = first + rand() % (last - first + 1)` — 0x80061B24
 * tests the move, 0x80061B2C draws BIOS rand(), 0x80061B8C stores the frame.
 * So the wrapper call costs ONE draw, taken at the call, whenever a move is
 * installed, and none when it is not.
 */

/*
 * A seed whose first masked draw is not a multiple of `span`, and whose first
 * two masked draws differ modulo `span`, under whatever C library this is
 * built against. The pick is only pinned if `first + r % span` cannot come
 * out equal to `first`, or equal to what the second draw would give. The
 * fixed srand(2024) failed that on the UCRT: its first draw is 6648, a
 * multiple of 6, so a pick that dropped the remainder passed.
 */
static unsigned seed_for_span(int span)
{
    unsigned s;

    for (s = 2024; s < 2024 + 1000; s++) {
        int a, b;

        srand(s);
        a = rand() & 0x7FFF;
        b = rand() & 0x7FFF;
        if (a % span != 0 && a % span != b % span)
            return s;
    }
    return 2024;
}

static void test_start_frame_pick(void)
{
    /* Six zeroed frames: static storage zero-fills them. Not const, because
     * MSVC's C4132 wants a const object initialised and a brace initialiser
     * for a struct array draws -Wmissing-braces elsewhere. */
    static q2_mframe frames[6];
    static void (*const start[3])(q2_monster *) = {
        q2_monster_walk_start, q2_monster_fly_start, q2_monster_swim_start
    };
    static const char *const what[3] = {
        "walkmonster_start picks a start frame with one draw (0x80062250)",
        "flymonster_start picks a start frame with one draw (0x800622B8)",
        "swimmonster_start picks a start frame with one draw (0x80062284)"
    };
    q2_mmove mv;
    q2_monster m;
    int k, r1, r2, next;
    bool none_drawn;
    s16 none_frame;
    unsigned seed;

    printf("monster_start's start-frame pick\n");

    /* The Insane's "Stand N", 59..64 — what its spawn installs at 0x80100960
     * before calling either wrapper. */
    memset(&mv, 0, sizeof(mv));
    mv.first_frame = 59;
    mv.last_frame  = 64;
    mv.frames      = frames;

    seed = seed_for_span(6);
    for (k = 0; k < 3; k++) {
        srand(seed);
        r1 = rand();
        r2 = rand();

        srand(seed);
        q2_monster_init(&m);
        m.currentmove = &mv;
        start[k](&m);
        next = rand();

        /* The seed's own conditions are in the test so that a library on
         * which none of the thousand seeds qualify fails loudly rather than
         * pinning nothing. */
        check((r1 & 0x7FFF) % 6 != 0 &&
              (r1 & 0x7FFF) % 6 != (r2 & 0x7FFF) % 6 &&
              m.frame == 59 + (r1 & 0x7FFF) % 6 && next == r2,
              what[k]);
    }

    /*
     * And NOT without a move: 0x80061B24 `beq v0, zero, 0x80061B90` jumps past
     * both the draw and the store. Checked together with the case above, so
     * the rule is "exactly when a move is installed".
     */
    srand(2024);
    r1 = rand();
    srand(2024);
    q2_monster_init(&m);
    m.frame = 7;
    q2_monster_walk_start(&m);
    none_drawn = (rand() != r1);
    none_frame = m.frame;

    srand(2024);
    r1 = rand();
    r2 = rand();
    srand(2024);
    q2_monster_init(&m);
    m.currentmove = &mv;
    q2_monster_walk_start(&m);
    next = rand();

    check(next == r2 && r1 != r2 && !none_drawn && none_frame == 7,
          "the pick draws exactly when a move is installed, and leaves the "
          "frame alone when none is (0x80061B24)");
}

/* ------------------------------------------------------------------------- */
/* The death drop — 0x800AB79C, 0x800AB814 and the four slots at 0x800C6D70   */
/* ------------------------------------------------------------------------- */
static int g_drop_calls;
static int g_drop_item;
static int g_drop_heading;
static s32 g_drop_pos[3];

static void spy_drop(const q2_monster_drop_request *req, void *user)
{
    (void)user;
    g_drop_calls++;
    g_drop_item    = req->item_id;
    g_drop_heading = req->heading;
    g_drop_pos[0]  = req->pos[0];
    g_drop_pos[1]  = req->pos[1];
    g_drop_pos[2]  = req->pos[2];
}

static void test_death_drop_table(void)
{
    /*
     * The class rows 0..37, against the 30 words dumped out of 0x800AB79C —
     * all but four. Rows 10, 25 and 26 read the player's weapon and are driven
     * below. Row 3 takes the same below-the-window path as rows 0, 1, 2 and 4
     * — `(s16)(3 - 5)` is -2, which `sltiu v0, v1, 30` at 0x800206E8 rejects —
     * so a line for it would pin nothing those four do not.
     *
     * The eleven rows that must come back 0 are as much of the reading as the
     * nineteen that spawn something: they are the ones that land on 0x800207D0
     * with s0 still zero, where `beq s0, zero, 0x80020848` spawns nothing.
     */
    static const struct { u8 row; s8 want; const char *what; } k[] = {
        {  0,  0, "class 0 is below the window" },
        {  1,  0, "1 Berserk is below the window" },
        {  2,  0, "2 Boss2 is below the window" },
        {  4,  0, "4 is below the window" },
        {  5, 26, "5 Ironmaiden -> 26 Rockets" },
        {  6,  0, "6 Flipper -> nothing" },
        {  7,  0, "7 -> nothing" },
        {  8, 24, "8 Flyer -> 24 Cells" },
        {  9, 28, "9 Gladiator -> 28 Slugs" },
        { 11, 24, "11 Hover -> 24 Cells" },
        { 12, 23, "12 Infantry -> 23 Bullets" },
        { 13,  0, "13 Jorg -> nothing" },
        { 14,  0, "14 Rider -> nothing" },
        { 15, 24, "15 Medic -> 24 Cells" },
        { 16,  0, "16 -> nothing" },
        { 17,  0, "17 Parasite -> nothing, so a flagged Parasite drops none" },
        { 18, 27, "18 Soldier (shotgun, 30hp) -> 27 Shells" },
        { 19, 24, "19 Soldier (blaster, 20hp) -> 24 Cells" },
        { 20, 23, "20 Soldier (machinegun, 40hp) -> 23 Bullets" },
        { 21,  0, "21 -> nothing" },
        { 22,  0, "22 -> nothing" },
        { 23,  0, "23 -> nothing" },
        { 24, 38, "24 Boss1 -> 38 Rocket Launcher" },
        { 27,  0, "27 DeadComm -> nothing" },
        { 28, 24, "28 Arachner -> 24 Cells" },
        { 29, 26, "29 Blitz -> 26 Rockets" },
        { 30, 25, "30 (no class row) -> 25 Grenades" },
        { 31, 29, "31 Flamer -> 29 Fuel" },
        { 32, 21, "32 Strider -> 21 A-M Bomb" },
        { 33,  0, "33 -> nothing" },
        { 34,  9, "34 Insane -> 9 Health" },
        { 35,  0, "35 Rider2 is past the window" },
        { 36,  0, "36 Rider3 is past the window" },
        { 37,  0, "37 RiderStand is past the window" }
    };
    u32 i;

    printf("the death-drop class table\n");

    /*
     * FIRST, while gp+276 is still zero: the Tank Commander's arm reads a
     * static, and once anything has written 23 into it the "left alone" half
     * can never be observed again. 0x800207A4 finds it zero and 0x800207B0
     * defaults to 23.
     */
    check_eq_i(q2_monster_drop_ammo_static(), 0,
               "gp+276 starts at zero, as the image stores it at 0x800AE714");
    check_eq_i(q2_monster_drop_item_for_class(25, 5, 0x000u), 23,
               "a Tankcomm with no Chaingun still drops 23 by the default");
    check_eq_i(q2_monster_drop_ammo_static(), 0,
               "and left the static alone: 0x80020790 did not take the store");

    check_eq_i(q2_monster_drop_item_for_class(25, 5, 0x010u), 23,
               "a Tankcomm with the Chaingun drops 23 through the store");
    check_eq_i(q2_monster_drop_ammo_static(), 23,
               "and 0x80020798 wrote 23 into gp+276");

    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        check_eq_i(q2_monster_drop_item_for_class(k[i].row, 1, 0), k[i].want,
                   k[i].what);

    /*
     * The Gunner arm, 0x80020730. `s0 = 23` sits in the branch's delay slot so
     * it is unconditional, and the current-weapon test is easy to miss.
     */
    check_eq_i(q2_monster_drop_item_for_class(10, 1, 0x000u), 23,
               "a Gunner drops Bullets when the player has no GL");
    check_eq_i(q2_monster_drop_item_for_class(10, 4, 0x040u), 23,
               "and Bullets with a GL owned but a Machinegun in hand");
    check_eq_i(q2_monster_drop_item_for_class(10, 5, 0x040u), 23,
               "and Bullets with a Chaingun in hand");
    check_eq_i(q2_monster_drop_item_for_class(10, 1, 0x040u), 25,
               "and Grenades only with a GL owned and neither in hand");
    check_eq_i(q2_monster_drop_item_for_class(10, 7, 0x040u), 25,
               "including while holding the GL itself");

    /*
     * The Tank Commander's INVARIANCE is the assertion. Ten of the eleven
     * entries at 0x800AB814 reach the same two instructions, the one arm that
     * differs writes the same 23, and the ammo-picker that would give the
     * static any other value (0x80020E88) has no callers on this build.
     */
    for (i = 1; i <= 11; i++) {
        check_eq_i(q2_monster_drop_item_for_class(25, (s16)i, 0x0000u), 23,
                   "a Tankcomm drops 23 for every weapon id, bits clear");
        check_eq_i(q2_monster_drop_item_for_class(25, (s16)i, 0xFFFFu), 23,
                   "and for every weapon id with every bit set");
        check_eq_i(q2_monster_drop_item_for_class(26, (s16)i, 0xFFFFu), 23,
                   "and class 26 takes the same arm");
    }
    check_eq_i(q2_monster_drop_item_for_class(25, 0, 0xFFFFu), 23,
               "and out of the 11-entry window too");
    check_eq_i(q2_monster_drop_item_for_class(25, 99, 0xFFFFu), 23,
               "at either end");
}

static void test_death_drop_queue(void)
{
    q2_monster m;
    int i;
    int want_heading;

    printf("the four-slot death-drop queue\n");

    q2_monster_drop_reset();
    q2_monster_set_drop_hook(spy_drop, NULL);
    q2_monster_set_player_weapon(1, 0);
    g_drop_calls = 0;

    /* Not flagged: 0x80062334 skips the call outright. */
    q2_monster_init(&m);
    m.pop_class_id = 18;
    m.svflags      = Q2_SVF_MONSTER;
    q2_monster_death_use(&m);
    check_eq_i(q2_monster_drop_pending(), 0,
               "a creature without spawnflag bit 26 records nothing");

    /* Flagged: one slot, and the flush turns it into one item at its position. */
    q2_monster_init(&m);
    m.pop_class_id = 18;                 /* Soldier, shotgun variant */
    m.spawnflags   = Q2_SPAWNFLAG_DROP_ITEM;
    m.pos[0] = 111; m.pos[1] = 222; m.pos[2] = 333;
    q2_monster_death_use(&m);
    check_eq_i(q2_monster_drop_pending(), 1, "a flagged death takes one slot");

    /*
     * The recorded position is the one at the moment of death, not wherever
     * the body has got to by the flush: 0x80020D60 copies obj+0x54 into the
     * slot then and there (0x80020DF0..0x80020E18).
     */
    m.pos[0] = 999;

    /*
     * 0x80020820 `jal 0x80089E28` / 0x8002082C `andi a1, v0, 0xFFF`: one draw,
     * low twelve bits, taken on the spawning path only.
     */
    srand(1234);
    want_heading = rand() & 0xFFF;
    srand(1234);
    q2_monster_drop_flush();
    check_eq_i(g_drop_calls, 1, "and the flush spawns exactly one item");
    check_eq_i(g_drop_item, 27, "27 Shells, from class row 18");
    check_eq_i(g_drop_pos[0], 111, "at the creature's x when it died");
    check_eq_i(g_drop_pos[1], 222, "y");
    check_eq_i(g_drop_pos[2], 333, "z");
    check_eq_i(g_drop_heading, want_heading,
               "with the toss heading drawn as rand() & 0xFFF");
    check_eq_i(q2_monster_drop_pending(), 0, "and the slot is free again");

    /* A class the table sends to 0x800207D0 occupies a slot and spawns
     * nothing — the queue does not know what it holds. */
    q2_monster_drop_reset();
    g_drop_calls = 0;
    q2_monster_init(&m);
    m.pop_class_id = 17;                 /* Parasite */
    m.spawnflags   = Q2_SPAWNFLAG_DROP_ITEM;
    q2_monster_death_use(&m);
    check_eq_i(q2_monster_drop_pending(), 1, "a flagged Parasite still queues");
    srand(1234);
    want_heading = rand();
    srand(1234);
    q2_monster_drop_flush();
    check_eq_i(g_drop_calls, 0, "but spawns nothing when it is picked");
    check_eq_i(rand(), want_heading,
               "and draws no random number: 0x800207D0 jumps past 0x80020820");

    /*
     * THROUGH THE KILL PATH, not just monster_death_use: T_Damage's killed arm
     * (q2_monster_damage_reaction, 0x80062978 onward) reaches 0x800622E8 for a
     * creature that was not already dead.
     */
    q2_monster_drop_reset();
    g_drop_calls = 0;
    q2_monster_init(&m);
    m.pop_class_id = 34;                 /* Insane -> 9 Health */
    m.spawnflags   = Q2_SPAWNFLAG_DROP_ITEM;
    m.svflags      = Q2_SVF_MONSTER;
    m.aiflags      = Q2_AI_GOOD_GUY;
    m.health       = -5;
    q2_monster_damage_reaction(&m, NULL, 20);
    check_eq_i(q2_monster_drop_pending(), 1,
               "a flagged creature killed through T_Damage records its drop");
    q2_monster_damage_reaction(&m, NULL, 20);
    check_eq_i(q2_monster_drop_pending(), 1,
               "and a second hit on the body records nothing more");
    q2_monster_drop_flush();
    check_eq_i(g_drop_item, 9, "an Insane leaves Health");

    /*
     * FIVE DEATHS IN ONE FRAME PRODUCE FOUR ITEMS. 0x80020D74 and 0x80020DA4
     * both branch to the `jr ra`, so a fifth is dropped on the floor with no
     * counter and no retry. Reproduced rather than papered over.
     */
    q2_monster_drop_reset();
    g_drop_calls = 0;
    for (i = 0; i < 5; i++) {
        q2_monster_init(&m);
        m.pop_class_id = 12;             /* Infantry -> 23 Bullets */
        m.spawnflags   = Q2_SPAWNFLAG_DROP_ITEM;
        q2_monster_death_use(&m);
    }
    check_eq_i(q2_monster_drop_pending(), 4, "only four slots ever fill");
    q2_monster_drop_flush();
    check_eq_i(g_drop_calls, 4, "so five deaths in one frame leave four items");

    /*
     * THE PLAYER'S WEAPON REACHES THE PICK THROUGH THE QUEUE, not only through
     * the table call above. 0x80020680 — which the flush runs per slot, `jal`
     * at 0x80020E5C — loads the current weapon (0x8002069C `lh a1, 102(v0)`)
     * and the owned-weapons mask (0x800206A0 `lw a0, 104(v0)`) off 0x800C7C60
     * itself, and the Gunner arm at 0x80020730 reads both. So a flagged Gunner
     * dropped with a Grenade Launcher owned gives Grenades unless a bullet
     * weapon is in hand, and only the pair set here can tell the flush which.
     */
    q2_monster_drop_reset();
    g_drop_calls = 0;
    g_drop_item  = 0;
    q2_monster_set_player_weapon(1, 0x040u);     /* Blaster in hand, GL owned */
    q2_monster_init(&m);
    m.pop_class_id = 10;                         /* Gunner */
    m.spawnflags   = Q2_SPAWNFLAG_DROP_ITEM;
    q2_monster_death_use(&m);
    q2_monster_drop_flush();
    check(g_drop_calls == 1 && g_drop_item == 25,
          "a flagged Gunner leaves 25 Grenades when the flush sees a GL owned "
          "and no bullet weapon in hand — the bits reach 0x80020730");

    g_drop_calls = 0;
    g_drop_item  = 0;
    q2_monster_set_player_weapon(5, 0x040u);     /* Chaingun in hand, GL owned */
    q2_monster_init(&m);
    m.pop_class_id = 10;
    m.spawnflags   = Q2_SPAWNFLAG_DROP_ITEM;
    q2_monster_death_use(&m);
    q2_monster_drop_flush();
    check(g_drop_calls == 1 && g_drop_item == 23,
          "and 23 Bullets with the same GL owned but the Chaingun in hand — "
          "the weapon id reaches `(u32)(id - 4) < 2` at 0x80020740");

    q2_monster_set_player_weapon(1, 0);
    q2_monster_set_drop_hook(NULL, NULL);
    q2_monster_drop_reset();
}

/* ------------------------------------------------------------------------- */
static void test_time_base(void)
{
    printf("AI clock\n");

    /* The AI clock is 10 Hz, distinct from the 25 Hz simulation tick. The
     * drowning timer is what establishes it: 120 units for twelve seconds. */
    check_eq_i(Q2_AI_HZ, 10, "ten AI ticks per second");
    check_eq_i(Q2_AI_SECONDS(12), 120, "twelve seconds is 120 ticks");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC monster framework tests\n\n");

    test_frame_stride();
    test_move_record();
    test_frame_distance();
    test_infront();
    test_damage();
    test_react_to_damage();
    test_damage_reaction();
    test_corpse();
    test_corpse_dissolve();
    test_start_go_kinds();
    test_start_frame_pick();
    test_death_drop_table();
    test_death_drop_queue();
    test_time_base();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
