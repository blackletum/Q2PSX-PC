/*
 * cre_tankcomm.c — the Tank Commander, transcribed from its own module.
 *
 * The Tank Commander ships on two of the disc's thirteen AI maps, COMMAND and
 * WASTE4. Its module is `COMMAND CreAIBin`, 8,684 bytes relocated, and every
 * address below is inside it at a base of 0x80100000. WASTE4's copy relocates
 * to the same addresses, so the module addresses in this file hold on both.
 *
 * That is the COMMON.DAT count, which is what "AI map" means everywhere else in
 * this port. The creature reaches ELEVEN more level directories through a
 * per-zone module instead — BIGGUN, BOSS1, JAIL2, JAIL3, JAIL4, JAIL5, LAB,
 * SECURITY, WASTE1, WASTE2 and WASTE3 each carry a copy inside a ZONE*.DAT.
 * Thirteen directories in total, which is the number the sound banks agree with
 * at the foot of this comment.
 *
 * ---------------------------------------------------------------------------
 * It is PC Quake II's `m_tank.c`, function for function
 * ---------------------------------------------------------------------------
 * This file used to carry ONE hand-written callback — the attack — with the
 * other seven and all sixteen methods on the generic handlers. Reading the rest
 * of the module turns out to be reading id's tank: eight monsterinfo callbacks
 * and sixteen methods, and every one of them lands on an id function, several
 * of them on constants that were not tuned to produce the match:
 *
 *     tank_run              module+0x14B0   AI_BRUTAL set from `enemy->client`
 *     tank_pain             module+0x1894   `damage <= 10` / `<= 30` / `<= 60`,
 *                                           `random() > 0.2` as 6554/32768,
 *                                           pain_debounce_time + 3 s as +30
 *     tank_die              module+0x1A18   gib arm, `deadflag == DEAD_DEAD`
 *                                           re-entry guard, one death move
 *     tank_dead             module+0x16C8   MOVETYPE_TOSS, SVF_DEADMONSTER,
 *                                           nextthink = 0
 *     tank_refire_rocket    module+0x1320   skill >= 2, alive, visible,
 *                                           `random() <= 0.4` as 13107/32768
 *     tank_reattack_blaster module+0x13E8   ...and `<= 0.6` as 19661/32768
 *     TankRocket            module+0xDE0    damage 50, speed 550
 *     TankMachineGun        module+0xF90    damage 20, kick 4, spread 300/500
 *
 * The two range bands the attack callback splits on are id's `125` and `250`
 * at the AI's world scale of twelve: 1500 and 3000, compiled as `< 1501` and
 * `< 3001`. And `pain_debounce_time = level.time + 5.0` in id's own
 * `tank_attack` is the `+ 50` this file previously recorded as an unidentified
 * store to `entity+0xA8` — the pain handler names that field, because it is
 * where `tank_pain` writes `level.time + 30` for its three seconds.
 *
 * ---------------------------------------------------------------------------
 * Frame numbering, checked against id's
 * ---------------------------------------------------------------------------
 * The console renumbered the animation but did not re-cut it. Every one of the
 * seventeen moves has exactly id's LENGTH, which is what says the ranges below
 * are the same animations and not merely plausible ones:
 *
 *     0-29    30 frames   stand01..stand30       tank_move_stand
 *     30-33    4 frames   walk01..walk04         tank_move_start_run
 *     34-49   16 frames   walk05..walk20         tank_move_walk / _run
 *     50-54    5 frames   walk21..walk25         tank_move_stop_walk
 *     55-70   16 frames   attak101..attak116     tank_move_attack_blast
 *     65-70    6 frames   attak111..attak116     tank_move_reattack_blast
 *     71-76    6 frames   attak117..attak122     tank_move_attack_post_blast
 *     77-114  38 frames   attak201..attak238     tank_move_attack_strike
 *     115-135 21 frames   attak301..attak321     tank_move_attack_pre_rocket
 *     136-144  9 frames   attak322..attak330     tank_move_attack_fire_rocket
 *     145-167 23 frames   attak331..attak353     tank_move_attack_post_rocket
 *     168-196 29 frames   attak401..attak429     tank_move_attack_chain
 *     197-200  4 frames   pain101..pain104       tank_move_pain1
 *     201-205  5 frames   pain201..pain205       tank_move_pain2
 *     206-221 16 frames   pain301..pain316       tank_move_pain3
 *     222-253 32 frames   death101..death132     tank_move_death
 *
 * The per-frame think placements agree too. The blaster fires on 64, 67 and 70
 * — offsets 9, 12 and 15 into 55-70, which is id's attak110, attak113 and
 * attak116. The rocket fires on 138, 141 and 144, id's attak324, attak327 and
 * attak330. The chaingun runs 173..191, nineteen frames, id's attak406..attak424.
 *
 * ---------------------------------------------------------------------------
 * TWO MOVES SHARE A FIRST FRAME, which the port's move lookup cannot express
 * ---------------------------------------------------------------------------
 * `tank_move_walk` (module+0x1BC0) and `tank_move_run` (module+0x1C3C) both
 * cover frames 34-49. They are different records with different per-frame AI
 * verbs — the walk's sixteen frames are `ai_walk`, the run's are `ai_run` —
 * and the console tells them apart by pointer.
 *
 * `q2_cre_set_move(m, 34)` cannot: `q2_cre_find_move` returns the first record
 * with that first frame, which is the walk, so a Tank Commander told to RUN
 * would have been given the walk record and driven by `ai_walk`, which does not
 * chase. So those two — and only those two — are installed by their MODULE
 * ADDRESS through `q2_cre_set_move_at` (crebind.h), which is the same lookup
 * keyed on the field that is actually unique. Every other move here uses the
 * house convention and its first frame.
 *
 * This file used to carry a private copy of that lookup, because it was the
 * second creature to need one — the Arachner's walk and run share frames 16-24
 * the same way. It is shared now and the copy here is gone.
 *
 * ---------------------------------------------------------------------------
 * What this transcription still owes, named rather than hidden
 * ---------------------------------------------------------------------------
 *  - THE MUZZLE POSITION AND THE BLASTER'S EFFECT ARE NOT PASSED. All three
 *    weapons' FIGURES now land — `q2_cre_shot` carries damage, speed, kick and
 *    spread, and `tankcomm_blaster`, `tankcomm_rocket` and
 *    `tankcomm_machinegun` fill it from the module's own arguments — but two
 *    things the module also computes have no field on that struct. The first is
 *    `start`: the module builds a world-space launch point from a model
 *    attachment (and the rocket LERPS three of them, one per firing frame), and
 *    the hook leaves the muzzle to the host. The second is the blaster's
 *    seventh argument, `8` = id's EF_BLASTER, which selects the bolt's trail.
 *    Both are recorded at their think functions and neither is invented.
 *
 *  - THE CHAINGUN NO LONGER FIRES WITHOUT A TARGET. `TankMachineGun` is the one
 *    fire think on this creature that tolerates `enemy == NULL` — the module
 *    flattens the pitch and shoots anyway — and the shared `q2_cre_fire_shot`
 *    guards `enemy` for every creature. The last frames of a burst whose target
 *    died are dropped rather than sprayed. Counted in `fire_no_enemy`.
 *
 *  - AND NONE OF THE THREE STOPS FOR A DEAD ONE. `q2_cre_fire_shot` also
 *    declines when `enemy->health <= 0`, and no fire think in this module reads
 *    the enemy's health at all — module+0xC4C, +0xDE0 and +0xF90 load
 *    entity+0xBC for the aim and never touch obj+0x108. The health test belongs
 *    to the two refire callbacks (module+0x136C and +0x144C) and to
 *    `tank_attack`'s `health < 0`, not to the shot. So an enemy that dies
 *    part-way through a burst or a blaster volley takes the remaining shots on
 *    the console and none here. Counted in `fire_dead_enemy`.
 *
 *  - THE FOOTSTEP'S RUMBLE IS NOT ROUTED. `tank_footstep` calls import +0x12C,
 *    the player proximity effect, with effect 14 at magnitude 4096 over radii
 *    2048..8192. Nothing in the port receives that, so only the sound is
 *    played. Named here because "the footstep is transcribed" would otherwise
 *    imply the shake is too.
 *
 *  - THE IDLE HANDLE'S SUBSTITUTION IS NOT ROUTED EITHER. module+0x2100 is
 *    registered as `tnk_idle1` and then overwritten with the `tnk_step` handle
 *    because no bank on the disc carries `tnk_idle1` (see below). The port
 *    resolves the handle by its REGISTERED name, so `tankcomm_think_idle` and
 *    the death move's frame 249 are silent where the console thuds. The fix is
 *    in the resolver, not here.
 *
 *  - THE SECOND VARIANT CANNOT BE PLACED. The spawn function's last two
 *    instructions are `if (class_id == 92) skinnum = 2`, and 92 is a class byte
 *    nothing on this disc carries. The line is transcribed and will never run;
 *    see `tankcomm_spawn`.
 *
 * ---------------------------------------------------------------------------
 * ONE SAMPLE WAS CUT, and the module carries the substitution for it
 * ---------------------------------------------------------------------------
 * An earlier draft of this header said all of the `tnk_` names were missing
 * from the disc and that the module's fallback therefore did nothing. Both
 * halves are wrong, and the banks say so. The module registers EIGHT names
 * (six of them `tnk_`) and the two maps that carry it in `COMMON.DAT` carry
 * SEVEN of them in `SNDVRAM.DAT`:
 *
 *     tnk_pain  tnk_death  tnk_step  tnk_sight1  tnk_atck1  pt1__strt
 *     msc_udeath                                  ... all seven are present
 *     tnk_idle1                                   ... is in NO bank at all
 *
 * `tnk_idle1` is absent from every one of the disc's SNDVRAM banks, and
 * `tnk_step` is in thirteen of them — exactly the thirteen level directories
 * that carry a copy of this module, two in COMMON.DAT and eleven in a ZONE*.DAT
 * (see the top of this file; this is NOT the thirteen-AI-map count, which is a
 * different thirteen). So `sound_index_by_name` (import +0x24) resolves seven
 * names and fails exactly one, and the module's fallback exists for that one:
 * module+0x808 tests the idle handle for zero and module+0x820 copies the STEP
 * handle over it. The fallback FIRES on every copy of the disc, and the step
 * handle is a real index, so on the console both readers of module+0x2100 —
 * think 14 and the death move's frame 249 — play `tnk_step`.
 *
 * THIS PORT DOES NOT REPRODUCE THAT SUBSTITUTION, and cannot from inside this
 * file. `client_cre_sound` resolves a handle ADDRESS against the module's own
 * registrations, so module+0x2100 comes back as the name it was REGISTERED
 * with, `tnk_idle1`, which no bank carries; it is counted as missing and
 * nothing is played. Reproducing the console needs the handle→name resolver to
 * apply the module's own post-registration fixups, which lives in creature.c.
 * Listed with the other things this transcription owes, above.
 *
 * The second oddity is a genuine hole in the ORIGINAL. The handle table is nine
 * words at module+0x2100..+0x2120 and only EIGHT are ever written;
 * module+0x2108 is never stored to by any instruction in the image — its one
 * and only reference in the whole 8,684 bytes is the load at module+0x186C. The
 * monsterinfo `idle` callback (module+0x185C) plays exactly that slot. So the
 * Tank Commander's idle CALLBACK plays sound handle zero, always, on every copy
 * of the disc. It is transcribed as it stands.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "creature.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame — and two of them by module address           */
