/*
 * cre_berserk.c — the Berserk, transcribed from its own module.
 *
 * The Berserk ships on exactly one map — `WASTE4 CreAIBin` module 'Berserk',
 * 6,148 bytes relocated — and every address below is inside it at a base of
 * 0x80100000. What was read, in the order it was read:
 *
 *     export 0  module+0x1040   the spawn function
 *     export 1  module+0x0C64   a per-animation-position blur trail
 *     export 2  module+0x0214   the module init: sounds, method table, blur
 *     export 3  module+0x1430   not code — the frame-name table starts there
 *
 * and then all eight monsterinfo callbacks and all nine registered methods.
 *
 * ---------------------------------------------------------------------------
 * It is PC Quake II's berserker, and several constants say so on their own
 * ---------------------------------------------------------------------------
 * These were not tuned to produce a match; they are what the immediates read:
 *
 *   berserk_fidget       module+0xDF8    `random() > 0.15` return, as 4916
 *   berserk_attack_spike module+0xEBC    aim (MELEE, up -24), 15 + rand()%6,
 *                                        kick 400
 *   berserk_attack_club  module+0xF60    aim (MELEE, up -4, right mins[0]),
 *                                        5 + rand()%6, kick 400
 *   berserk_pain         module+0x1224   `damage < 20 || random() < 0.5`,
 *                                        debounce 3 s, no flinch at skill 3
 *   berserk_die          module+0x1324   `damage >= 50` picks the long death
 *   berserk_dead         module+0x1010   MOVETYPE_TOSS, SVF_DEADMONSTER,
 *                                        nextthink 0
 *   the spawn function   module+0x1040   mass 250, movetype 5, solid 2
 *
 * MOVETYPE_STEP is 5 and MOVETYPE_TOSS is 7 in id's enum, SOLID_BBOX is 2 and
 * SVF_DEADMONSTER is 2, and all four fall out of bitfield writes that were read
 * before anything was looked up. Mass 250 is id's berserker exactly. monster.h
 * now carries the bit layout and the enums for all four, so this file names
 * them where it used to write the literals.
 *
 * ---------------------------------------------------------------------------
 * A CORRECTION TO WHAT THE DECODER REPORTS
 * ---------------------------------------------------------------------------
 * `q2psx-inspect creatures` prints the two melees against aims of (0,0,0) and
 * (1020,48,0). Both aims are wrong, and the module says so:
 *
 *   THE AIM VECTORS ARE (1020, 288, 0) AND (1020, 48, mins[0]). The spike's
 *   three components are loaded from the static triple at module+0x1744 —
 *   0x3FC, 0x120, 0 — which the decoder reports as zeroes; the club's third
 *   component is the return of import +0x114, `entity_mins_x`, which the
 *   decoder reports as zero because it is not a constant. 288 is 24 x 12 and
 *   48 is 4 x 12, and id's two aims are {MELEE_DISTANCE, 0, -24} and
 *   {MELEE_DISTANCE, self->mins[0], -4}. Y is down here, so id's negative `up`
 *   becomes a positive component 1, and component 2 is id's `right`.
 *
 * THE DIVISOR was the second correction and it no longer is. The decoder used
 * to print `15 + r%3` and `5 + r%3` because it stopped at the `q*2 + q` of the
 * standard magic multiply — `mult v0, 0x2AAAAAAB`, then `q*2 + q`, then `<<1`
 * — and missed the shift that turns 3q into 6q (module+0xF04..+0xF10 and
 * +0xFC4..+0xFD0). It now prints `r%6`, which is what this file read out of
 * those two windows and what id's berserker has in both attacks.
 *
 * `fire_hit` is reached as SIX arguments, not four: a0 self, a1..a3 the aim
 * vector BY VALUE, sp+16 damage, sp+20 kick. That is what lets a whole aim
 * triple live in registers, and it is why the decoder's stack-slot reader finds
 * damage and kick but only sees constants for the aim.
 *
 * ---------------------------------------------------------------------------
 * THINK 7 IS EMPTY, AND IT IS EMPTY IN id TOO
 * ---------------------------------------------------------------------------
 * The census says one of the Berserk's five used think indices is `jr ra` and
 * nothing else. It is index 7, at module+0x1008, and it is the only think in
 * the move "Attack3" (96-109) other than the swing sound. id's berserk.c has
 * exactly one function like that:
 *
 *     void berserk_strike (edict_t *self)   { }
 *
 * whose entire body in id's source is a one-line comment reading "FIXME play
 * impact sound". And `berserk_move_attack_strike`, the move it belongs to, is
 * defined in id's source and installed by no callback there either. Attack3 is
 * the same: its move record at module+0x166C is reachable from nothing (the
 * census marks it `via -2`), and its two thinks are the swing sound and this
 * empty one. So a missing impact sound and an unreachable animation are both
 * inherited rather than lost in the port, which is why think 7 below is a real
 * empty function with a comment rather than a bare NULL.
 *
 * ---------------------------------------------------------------------------
 * THE SOUNDS ARE PAIRS, WITH THE INFANTRY AS THE FALLBACK
 * ---------------------------------------------------------------------------
 * The module init resolves each sound by name through import +0x24 and keeps
 * the handle in its own BSS. Three of the seven are resolved TWICE — the
 * berserk name first, and if that came back zero the infantry's equivalent:
 *
 *     module+0x1750   ber_pain2  else inf_pain1     (module+0x620, +0x6C8)
 *     module+0x1754   ber_deth2  else inf_deth2     (module+0x778, +0x820)
 *     module+0x1758   ber_idle1                     (module+0x8CC)
 *     module+0x175C   ber_attack else inf_atck2     (module+0x97C, +0xA24)
 *     module+0x1760   ber_sight                     (module+0xAD0)
 *     module+0x1764   ber_srch1                     (module+0xB7C)
 *     module+0x1768   msc_udeath                    (module+0xC2C)
 *
 * `bne v0, zero, <past the second>` is the test, so the fallback runs only when
 * the map's bank does not carry the berserk sample. That is what the census's
 * "two names for one address" means, and it explains why the module carries
 * infantry names at all.
 *
 * A PRECISE NEGATIVE: `msc_udeath` at module+0x1768 is registered and NEVER
 * PLAYED. Sweeping the module's whole code range for a load off one of these
 * seven words finds exactly six, and none of them is this one: +0x1758 at
 * module+0xE50 (think 1), +0x175C at +0xE94 (think 3), +0x1760 at +0x1168
 * (sight), +0x1764 at +0x11A0 (search), +0x1750 at +0x12A4 (pain) and +0x1754
 * at +0x13B8 (die). So the Berserk's gib arm is silent, where the Soldier's
 * plays this very sound. That is not a gap in this transcription; it is the
 * disc.
 *
 * `ber_idle1` and `ber_srch1` are in no map's bank anywhere on the disc, so on
 * the console those two resolve to a null handle and play nothing. The port's
 * sound hook already reaches the same conclusion by name (main.c).
 *
 * ---------------------------------------------------------------------------
 * The sound hook wants an ADDRESS
 * ---------------------------------------------------------------------------
 * `cre_actions.c` hands the host `q2_cre_step.addr` — the module address of the
 * handle — and the client resolves it against the module's own registrations.
 * So a transcribed creature has to pass the same thing, not a table index. The
 * Soldier passes an index because its own eleven names are transcribed and
 * `q2_cre_soldier_sound_name` exists; nothing like that exists here, and it
 * does not need to.
 *
 * ---------------------------------------------------------------------------
 * What was read and deliberately not modelled
 * ---------------------------------------------------------------------------
 * Export 1 (module+0xC64) is the swing blur. It takes the OBJECT, not the
 * entity, reads the animation position at object+0x100, and for each of three
 * windows calls import +0x78 `blur_trail_start` with a short list of attachment
 * positions: {280,281,286}, {275,263} and {274,265} at module+0x1F8, +0x204 and
 * +0x20C, colour word 0x00202020 and count 6. The windows are [base+60,
 * base+140] of the moves "Attack1", "Attack2" and "Attack3", resolved by name
 * at init through import +0x74 and stored at module+0x176C..+0x1777; when a
 * name does not resolve the pair is written (1, 0) so the window can never
 * match. None of that has a home: `q2_cre_impl` has no per-frame object hook
 * and `q2_monster` has no object. Named here so it is not mistaken for absent.
 *
 * The spawn function's `mass = 250`, `solid = SOLID_BBOX` and
 * `movetype = MOVETYPE_STEP` are no longer in this list: monster.h carries all
 * three now and the spawn hook writes them. What is left of that function with
 * nowhere to go is its two `link_entity` calls (import +0x1C, with 5 and then
 * 0x85) and its closing `walkmonster_start` (import +0xFC) — this port has no
 * server-side link, and `q2_creature_spawn` IS the walkmonster_start.
 *
 * ---------------------------------------------------------------------------
 * THE DAMAGE ARGUMENT ARRIVED, AND THERE IS NO SHOT TO WIRE
 * ---------------------------------------------------------------------------
 * Two branches could not be run in the first pass, and both were the same
 * missing argument: the module takes the damage in a3 (module+0x1264 stashes it
 * in s2, module+0x1350 in s1) and this port's `pain` and `die` took only the
 * entity. They now carry an `s16 damage` (monster.h) and
 * `q2_monster_damage_reaction` passes T_Damage's own amount, so both branches
 * are live and transcribed rather than approximated:
 *
 *   berserk_pain  `slti v0, s2, 20` at module+0x12D0. A `<` compiles as its own
 *                 immediate where a `<=` would have compiled as one above it,
 *                 so this is id's `damage < 20` exactly.
 *   berserk_die   `slti v0, s1, 50` at module+0x13FC, and the LONG death is the
 *                 arm taken when it is false — id's `damage >= 50`.
 *
 * Nothing in this file stands in for a damage test any more.
 *
 * AND THERE IS NO SHOT. `q2_cre_fire_shot` and the figures it carries are for a
 * creature with a ranged attack, and the Berserk has none: its spawn function
 * writes ZERO to the attack callback slot at entity+0xF8 (module+0x10F4), which
 * is a store rather than an omission, and the only two things it does to an
 * enemy are the spike and the club, both through `fire_hit`. So there is no
 * `q2_cre_shot` below and no `Q2_IMP_FIRE_*` slot, and the absence is the
 * module's own answer rather than an unread one.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame                                               */
