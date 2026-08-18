/*
 * cre_soldier.c — the Soldier, transcribed from its own module.
 *
 * The Soldier ships on seven of the disc's thirteen AI maps — BASE0 through
 * BASE3, JAIL5, MAGDEMO and WASTE1 — which makes it most of the monsters in
 * the game. Its module is `BASE1 CreAIBin` module 0, 13,164 bytes relocated,
 * and every address below is inside it at a base of 0x80100000.
 *
 * ---------------------------------------------------------------------------
 * It is PC Quake II's soldier, line for line
 * ---------------------------------------------------------------------------
 * Every function decoded lands on id's own source, and several of them do so
 * on constants that were not tuned to produce the match:
 *
 *   soldier_stand         module+0x19A0   `random() < 0.8`  as 26214/32767
 *   soldier_idle          module+0x1A0C   `random() > 0.8`  as 26215/32767
 *   soldier_walk1_random  module+0x1AA0   `random() > 0.1`  as  3278/32767
 *   soldier_fire          module+0x1120   three flash tables chosen by skinnum
 *
 * The spawn function writes exactly id's seven callbacks in id's order, with
 * `melee` explicitly zeroed because a soldier has none, and sets `mass = 100`.
 *
 * ---------------------------------------------------------------------------
 * The three soldiers are one creature with three skins
 * ---------------------------------------------------------------------------
 * `skinnum` (entity+0x3A) selects both the weapon and the variant, and the
 * class table's three Soldier rows carry the health that goes with each:
 *
 *     skinnum 0..1   blaster      class 19, health 20   (light guard)
 *     skinnum 2..3   shotgun      class 18, health 30   (shotgun guard)
 *     skinnum 4+     machinegun   class 20, health 40   (machinegun guard)
 *
 * Those are PC Quake II's own figures, creature for creature.
 *
 * ---------------------------------------------------------------------------
 * Frame numbers are the animation's identity
 * ---------------------------------------------------------------------------
 * Moves are looked up by their first frame rather than by position, so the
 * constants below are the disc's own frame numbering and can be checked
 * against `q2psx-inspect creatures` directly.
 */
#include "ai.h"
#include "crebind.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame                                               */
/* ------------------------------------------------------------------------- */
#define SOL_ATTACK1     0     /*   0..11   attak1  */
#define SOL_ATTACK2    12     /*  12..29   attak2  */
#define SOL_ATTACK4    39     /*  39..44   attak4  */
#define SOL_PAIN1      50     /*  50..54           */
#define SOL_PAIN2      55     /*  55..61           */
#define SOL_PAIN3      62     /*  62..79           */
#define SOL_PAIN4      80     /*  80..96           */
#define SOL_START_RUN  97     /*  97..98           */
#define SOL_RUN        99     /*  99..104          */
#define SOL_ATTACK6   109     /* 109..122  runs    */
#define SOL_STAND1    146     /* 146..175          */
#define SOL_STAND3    176     /* 176..214          */
#define SOL_WALK1     215     /* 215..247          */
#define SOL_WALK2     256     /* 256..265          */
#define SOL_DEATH1    272     /* 272..307          */
#define SOL_DEATH2    308     /* 308..342          */
/*
 * The module's four death moves are Death1, Death2, Death5 and Death6 — NOT
 * 1/2/4/6. Death3 (343..387) and Death4 (388..440) exist in the table and no
 * module code references either. The frames below were always right; the label
 * on the third was not, because the move-name reader was pairing every name
 * with the next record's frames (creature.c).
 */
#define SOL_DEATH5    441     /* 441..464          */
#define SOL_DEATH6    465     /* 465..474          */

/* Frames a think function jumps to by name. */
#define FRAME_attak102     1
#define FRAME_attak110     9
#define FRAME_attak204    15
#define FRAME_attak216    27
#define FRAME_runs03      111
#define FRAME_walk101     215