/* ------------------------------------------------------------------------- */
#define TANK_STAND            0   /*   0..29   module+0x1B64  tank_move_stand */
/* 30..33, module+0x1BFC. Defined for the frame map above and NOT used: the run
 * callback compares all three of its moves by pointer, so start_run is reached
 * through TANK_ADDR_START_RUN below, with walk and run, rather than by frame. */
#define TANK_START_RUN       30
/* 50..54, module+0x1BE0. Defined and NEVER INSTALLED, which is the disc's own
 * state rather than a gap here: it is a real record whose endfunc is the stand
 * callback, and no callback and no method in this module writes it to
 * currentmove. The census marks it via -2 for exactly that reason. Whether id's
 * own tank_move_stop_walk has a caller was not checked and is not claimed. */
#define TANK_STOP_WALK       50
#define TANK_ATTACK_BLAST    55   /*  55..70   module+0x1D18                  */
#define TANK_REATTACK_BLAST  65   /*  65..70   module+0x1D3C                  */
#define TANK_POST_BLAST      71   /*  71..76   module+0x1D60                  */
#define TANK_STRIKE          77   /*  77..114  module+0x1DE4                  */
#define TANK_PRE_ROCKET     115   /* 115..135  module+0x1E34                  */
#define TANK_FIRE_ROCKET    136   /* 136..144  module+0x1E60                  */
#define TANK_POST_ROCKET    145   /* 145..167  module+0x1EB8                  */
#define TANK_ATTACK_CHAIN   168   /* 168..196  module+0x1F20                  */
#define TANK_PAIN1          197   /* 197..200  module+0x1C78                  */
#define TANK_PAIN2          201   /* 201..205  module+0x1C98                  */
#define TANK_PAIN3          206   /* 206..221  module+0x1CD8                  */
#define TANK_DEATH          222   /* 222..253  module+0x1F90                  */

/*
 * The walk/run pair, which share frames 34-49 and therefore cannot be told
 * apart by first frame. See the header.
 */
#define TANK_ADDR_WALK       0x80101BC0u  /* 34..49, ai_walk on all sixteen */
#define TANK_ADDR_RUN        0x80101C3Cu  /* 34..49, ai_run  on all sixteen */
/* start_run's frames ARE unique, but the run callback compares all three
 * against `currentmove` by pointer, so all three are looked up the same way. */
#define TANK_ADDR_START_RUN  0x80101BFCu  /* 30..33 */