/* ------------------------------------------------------------------------- */
/*
 * Eleven moves, all eleven named by the module's own 20-byte
 * {u16 first, u16 last, char name[16]} table at module+0x1430. The addresses
 * are the move records, which is what the callbacks below store to entity+0xD8.
 *
 * All eleven start on a different frame, so `q2_cre_set_move` is unambiguous
 * here and the address-keyed `q2_cre_set_move_at` is not wanted: that one is
 * for the Arachner's and the Tank Commander's overlapping walk/run pairs, and
 * this creature has no pair that overlaps.
 */
#define BERSERK_STAND     0     /*   0..4    "Stand"    module+0x1530 */
#define BERSERK_FIDGET    5     /*   5..24   "Fidget"   module+0x157C */
#define BERSERK_WALK     25     /*  25..35   "Walk"     module+0x15B0 */
#define BERSERK_RUN      36     /*  36..41   "Run"      module+0x15D4 */
#define BERSERK_SPIKE    76     /*  76..83   "Attack1"  module+0x15FC */
#define BERSERK_CLUB     84     /*  84..95   "Attack2"  module+0x1630 */
/*
 * "Attack3", 96..109, module+0x166C. Present, named, and installed by nothing —
 * see the header. It carries thinks 3 and 7, and think 7 is the empty one.
 */