/* ------------------------------------------------------------------------- */
/*
 * Sounds — and the module names them itself.
 *
 * This used to be five ids numbered 0..4 with addresses recorded for the first
 * three and the last two simply following on. Two of them were wrong, and the
 * module says so: it carries **two parallel tables**, eleven resolved handles
 * at `module+0x32A0` on a 4-byte stride and eleven 12-byte NAME fields at
 * `module+0x1D0`, sharing one index. The handles are zero on disc because the
 * engine fills them at load; the names are on the disc and read straight out:
 *
 *     0  sol_idle1     4  sol_pain3     8  wep_machgf1b
 *     1  sol_sght1     5  sol_deth1     9  wep_shotgf1b
 *     2  sol_pain1     6  sol_deth2    10  msc_udeath
 *     3  sol_pain2     7  sol_deth3
 *
 * That confirms the three ids the enum already had — `SOL_SND_COCK` at
 * `+0x32C4` is index 9, `wep_shotgf1b`, which is a shotgun guard's own fire
 * sound — and corrects the two it had guessed: pain is index 2 and death is
 * index 5, not 3 and 4, which were `sol_pain2` and `sol_pain3`.
 *
 * `q2_cre_soldier_sound_name` hands the host the name so it can be matched in
 * the map's bank, where the same eleven exist.
 */
typedef enum sol_sound {
    SOL_SND_IDLE  = 0,      /* module+0x32A0  sol_idle1    */
    SOL_SND_SIGHT = 1,      /* module+0x32A4  sol_sght1    */
    SOL_SND_PAIN  = 2,      /* module+0x32A8  sol_pain1    */
    SOL_SND_DEATH = 5,      /* module+0x32B4  sol_deth1    */
    SOL_SND_COCK  = 9,      /* module+0x32C4  wep_shotgf1b */
    /* The eleventh handle, and the only thing a Soldier ever uses it for is
     * being blown apart -- see the gib arm in soldier_die. */
    SOL_SND_UDEATH = 10,    /* module+0x32C8  msc_udeath   */
    SOL_SND_COUNT = 11
} sol_sound_id;

/* The eleven names, in the module's own order. */
static const char *const k_sol_sound_names[SOL_SND_COUNT] = {
    "sol_idle1", "sol_sght1", "sol_pain1", "sol_pain2", "sol_pain3",
    "sol_deth1", "sol_deth2", "sol_deth3",
    "wep_machgf1b", "wep_shotgf1b", "msc_udeath"
};

const char *q2_cre_soldier_sound_name(int which)
{
    if (which < 0 || which >= SOL_SND_COUNT)
        return NULL;
    return k_sol_sound_names[which];
}

void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
void  *q2_cre_sound_user;
void (*q2_cre_melee_fn)(q2_monster *m, const s32 aim[3],
                        s32 damage, s32 kick, void *user);
void  *q2_cre_melee_user;

#define g_sound      q2_cre_sound_fn
#define g_sound_user q2_cre_sound_user

void q2_cre_set_sound_hook(void (*fn)(q2_monster *, int, void *), void *user)
{
    g_sound      = fn;
    g_sound_user = user;
}

void q2_cre_set_melee_hook(void (*fn)(q2_monster *, const s32 *, s32, s32,
                                      void *), void *user)
{
    q2_cre_melee_fn   = fn;
    q2_cre_melee_user = user;
}

static void sol_play(q2_monster *m, sol_sound_id which)
{
    if (g_sound)
        g_sound(m, (int)which, g_sound_user);
}

/* ------------------------------------------------------------------------- */
/* random(), in the module's own fixed point                                  */
/* ------------------------------------------------------------------------- */
#include <stdlib.h>

static s32 sol_rand(void)
{
    return (s32)(rand() & 0x7FFF);
}

/* The module's literal thresholds, kept as the integers they are. */
#define SOL_R_0_8   26214       /* `random() < 0.8`  at module+0x19D8 */
#define SOL_R_0_8b  26215       /* `random() > 0.8`  at module+0x1A34 */
#define SOL_R_0_1    3278       /* `random() > 0.1`  at module+0x1ABC */
#define SOL_R_0_5   16383

