/*
 * test_monster.c — the creature framework's arithmetic.
 *
 * The interesting things to pin here are the ones that are easy to get subtly
 * wrong and hard to notice in play: the unpadded frame stride, the distance
 * scaling, and the width of the forward cone.
 */
#include <stdio.h>
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
    test_time_base();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
