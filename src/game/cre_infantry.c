/*
 * cre_infantry.c — the Infantry, transcribed from its own module.
 *
 * The Infantry ships on exactly one map, POWER2, as `CreAIBin` module
 * "Infantry": 7,708 bytes relocated to 0x80100000. Every address below is
 * inside that image, and the census that names the moves and think indices is
 * `q2psx-inspect creatures`.
 *
 * This replaces a one-function file. What was here before transcribed the
 * attack callback (correctly) and carried two claims in its header that the
 * disassembly does not support: that think 8 is `call(+0x84)`, the hitscan —
 * it is not, it is the shotgun-rack sound — and that think 11's `call(+0xFC)`
 * is "the spread". `+0xFC` is `walkmonster_start` (0x80062240), and it is not
 * in think 11 at all: the decoder walked off the end of `infantry_smack` at
 * 0x801013EC straight into `infantry_spawn` at 0x801013F0 and attributed the
 * spawn function's four steps to the punch. A decoded Infantry was therefore
 * re-running its own spawn every time it hit you.
 *
 * ---------------------------------------------------------------------------
 * The module's four exports, and which of them this file is
 * ---------------------------------------------------------------------------
 *     export 0  module+0x13F0   infantry_spawn — the callbacks and the setup
 *     export 1  module+0x0DC0   a RENDER-TIME hook, and NOT transcribed
 *     export 2  module+0x02C4   the init: sounds, the method table, the pose
 *     export 3  module+0x1BD4   not code — the move-name table starts there,
 *                               as 20-byte {u16 first, u16 last, char[16]}
 *                               records: 74/85 "Walk", 92/99 "Run",
 *                               184/198 "Attack1", and so on
 *
 * EXPORT 1 IS THE GAP IN THIS FILE, and it is named rather than left implicit.
 * It takes the OBJECT (not the entity) and does two things per frame:
 *
 *   - while the object's animation position at +0x100 lies inside the window
 *     [module+0x1D88, module+0x1E18] it calls import +0x78 `blur_trail_start`
 *     with the position list at module+0x2B8, the four bytes at module+0x2C0
 *     passed by value, and 6 (0x80100DD4..0x80100E3C). The window's two ends
 *     are written by the init from the clip named at module+0x25C — halfwords
 *     +12 and +14 of what import +0x74 `find_move_by_name` returns — and when
 *     that lookup fails the init writes first=1, last=0 (0x80100D34..0x80100D40)
 *     so the window is empty and no trail is ever drawn.
 *
 *   - when the word at object+0x10C is NEGATIVE it calls import +0xA0
 *     `muzzle_flash_light` with the cached muzzle pose at module+0x1D80 packed
 *     into a1/a2 and 120 in a3 (0x80100E44..0x80100E88). Neither the meaning of
 *     object+0x10C nor the meaning of that fourth argument is established —
 *     the import table leaves a3 unnamed too — so only the sign test and the
 *     literal are claimed here.
 *
 * Neither is creature behaviour — they are effects the renderer would run —
 * and this port has no per-object render hook for a creature to install, so
 * both are owed rather than done. The Gunner's file records the same call at
 * +0xA0 from ITS export 1, so this is the second module to want one.
 *
 * ---------------------------------------------------------------------------
 * It is PC Quake II's `infantry.c`, and the evidence is arithmetic
 * ---------------------------------------------------------------------------
 * Nine constants land on id's own numbers without having been aimed at them:
 *
 *   infantry_cock_gun    module+0x122C   `(rand() & 15) + 3 + 7` ticks — id's
 *                                        own odd spelling of 10, kept whole
 *   InfantryMachineGun   module+0x10A4   damage 3, kick 4, hspread 300,
 *                                        vspread 500 — DEFAULT_BULLET_HSPREAD
 *                                        and DEFAULT_BULLET_VSPREAD exactly
 *   InfantryMachineGun   module+0xFA8    the enemy is led by -0.2 of its own
 *                                        velocity, written as -819/4096
 *   infantry_smack       module+0x1358   `5 + rand() % 5` damage and kick 50,
 *                                        id's to the unit. The AIM VECTOR is
 *                                        NOT id's and is not counted here —
 *                                        see the function.
 *   infantry_dodge       module+0x17E8   `random() > 0.25` declines, as
 *                                        `rand() < 8192` accepts
 *   infantry_pain        module+0x1530   skinnum 1 below half health, a three
 *                                        second debounce, no anim on skill 3,
 *                                        `rand() % 2` between two flinches
 *   infantry_die         module+0x1670   deadflag DEAD_DEAD, takedamage
 *                                        DAMAGE_YES, `rand() % 3` deaths
 *   infantry_dead        module+0x112C   MOVETYPE_TOSS, `svflags |= 2`
 *   class table row 12                   health 100, gib_health -40
 *
 * And the twelve-record aim table at module+0x1CE4, which the dying Infantry
 * indexes as it empties its gun on the way down. Read out as degrees (the
 * module stores 4096 to the turn) it is id's `aimangles` verbatim, with the
 * second and third components swapped because on this console the vertical
 * axis is Y and yaw is angles[2]:
 *
 *     module        as degrees      id's aimangles[]
 *     ( 0, 0,  57)  ( 0, 0,  5)     {  0.0,  5.0, 0.0 }
 *     (114, 0, 171)  (10, 0, 15)    { 10.0, 15.0, 0.0 }
 *     (228, 0, 284)  (20, 0, 25)    { 20.0, 25.0, 0.0 }
 *     (284, 0, 398)  (25, 0, 35)    { 25.0, 35.0, 0.0 }
 *     (341, 0, 455)  (30, 0, 40)    { 30.0, 40.0, 0.0 }
 *     (341, 0, 512)  (30, 0, 45)    { 30.0, 45.0, 0.0 }
 *     (284, 0, 569)  (25, 0, 50)    { 25.0, 50.0, 0.0 }
 *     (228, 0, 455)  (20, 0, 40)    { 20.0, 40.0, 0.0 }
 *     (171, 0, 398)  (15, 0, 35)    { 15.0, 35.0, 0.0 }
 *     (455, 0, 398)  (40, 0, 35)    { 40.0, 35.0, 0.0 }
 *     (796, 0, 398)  (70, 0, 35)    { 70.0, 35.0, 0.0 }
 *    (1024, 0, 398)  (90, 0, 35)    { 90.0, 35.0, 0.0 }
 *
 * Twenty-four values, all of them id's. It is not reproduced as code below,
 * for the reason given on `infantry_machinegun`: nothing downstream of this
 * port's fire hook can be handed a direction yet.
 *
 * ---------------------------------------------------------------------------
 * The method table, straight out of export 2
 * ---------------------------------------------------------------------------
 * `infantry_init` (module+0x2C4) builds a twelve-entry table at module+0x1D98
 * and registers it for class byte 81 through import +0x118 (0x80100444). The
 * table is the whole of what an animation frame can call, and every entry is
 * named here:
 *
 *     [ 0]  0            inert — the module stores an EXPLICIT zero here
 *                        (0x801003CC). Plenty of frames carry think byte 0;
 *                        none of them calls anything, because the frame driver
 *                        skips a zero think before it ever indexes the table
 *                        (monster.c, `if (frame->think)`).
 *     [ 1]  0x80100EA0   InfantryMachineGun
 *     [ 2]  0x801010EC   infantry_stand
 *     [ 3]  0x801010FC   infantry_run
 *     [ 4]  0x8010112C   infantry_dead
 *     [ 5]  0x80101158   monster_duck_down
 *     [ 6]  0x801011B4   monster_duck_hold
 *     [ 7]  0x801011FC   monster_duck_up
 *     [ 8]  0x8010122C   infantry_cock_gun
 *     [ 9]  0x8010129C   infantry_fire
 *     [10]  0x80101320   infantry_swing
 *     [11]  0x80101358   infantry_smack
 *
 * Slots 2, 3 and 4 are there because they are also MOVE END callbacks, and the
 * binder resolves an endfunc through this table (crebind.c). That is what makes
 * the three death moves stop rather than loop.
 *
 * ---------------------------------------------------------------------------
 * The dying Infantry keeps shooting, and that is what think 1 is doing
 * ---------------------------------------------------------------------------
 * "Death2" (145-169) carries `0*10 1*12 0*3` — twelve consecutive think 1s in
 * the middle of a death animation. Think 1 is `InfantryMachineGun` itself, and
 * id's `infantry_frames_death2` is exactly ten silent frames, twelve
 * `InfantryMachineGun` frames and three silent ones. The creature falls
 * backwards emptying the magazine, and the aim table above is what sweeps the
 * muzzle across the room while it does. The frame test inside
 * `InfantryMachineGun` — `self->frame == 194` — is the same test id writes as
 * `self->s.frame == FRAME_attak111`; 194 is the eleventh frame of the 184-198
 * attack, so the one branch is "I am shooting on purpose" and the other is
 * "I am shooting because I am being killed".
 *
 * ---------------------------------------------------------------------------
 * Sounds are addressed by their handle, not by an index
 * ---------------------------------------------------------------------------
 * The Soldier's transcription hands the host an index into a table it also
 * publishes. This one hands over the MODULE ADDRESS of the handle instead,
 * which is what `cre_actions.c` already passes for a decoded creature and what
 * the client already resolves first (`q2_creature_world_sound_for_addr`). So a
 * transcribed Infantry and a decoded one name a sound the same way, and no
 * shared header has to grow a second `*_sound_name` accessor.
 *
 * The module registers twelve names into eleven slots, and two of the twelve
 * are ALTERNATIVES rather than a decoder artefact: 0x801007EC asks the bank for
 * `wep_sshotr1b` and, only if that comes back 0, 0x8010089C asks for
 * `wep_shotgr1b` and overwrites the same slot (the branch at 0x801007F8 is what
 * makes it a fallback). The init does the same trick again at 0x80100BFC and
 * 0x80100C34 for the two pain handles and the two death handles: if either of a
 * pair failed to resolve, both are set to the OR of the two, so a bank carrying
 * only one of `inf_pain1`/`inf_pain2` plays that one for both flinches.
 *
 * The twelve name fields run from module+0x1CC on a 12-byte stride and are, in
 * order: `inf_pain1`, `inf_pain2`, `inf_deth1`, `inf_deth2`, `wep_machgf1b`,
 * `wep_sshotr1b`, `wep_shotgr1b`, `inf_atck2`, `inf_atck3`, `inf_sght1`,
 * `inf_srch1`, `inf_idle1`. Note the three `wep_` names are twelve characters
 * and therefore fill the field with no terminator — `q2psx-inspect creatures`
 * prints them one character short (`wep_machgf1`), which is the reader stopping
 * at eleven rather than the disc carrying eleven. The Soldier's own table has
 * the same shape and cre_soldier.c already spells them with the trailing `b`.
 *
 * Three registered sounds are never played by any code in this module:
 * `inf_atck3` (+0x1BC4), `inf_srch1` (+0x1BCC) and `inf_idle1` (+0x1BD0). The
 * last two have an obvious reason — the module installs no search callback at
 * all, and its idle callback is two instructions with no sound in them, where
 * id's `infantry_fidget` plays `sound_idle`. Those are the disc's omissions,
 * not this port's.
 */