/* ------------------------------------------------------------------------- */
/* Skill, which several think functions gate on                               */
/* ------------------------------------------------------------------------- */
static s32 g_skill = 1;

void q2_cre_set_skill(s32 skill) { g_skill = skill; }
s32  q2_cre_skill(void)          { return g_skill; }

/* ------------------------------------------------------------------------- */
/* The firing hook. The module reaches the engine's bullet and bolt spawners  */
/* through its import table; the port routes them through one callback so     */
/* combat stays in combat.c rather than leaking into every creature.          */
/* ------------------------------------------------------------------------- */
/* Shared with cre_actions.c, the same way the sound and melee hooks are: a
 * decoded creature reaches the engine's spawners through the same one callback
 * a transcribed one does. */
void (*q2_cre_fire_fn)(q2_monster *m, int flash_index, void *user);
void  *q2_cre_fire_user;

#define g_fire      q2_cre_fire_fn
#define g_fire_user q2_cre_fire_user

void q2_cre_set_fire_hook(void (*fn)(q2_monster *, int, void *), void *user)
{
    g_fire      = fn;
    g_fire_user = user;
}

/*
 * soldier_fire — module+0x1120.
 *
 * Three muzzle-flash tables, chosen by skinnum, indexed by the caller's flash
 * number. The tables are at module+0x3240, +0x3260 and +0x3280 and are what
 * decide whether the shot is a blaster bolt, a shotgun spread or a machinegun
 * burst; the flash index they yield is passed straight through.
 */
/* Which of the three flash tables this soldier's skin selects. */
static u8 soldier_skin(const q2_monster *m);

static void soldier_fire(q2_monster *self, int flash_number)
{
    u8 skin = soldier_skin(self);
    int table = (skin < 2) ? 0 : (skin < 4 ? 1 : 2);

    /*
     * The port hands the host a (weapon, flash) pair rather than reproducing
     * the muzzle-offset arithmetic here: which bullet spread a shotgun guard
     * throws is a combat question, and it already has an answer in combat.c.
     */
    if (g_fire)
        g_fire(self, table * 8 + flash_number, g_fire_user);
}

/* ------------------------------------------------------------------------- */
/* Think functions, by the index the animation frames use                     */
/* ------------------------------------------------------------------------- */

/* [2] soldier_idle — module+0x1A0C */
static void soldier_idle(q2_monster *self)
{
    if (sol_rand() >= SOL_R_0_8b)
        sol_play(self, SOL_SND_IDLE);
}

/* [3] soldier_cock — module+0x1A68 */
static void soldier_cock(q2_monster *self)
{
    sol_play(self, SOL_SND_COCK);
}

/*
 * [4] soldier_walk1_random — module+0x1AA0.
 *
 * Nine times out of ten it sends the animation back to the start of walk1, so
 * a patrolling soldier loops the long walk cycle and only occasionally falls
 * through into walk2. That one line is the whole difference between a soldier
 * that paces and one that marches.
 */
static void soldier_walk1_random(q2_monster *self)
{
    if (sol_rand() >= SOL_R_0_1)
        self->nextframe = FRAME_walk101;
}

/* [6] [9] [16] [17] [20] [21] — the six firing frames, module+0x1B3C onward */
static void soldier_fire1(q2_monster *s) { soldier_fire(s, 0); }
static void soldier_fire2(q2_monster *s) { soldier_fire(s, 1); }
static void soldier_fire4(q2_monster *s) { soldier_fire(s, 3); }
static void soldier_fire6(q2_monster *s) { soldier_fire(s, 5); }
static void soldier_fire7(q2_monster *s) { soldier_fire(s, 6); }
static void soldier_fire8(q2_monster *s) { soldier_fire(s, 7); }