#define BERSERK_STRIKE   96
#define BERSERK_PAIN1   199     /* 199..202  "Pain1"    module+0x1688 */
#define BERSERK_PAIN2   203     /* 203..222  "Pain2"    module+0x16D4 */
#define BERSERK_DEATH1  223     /* 223..235  "Death"    module+0x170C */
#define BERSERK_DEATH2  236     /* 236..243  "Death2"   module+0x1734 */

/* ------------------------------------------------------------------------- */
/* Sounds, as the module addresses of their handles                           */
/* ------------------------------------------------------------------------- */
#define BER_SND_PAIN    0x80101750u   /* ber_pain2, else inf_pain1  */
#define BER_SND_DEATH   0x80101754u   /* ber_deth2, else inf_deth2  */
#define BER_SND_IDLE    0x80101758u   /* ber_idle1                  */
#define BER_SND_SWING   0x8010175Cu   /* ber_attack, else inf_atck2 */
#define BER_SND_SIGHT   0x80101760u   /* ber_sight                  */
#define BER_SND_SEARCH  0x80101764u   /* ber_srch1                  */
/*
 * module+0x1768 is `msc_udeath`. It is registered and no instruction in the
 * module reads it, so it is not defined here — a constant nothing uses would
 * read as an unfinished gib sound, and the silence is the disc's.
 */

/* The hooks, shared with cre_soldier.c (which defines them) and cre_actions.c. */
extern void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
extern void  *q2_cre_sound_user;
extern void (*q2_cre_melee_fn)(q2_monster *m, const s32 aim[3],
                               s32 damage, s32 kick, void *user);
extern void  *q2_cre_melee_user;

static void ber_play(q2_monster *m, u32 handle_addr)
{
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle_addr, q2_cre_sound_user);
}

/* import +0x14, 0..32767. The thresholds below are fractions of 32768. */
static s32 ber_rand(void)
{
    return (s32)(rand() & 0x7FFF);
}

#define BER_R_0_15   4916       /* `random() > 0.15` at module+0xE34 */
#define BER_R_0_5   16384       /* `random() < 0.5`  at module+0x12EC */

/*
 * The kick both attacks pass, and the damage spread they share. Written as the
 * module's own integers: `addiu v0, v0, 15` / `addiu v0, v0, 5` after a divide
 * by six, and `addiu v1, zero, 400` into the sixth argument slot.
 */