#include <stdlib.h>

#include "ai.h"
#include "creature.h"
#include "crebind.h"
#include "monster.h"

/* ------------------------------------------------------------------------- */
/* The move set, by first frame — how every caller in this port names a move  */
/* (crebind.h). The module address of each record is beside it.               */
/* ------------------------------------------------------------------------- */
#define INF_FIDGET       1     /*   1..49   "Fidget"   module+0x1980 */
#define INF_STAND       50     /*  50..71   "Stand1"   module+0x18DC */
#define INF_WALK        74     /*  74..85   "Walk"     module+0x19B4 */
#define INF_RUN         92     /*  92..99   "Run"      module+0x19DC */
#define INF_PAIN1      100     /* 100..109  "Pain1"    module+0x1A0C */
#define INF_PAIN2      110     /* 110..119  "Pain2"    module+0x1A3C */
#define INF_DUCK       120     /* 120..124  "Duck"     module+0x1B30 */
#define INF_DEATH1     125     /* 125..144  "Death1"   module+0x1A88 */
#define INF_DEATH2     145     /* 145..169  "Death2"   module+0x1AE4 */
#define INF_DEATH3     170     /* 170..178  "Death3"   module+0x1B10 */
#define INF_ATTACK_GUN 184     /* 184..198  "Attack1"  module+0x1B70 */
#define INF_ATTACK_FIST 199   /* 199..206  "Attack2"  module+0x1B98 */