/*
 * The variant, and it comes from the CLASS TABLE ROW.
 *
 * The module registers all three class bytes — 87, 89, 88 — against ONE shared
 * method table (0x801003C8, 0x801004F4, 0x80100510) and then dispatches inside
 * its spawn function on the class-table id instead: 0x80101604 reads
 * `lh v1, 0xD2(entity+0x24)` and branches on 19 / 18 / 20, writing
 *
 *     id 19 -> skinnum 0, health 20    (0x80102694, 0x80102698)   blaster
 *     id 18 -> skinnum 2, health 30    (0x801027C4, 0x801027CC)   shotgun
 *     id 20 -> skinnum 4, health 40    (0x801028F8, 0x801028FC)   machinegun
 *
 * and `q2psx-inspect classes` confirms rows 18/19/20 carry health 30/20/40.
 *
 * This used to reconstruct the variant from `class_byte` (87 -> 0, 88 -> 2,
 * else -> 4), which the module never does — and because the loader maps a row
 * to a class byte by ORDINAL, all three came out permuted: the 30-hp shotgun
 * guard fired a blaster, the 20-hp light guard fired a machinegun and the 40-hp
 * machinegun guard fired a shotgun. It also mis-steered soldier_attack and both
 * refire pairs, which gate on `skinnum < 4` and `skinnum < 2`.
 */
static u8 soldier_skin(const q2_monster *m)
{
    u8 base;

    switch (m->pop_class_id) {
    case 19: base = 0; break;   /* blaster    */
    case 18: base = 2; break;   /* shotgun    */
    case 20: base = 4; break;   /* machinegun */
    default:
        /* A Soldier placed from a row this build does not know: the module's
         * own fall-through is the last arm. */
        base = 4;
        break;
    }

    /* And the wounded half of the pair once the pain handler has set the low
     * bit — see q2_monster.hurt. */
    return m->hurt ? (u8)(base | 1u) : base;
}

/*
 * [7] soldier_attack1_refire1 — module+0x1B5C.
 * [8] soldier_attack1_refire2 — module+0x1C18.
 *
 * The two halves of one rule, split by weapon: a blaster soldier re-aims and
 * fires again, a shotgun or machinegun soldier only does so at the highest
 * skill or in melee range. `skinnum < 2` is the blaster.
 */
static void soldier_attack1_refire1(q2_monster *self)
{
    if (soldier_skin(self) > 1)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak102;
    else
        self->nextframe = FRAME_attak110;
}

static void soldier_attack1_refire2(q2_monster *self)
{
    if (soldier_skin(self) < 2)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak102;
}

/*
 * [10] soldier_attack2_refire1 — module+0x1CF0.
 * [11] soldier_attack2_refire2 — module+0x1DAC.
 *
 * The same split for the second attack sequence.
 */
static void soldier_attack2_refire1(q2_monster *self)
{
    if (soldier_skin(self) > 1)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak204;
    else
        self->nextframe = FRAME_attak216;
}

static void soldier_attack2_refire2(q2_monster *self)
{
    if (soldier_skin(self) < 2)
        return;
    if (!self->enemy || self->enemy->health <= 0)
        return;

    if ((g_skill == 3 && sol_rand() < SOL_R_0_5)
        || q2_range(self, self->enemy) == Q2_RANGE_MELEE)
        self->nextframe = FRAME_attak204;
}

/*
 * [18] soldier_attack6_refire — module+0x1FDC.
 *
 * The running shot. It keeps firing only at the top skill and only while the
 * target is still far enough away to be worth shooting at on the move.
 */
static void soldier_attack6_refire(q2_monster *self)
{
    if (!self->enemy || self->enemy->health <= 0)
        return;
    if (q2_range(self, self->enemy) < Q2_RANGE_MID)
        return;
    if (g_skill == 3)
        self->nextframe = FRAME_runs03;
}

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/* soldier_stand — module+0x19A0 */
static void soldier_stand(q2_monster *self)
{
    const q2_mmove *cur = self->currentmove;
    const q2_mmove *s3  = q2_cre_find_move(self, SOL_STAND3);

    if ((s3 && cur == s3) || sol_rand() < SOL_R_0_8)
        q2_cre_set_move(self, SOL_STAND1);
    else
        q2_cre_set_move(self, SOL_STAND3);
}