#define BER_MELEE_KICK       400
#define BER_MELEE_SPREAD       6

/*
 * The two damage thresholds, each one a `slti` against the module's own
 * immediate — `slti v0, s2, 20` at module+0x12D0 and `slti v0, s1, 50` at
 * module+0x13FC. Both are strict `<` in the source they came from, because a
 * `<=` would have compiled with the immediate one higher; so the pain test is
 * id's `damage < 20` and the death test is id's `damage >= 50` on the arm the
 * branch falls through to.
 */
#define BER_PAIN_LIGHT        20
#define BER_DEATH_HEAVY       50

/* ------------------------------------------------------------------------- */
/* Think functions, by the index the animation frames use                     */
/* ------------------------------------------------------------------------- */

/*
 * [1] berserk_fidget — module+0xDF8.
 *
 *     if (aiflags & 1) return;          ; AI_STAND_GROUND, entity+0xDC
 *     if (rand() >= 4916) return;       ; id's `random() > 0.15`
 *     currentmove = Fidget;             ; module+0x157C -> entity+0xD8
 *     play(module+0x1758);              ; ber_idle1
 *
 * The order matters and is the module's: the move is installed BEFORE the
 * sound, so a fidget that is about to be interrupted still grunts.
 */
static void berserk_fidget(q2_monster *self)
{
    if (self->aiflags & Q2_AI_STAND_GROUND)
        return;
    if (ber_rand() >= BER_R_0_15)
        return;

    q2_cre_set_move(self, BERSERK_FIDGET);
    ber_play(self, BER_SND_IDLE);
}

/* [3] berserk_swing — module+0xE84. One call: play(module+0x175C), the
 * ber_attack sample. id's `berserk_swing` plays "berserk/attack.wav". */
static void berserk_swing(q2_monster *self)
{
    ber_play(self, BER_SND_SWING);
}

/*
 * [4] berserk_attack_spike — module+0xEBC.
 *
 *     r = rand();
 *     fire_hit(self, 1020, 288, 0, 15 + r % 6, 400);
 *
 * The aim triple is the static at module+0x1744 = {0x3FC, 0x120, 0}. Component
 * 1 is the vertical one and it points DOWN, so 288 is id's `up = -24` scaled by
 * the AI's 12; component 2 is id's `right`, which the spike leaves at zero.
 */
static void berserk_attack_spike(q2_monster *self)
{
    s32 aim[3];

    aim[0] = Q2_MELEE_DISTANCE;         /* 1020, module+0x1744 */
    aim[1] = Q2_AI_UNITS(24);           /*  288, module+0x1748 */
    aim[2] = 0;                         /*    0, module+0x174C */

    if (q2_cre_melee_fn)
        q2_cre_melee_fn(self, aim, 15 + (ber_rand() % BER_MELEE_SPREAD),
                        BER_MELEE_KICK, q2_cre_melee_user);
}

/*
 * [6] berserk_attack_club — module+0xF60.
 *
 *     a = import[+0x114](self);         ; entity_mins_x
 *     r = rand();
 *     fire_hit(self, 1020, 48, a, 5 + r % 6, 400);
 *
 * 48 is id's `up = -4` with the axis flipped and the scale applied; the third
 * component is the body's own left edge, which is what swings the club wide.
 *
 * A DEPARTURE, named: import +0x114 reads the box out of the entity's OBJECT
 * (object+0x6C) and returns -96 when there is no object. This port has no
 * object and keeps the hull on the creature, so `m->mins[0]` is used directly
 * — and it is now filled: `q2_monster_init` writes the shared -286 cube to
 * every creature (monster.c), so the club swings from -286 where the console
 * swings from whatever the Berserk's own object carries. That is a wider
 * swing, not a missing one; the first pass had it at zero because nothing
 * filled the field then. The difference is lateral offset only, and the reach
 * along the aim is the same 1020 either way.
 */
static void berserk_attack_club(q2_monster *self)
{
    s32 aim[3];

    aim[0] = Q2_MELEE_DISTANCE;         /* 1020, module+0xF6C */
    aim[1] = Q2_AI_UNITS(4);            /*   48, module+0xF88 */
    aim[2] = (s32)self->mins[0];        /* import +0x114      */

    if (q2_cre_melee_fn)
        q2_cre_melee_fn(self, aim, 5 + (ber_rand() % BER_MELEE_SPREAD),
                        BER_MELEE_KICK, q2_cre_melee_user);
}