/*
 * The frame `InfantryMachineGun` tests for, at 0x80100F90..0x80100F94.
 *
 * 194 is 184 + 10, the eleventh frame of the gun attack — id's FRAME_attak111.
 * Everything else that reaches the shot is a Death2 frame in 155..166, and the
 * module reads its aim table at index `frame - 155` (0x8010103C).
 *
 * Neither is referenced by the code below, and that is not an oversight: the
 * only thing the branch decides is a DIRECTION, and a direction cannot cross
 * this port's fire hook. They are here because the numbers are the module's and
 * a reader checking the disassembly wants them named rather than in prose.
 */
#define INF_FRAME_ATTACK_FIRE 194
#define INF_FRAME_DEATH_FIRE0 155

/* ------------------------------------------------------------------------- */
/* Sound handles, by the module address the init writes them to               */
/* ------------------------------------------------------------------------- */
#define INF_SND_PAIN1   0x80101BA8u  /* inf_pain1                            */
#define INF_SND_PAIN2   0x80101BACu  /* inf_pain2                            */
#define INF_SND_DEATH1  0x80101BB0u  /* inf_deth1                            */
#define INF_SND_DEATH2  0x80101BB4u  /* inf_deth2                            */
#define INF_SND_FIRE    0x80101BB8u  /* wep_machgf1b                         */
#define INF_SND_COCK    0x80101BBCu  /* wep_sshotr1b, falling back to
                                      * wep_shotgr1b — see the header        */
#define INF_SND_SWING   0x80101BC0u  /* inf_atck2                            */
#define INF_SND_ATCK3   0x80101BC4u  /* inf_atck3  — registered, never played */
#define INF_SND_SIGHT   0x80101BC8u  /* inf_sght1                            */
#define INF_SND_SEARCH  0x80101BCCu  /* inf_srch1  — registered, never played */
#define INF_SND_IDLE    0x80101BD0u  /* inf_idle1  — registered, never played */

/* ------------------------------------------------------------------------- */
/* The hooks, shared with cre_soldier.c and cre_actions.c                     */
/* ------------------------------------------------------------------------- */
extern void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
extern void  *q2_cre_sound_user;
/*
 * `q2_cre_fire_fn`, the one-int hook, is no longer declared here: the shot goes
 * through `q2_cre_fire_shot` with the module's own figures. The old hook still
 * exists for the Soldier's three flash tables, which are an index and not a set
 * of figures.
 */
extern void (*q2_cre_melee_fn)(q2_monster *m, const s32 aim[3],
                               s32 damage, s32 kick, void *user);
extern void  *q2_cre_melee_user;

static void inf_play(q2_monster *m, u32 handle)
{
    if (q2_cre_sound_fn)
        q2_cre_sound_fn(m, (int)handle, q2_cre_sound_user);
}

/* The module's random import (+0x14, 0x80089E28) returns 0..32767. */
static s32 inf_rand(void)
{
    return (s32)(rand() & 0x7FFF);
}

/* ------------------------------------------------------------------------- */
/* Entity state, and every place this module writes it                        */
/* ------------------------------------------------------------------------- */
/*
 * Four packed fields live in the two words at entity+0x1C and entity+0x20, and
 * this module writes all four. Their bit positions were read out of the
 * module's own masks and every one lands on an id enumerant:
 *
 *   +0x1C bits 30..31   takedamage   DAMAGE_YES 1 at 0x8010118C (duck down)
 *                                    and 0x8010172C (the ordinary death),
 *                                    DAMAGE_AIM 2 at 0x8010121C (duck up)
 *   +0x20 bits 16..17   solid        SOLID_BBOX 2 at 0x801014B0 (spawn)
 *   +0x20 bits 18..21   movetype     MOVETYPE_STEP 5 at 0x801014B0 (spawn),
 *                                    MOVETYPE_TOSS 7 at 0x801016D8 (the gib
 *                                    arm) and 0x80101148 (infantry_dead)
 *   +0x20 bits 22..23   deadflag     DEAD_DEAD 2 at 0x801016D8 and 0x8010171C,
 *                                    and tested at 0x801016F0..0x801016F8
 *
 * ALL FOUR ARE NOW WRITTEN. This note used to end by saying which offsets had
 * been read and left on the floor, because `q2_monster` carried none of them;
 * it carries all four now, with this exact bit layout documented against the
 * same masks (monster.h), so there is nothing left to excuse. `mass` and
 * `skinnum` arrived in the same pass and are written too — 200 at 0x80101418
 * and 1 at 0x80101570.
 *
 * `deadflag` and this port's `dead` bool model the same fact and are set
 * together. The module's own already-dead guard reads the bitfield
 * (0x801016F0); `q2_monster_damage_reaction` folds the two into one answer —
 * `dead || deadflag == Q2_DEAD_DEAD` (monster.c) — and writes both back when
 * the call returns. Raising both in the same arm is what stops the two from
 * drifting far enough for that OR to matter.
 *
 * `svflags` was always here. Bit 1 of it is now named by monster.h as
 * Q2_SVF_DEADMONSTER, which every transcribed creature's death arm raises, so
 * the local #define this file grew for it is gone.
 */