/* ------------------------------------------------------------------------- */
/* Sounds, as the module addresses of their handles                           */
/* ------------------------------------------------------------------------- */
/*
 * Eight names at a 12-byte stride from module+0x1C4, resolved through import
 * +0x24 into nine words at module+0x2100. The registration order is the source
 * of this mapping — module+0x474, +0x50C, +0x5A8, +0x648, +0x6E0, +0x780,
 * +0x814 and +0x908 are the eight stores — and it SKIPS module+0x2108.
 */
#define TANK_SND_IDLE      0x80102100u  /* Registered as tnk_idle1, which NO
                                         * SNDVRAM bank on the disc carries, so
                                         * module+0x808 finds zero here and
                                         * module+0x820 copies the tnk_step
                                         * handle over it. On the console this
                                         * word holds the STEP sound.        */
#define TANK_SND_PAIN      0x80102104u  /* tnk_pain                          */
#define TANK_SND_MI_IDLE   0x80102108u  /* NOTHING EVER WRITES THIS. The idle
                                         * callback plays it anyway; see the
                                         * header and tankcomm_idle.         */
#define TANK_SND_DEATH     0x8010210Cu  /* tnk_death                         */
#define TANK_SND_STEP      0x80102110u  /* tnk_step                          */
#define TANK_SND_SIGHT     0x80102114u  /* tnk_sight1                        */
#define TANK_SND_WINDUP    0x80102118u  /* pt1__strt. Cloned at module+0x82C
                                         * through import +0x3C and given
                                         * volume 60 through import +0x44, so
                                         * the handle here is the CLONE's.    */
#define TANK_SND_STRIKE    0x8010211Cu  /* tnk_atck1                         */
/*
 * module+0x2120 is `msc_udeath`. It is registered and no instruction in the
 * module reads it — the gib arm of tank_die plays nothing — so it is not
 * defined here. id's tank_die does play misc/udeath.wav on the gib; the console
 * dropped that call, and a constant nothing uses would read as an oversight in
 * this file rather than in the module.
 */

/* ------------------------------------------------------------------------- */
/* The sound hook, shared with cre_soldier.c (which defines it)               */
/* ------------------------------------------------------------------------- */
extern void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
extern void  *q2_cre_sound_user;

static void tank_play(q2_monster *m, u32 handle_addr)
{
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle_addr, q2_cre_sound_user);
}

/*
 * THE THREE WEAPONS, as the arguments their own module passes.
 *
 * Every figure here is read at the call site — see the three think functions
 * for the disassembly — and `q2_cre_fire_shot` carries them to the host, which
 * no longer has to guess a gun from an import slot. A field the weapon does not
 * use stays zero, which is what the struct's comment asks for; `count` is 1
 * because none of these three is a shotgun.
 *
 * `flash` is 0 on all three, and that is the module's own argument rather than
 * a placeholder — module+0xDB4, +0xF74 and +0x11A0 all store `zero`. This
 * module does not number its muzzle flashes at all; it carries MODEL POINTS
 * instead (module+0x2130 for the blaster, +0x2128 for the chaingun, and the
 * three lerped +0x2140/+0x2148/+0x2150 for the rocket) and hands the spawner a
 * world-space `start` built from them.
 */
static const q2_cre_shot tank_shot_blaster = {
    Q2_IMP_FIRE_BLASTER, 0, 16, 800, 0, 0, 0, 1
};
static const q2_cre_shot tank_shot_rocket = {
    Q2_IMP_FIRE_ROCKET, 0, 50, 550, 0, 0, 0, 1
};
static const q2_cre_shot tank_shot_bullet = {
    Q2_IMP_FIRE_BULLET, 0, 20, 0, 4, 300, 500, 1
};

/* import +0x14, 0..32767. Every threshold below is a fraction of 32768. */
static s32 tank_rand(void)
{
    return (s32)(rand() & 0x7FFF);
}

#define TANK_R_0_2    6554      /* `random() > 0.2`   module+0x1930 */
#define TANK_R_0_33  10813      /* `random() < 0.33`  module+0x12B8 */
#define TANK_R_0_4   13106      /* `random() < 0.4`   module+0x1288 */
#define TANK_R_0_4b  13107      /* `random() <= 0.4`  module+0x13A4 */
#define TANK_R_0_5   16384      /* `random() < 0.5`   module+0x12A4 */
#define TANK_R_0_6   19661      /* `random() <= 0.6`  module+0x146C */
#define TANK_R_0_66  21626      /* `random() < 0.66`  module+0x12CC */

/*
 * The attack callback's two range bands, SQUARED.
 *
 * The module takes a real length — import +0xB8, `VectorLength` — and compares
 * it against 1501 and 3001. This compares the squared distance against those
 * squared. The bands are the module's own numbers; only the comparison is
 * rearranged, and the rearrangement is exact ONLY UNDER THIS PORT'S MODEL OF
 * THE SQUARE ROOT, which is worth saying out loud:
 *
 *   - `ai.c` transcribes 0x8005C4E8 with an exact integer `isqrt64`, and under
 *     that model `floor(sqrt(d2)) < k` and `d2 < k*k` are the same test, so
 *     this file and `q2_vector_length` order identically at every distance.
 *
 *   - the CONSOLE's is not exact. 0x8005C4E8 hands the sum of squares to
 *     0x8008A7E8, which normalises by leading-zero count, truncates to roughly
 *     eight significant bits, indexes a table and shifts. It is monotone but it
 *     under-reports by up to a few parts in a thousand, so the console's band
 *     edge sits a handful of units OUTSIDE 1500 rather than exactly on it. The
 *     ordering is the same; the boundary is not, to about 0.3%. Nothing here
 *     depends on that and it is not corrected, because correcting it would mean
 *     modelling the GTE table rather than the module.
 *
 * 1500 and 3000 are id's `125` and `250` at the AI's world scale of twelve.
 */
#define TANK_NEAR_SQ  (1501 * 1501)
#define TANK_MID_SQ   (3001 * 3001)

/* Three seconds of pain debounce, on the 10 Hz AI clock. module+0x1994. */
#define TANK_PAIN_DEBOUNCE   30
/* Five seconds of it, which the long-range rocket opening buys itself so the
 * twenty-one frame wind-up cannot be interrupted. module+0x12F0, and id's own
 * `self->pain_debounce_time = level.time + 5.0`. */
#define TANK_ROCKET_NO_PAIN  50

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/*
 * tank_stand — module+0x15B4. Three instructions: `currentmove = module+0x1B64`.
 * id's `tank_stand` is the same one line.
 */
static void tankcomm_stand(q2_monster *self)
{
    q2_cre_set_move(self, TANK_STAND);
}

/*
 * tank_idle — module+0x185C, the monsterinfo slot at entity+0xE4.
 *
 *     play(module+0x2108, self->[0x24]);
 *
 * and module+0x2108 is the handle nothing registers. This plays it as written.
 * The sound hook resolves a handle address against the module's own
 * registrations, finds none for this one, and stays silent — which is what the
 * console does with a zero handle. See the header.
 *
 * NOT the same function as think 14, which is id's `tank_idle` proper and plays
 * the real `tnk_idle1` at module+0x2100. The module has both.
 */