/*
 * [7] berserk_strike — module+0x1008, and it is TWO INSTRUCTIONS: `jr ra` and
 * a delay-slot nop.
 *
 * This is the empty function the census counts, and it is empty on purpose: it
 * is id's `berserk_strike`, whose entire body is the comment "FIXME play impact
 * sound". Written out rather than left NULL so the table below says "the disc
 * has a handler here and it does nothing" instead of "not transcribed".
 *
 * Its only frame is frame 103 of "Attack3" (96-109), which no callback reaches
 * — so on this disc it never runs at all.
 */
static void berserk_strike(q2_monster *self)
{
    (void)self;
}

/*
 * [8] berserk_dead — module+0x1010, the end callback on both death moves.
 *
 *     nextthink = 0;                    ; entity+0x90
 *     movetype  = 7;                    ; entity+0x20 bits 18..21, TOSS
 *     svflags  |= 2;                    ; entity+0x40, SVF_DEADMONSTER
 *
 * id's berserk_dead does those three and two more — it shrinks the bounding box
 * to (-16,-16,-24)..(16,16,-8) and re-links the entity. The module does
 * neither, which is a real difference between the console and the PC and not a
 * shortfall here.
 *
 * All three stores land now. The movetype used to be dropped for want of a
 * field and the SVF bit used to be a bare 2; monster.h names both, and the
 * frame driver tests Q2_SVF_DEADMONSTER for the same reason the console does —
 * it is what stops a corpse replaying its own death for ever.
 *
 * It does NOT touch deadflag — `berserk_die` raised that before installing the
 * move, so by the time this runs the body is already DEAD_DEAD. Left alone here
 * for the same reason: writing `dead` again would be a store the module does
 * not make.
 */
static void berserk_dead(q2_monster *self)
{
    self->next_think = 0;                      /* module+0x1020 */
    self->movetype   = Q2_MOVETYPE_TOSS;       /* module+0x1030, 7 << 18 */
    self->svflags   |= Q2_SVF_DEADMONSTER;     /* module+0x103C */
}

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/* berserk_stand — module+0xE74. Four instructions: currentmove = Stand. */
static void berserk_stand(q2_monster *self)
{
    q2_cre_set_move(self, BERSERK_STAND);
}

/* berserk_walk — module+0x11C8. Four instructions: currentmove = Walk. */
static void berserk_walk(q2_monster *self)
{
    q2_cre_set_move(self, BERSERK_WALK);
}

/*
 * berserk_run — module+0xF50.
 *
 * FOUR INSTRUCTIONS, and that is the whole function: `lui`, `addiu`, `jr ra`,
 * `sw v0, 0xD8(a0)`. There is NO stand-ground branch.
 *
 * id's berserk_run opens with `if (aiflags & AI_STAND_GROUND) currentmove =
 * berserk_move_stand;` and the console dropped it. Stated because every other
 * transcribed run callback in this port has that branch and its absence here
 * would otherwise look like an omission: a Berserk told to hold its ground
 * runs anyway.
 */
static void berserk_run(q2_monster *self)
{
    q2_cre_set_move(self, BERSERK_RUN);
}

/*
 * berserk_melee — module+0x11D8.
 *
 *     r = import[+0x14]();
 *     currentmove = (r & 1) ? Attack2 : Attack1;
 *
 * A coin and nothing else — no range test, no health test, which is id's
 * `if ((rand() % 2) == 0) spike; else club;`. The two differ in which second
 * think follows the shared swing sound: Attack1 (76-83) carries thinks 3 and 4,
 * the 15-damage spike; Attack2 (84-95) carries thinks 3 and 6, the 5-damage
 * club. So the Berserk alternates its two swings instead of always throwing the
 * heavy one, which is what the generic handler was doing.
 */
static void berserk_melee(q2_monster *self)
{
    q2_cre_set_move(self, (ber_rand() & 1) ? BERSERK_CLUB : BERSERK_SPIKE);
}

/*
 * berserk_sight — module+0x1158. One call: play(module+0x1760), ber_sight.
 *
 * It takes (self, other) like every sight callback and ignores `other`; the
 * module passes only a0 through to the play, as `a1 = self->object`.
 */
static void berserk_sight(q2_monster *self, q2_monster *other)
{
    (void)other;
    ber_play(self, BER_SND_SIGHT);
}

/* berserk_search — module+0x1190. One call: play(module+0x1764), ber_srch1. */
static void berserk_search(q2_monster *self)
{
    ber_play(self, BER_SND_SEARCH);
}