/* ------------------------------------------------------------------------- */
/* Think functions, in the module's own method-table order                    */
/* ------------------------------------------------------------------------- */

/*
 * [1] InfantryMachineGun — module+0xEA0.
 *
 * The whole of the original is muzzle geometry and one shot. The geometry:
 * take the cached muzzle pose at module+0x1D80 (the init blends animation
 * positions 200 and 205 into it at 0x80100D64), turn it into a model-local
 * point with import +0x2C, rotate it by the object's matrix at +0x2C0 with
 * import +0x28, and add the object's world origin at +0xA4..+0xAC. That gives
 * `start`.
 *
 * Then the aim, and this is the branch worth stating:
 *
 *   frame == 194            vec = enemy->origin - 0.2 * enemy->velocity;
 *   with an enemy           vec[1] += enemy->view_height;
 *                           forward = normalize(vec - start);
 *
 *   frame == 194            AngleVectors(self->angles, forward, right, NULL)
 *   with no enemy
 *
 *   any other frame         AngleVectors(self->angles - aimtable[frame - 155],
 *   (the death burst)                    forward, NULL, NULL)
 *
 * and finally import +0x84, `monster_fire_bullet`, at 0x801010C8. Its five
 * figures are read off the call rather than assumed: a3 = 3 damage, sp+16 = 4
 * kick, sp+20 = 300 hspread, sp+24 = 500 vspread, sp+28 = 0 for the muzzle
 * flash. 300 and 500 are id's DEFAULT_BULLET_HSPREAD and DEFAULT_BULLET_VSPREAD
 * exactly, and they recur unchanged in three other modules.
 *
 * THE FIGURES CROSS NOW; THE DIRECTION STILL DOES NOT. `q2_cre_fire_shot` takes
 * the whole shot — spawner slot, flash, damage, kick and both spreads — so the
 * five numbers above are handed over instead of merely documented. `speed` is
 * zero because a hitscan has none and `count` is 1 because this is not a
 * shotgun; both are the struct's "the weapon does not use this" value rather
 * than a figure that went unread.
 *
 * WHAT IS STILL NOT WRITTEN is `start` and `forward`. The hook carries no
 * origin and no direction, so the muzzle geometry above and the twelve-record
 * aim table in the header stay prose: computing a direction here and dropping
 * it at the call would be dead code pretending to be a transcription. That is
 * the one thing this file still needs from outside itself.
 *
 * ONE DEPARTURE, and it belongs to the shared helper rather than to this file.
 * The module fires from the death animation with NO enemy check at all — the
 * enemy is loaded only on the `frame == 194` arm (0x80100F9C), and the death
 * burst at 0x80101038 never touches it. `q2_cre_fire_shot` opens with the
 * enemy-alive guards that every refire function on the disc carries, so an
 * Infantry dying with no live enemy will not empty its magazine the way the
 * console's does. The guards are right for every other shot on the disc and
 * wrong for this one arm, and they live in crebind.c, not here.
 */
static const q2_cre_shot k_infantry_machinegun = {
    Q2_IMP_FIRE_BULLET,   /* import +0x84, loaded at 0x801010BC            */
    0,                    /* flash    — sp+28, an explicit zero            */
    3,                    /* damage   — a3                                 */
    0,                    /* speed    — a bullet has none                  */
    4,                    /* kick     — sp+16                              */
    300,                  /* hspread  — sp+20, id's DEFAULT_BULLET_HSPREAD */
    500,                  /* vspread  — sp+24, id's DEFAULT_BULLET_VSPREAD */
    1                     /* pellets  — not a shotgun                      */
};

static void infantry_machinegun(q2_monster *self)
{
    q2_cre_fire_shot(self, &k_infantry_machinegun);
}

/*
 * [5] monster_duck_down — module+0x1158.
 *
 * The shared duck helper. It refuses to re-enter, raises AI_DUCKED, sets
 * takedamage to DAMAGE_YES and gives itself one second on the AI clock.
 * creature.h already records that the Gunner and the Infantry carry all three
 * duck helpers byte for byte identical; only the Infantry's copy was read here.
 *
 * The original also shrinks the bounding box and relinks (id's
 * `self->maxs[2] -= 32; gi.linkentity(self)`); this module does neither, so
 * there is nothing to leave out.
 */
static void infantry_duck_down(q2_monster *self)
{
    if (self->aiflags & Q2_AI_DUCKED)
        return;

    self->aiflags   |= Q2_AI_DUCKED;
    self->takedamage = Q2_DAMAGE_YES;   /* 0x8010118C */
    self->pausetime  = q2_level_state.time + Q2_AI_SECONDS(1);
}

/*
 * [6] monster_duck_hold — module+0x11B4.
 *
 * Holds the animation on its current frame until the pause expires. Both arms
 * are stores and both are transcribed; the branch at 0x801011CC is
 * `level.time < pausetime`.
 */