static void tankcomm_idle(q2_monster *self)
{
    tank_play(self, TANK_SND_MI_IDLE);
}

/*
 * tank_walk — module+0x14A0. `currentmove = module+0x1BC0`, id's one line.
 *
 * Addressed by module address rather than by frame 34: the run move covers the
 * same frames and would win the lookup. See the header.
 */
static void tankcomm_walk(q2_monster *self)
{
    q2_cre_set_move_at(self, TANK_ADDR_WALK);
}

/*
 * tank_run — module+0x14B0, and it is id's `tank_run` line for line:
 *
 *     if (self->enemy && self->enemy->client)
 *         self->aiflags |=  AI_BRUTAL;      ; 0x200, entity+0xDC
 *     else
 *         self->aiflags &= ~AI_BRUTAL;
 *
 *     if (self->aiflags & AI_STAND_GROUND) { currentmove = stand; return; }
 *
 *     if (currentmove == walk || currentmove == start_run) currentmove = run;
 *     else                                                 currentmove = start_run;
 *
 * The `client` test is `lw v0, 60(v0)` on the enemy — entity+0x3C, which
 * monster.h records as non-NULL for a player. So a Tank Commander is BRUTAL
 * against the player and not against anything else, which is what id's flag is
 * for: `M_CheckAttack` reads it at 0x8005DDE8.
 */
static void tankcomm_run(q2_monster *self)
{
    const q2_mmove *cur;

    if (self->enemy && self->enemy->client)
        self->aiflags |= Q2_AI_BRUTAL;
    else
        self->aiflags &= ~(u32)Q2_AI_BRUTAL;

    if (self->aiflags & Q2_AI_STAND_GROUND) {
        q2_cre_set_move(self, TANK_STAND);
        return;
    }

    cur = self->currentmove;

    if (cur && (cur == q2_cre_find_move_at(self, TANK_ADDR_WALK) ||
                cur == q2_cre_find_move_at(self, TANK_ADDR_START_RUN)))
        q2_cre_set_move_at(self, TANK_ADDR_RUN);
    else
        q2_cre_set_move_at(self, TANK_ADDR_START_RUN);
}

/*
 * tank_attack — module+0x11BC, and it is id's `tank_attack`:
 *
 *     if (self->enemy->health < 0) {
 *         currentmove = attack_strike;      ; module+0x1DE4, 77-114
 *         aiflags &= ~AI_BRUTAL;
 *         return;
 *     }
 *     d = VectorLength(enemy->origin - self->origin);   ; import +0xB8
 *     r = rand();                                       ; import +0x14
 *
 *     if      (d < 1501)  move = (r < 13106) ? chain : blast;
 *     else if (d < 3001)  move = (r < 16384) ? chain : blast;
 *     else if (r < 10813) move = chain;
 *     else if (r < 21626) { move = pre_rocket;
 *                           self->[0xA8] = level.time + 50; }
 *     else                move = blast;
 *
 * `entity+0xA8` was recorded here as an unidentified timer. It is
 * `pain_debounce_time`: `tank_pain` at module+0x1984 reads it against
 * `level.time` and writes `level.time + 30` to it. So the long-range rocket
 * opening buys itself five seconds of flinch immunity, which is id's own
 * `self->pain_debounce_time = level.time + 5.0; // no pain for a while`.
 *
 * `health < 0`, not `<= 0`: a creature killed exactly to zero is still shot at,
 * which is the module's own boundary (`bgez` at module+0x11EC).
 */
static void tankcomm_attack(q2_monster *self)
{
    s64 d2;
    s32 roll;

    if (!self)
        return;

    /*
     * The module is only ever called with an enemy and dereferences it without
     * a test. Returning is the fallback that fails safely: the generic handler
     * would install the FIRST move this callback installs, which is the strike
     * at 77-114 — the after-the-kill animation, and the bug this file was
     * originally written to fix. Playing it for a creature with no enemy at all
     * would put that bug straight back.
     */
    if (!self->enemy)
        return;

    if (self->enemy->health < 0) {
        q2_cre_set_move(self, TANK_STRIKE);
        self->aiflags &= ~(u32)Q2_AI_BRUTAL;
        return;
    }

    d2   = q2_monster_dist_sq(self, self->enemy->pos);
    roll = tank_rand();

    if (d2 < TANK_NEAR_SQ) {
        q2_cre_set_move(self, roll < TANK_R_0_4 ? TANK_ATTACK_CHAIN
                                                : TANK_ATTACK_BLAST);
    } else if (d2 < TANK_MID_SQ) {
        q2_cre_set_move(self, roll < TANK_R_0_5 ? TANK_ATTACK_CHAIN
                                                : TANK_ATTACK_BLAST);
    } else if (roll < TANK_R_0_33) {
        q2_cre_set_move(self, TANK_ATTACK_CHAIN);
    } else if (roll < TANK_R_0_66) {
        q2_cre_set_move(self, TANK_PRE_ROCKET);
        self->pain_debounce = q2_level_state.time + TANK_ROCKET_NO_PAIN;
    } else {
        q2_cre_set_move(self, TANK_ATTACK_BLAST);
    }
}

/*
 * tank_sight — module+0x1824. One call: `play(module+0x2114, self->[0x24])`,
 * the `tnk_sight1` handle. id's `tank_sight` is the same one line.
 *
 * `other` is loaded into a1 at entry and immediately overwritten with the
 * object pointer, so the module genuinely ignores it.
 */
static void tankcomm_sight(q2_monster *self, q2_monster *other)
{
    (void)other;
    tank_play(self, TANK_SND_SIGHT);
}