/*
 * berserk_pain — module+0x1224, called as (self, other, kick, damage).
 *
 * ONLY a0 AND a3 ARE READ. `other` and `kick` are id's names for a1 and a2,
 * not a reading: no instruction in this function touches either register, so
 * the middle two arguments are named by position against id's
 * `pain (edict_t *self, edict_t *other, float kick, int damage)` and by
 * nothing else. a3 IS established — module+0x1264 copies it to s2 and
 * module+0x12D0 compares it against 20, which is id's own `damage < 20`.
 *
 * In order, and every branch is the module's:
 *
 *   1. `if (health < max_health / 2) skinnum = 1` — module+0x123C..+0x126C.
 *      max_health is the halfword at entity+0x50, health the halfword at
 *      object+0x108, and the division is a signed `>> 1` with the sign bias.
 *      The module ASSIGNS 1 where id writes `skinnum |= 1`; the Berserk has
 *      only skins 0 and 1, so the two are the same thing. Both `skinnum` and
 *      `hurt` are written: the halfword store at entity+0x3A is the module's
 *      and `skinnum` is now a field, while `hurt` is the port's own name for
 *      that low bit and is what a creature's skin is derived through
 *      (monster.h, soldier_skin). One fact, two spellings, set together so
 *      they cannot drift.
 *
 *   2. `if (level.time < pain_debounce_time) return;` then
 *      `pain_debounce_time = level.time + 30` — module+0x1278..+0x129C. Thirty
 *      ticks of the 10 Hz AI clock is id's own three seconds. level.time is the
 *      second word of import +0x48.
 *
 *   3. play(module+0x1750) — ber_pain2, or inf_pain1 where the bank lacks it.
 *
 *   4. `if (skill->value == 3) return;` — module+0x12B4..+0x12CC, reading
 *      cvar[0]->value through import +0x4C. No flinch animation on nightmare,
 *      which is id's rule and the same one soldier_pain carries.
 *
 *   5. `if (damage < 20 || random() < 0.5) Pain1 else Pain2`.
 *
 * (5) IS LIVE NOW, both halves. `slti v0, s2, 20` at module+0x12D0 branches
 * straight to the Pain1 arm without reaching the `jalr` on the rand import, so
 * the module short-circuits and so does C's `||` — a light hit does not consume
 * a random number on either machine, which matters because the same generator
 * feeds the fidget roll. A Berserk grazed by a blaster bolt now takes the
 * four-frame flinch every time, where the first pass gave it the twenty-frame
 * Pain2 half the time for want of the argument.
 */
static void berserk_pain(q2_monster *self, s16 damage)
{
    /*
     * A CORPSE DOES NOT FLINCH. Not the module's guard — the module has none
     * here, because the console's damage path routes to `die` or to `pain` and
     * never to both. This port's `q2_monster_damage_reaction` has the same
     * split, so the guard is belt and braces rather than a departure; it is
     * kept because the Berserk's death moves are 8 and 13 frames long and a
     * re-entered flinch would freeze the body part-way down.
     */
    if (self->dead)
        return;

    if (self->health < self->max_health / 2) {
        self->skinnum = 1;      /* `sh v0, 0x3A(s0)`, module+0x126C */
        self->hurt    = true;
    }

    if (q2_level_state.time < self->pain_debounce)
        return;
    self->pain_debounce = q2_level_state.time + 30;   /* 3 s at 10 Hz */

    ber_play(self, BER_SND_PAIN);

    if (q2_cre_skill() == 3)
        return;

    q2_cre_set_move(self, (damage < BER_PAIN_LIGHT ||
                           ber_rand() < BER_R_0_5) ? BERSERK_PAIN1
                                                   : BERSERK_PAIN2);
}

/*
 * berserk_die — module+0x1324, called as (self, inflictor, attacker, damage).
 *
 * As in the pain handler, `inflictor` and `attacker` are id's names for a1 and
 * a2 and are not read here — module+0x1338 overwrites a1 with the object
 * pointer before anything looks at it. a3 is established the same way: stashed
 * in s1 at module+0x1350 and compared against 50 at module+0x13FC.
 *
 * Two arms, and the gib one is tested FIRST and without a dead guard:
 *
 *   `if (health <= gib_health)` — module+0x133C..+0x134C, comparing the
 *   halfword at object+0x108 against the one at entity+0x52 — then
 *   nextthink = 0, deadflag = DEAD_DEAD (entity+0x20 bits 22..23 = 2),
 *   movetype = MOVETYPE_TOSS (bits 18..21 = 7), svflags |= SVF_DEADMONSTER.
 *   NO SOUND and NO MOVE: the body simply becomes a toss-corpse where it
 *   stands. `msc_udeath` is registered at module+0x1768 and no instruction
 *   loads it, so the Berserk's gib is silent — see the header.
 *
 *   Otherwise: `if (deadflag == DEAD_DEAD) return;` — module+0x1398..+0x13A8,
 *   which is the port's `dead` (monster.c says so at the same offset) — then
 *   play(module+0x1754) ber_deth2, deadflag = DEAD_DEAD, takedamage =
 *   DAMAGE_YES (entity+0x1C bits 30..31 = 1, so a corpse can still be gibbed),
 *   and `if (damage >= 50) Death else Death2`.
 *
 * THE DAMAGE TEST RUNS NOW. `slti v0, s1, 50` at module+0x13FC branches to the
 * Death2 record (module+0x1734, frames 236-243) when the damage is under 50 and
 * falls through to Death (module+0x170C, 223-235) when it is not, which is id's
 * `if (damage >= 50) death1 else death2`. The first pass had to pin this to
 * Death2 and said so; that departure is gone, and with it the last choice in
 * this file that was not a reading.
 */