static void infantry_duck_hold(q2_monster *self)
{
    if (q2_level_state.time < self->pausetime)
        self->aiflags |= Q2_AI_HOLD_FRAME;
    else
        self->aiflags &= ~(u32)Q2_AI_HOLD_FRAME;
}

/*
 * [7] monster_duck_up — module+0x11FC.
 *
 * Stands back up: takedamage returns to DAMAGE_AIM and AI_DUCKED clears. Both
 * halves are stores and both are now written; the order is the module's.
 */
static void infantry_duck_up(q2_monster *self)
{
    self->takedamage = Q2_DAMAGE_AIM;   /* 0x8010121C */
    self->aiflags   &= ~(u32)Q2_AI_DUCKED;
}

/*
 * [8] infantry_cock_gun — module+0x122C.
 *
 * Racks the gun and arms the burst timer that [9] then holds the animation
 * against. `(rand() & 15) + 10` is id's `(rand() & 15) + 3 + 7` with the two
 * addends already folded by the compiler — the mask and the total are both
 * id's, which is the check that this is the same function rather than a
 * similar one.
 */
static void infantry_cock_gun(q2_monster *self)
{
    inf_play(self, INF_SND_COCK);
    self->pausetime = q2_level_state.time + (inf_rand() & 0xF) + 10;
}

/*
 * [9] infantry_fire — module+0x129C.
 *
 * Fires, reports, and then holds the frame while the timer [8] set is still
 * running — which is what turns one animation frame into a burst of a random
 * ten to twenty-five ticks. The tail is `monster_duck_hold` inlined, not
 * called: 0x801012D4..0x80101308 is the same pair of stores as [6], against the
 * same `pausetime`. One field doing duty for both the duck and the burst is
 * id's own economy, not a misread.
 */
static void infantry_fire(q2_monster *self)
{
    infantry_machinegun(self);
    inf_play(self, INF_SND_FIRE);
    infantry_duck_hold(self);
}

/* [10] infantry_swing — module+0x1320. The wind-up before the punch. */
static void infantry_swing(q2_monster *self)
{
    inf_play(self, INF_SND_SWING);
}

/*
 * [11] infantry_smack — module+0x1358.
 *
 * The punch. `fire_hit` (import +0xEC) with the aim vector passed BY VALUE in
 * a1/a2/a3 and the damage and kick on the stack at sp+16 and sp+20.
 *
 * 1020 IS THE CONSOLE'S OWN MELEE REACH and nothing else. It is the same
 * constant `range` compares against — read out of the executable at 0x8005EF84
 * as 1020^2-1 and recorded in monster.h as `Q2_MELEE_DISTANCE` — so the module
 * and the range test agree with each other, which is the whole of the check.
 *
 * It is NOT derived from id's `MELEE_DISTANCE`, which is 80. The AI's scale of
 * 12 would make that 960, and 1020 is not 960; running the division the other
 * way gives 85, and 85 is not id's number either, so there is no quotient here
 * to attribute to id. An earlier pass of this file called 85 id's constant.
 * The Berserk and the Arachner spell the same 1020 the same way.
 *
 * The damage and the kick ARE id's to the unit: `5 + (rand() % 5)` and 50.
 *
 * The original tests `fire_hit`'s return and plays a hit sound on true; this
 * module does neither, and registers no name for one — id's `sound_punch_hit`
 * is a `melee2` sound (the exact path is not checkable from this disc) and no
 * `melee2` appears anywhere in the module's twelve registrations, which is the
 * half of that claim the disc does settle.
 */
static const s32 k_infantry_smack_aim[3] = { Q2_MELEE_DISTANCE, 0, 0 };

static void infantry_smack(q2_monster *self)
{
    if (q2_cre_melee_fn)
        q2_cre_melee_fn(self, k_infantry_smack_aim,
                        5 + (inf_rand() % 5), 50, q2_cre_melee_user);
}

/*
 * [4] infantry_dead — module+0x112C.
 *
 * The end callback on all three death moves, and also method 4, which is how
 * the binder resolves it (crebind.c walks the callback set and then the method
 * table). It drops the body to MOVETYPE_TOSS and marks it a dead monster so the
 * world stops treating it as an obstacle.
 *
 * id also shrinks the box to (-16,-16,-24)..(16,16,-8), zeroes nextthink and
 * relinks. This module does none of the three, so the transcription is the
 * whole function rather than part of it.
 */
static void infantry_dead(q2_monster *self)
{
    self->movetype = Q2_MOVETYPE_TOSS;      /* 0x80101148 */
    self->svflags |= Q2_SVF_DEADMONSTER;    /* 0x80101154 */
}

/* ------------------------------------------------------------------------- */
/* The monsterinfo callbacks                                                  */
/* ------------------------------------------------------------------------- */

/* [2] and callback 0: infantry_stand — module+0x10EC. Two instructions. */
static void infantry_stand(q2_monster *self)
{
    q2_cre_set_move(self, INF_STAND);
}

/*
 * callback 1: infantry_fidget — module+0x1510.
 *
 * Two instructions, and that is the whole of it — where id's `infantry_fidget`
 * also plays `sound_idle`. The module registers `inf_idle1` at +0x1BD0 and
 * never reaches it from anywhere, so the sound is on the disc and the call to
 * it is not. Stated rather than restored: adding the play would be inventing a
 * line the module does not have.
 *
 * The Fidget move (1-49) ends on `infantry_stand`, so the creature falls back
 * into its stand loop afterwards.
 */
static void infantry_fidget(q2_monster *self)
{
    q2_cre_set_move(self, INF_FIDGET);
}