/* soldier_walk — module+0x2180 */
static void soldier_walk(q2_monster *self)
{
    if (sol_rand() < SOL_R_0_5)
        q2_cre_set_move(self, SOL_WALK1);
    else
        q2_cre_set_move(self, SOL_WALK2);
}

/*
 * soldier_run — module+0x1ADC.
 *
 * A soldier told to stand its ground stands rather than running, and one that
 * was walking breaks into the run only after a two-frame start; anything else
 * plays that start first. That is what stops it snapping from a walk cycle
 * straight into a sprint.
 */
static void soldier_run(q2_monster *self)
{
    const q2_mmove *cur = self->currentmove;

    if (self->aiflags & Q2_AI_STAND_GROUND) {
        q2_cre_set_move(self, SOL_STAND1);
        return;
    }

    if (cur == q2_cre_find_move(self, SOL_WALK1)
        || cur == q2_cre_find_move(self, SOL_WALK2)
        || cur == q2_cre_find_move(self, SOL_START_RUN))
        q2_cre_set_move(self, SOL_RUN);
    else
        q2_cre_set_move(self, SOL_START_RUN);
}

/* soldier_attack — module+0x21CC */
static void soldier_attack(q2_monster *self)
{
    if (soldier_skin(self) < 4) {
        if (sol_rand() < SOL_R_0_5)
            q2_cre_set_move(self, SOL_ATTACK1);
        else
            q2_cre_set_move(self, SOL_ATTACK2);
    } else {
        q2_cre_set_move(self, SOL_ATTACK4);
    }
}

/*
 * soldier_sight — module+0x2240.
 *
 * Plays the sight sound, and above the lowest skill a soldier that spots you
 * from a distance opens with the running shot rather than closing first.
 */
static void soldier_sight(q2_monster *self, q2_monster *other)
{
    (void)other;

    sol_play(self, SOL_SND_SIGHT);

    if (g_skill > 0 && self->enemy
        && q2_range(self, self->enemy) >= Q2_RANGE_MID) {
        if (sol_rand() > SOL_R_0_5)
            q2_cre_set_move(self, SOL_ATTACK6);
    }
}

/* soldier_pain — module+0xFA8 */
static void soldier_pain(q2_monster *self)
{
    s32 r;

    /*
     * BLOODIED AT HALF HEALTH, and this is `skinnum |= 1` — not a state
     * change. The port wrote `attack_state = Q2_AS_STRAIGHT` here, which is a
     * different field entirely: it told the AI to charge straight at its enemy
     * every time it was hurt, and left the skin clean for the rest of the
     * creature's life. Soldiers that should have been backing off and turning
     * bloody instead walked at you unmarked.
     */
    if (self->health < self->max_health / 2)
        self->hurt = true;

    /*
     * THE DEBOUNCE, which the port did not have at all.
     *
     * Three seconds on the class's own clock, 30 of the 10 Hz AI ticks. Every
     * hit re-entered the flinch from its first frame without it, so a soldier
     * held under automatic fire never advanced past pose one and never
     * returned to running — it stood still being shot, which is most of "the
     * soldier AI is broken".
     *
     * A hit taken WHILE the debounce is running is not simply ignored: one
     * that throws the body upward hard enough promotes an ordinary flinch to
     * PAIN4, the off-its-feet one. +Y is down here, so "upward" is a large
     * NEGATIVE vertical velocity.
     */
    if (q2_level_state.time < self->pain_debounce) {
        if (self->velocity[1] < -1200 && self->currentmove &&
            (self->currentmove->first_frame == SOL_PAIN1 ||
             self->currentmove->first_frame == SOL_PAIN2 ||
             self->currentmove->first_frame == SOL_PAIN3))
            q2_cre_set_move(self, SOL_PAIN4);
        return;
    }

    self->pain_debounce = q2_level_state.time + 30;

    sol_play(self, SOL_SND_PAIN);

    /*
     * NO FLINCH ON THE HARDEST SKILL. The sound still plays and the skin still
     * turns; the animation is what is skipped, which is what makes skill 3
     * soldiers keep firing through damage instead of being staggered out of
     * every attack.
     */
    if (g_skill == 3)
        return;

    /*
     * A THREE-WAY SPLIT, not four. PAIN4 is reachable only through the
     * promotion above — it is the knocked-off-its-feet animation and a plain
     * hit never selects it. Splitting four ways gave it to a quarter of all
     * hits, so soldiers were constantly being thrown about by rifle fire.
     */
    r = sol_rand();
    if (r < 10813)
        q2_cre_set_move(self, SOL_PAIN1);
    else if (r < 21626)
        q2_cre_set_move(self, SOL_PAIN2);
    else
        q2_cre_set_move(self, SOL_PAIN3);
}