/*
 * tank_pain — module+0x1894, entity+0xA0. id's `tank_pain` in id's order:
 *
 *     if (health < max_health / 2) skinnum |= 1;        ; module+0x18D0
 *     if (damage <= 10) return;                         ; module+0x18EC
 *     if (level.time < pain_debounce_time) return;      ; module+0x190C
 *     if (damage <= 30 && rand() >= 6554) return;       ; module+0x1930
 *     if (skill >= 2) {                                 ; module+0x1954
 *         if (frame >= 115 && frame <= 144) return;     ; the rocket wind-up
 *         if (frame >=  55 && frame <=  70) return;     ; the blaster burst
 *     }
 *     pain_debounce_time = level.time + 30;             ; module+0x1998
 *     play(module+0x2104);                              ; tnk_pain
 *     if (skill == 3) return;                           ; module+0x19C4
 *     if      (damage <= 30) currentmove = pain1;       ; 197-200
 *     else if (damage <= 60) currentmove = pain2;       ; 201-205
 *     else                   currentmove = pain3;       ; 206-221
 *
 * The two frame ranges are id's `attak301..attak330` and `attak101..attak116`
 * on this disc's numbering, and they are the whole of the rocket wind-up and
 * the whole of the blaster burst — a Tank Commander on hard or nightmare cannot
 * be staggered out of either. The module writes both as unsigned range tests
 * (`addiu` then `sltiu`, module+0x1968 and +0x1974); they are written as
 * two-sided comparisons here, which is the same test for every value `frame`
 * can hold.
 *
 * 6554/32768 is id's `random() > 0.2`; 30 ticks on the 10 Hz clock is its
 * three seconds.
 *
 * ---------------------------------------------------------------------------
 * `damage` IS THE ARGUMENT, and it is now passed
 * ---------------------------------------------------------------------------
 * This function used to be called with a sentinel, because `q2_monster.pain`
 * carried only the entity, and the three lines that want the figure had to be
 * approximated — the small-hit return skipped, the 0.2 roll applied to every
 * hit, and the move pinned to PAIN1. All three of those departures are gone.
 * `monster.h` now declares `pain(m, s16 damage)` and
 * `q2_monster_damage_reaction` passes T_Damage's own amount, so the three
 * comparisons below are the module's `slti` immediates read straight off:
 *
 *     module+0x18EC   slti v0, s3, 11   ->  damage <= 10
 *     module+0x1914   slti s2, s3, 31   ->  damage <= 30
 *     module+0x19DC   slti v0, s3, 61   ->  damage <= 60
 *
 * and `s3` is `addu s3, a3, zero` at module+0x18D8 — the fourth argument, which
 * is id's `pain (edict_t *self, edict_t *other, float kick, int damage)`.
 * module+0x1914 is a delay-slot instruction and `s2` survives to module+0x19CC,
 * which is why the SAME `damage <= 30` decides both the 0.2 roll and PAIN1.
 */
static void tankcomm_pain(q2_monster *self, s16 damage)
{
    const s32 skill = q2_cre_skill();
    s16       f;

    if (!self)
        return;

    /* `skinnum |= 1`, and it happens before every gate — even a hit too small
     * to flinch at turns the tank bloody. id does it first too. `hurt` is the
     * port's name for that low bit; see monster.h. */
    if (self->health < self->max_health / 2)
        self->hurt = true;

    if (damage <= 10)
        return;

    if (q2_level_state.time < self->pain_debounce)
        return;

    if (damage <= 30) {
        if (tank_rand() >= TANK_R_0_2)
            return;
    }

    if (skill >= 2) {
        f = self->frame;
        if (f >= TANK_PRE_ROCKET && f <= 144)
            return;
        if (f >= TANK_ATTACK_BLAST && f <= 70)
            return;
    }

    self->pain_debounce = q2_level_state.time + TANK_PAIN_DEBOUNCE;
    tank_play(self, TANK_SND_PAIN);

    /* No pain animations in nightmare. The sound still plays and the skin has
     * already turned; only the flinch is skipped. */
    if (skill == 3)
        return;

    if (damage <= 30)
        q2_cre_set_move(self, TANK_PAIN1);
    else if (damage <= 60)
        q2_cre_set_move(self, TANK_PAIN2);
    else
        q2_cre_set_move(self, TANK_PAIN3);
}

/*
 * tank_die — module+0x1A18, entity+0xA4. id's `tank_die`:
 *
 *     if (health <= gib_health) {                  ; module+0x1A38
 *         nextthink = 0;                           ; entity+0x90
 *         movetype  = MOVETYPE_TOSS;               ; bits 18..21 of +0x20 = 7
 *         deadflag  = DEAD_DEAD;                   ; bits 22..23 of +0x20 = 2
 *         svflags  |= SVF_DEADMONSTER;             ; entity+0x40 |= 2
 *         return;
 *     }
 *     if (deadflag == DEAD_DEAD) return;           ; module+0x1A98
 *     play(module+0x210C);                         ; tnk_death
 *     currentmove = module+0x1F90;                 ; 222-253
 *     deadflag    = DEAD_DEAD;
 *     takedamage  = DAMAGE_YES;                    ; bits 30..31 of +0x1C = 1
 *
 * The two bitfields are read out of the masks: `& 0xFFC3FFFF | 0x001C0000` puts
 * 7 in a four-bit field at bit 18, and 7 is MOVETYPE_TOSS; `& 0xFF3FFFFF |
 * 0x00800000` puts 2 in a two-bit field at bit 22, and 2 is DEAD_DEAD; `&
 * 0x3FFFFFFF | 0x40000000` puts 1 in a two-bit field at bit 30, and 1 is
 * DAMAGE_YES. `tank_dead` at module+0x16C8 writes the same movetype and the
 * same svflag, which is the second reading of both.
 *
 * The gib arm has NO SOUND. id plays misc/udeath.wav there and the module
 * registers `msc_udeath` at module+0x2120 — and then never reads it. That is
 * the console's, not this port's.
 *
 * The gib test runs BEFORE the already-dead guard, so a body can still be blown
 * apart by a later explosion. All five stores land now: `deadflag`, `movetype`
 * and `takedamage` are fields on `q2_monster`, and the port's `dead` bool stays
 * the guard the already-dead test reads, so the two never disagree.
 *
 * `damage` is the fourth argument here as it is in `tank_pain`, and this
 * function never reads it — module+0x1A18 touches only `a0`. It is in the
 * signature because the callback carries it, not because the tank wants it.
 */
static void tankcomm_die(q2_monster *self, s16 damage)
{
    (void)damage;

    if (self->health <= self->gib_health) {
        /* The port's word for "blown apart, no death animation"; the module
         * expresses it by leaving `currentmove` alone and switching the corpse
         * to MOVETYPE_TOSS, which is written just below. */
        self->gibbed     = true;
        self->dead       = true;
        self->deadflag   = Q2_DEAD_DEAD;
        self->next_think = 0;
        self->movetype   = Q2_MOVETYPE_TOSS;
        self->svflags   |= Q2_SVF_DEADMONSTER;
        return;
    }

    if (self->dead)
        return;

    tank_play(self, TANK_SND_DEATH);
    q2_cre_set_move(self, TANK_DEATH);
    self->dead     = true;
    self->deadflag = Q2_DEAD_DEAD;
    /* A Tank Commander's corpse can still be shot, which is what makes the gib
     * arm above reachable after death. module+0x1AE4..+0x1AF4. */
    self->takedamage = Q2_DAMAGE_YES;
}

/* ------------------------------------------------------------------------- */
/* Think functions, by the index the animation frames use                     */
/* ------------------------------------------------------------------------- */

/*
 * [1] tank_refire_rocket — module+0x1320, the endfunc of 136-144.
 *
 *     if (skill >= 2)
 *         if (enemy->health > 0)
 *             if (visible(self, enemy))            ; import +0xE4
 *                 if (rand() < 13107)              ; id's `random() <= 0.4`
 *                     { currentmove = fire_rocket; return; }
 *     currentmove = post_rocket;
 *
 * id's `tank_refire_rocket` including the order of the three guards and the
 * comment id puts above it: only on hard or nightmare.
 */
static void tankcomm_refire_rocket(q2_monster *self)
{
    if (q2_cre_skill() >= 2 && self->enemy && self->enemy->health > 0 &&
        q2_visible(self, self->enemy) && tank_rand() < TANK_R_0_4b) {
        q2_cre_set_move(self, TANK_FIRE_ROCKET);
        return;
    }
    q2_cre_set_move(self, TANK_POST_ROCKET);
}