/* callback 3: infantry_walk — module+0x1520. Two instructions. */
static void infantry_walk(q2_monster *self)
{
    q2_cre_set_move(self, INF_WALK);
}

/*
 * [3] and callback 4: infantry_run — module+0x10FC.
 *
 * One branch: a creature holding its ground stands instead of running. That is
 * the same opening every transcribed creature's run callback has, and here it
 * is the entire function.
 */
static void infantry_run(q2_monster *self)
{
    if (self->aiflags & Q2_AI_STAND_GROUND)
        q2_cre_set_move(self, INF_STAND);
    else
        q2_cre_set_move(self, INF_RUN);
}

/*
 * callback 5: infantry_dodge — module+0x17E8.
 *
 * Ducks one time in four, adopts its attacker if it had no enemy, and drops
 * into the five-frame Duck move. 8192 is a quarter of 32768, and the sense of
 * the test matches id exactly: id returns when `random() > 0.25`, this one
 * proceeds when `rand() < 8192`.
 *
 * The third argument — id's `eta`, how long until the shot arrives — is
 * accepted and ignored, which is what the module does: a2 is never read.
 *
 * IT HAS NO CALLER, and that is the console's doing rather than this port's.
 * crebind.c records the sweep: nothing in `SLES_015.34` loads entity+0xF4 on
 * any base but `sp`, so the slot the module writes is never called. The handler
 * is written and installed because the module writes it; it will fire the day
 * something invokes slot 5, and not before.
 */
static void infantry_dodge(q2_monster *self, q2_monster *other, s32 eta)
{
    (void)eta;

    if (inf_rand() >= 8192)         /* 8192/32768 — id's `random() > 0.25` */
        return;

    if (!self->enemy)
        self->enemy = other;

    q2_cre_set_move(self, INF_DUCK);
}

/*
 * callback 6: infantry_attack — module+0x1848.
 *
 * One range test and no roll: melee range takes the fists, everything else
 * takes the gun. `import[+0xB4]` is `q2_range` (0x8005EF84), and a return of
 * zero is Q2_RANGE_MELEE.
 *
 *     184-198  "Attack1"  0*3 8 0*6 9 0*4    cock at index 3, fire at index 10
 *     199-206  "Attack2"  0*2 10 0*2 11 0*2  swing at index 2, smack at index 5
 *
 * id's `infantry_frames_attack1` is fifteen frames with `infantry_cock_gun` on
 * the fourth and `infantry_fire` on the eleventh — the same length and the
 * same two positions.
 *
 * The NULL-enemy arm is this port's, not the module's: the original calls
 * `range(self, self->enemy)` with no guard because it is only ever reached with
 * an enemy set. Falling back to the generic handler is the safe failure rather
 * than a dereference.
 */
static void infantry_attack(q2_monster *m)
{
    if (!m)
        return;

    if (!m->enemy) {
        q2_cre_generic_attack(m);
        return;
    }

    q2_cre_set_move(m, q2_range(m, m->enemy) == Q2_RANGE_MELEE
                           ? INF_ATTACK_FIST : INF_ATTACK_GUN);
}

/*
 * callback 8: infantry_sight — module+0x1638.
 *
 * Plays `inf_sght1` and nothing else. No skill test, no opening move — unlike
 * the Soldier, which breaks into a running shot when it spots you from a
 * distance.
 */
static void infantry_sight(q2_monster *self, q2_monster *other)
{
    (void)other;
    inf_play(self, INF_SND_SIGHT);
}

/*
 * callback 11: infantry_pain — module+0x1530.
 *
 * id's `infantry_pain` line for line, in id's order:
 *
 *     health < max_health/2   ->  skinnum = 1
 *     level.time < pain_debounce_time (entity+0xA8)  ->  return
 *     pain_debounce_time = level.time + 3 s
 *     skill == 3              ->  return, no pain anims in nightmare
 *     rand() % 2              ->  Pain1 + inf_pain1, or Pain2 + inf_pain2
 *
 * Four details worth naming.
 *
 * THE DAMAGE IS PASSED AND THIS HANDLER DOES NOT READ IT. `pain` now carries
 * T_Damage's own amount, and other modules branch on it — the Gunner, the Tank
 * Commander and the Berserk all do. Not this one.
 *
 * `pain` is called as (self, other, kick, damage), so the amount arrives in a3
 * and not in a1: that is cre_berserk.c's reading rather than a guess from id's
 * prototype, established by the Berserk's own module+0x1264 stashing a3 in s2
 * and module+0x12D0 comparing it. `infantry_pain` touches NONE of a1, a2 or a3
 * anywhere between 0x80101530 and 0x80101634 — a0 goes to s0 at 0x80101538,
 * every load after that is off s0 or off the import table, and a1's first
 * appearance is the WRITE at 0x801015F0 setting up the sound call. The
 * argument is accepted and voided rather than quietly used for something it
 * does not decide.
 *
 * The skin write is `= 1`, not `|= 1`: the Infantry has one skin pair and the
 * module stores the literal into the halfword at entity+0x3A (0x80101570).
 * `skinnum` is a real field now, so the store is transcribed; `m->hurt` stays
 * beside it as the port's own name for the wounded low bit, per monster.h, and
 * the two are raised together so they cannot disagree.
 *
 * The health it compares is the OBJECT's, at object+0x108 — the module reads
 * `lh v0, 264(entity+0x24)` — while `max_health` is the entity's own halfword
 * at +0x50. Both are one field in `q2_monster`.
 *
 * There is NO dead guard, and this is a deliberate difference from
 * `soldier_pain`, which carries one. It does not need one: this port routes
 * damage through `q2_monster_damage_reaction`, which reaches `pain` only while
 * health is above zero and `die` otherwise, so a corpse never arrives here.
 * Adding a guard the module does not have would be a second thing claiming to
 * own the same fact.
 */