static void berserk_die(q2_monster *self, s16 damage)
{
    /* The gib arm, tested first and with no dead guard — module+0x1354. Every
     * store in it is idempotent, so the module running it twice is harmless
     * and so is this. */
    if (self->health <= self->gib_health) {
        /*
         * `gibbed` IS THE PORT'S, NOT THE MODULE'S. The module's gib arm makes
         * exactly four stores — nextthink 0 (module+0x1368), deadflag DEAD_DEAD
         * and movetype TOSS into the one word at entity+0x20 (module+0x1388),
         * and svflags |= SVF_DEADMONSTER (module+0x1394) — and there is no
         * fifth field for "was blown apart". It does not need one: the
         * console's gibs are thrown by the arm itself and the body is gone.
         * This port keeps the distinction on the creature (monster.h:
         * "Separate from `dead` because a body already dead can still be
         * gibbed by a later explosion"), and cre_soldier.c raises the same flag
         * in the same place. So `gibbed` models the arm rather than
         * transcribing a store, and it is the only line here that does; the
         * other four are now fields and are written as the module writes them,
         * in the module's order.
         */
        self->gibbed     = true;
        self->next_think = 0;
        self->deadflag   = Q2_DEAD_DEAD;        /* 2 << 22 */
        self->movetype   = Q2_MOVETYPE_TOSS;    /* 7 << 18 */
        self->svflags   |= Q2_SVF_DEADMONSTER;
        /* `deadflag` is the module's field and `dead` is the guard this port
         * and `q2_monster_damage_reaction` test, so both are raised on the same
         * line of the module and never one without the other. */
        self->dead       = true;
        return;
    }

    if (self->dead)
        return;

    /* The module's order: the sound at module+0x13BC, then deadflag and
     * takedamage at module+0x13EC and +0x13F8. Nothing here reads either, so
     * they are interchangeable — kept in the module's order anyway so the code
     * reads as the comment above describes it.
     *
     * DAMAGE_YES on a body that has just died is deliberate and is id's: it is
     * what lets a corpse be blown apart by a later explosion, which is the
     * branch above. */
    ber_play(self, BER_SND_DEATH);
    self->deadflag   = Q2_DEAD_DEAD;    /* module+0x13EC, 2 << 22  */
    self->takedamage = Q2_DAMAGE_YES;   /* module+0x13F8, 1 << 30  */
    self->dead       = true;            /* and its guard, as above */

    q2_cre_set_move(self, damage >= BER_DEATH_HEAVY ? BERSERK_DEATH1
                                                    : BERSERK_DEATH2);
}

/* ------------------------------------------------------------------------- */
/* The spawn function                                                         */
/* ------------------------------------------------------------------------- */
/*
 * module+0x1040, past the callbacks the framework already installs.
 *
 * The full store list, in the module's order: mass 250 to entity+0x4E; the
 * eight callbacks; speed scale 10 to entity+0x13B; currentmove = Stand to
 * entity+0xD8; zero to entity+0xF4 and +0xF8 (no dodge, NO ATTACK — a Berserk
 * is melee only, and the module says so by writing the zeroes rather than by
 * omitting them); solid = SOLID_BBOX and movetype = MOVETYPE_STEP into
 * entity+0x20; then link_entity(self, 5), link_entity(self, 0x85) and
 * walkmonster_start(self) through imports +0x1C and +0xFC.
 *
 * The class byte, the speed scale and the callbacks are the framework's
 * (crebind.c). Of the rest, four are now fields on `q2_monster` and are
 * written below in the module's own order — mass, then the opening move, then
 * the movetype and solid halves of the one word at entity+0x20. Only the two
 * `link_entity` calls have nowhere to go, and they are named in the header.
 *
 * WHAT IT DOES NOT WRITE IS AS MUCH OF A READING AS WHAT IT DOES. Its whole
 * store list is above and it holds no health — neither the halfword at
 * object+0x108 that pain and die read, nor max_health at entity+0x50 nor
 * gib_health at +0x52 — and no skinnum (+0x3A) and no takedamage. The class
 * row's values therefore stand untouched, which now matters: `creworld.c`
 * applies the row's health BEFORE this runs (0x8007E68C and 0x8007E698 write
 * it, and 0x8007E6AC only then calls export 0), so a Berserk that wrote its own
 * would be overwriting them. It does not. The Soldier's spawn — which
 * dispatches on the class row and writes both a health and a skinnum — is the
 * counter-example that shows the difference is a reading and not an oversight.
 *
 * `q2_creature_spawn` runs this last, after the class byte, the scale and the
 * callbacks, which is the loader's order.
 */