/*
 * [2] tank_doattack_rocket — module+0x13D8, the endfunc of 115-135. Three
 * instructions: `currentmove = module+0x1E60`. id's one-liner of the same name.
 */
static void tankcomm_doattack_rocket(q2_monster *self)
{
    q2_cre_set_move(self, TANK_FIRE_ROCKET);
}

/*
 * [3] tank_reattack_blaster — module+0x13E8, the endfunc of both 55-70 and
 * 65-70.
 *
 *     if (skill >= 2)
 *         if (visible(self, enemy))
 *             if (enemy->health > 0)
 *                 if (rand() < 19661)              ; id's `random() <= 0.6`
 *                     { currentmove = reattack_blast; return; }
 *     currentmove = post_blast;
 *
 * id's `tank_reattack_blaster`, and note that its two middle guards are in the
 * OPPOSITE order to the rocket's above — visible first here, health first
 * there. The module has them the same way round id does, both times.
 */
static void tankcomm_reattack_blaster(q2_monster *self)
{
    /* The NULL test is this port's; the module is only reached with an enemy
     * and dereferences it in both guards. */
    if (q2_cre_skill() >= 2 && self->enemy && q2_visible(self, self->enemy) &&
        self->enemy->health > 0 && tank_rand() < TANK_R_0_6) {
        q2_cre_set_move(self, TANK_REATTACK_BLAST);
        return;
    }
    q2_cre_set_move(self, TANK_POST_BLAST);
}

/*
 * [6] tank_footstep — module+0x153C.
 *
 *     import +0x12C (4, 14, 4096, self, {2048, 8192});
 *     play(module+0x2110, self->[0x24]);           ; tnk_step
 *
 * Import +0x12C is the player proximity effect (creature.h): `4` means every
 * player, `14` is the effect descriptor id, 4096 is the magnitude and 2048 and
 * 8192 are the inner and outer radii — 170 and 683 id units at the AI's scale
 * of twelve. It is the console shaking the pad when something that heavy puts a
 * foot down. The fourth argument is the ENTITY (`addu a3, s1, zero` at
 * module+0x1580, and `s1` is `a0` at entry), not its origin; the two radii are
 * `sh` stores to sp+16 and sp+18, so they share one argument slot as a pair of
 * halfwords rather than being two words. Nothing in the port receives any of
 * it, so only the sound is played and the call is recorded here rather than
 * approximated.
 *
 * id's `tank_footstep` is the sound alone; the rumble is the console's addition.
 */
static void tankcomm_footstep(q2_monster *self)
{
    tank_play(self, TANK_SND_STEP);
}

/*
 * [8] TankBlaster — module+0xC4C. Fires on frames 64, 67 and 70.
 *
 * The muzzle: import +0x2C resolves the model attachment named by the eight
 * bytes at module+0x2130 — a blend of animation positions 287 and 289, built
 * once at module+0xA10 and checked with the module's own error string, "Tank
 * Blaster muzzle points are from different submodels" (module+0x260). Import
 * +0x28 rotates it by the object's matrix at +0x2C0, and the object's world
 * origin at +0xA4..+0xAC is added.
 *
 * The aim: `enemy->origin + (0, enemy->view_height, 0) - start`, normalised
 * through import +0xC4.
 *
 * The shot: import +0x80, `monster_fire_blaster`, with
 *
 *     damage 16, speed 800, flash 0, effect 8
 *
 * read at module+0xDA8 (a3 = 16), +0xDAC (sp+16 = 800), +0xDB4 (sp+20 = 0) and
 * +0xDBC (sp+24 = 8).
 *
 * 8 is id's EF_BLASTER, and the executable's wrapper at 0x80062000 tests that
 * argument against 0x40 — id's EF_HYPERBLASTER — to choose between two bolt
 * styles, so the argument position is settled from the other side. Speed 800 is
 * id's own, and 0x80062000 never reads it: the console's bolt carries its own.
 * Damage 16 is NOT id's, which is 30. Read, not adjusted.
 *
 * Damage and speed go through `q2_cre_shot`; the EFFECT does not, because the
 * struct has no field for it. That is the one figure of this shot the host does
 * not receive, and it is in the header's list.
 *
 * Unlike id, the console picks ONE muzzle attachment for all three shots rather
 * than three flash offsets, so there is no per-frame selection here at all.
 */
static void tankcomm_blaster(q2_monster *self)
{
    /* The module dereferences `enemy` without a test to build the aim, so the
     * NULL guard `q2_cre_fire_shot` makes for every creature costs this shot
     * nothing. Its enemy-ALIVE guard is NOT the module's — no fire think here
     * reads the enemy's health — and that one is a departure; see the header. */
    q2_cre_fire_shot(self, &tank_shot_blaster);
}

/*
 * [9] TankStrike — module+0x15C4. One call: `play(module+0x211C)`, the
 * `tnk_atck1` handle, on frame 102 of the strike. id's `TankStrike` is the same
 * one line and plays `sound_strike`.
 */
static void tankcomm_strike(q2_monster *self)
{
    tank_play(self, TANK_SND_STRIKE);
}

/*
 * [10] TankRocket — module+0xDE0. Fires on frames 138, 141 and 144.
 *
 * The muzzle IS selected per frame here, which is id's three flash offsets:
 *
 *     frame 138 -> module+0x2140      id's MZ2_TANK_ROCKET_1
 *     frame 141 -> module+0x2148      id's MZ2_TANK_ROCKET_2
 *     otherwise -> module+0x2150      id's MZ2_TANK_ROCKET_3
 *
 * The three are not three attachments. module+0xAA4..+0xC00 blends animation
 * positions 307/306 and 304/305 into two endpoints and then LERPS three points
 * evenly between them — `base + (i * delta + delta/2) / 3` in 1.5 fixed point,
 * i = 0, 1, 2. So the console derives the three launch tubes from two model
 * points instead of shipping three.
 *
 * The aim is the blaster's: enemy eye minus start, normalised.
 *
 * The shot: import +0x98, `monster_fire_rocket`, with
 *
 *     damage 50, speed 550, flash 0
 *
 * at module+0xF60 (a3 = 50), +0xF68 (sp+16 = 550) and +0xF74 (sp+20 = 0), both
 * figures being id's `monster_fire_rocket (self, start, dir, 50, 550, ...)`
 * exactly.
 *
 * The two figures now reach the host. The MUZZLE does not: `q2_cre_shot` names
 * the spawner and the shot, not the start point, so the three-way selection
 * above is recorded here and the host launches from wherever it launches. That
 * costs the rocket its per-frame launch tube and nothing else.
 */
static void tankcomm_rocket(q2_monster *self)
{
    q2_cre_fire_shot(self, &tank_shot_rocket);
}