static void infantry_pain(q2_monster *self, s16 damage)
{
    (void)damage;   /* a3, and no instruction in the original reads it */

    if (self->health < self->max_health / 2) {
        self->skinnum = 1;      /* 0x80101570 */
        self->hurt    = true;
    }

    if (q2_level_state.time < self->pain_debounce)
        return;

    self->pain_debounce = q2_level_state.time + Q2_AI_SECONDS(3);

    if (q2_cre_skill() == 3)
        return;

    if (inf_rand() % 2 == 0) {
        q2_cre_set_move(self, INF_PAIN1);
        inf_play(self, INF_SND_PAIN1);
    } else {
        q2_cre_set_move(self, INF_PAIN2);
        inf_play(self, INF_SND_PAIN2);
    }
}

/*
 * callback 12: infantry_die — module+0x1670.
 *
 * Three arms, in the module's order.
 *
 * GIBBED, when health has fallen to or below `gib_health` (entity+0x52, read as
 * `gib_health < health` at 0x8010169C so the boundary is inclusive). It sets
 * deadflag to DEAD_DEAD and movetype to MOVETYPE_TOSS in one store at
 * 0x801016D8, raises SVF_DEADMONSTER at 0x801016E4, and returns — with no sound
 * and no animation at all. It does not zero `nextthink` either, which the
 * Gunner's and the Berserk's gib arms both do. That is the one place this
 * creature differs audibly from the Soldier, which plays `msc_udeath` here:
 * the Infantry registers no such name, so the silence is the disc's.
 *
 * `gibbed` IS THE PORT'S FIELD, NOT THE MODULE'S. The module's gib arm makes
 * exactly two stores — entity+0x20 at 0x801016D8 and entity+0x40 at
 * 0x801016E4 — and has no third field for "was blown apart"; it does not need
 * one, because on the console the body is simply gone. monster.h keeps the
 * distinction ("a body already dead can still be gibbed by a later
 * explosion"), and cre_soldier.c, cre_berserk.c and cre_tankcomm.c all raise
 * the same flag in the same place. So that one line models the arm rather than
 * transcribing a store, and it is the only line here that does.
 *
 * There is no already-gibbed guard, because the module has none and every
 * store in the arm is idempotent — a body hit again past `gib_health` runs it
 * a second time on the console and nothing changes. The Gunner's file carries
 * such a guard for one reason this creature does not have: a sound to suppress.
 *
 * ALREADY DEAD, tested as `deadflag == 2` at 0x801016F0..0x801016F8 — id's
 * `if (self->deadflag == DEAD_DEAD) return;`. The guard here reads `m->dead`
 * rather than the new `deadflag` field, deliberately: the caller has already
 * folded the two — `q2_monster_damage_reaction` takes `dead || deadflag ==
 * DEAD_DEAD` and only reaches `die` when that is false — so by the time
 * control arrives here either spelling gives the same answer, and the bool is
 * the one this port's damage path is written against. Both are SET below, in
 * the same arm, so they cannot come apart.
 *
 * THE DAMAGE IS PASSED AND NOT READ. `die` now carries T_Damage's amount and
 * the Berserk's picks its long death off it. `die` is called as (self,
 * inflictor, attacker, damage), so the amount is in a3 — the same position the
 * Berserk's die reads it from (module+0x1350 into s1, compared at +0x13FC) —
 * and this one touches none of a1, a2 or a3 between 0x80101670 and
 * 0x801017E4. a1 appears only as the write at 0x8010177C and its two twins
 * feeding the sound call, a2 only as the `mfhi` of the `rand() % 3` at
 * 0x80101758, and a3 nowhere at all. The argument is voided rather than used.
 *
 * THE ORDINARY DEATH, which sets deadflag DEAD_DEAD and takedamage DAMAGE_YES
 * (so a body can still be shot apart), then rolls `rand() % 3`:
 *
 *     2 -> Death1 (125-144) with inf_deth2
 *     1 -> Death2 (145-169) with inf_deth1
 *     0 -> Death3 (170-178) with inf_deth2
 *
 * The pairing is the module's, read one arm at a time from 0x80101774,
 * 0x80101794 and 0x801017AC, and two of the three arms share `inf_deth2`,
 * which is why it looks lopsided. WHICH `n` id GIVES EACH DEATH IS NOT SETTLED
 * HERE and no claim is made about it: the disassembly shows this module's
 * numbering and nothing about the other side. An earlier draft of this note
 * asserted a difference — and put two of id's arms on one `sound_die` — with
 * no way to check either.
 *
 * This module writes NO SKIN at all in `die`, and that is a read rather than a
 * comparison: there is no store to entity+0x3A anywhere between 0x80101670 and
 * 0x801017E4. The wounded skin only ever comes from `pain`.
 */
static void infantry_die(q2_monster *self, s16 damage)
{
    s32 n;

    (void)damage;   /* a3, and no arm of the original reads it */

    if (self->health <= self->gib_health) {
        self->gibbed   = true;  /* the port's field, not a store; see above */
        self->dead     = true;  /* the bool beside deadflag; see the note   */
        self->deadflag = Q2_DEAD_DEAD;          /* 0x801016D8 */
        self->movetype = Q2_MOVETYPE_TOSS;      /* the same store */
        self->svflags |= Q2_SVF_DEADMONSTER;    /* 0x801016E4 */
        return;
    }

    if (self->dead)
        return;

    self->dead       = true;
    self->deadflag   = Q2_DEAD_DEAD;    /* 0x8010171C */
    self->takedamage = Q2_DAMAGE_YES;   /* 0x8010172C */

    n = inf_rand() % 3;
    if (n == 2) {
        q2_cre_set_move(self, INF_DEATH1);
        inf_play(self, INF_SND_DEATH2);
    } else if (n == 1) {
        q2_cre_set_move(self, INF_DEATH2);
        inf_play(self, INF_SND_DEATH1);
    } else {
        q2_cre_set_move(self, INF_DEATH3);
        inf_play(self, INF_SND_DEATH2);
    }
}