static void berserk_spawn(q2_monster *self)
{
    self->mass = 250;                       /* `sh 250, 0x4E`, module+0x1068 */

    q2_cre_set_move(self, BERSERK_STAND);   /* module+0x10E4                 */

    /* One word at entity+0x20, two fields, module+0x10F8..+0x110C: the
     * movetype bits are cleared with 0xFFC3FFFF and set to 0x00140000 (5 << 18)
     * and the solid bits with 0xFFFCFFFF and 0x00020000 (2 << 16). */
    self->movetype = Q2_MOVETYPE_STEP;
    self->solid    = Q2_SOLID_BBOX;
}

/* ------------------------------------------------------------------------- */
const q2_cre_impl q2_cre_berserk = {
    "Berserk",
    {
        berserk_stand,      /*  0 stand       module+0xE74  */
        NULL,               /*  1 idle        — the module installs none: the
                             *                  spawn function never writes
                             *                  entity+0xE4. The fidget it
                             *                  would have driven is think 1
                             *                  instead, on the stand move's
                             *                  first frame. */
        berserk_search,     /*  2 search      module+0x1190 */
        berserk_walk,       /*  3 walk        module+0x11C8 */
        berserk_run,        /*  4 run         module+0xF50  */
        NULL,               /*  5 dodge       — the spawn function writes ZERO
                             *                  to entity+0xF4 (module+0x10F0),
                             *                  so this is the module's own
                             *                  decision rather than a gap. */
        NULL,               /*  6 attack      — likewise zero, to entity+0xF8
                             *                  (module+0x10F4). The Berserk
                             *                  has no ranged attack at all;
                             *                  everything goes through melee. */
        berserk_melee,      /*  7 melee       module+0x11D8 */
        (q2_class_method)(void *)berserk_sight,  /* 8 sight  module+0x1158 */
        NULL,               /*  9 checkattack — the module installs none, so
                             *                  monster_start fills the slot
                             *                  with the shared M_CheckAttack
                             *                  (import +0xE8, and 0x80061B18
                             *                  only writes it when NULL). */
        NULL,               /* 10 bigturn     — the module installs none;
                             *                  entity+0x108 is never written
                             *                  and the threshold at +0x140
                             *                  stays zero, which disables it. */
        /* 11 and 12 take the damage as well as the entity, so they go through
         * the table the same way `sight` does — the slot type is the bare
         * one-argument `q2_class_method` and crebind.c casts each back to its
         * real signature on the way to `m->pain` and `m->die`. */
        (q2_class_method)(void *)berserk_pain,   /* 11 pain  module+0x1224 */
        (q2_class_method)(void *)berserk_die     /* 12 die   module+0x1324 */
    },
    {
        /*
         * Nine methods, from the table the module builds at module+0x1780 and
         * registers for class byte 70 with import +0x118 (module+0x2B8).
         * Slot 0 is written as an explicit zero by module+0x25C, so it is the
         * module's own NULL and not an omission — no frame uses think 0.
         */
        NULL,                       /*  0 — the module writes zero here      */
        berserk_fidget,             /*  1   module+0xDF8                     */
        berserk_stand,              /*  2   module+0xE74, the same function
                                     *      the stand callback uses; no frame
                                     *      references it, but it is in the
                                     *      module's table so it is here      */
        berserk_swing,              /*  3   module+0xE84                     */
        berserk_attack_spike,       /*  4   module+0xEBC                     */
        berserk_run,                /*  5   module+0xF50, the run callback
                                     *      again, and likewise unreferenced
                                     *      by any frame                      */
        berserk_attack_club,        /*  6   module+0xF60                     */
        berserk_strike,             /*  7   module+0x1008 — EMPTY on the disc */
        berserk_dead,               /*  8   module+0x1010, and it must be here
                                     *      or the two death moves' endfunc
                                     *      cannot be resolved: crebind.c maps
                                     *      an endfunc address to a callback or
                                     *      a method, and 0x80101010 is only a
                                     *      method                            */
        /*
         * 9..31 — the module registers nine methods and no more, so these are
         * absent rather than unwritten. crebind.c falls each of them back to
         * the generic trampoline, which finds no decoded think and does
         * nothing, which is what an unregistered slot does on the console too.
         */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL
    },
    berserk_spawn
};