/* soldier_die — module+0x22F8 */
static void soldier_die(q2_monster *self)
{
    s32 r;

    /*
     * THE GIB ARM, which this had none of (0x80102310).
     *
     * Damage past `gib_health` destroys the body outright: it plays the
     * shared `msc_udeath` — the eleventh of the module's own sound handles,
     * and the only reason a Soldier registers a sound it otherwise never uses
     * — and there is no death animation at all, because there is nothing left
     * to animate. Without this arm a soldier hit by a rocket lay down and
     * played a normal death like one shot with a pistol.
     *
     * Tested before the `dead` guard below, so a body already dead can still
     * be blown apart by a later explosion, which is the order the class uses.
     */
    if (self->health <= self->gib_health) {
        if (!self->gibbed) {
            self->gibbed = true;
            self->dead   = true;
            sol_play(self, SOL_SND_UDEATH);
        }
        return;
    }

    if (self->dead)
        return;

    self->dead = true;
    /* The wounded skin, which a death sets whether or not pain ever did. */
    self->hurt = true;
    sol_play(self, SOL_SND_DEATH);

    r = sol_rand();
    if (r < 8192)
        q2_cre_set_move(self, SOL_DEATH1);
    else if (r < 16384)
        q2_cre_set_move(self, SOL_DEATH2);
    else if (r < 24576)
        q2_cre_set_move(self, SOL_DEATH5);
    else
        q2_cre_set_move(self, SOL_DEATH6);
}

/* ------------------------------------------------------------------------- */
const q2_cre_impl q2_cre_soldier = {
    "Soldier",
    {
        soldier_stand,      /*  0 stand       */
        NULL,               /*  1 idle        — the module installs none */
        NULL,               /*  2 search      — nor one of these         */
        soldier_walk,       /*  3 walk        */
        soldier_run,        /*  4 run         */
        NULL,               /*  5 dodge       */
        soldier_attack,     /*  6 attack      */
        NULL,               /*  7 melee       — a soldier has none       */
        (q2_class_method)(void *)soldier_sight,  /* 8 sight */
        NULL,               /*  9 checkattack */
        NULL,               /* 10 bigturn     */
        soldier_pain,       /* 11 pain        */
        soldier_die         /* 12 die         */
    },
    {
        NULL,                       /*  0 — never referenced by a frame */
        soldier_stand,              /*  1 */
        soldier_idle,               /*  2 */
        soldier_cock,               /*  3 */
        soldier_walk1_random,       /*  4 */
        soldier_run,                /*  5 */
        soldier_fire1,              /*  6 */
        soldier_attack1_refire1,    /*  7 */
        soldier_attack1_refire2,    /*  8 */
        soldier_fire2,              /*  9 */
        soldier_attack2_refire1,    /* 10 */
        soldier_attack2_refire2,    /* 11 */
        NULL, NULL, NULL, NULL,     /* 12..15 — present but unreferenced */
        soldier_fire4,              /* 16 */
        soldier_fire8,              /* 17 */
        soldier_attack6_refire,     /* 18 */
        NULL,                       /* 19 */
        soldier_fire6,              /* 20 */
        soldier_fire7,              /* 21 */
        NULL,                       /* 22 */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
    },
    NULL
};