/* ------------------------------------------------------------------------- */
/*
 * `infantry_spawn` — module+0x13F0, export 0.
 *
 * Everything it does beyond writing the nine callbacks and the one explicit
 * zero, in the module's own order, with what happens to each:
 *
 *   sh 200, 0x4E(self)      mass = 200 — id's `self->mass = 200`. WRITTEN at
 *                           0x80101418. `q2_monster` has a `mass` field now and
 *                           nothing else in this tree assigns it: the decoder
 *                           reads the module's 200 into `q2_creature.mass` and
 *                           `q2_creature_spawn` does not copy it across, so
 *                           this hook is the only thing that puts it on a live
 *                           entity.
 *   +0x20 bitfields         solid = SOLID_BBOX and movetype = MOVETYPE_STEP,
 *                           one store at 0x801014B0 (mask 0xFFFCFFFF then
 *                           or 0x20000, mask 0xFFC3FFFF then or 0x140000).
 *                           Both WRITTEN.
 *   call import +0x1C       link_entity(self, 5) then link_entity(self, 133) —
 *                           copy origin and yaw, then again without the second
 *                           position copy (bit 7). NOT made: the port's link is
 *                           a hook the creature world drives at wake, where
 *                           `q2_monster_start_go` links with 4, and a second
 *                           copy of a pose that has not changed is not what
 *                           this hook is for. Named rather than dropped.
 *   sw module+0x18DC, 0xD8  currentmove = Stand1. Not made, and covered: that
 *                           record is the Stand1 move at frame 50, and
 *                           `q2_monster_start_go` calls `stand`, which installs
 *                           exactly it.
 *   sb 10, 0x13B(self)      speed_scale = 10. `q2_creature_spawn` already reads
 *                           this out of the decoded module and writes it.
 *   call import +0xFC       walkmonster_start(self). The port's equivalent is
 *                           `q2_monster_start_go`, which the creature world
 *                           already runs.
 *
 * WHAT IT DOES NOT WRITE, checked rather than assumed: no health, no skinnum
 * and no takedamage. There is no store to entity+0x1C anywhere in the function,
 * so the initial takedamage is the engine's and not this module's; the
 * Infantry's 100 health and -40 gib threshold come from class table row 12,
 * which `creworld.c` applies BEFORE this hook runs — the loader's own order,
 * health at 0x8007E68C and 0x8007E698 and export 0 at 0x8007E6AC.
 *
 * This note used to end by saying `q2_cre_impl.spawn` had no caller anywhere in
 * the tree and that a hook written here would be dead code. `q2_creature_spawn`
 * runs it now, last, so the three stores above have a home.
 */
static void infantry_spawn(q2_monster *self)
{
    self->mass     = 200;                   /* 0x80101418 */
    self->solid    = Q2_SOLID_BBOX;         /* 0x801014B0 */
    self->movetype = Q2_MOVETYPE_STEP;      /* the same store */
}

const q2_cre_impl q2_cre_infantry = {
    "Infantry",
    {
        infantry_stand,     /*  0 stand       module+0x10EC */
        infantry_fidget,    /*  1 idle        module+0x1510 */
        NULL,               /*  2 search      — the module installs none, which
                             *                  is why inf_srch1 is registered
                             *                  and never played */
        infantry_walk,      /*  3 walk        module+0x1520 */
        infantry_run,       /*  4 run         module+0x10FC */
        (q2_class_method)(void *)infantry_dodge,  /* 5 dodge  module+0x17E8 */
        infantry_attack,    /*  6 attack      module+0x1848 */
        NULL,               /*  7 melee       — the module installs none, and
                             *                  says so: infantry_spawn writes
                             *                  a literal zero to entity+0xFC
                             *                  at 0x80101498. The punch is an
                             *                  ATTACK move, not a melee
                             *                  callback. */
        (q2_class_method)(void *)infantry_sight,  /* 8 sight  module+0x1638 */
        NULL,               /*  9 checkattack — the module installs none, so
                             *                  monster_start leaves the
                             *                  engine's M_CheckAttack in place
                             *                  (0x80061B18) */
        NULL,               /* 10 bigturn     — the module installs none */
        /* Both carry T_Damage's own amount now and so take an extra
         * argument, which is why they are cast in like dodge and sight. */
        (q2_class_method)(void *)infantry_pain,   /* 11 pain module+0x1530 */
        (q2_class_method)(void *)infantry_die     /* 12 die  module+0x1670 */
    },
    {
        NULL,                   /*  0 — the module's own slot 0 is zero      */
        infantry_machinegun,    /*  1 */
        infantry_stand,         /*  2 — also the Fidget move's end callback  */
        infantry_run,           /*  3 — also five moves' end callback        */
        infantry_dead,          /*  4 — the three death moves' end callback  */
        infantry_duck_down,     /*  5 */
        infantry_duck_hold,     /*  6 */
        infantry_duck_up,       /*  7 */
        infantry_cock_gun,      /*  8 */
        infantry_fire,          /*  9 */
        infantry_swing,         /* 10 */
        infantry_smack,         /* 11 */
        /* 12..31 — the module's table is twelve entries long and no frame
         * carries a think byte above 11. */
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
    },
    infantry_spawn
};