/*
 * [11] tank_windup — module+0x15FC. One call: `play(module+0x2118)`, the
 * `pt1__strt` handle, on frame 95 — seven frames before the strike lands. id's
 * `tank_windup` plays `sound_windup` in the same place.
 *
 * This is the one sound the module post-processes: module+0x82C clones the
 * handle through import +0x3C and sets the clone's volume to 60 through import
 * +0x44, leaving pan and pitch at their sentinels. So the wind-up is quieter
 * than everything else the creature plays, deliberately.
 */
static void tankcomm_windup(q2_monster *self)
{
    tank_play(self, TANK_SND_WINDUP);
}

/*
 * [12] tank_poststrike — module+0x1634, the endfunc of 77-114.
 *
 *     self->enemy = NULL;
 *     aiflags &= ~AI_BRUTAL;
 *     if (aiflags & AI_STAND_GROUND) { currentmove = stand; return; }
 *     if (currentmove == walk || currentmove == start_run) currentmove = run;
 *     else                                                 currentmove = start_run;
 *
 * id's `tank_poststrike` is two lines — clear the enemy, then call `tank_run` —
 * and the module INLINED the call. That is why the tail from module+0x1660
 * repeats the run callback's chain instruction for instruction: with `enemy`
 * just cleared, `enemy && enemy->client` is false, so the inliner folded the
 * AI_BRUTAL branch down to the clear.
 *
 * Written here as id writes it, because calling `tankcomm_run` after clearing
 * the enemy produces the identical sequence and says what it is.
 */
static void tankcomm_poststrike(q2_monster *self)
{
    self->enemy = NULL;
    tankcomm_run(self);
}

/*
 * [13] TankMachineGun — module+0xF90. Nineteen consecutive frames, 173..191,
 * which is id's attak406..attak424.
 *
 * The muzzle is one attachment for all nineteen: the eight bytes at
 * module+0x2128, a blend of animation positions 251 and 260 built at
 * module+0x9E8 behind the error string "Tank ChainGun muzzle points are from
 * different submodels" (module+0x224).
 *
 * The aim is built as an ANGLE TRIPLE rather than a vector, exactly as id does:
 *
 *     pitch = enemy ? vectoangles(enemy_eye - start)[0] : 0;   ; import +0xC8
 *     roll  = 0;
 *     yaw   = (frame < 182) ? self->angles[YAW] + 91 * (frame - 174)
 *                           : self->angles[YAW] - 128 * (frame - 187);
 *     AngleVectors(angles, forward, NULL, NULL);               ; import +0xD8
 *
 * 91/4096 of a turn is 8.0 degrees, which is id's `8 * (self->s.frame - ...)`
 * to two figures. The second arm's 128 is 11.25 degrees and is NOT id's 8 —
 * the console widened the second half of the sweep. The two arms very nearly
 * meet: frame 181 gives yaw+637 and frame 182 gives yaw+640, a step of 3/4096
 * (0.26 degrees), so the barrel tracks up across the first nine frames and back
 * down across the last ten without a visible jump. id sweeps the other way
 * round, down then up, and pivots on different frames — id's arms hinge on
 * attak411 and attak419, this module's on 174 and 187, which on this disc's
 * numbering are attak407 and attak420. The two are the same gesture, not the
 * same arithmetic.
 *
 * The `enemy == NULL` arm is the module's own (`beq a3, zero` at module+0x107C)
 * and is id's `else dir[0] = 0;` — the only fire think on this creature that
 * tolerates having no target.
 *
 * The shot: import +0x84, `monster_fire_bullet`, with
 *
 *     damage 20, kick 4, hspread 300, vspread 500, flash 0
 *
 * at module+0x117C (a3 = 20), +0x1180 (sp+16 = 4), +0x1188 (sp+20 = 300),
 * +0x1194 (sp+24 = 500) and +0x11A0 (sp+28 = 0) — every one of which is id's:
 * `monster_fire_bullet (self, start, forward, 20, 4, DEFAULT_BULLET_HSPREAD,
 * DEFAULT_BULLET_VSPREAD, flash_number)`, and id's two spread constants are 300
 * and 500. Four independent numbers landing together is the check that this
 * read is right rather than merely self-consistent — and 300/500 recur in three
 * other modules, which is the same check made a second way.
 *
 * A DEPARTURE, and it is this creature's alone. The module tolerates a NULL
 * enemy here and fires anyway with a flat pitch; `q2_cre_fire_shot` guards
 * `enemy` for every creature and declines the shot instead. So a Tank Commander
 * that loses its target mid-burst stops shooting where the console would spray
 * the last ten frames level. It is counted in `fire_no_enemy`, so it shows.
 */
static void tankcomm_machinegun(q2_monster *self)
{
    q2_cre_fire_shot(self, &tank_shot_bullet);
}

/*
 * [14] tank_idle — module+0x1690. One call: `play(module+0x2100)`. id's
 * `tank_idle` is the same one line.
 *
 * This is the WRITTEN handle, unlike the monsterinfo `idle` callback's, which
 * nothing ever stores to. But it is not `tnk_idle1` at run time: that name is
 * in no bank on the disc, so module+0x820 has already copied the `tnk_step`
 * handle over this word and the console plays a footfall here. The port
 * resolves the address to its registered name and finds no sample; see the
 * header. Frame 249 of the death move carries this same think — the console
 * plays a footfall twenty-seven frames into the fall, which is a body hitting
 * the floor. Whether the console MEANT the substitution to land there or only
 * tolerated it is not established; the module has one fallback and both readers
 * of the handle get it.
 */
static void tankcomm_think_idle(q2_monster *self)
{
    tank_play(self, TANK_SND_IDLE);
}

/*
 * [15] tank_dead — module+0x16C8, the endfunc of the death move.
 *
 *     nextthink = 0;                       ; entity+0x90
 *     movetype  = MOVETYPE_TOSS;           ; bits 18..21 of entity+0x20 = 7
 *     svflags  |= SVF_DEADMONSTER;         ; entity+0x40 |= 2
 *
 * Three of id's four lines. The fourth, `VectorSet(self->mins/maxs, ...)`, is
 * absent — the console never resizes the corpse's box — and so is the
 * `gi.linkentity` that follows it in id.
 *
 * It does NOT touch `deadflag`: `tank_die` raised that before installing the
 * death move, so this endfunc only has to settle the corpse.
 */
static void tankcomm_dead(q2_monster *self)
{
    self->next_think = 0;
    self->movetype   = Q2_MOVETYPE_TOSS;
    self->svflags   |= Q2_SVF_DEADMONSTER;
}

/* ------------------------------------------------------------------------- */
/* Spawn                                                                      */
/* ------------------------------------------------------------------------- */
/*
 * export 0 — module+0x16F8. Past the eight callbacks and the two explicit
 * zeroes it does nine more things, in this order:
 *
 *     mass          = 500                  ; entity+0x4E, id's tank mass
 *     movetype      = MOVETYPE_STEP        ; bits 18..21 of entity+0x20 = 5
 *     solid         = SOLID_BBOX           ; bits 16..17 of entity+0x20 = 2
 *     link_entity(self, 5);                ; import +0x1C, origin + yaw
 *     link_entity(self, 133);              ; ...again with bit 7, render only
 *     currentmove   = module+0x1B64        ; the stand move
 *     speed_scale   = 10                   ; entity+0x13B
 *     walkmonster_start(self);             ; import +0xFC
 *     if (class_id == 92) skinnum = 2;     ; entity+0x23 / entity+0x3A
 *
 * `q2_creature_spawn` already carries the speed scale and the callbacks out of
 * the decoded module, and the AI framework is what `walkmonster_start` reaches.
 * What is left for this hook is the mass, the two packed entity fields, the
 * stand move and the variant arm.
 *
 * THE MASS IS WRITTEN HERE, not by the framework. `q2_creature.mass` decodes to
 * 500 from the module and `q2_monster.mass` is where it lands; 500 is id's own
 * tank mass. `sh v0, 78(s1)` at module+0x1720 is the store, and entity+0x4E is
 * the field monster.h names.
 *
 * The packed word at entity+0x20 takes two rewrites in a row, module+0x179C
 * through +0x17B0: `& 0xFFC3FFFF | 0x00140000` puts 5 in the four-bit field at
 * bit 18, and 5 is MOVETYPE_STEP; `& 0xFFFCFFFF | 0x00020000` puts 2 in the
 * two-bit field at bit 16, and 2 is SOLID_BBOX. Neither `health` nor
 * `takedamage` is touched — the class row's health is applied before this hook
 * runs, which is the loader's order (0x8007E68C, 0x8007E698, then 0x8007E6AC),
 * and this module is content to leave it.
 *
 * THE CLASS-92 ARM IS UNREACHABLE ON THIS DISC, and it is written anyway
 * because it costs one comparison to be exact. `q2psx-inspect classes` has one
 * Tankcomm row, number 25, health 750 and gib_health -300, and its class byte is
 * 91 — which is also the only byte export 2 registers a method table for
 * (module+0x9C4). 92 would be the second variant: id ships `monster_tank` and
 * `monster_tank_commander` off one file, and the commander is exactly
 * `skinnum = 2` plus more health. `class_id` is the entity+0x23 the module
 * reads (`lbu v1, 35(s1)` at module+0x17FC) and `q2_creature_spawn` fills it
 * with the class byte, so the test is the module's own; it will simply never be
 * true on this disc.
 */
static void tankcomm_spawn(q2_monster *self)
{
    self->mass     = 500;
    self->movetype = Q2_MOVETYPE_STEP;
    self->solid    = Q2_SOLID_BBOX;

    q2_cre_set_move(self, TANK_STAND);

    if (self->class_id == 92)
        self->skinnum = 2;
}

/* ------------------------------------------------------------------------- */
const q2_cre_impl q2_cre_tankcomm = {
    "Tankcomm",
    {
        tankcomm_stand,     /*  0 stand       module+0x15B4 */
        tankcomm_idle,      /*  1 idle        module+0x185C */
        NULL,               /*  2 search      — the module installs none: the
                             *                  spawn function never writes
                             *                  entity+0xE8. id's tank has no
                             *                  search either. */
        tankcomm_walk,      /*  3 walk        module+0x14A0 */
        tankcomm_run,       /*  4 run         module+0x14B0 */
        NULL,               /*  5 dodge       — the spawn function writes ZERO
                             *                  to entity+0xF4 (module+0x1794),
                             *                  so this is the module's own
                             *                  decision rather than a gap. */
        tankcomm_attack,    /*  6 attack      module+0x11BC */
        NULL,               /*  7 melee       — likewise an explicit zero, to
                             *                  entity+0xFC (module+0x1798). A
                             *                  Tank Commander has no melee; its
                             *                  strike is an ATTACK move chosen
                             *                  when the enemy is already dead. */
        (q2_class_method)(void *)tankcomm_sight,  /* 8 sight module+0x1824 */
        NULL,               /*  9 checkattack — the module installs none, so
                             *                  monster_start fills the slot
                             *                  with the shared M_CheckAttack
                             *                  (import +0xE8, and 0x80061B18
                             *                  only writes it when NULL). */
        NULL,               /* 10 bigturn     — the module installs none;
                             *                  entity+0x108 is never written
                             *                  and the threshold at +0x140
                             *                  stays zero, which disables it. */
        /* Both take the damage as a second argument, so both go into the flat
         * table through a cast, exactly as sight does. crebind.c casts them
         * back to `(q2_monster *, s16)` when it installs them. */
        (q2_class_method)(void *)tankcomm_pain,   /* 11 pain module+0x1894 */
        (q2_class_method)(void *)tankcomm_die     /* 12 die  module+0x1A18 */
    },
    {
        /*
         * Sixteen methods, from the table the module builds at module+0x2168
         * and registers for class byte 91 with import +0x118 (module+0x9C4).
         * Slot 0 is written as an explicit zero by module+0x914, so it is the
         * module's own NULL and not an omission — no frame uses think 0.
         *
         * Slots 4, 5 and 7 are the walk, run and stand CALLBACKS registered a
         * second time as methods. No animation frame references any of them:
         * the census's think set is {6, 8, 9, 10, 11, 13, 14} and nothing else.
         *
         * They are here because the MODULE puts them here, which is the only
         * reason needed — and not, as this comment used to claim, because the
         * endfunc resolver needs them. It does not: `resolve_endfunc`
         * (crebind.c) searches the thirteen CALLBACK addresses before it
         * searches the methods, and all three of these are callbacks, so the
         * eight moves that end on module+0x14B0 (seven of them) and
         * module+0x15B4 (the 50-54 stop-walk) resolve either way. module+0x14A0
         * is the endfunc of no move at all.
         */
        NULL,                        /*  0 — the module writes zero here     */
        tankcomm_refire_rocket,      /*  1   module+0x1320                   */
        tankcomm_doattack_rocket,    /*  2   module+0x13D8                   */
        tankcomm_reattack_blaster,   /*  3   module+0x13E8                   */
        tankcomm_walk,               /*  4   module+0x14A0, the callback     */
        tankcomm_run,                /*  5   module+0x14B0, the callback     */
        tankcomm_footstep,           /*  6   module+0x153C                   */
        tankcomm_stand,              /*  7   module+0x15B4, the callback     */
        tankcomm_blaster,            /*  8   module+0xC4C                    */
        tankcomm_strike,             /*  9   module+0x15C4                   */
        tankcomm_rocket,             /* 10   module+0xDE0                    */
        tankcomm_windup,             /* 11   module+0x15FC                   */
        tankcomm_poststrike,         /* 12   module+0x1634                   */
        tankcomm_machinegun,         /* 13   module+0xF90                    */
        tankcomm_think_idle,         /* 14   module+0x1690                   */
        tankcomm_dead,               /* 15   module+0x16C8                   */
        /*
         * 16..31 — the module registers sixteen methods and no more, so these
         * are absent rather than unwritten. crebind.c falls each of them back
         * to the generic trampoline, which finds no decoded think and does
         * nothing, which is what an unregistered slot does on the console too.
         */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
    },
    tankcomm_spawn
};
