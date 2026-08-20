/*
 * monster.h — the creature entity model and the animation frame driver.
 *
 * The PSX AI is a port of PC Quake II's monster code, and this is no longer an
 * inference: the whole framework has been read out of the executable and the
 * numbers land on id's own constants once the world scale is applied. Every
 * address quoted below is in `SLES_015.34`.
 *
 * ---------------------------------------------------------------------------
 * How a creature is driven
 * ---------------------------------------------------------------------------
 * `monster_start_go` (0x80061BA4) points the entity's think at
 * `M_MoveFrame` (0x8006175C) and sets `nextthink = level.time + 1`. From then
 * on the engine calls it once per AI tick, and it does three things:
 *
 *   1. advance the animation by one frame within the current move,
 *   2. run the frame's movement verb with the frame's distance,
 *   3. run the frame's think index against the creature's own method table.
 *
 * There is no separate `monster_think` wrapper — the PSX put the ground and
 * water checks in the generic entity physics, so the think IS the frame driver.
 *
 * ---------------------------------------------------------------------------
 * The method table, which is what makes a creature a creature
 * ---------------------------------------------------------------------------
 * A creature module does not export named callbacks. It registers handlers
 * into a two-level table built by 0x80061D10:
 *
 *     void (*classMethods[256][32])(edict_t *)      base 0x800D519C
 *
 * Every one of the 256 class slots starts out pointing at one shared 32-entry
 * table (0x800D559C) filled with a do-nothing handler (0x80062624), so an
 * unregistered class is inert rather than a crash. A module claims a slot by
 * calling the setter at 0x80061DE4 — which the loader hands it as import
 * +0x118 — once per method it implements.
 *
 * The 32 slots split in two at index 26:
 *
 *     [0..25]   `think` handlers, selected by an animation frame's think byte
 *     [26..31]  the creature's OWN movement verbs, selected by a frame whose
 *               ai byte has bit 7 set: index = 26 + (ai & 0x7F)
 *
 * That is why the frame census finds `ai` in {0..5} and {129..133} and `think`
 * in 0..19: two disjoint ranges into one table. A local verb is called with
 * only the entity — it gets no distance, because a creature that steers itself
 * does not want the frame's.
 *
 * ---------------------------------------------------------------------------
 * Animation
 * ---------------------------------------------------------------------------
 * A move is a frame range plus a per-frame script and an optional end callback:
 *
 *     q2_mmove   16 bytes  { s32 first; s32 last; frames*; endfunc }
 *     q2_mframe   3 bytes  { u8 ai; s8 dist; u8 think }
 *
 * The three-byte frame is worth noticing: it is not padded to four. A reader
 * that assumes alignment walks off the end of every animation. 0x80061890
 * computes the frame address as `frames + index*3` with an explicit
 * shift-and-add, which is the proof.
 *
 * Distance travelled on a frame (0x80061930..0x8006196C):
 *
 *     dist = (aiflags & AI_HOLD_FRAME) ? 0 : (dist * scale * 12) / 10
 *
 * The /10 is a magic-number division by 10 in the original, not a shift.
 *
 * ---------------------------------------------------------------------------
 * Scale, angles and axes — three things a port gets wrong silently
 * ---------------------------------------------------------------------------
 * **World scale is 12, not 10.** Every length constant in the AI is id's own
 * number times twelve, and they were read independently: melee 1020, near 6000
 * (id 500), mid 12000 (id 1000), step height 216 (id 18), chase-direction
 * deadband 120 (id 10), flyer height bands 480 and 360 (id 40 and 30), the
 * corner-peek sidestep 192 (id 16). Six unrelated constants agreeing on one
 * factor is not a coincidence. Note this is the AI's scale; other subsystems
 * were established at 10, so the two are genuinely different and neither is a
 * mistake in the other's reading.
 *
 * **Angles are 4096 to the turn.** `anglemod` (0x8005C7B8) is one instruction:
 * `and 0xFFF`. 180 degrees is 2048 and 90 is 1024, and the sidestep in
 * ai_run_slide is literally `± 1024`.
 *
 * **The vertical axis is Y, and it points DOWN.** Movement is built as
 * (sin, 0, cos) so the ground plane is XZ, yaw is atan2(x, z), and the height
 * comparisons in SV_movestep have exactly id's four branches with their signs
 * mirrored. A port that assumes Z-up gets monsters that will not climb steps.
 *
 * ---------------------------------------------------------------------------
 * The clock
 * ---------------------------------------------------------------------------
 * 10 ticks per second. Established arithmetically before and confirmed by the
 * AI: `idle_time = level.time + 150 + random()*150` where id waits 15 + 15
 * seconds, `search_time = level.time + 50` where id waits 5, the dead-monster
 * pause of 1e9 where id uses 1e8. This is NOT the 25 Hz simulation tick.
 */
#ifndef Q2PSX_MONSTER_H
#define Q2PSX_MONSTER_H

#include "q2psx.h"

/* The AI clock: 10 ticks per second. */
#define Q2_AI_HZ 10
#define Q2_AI_SECONDS(s) ((s) * Q2_AI_HZ)

/* The AI's world scale. id's units times this gives the console's. */
#define Q2_AI_SCALE 12
#define Q2_AI_UNITS(pc) ((pc) * Q2_AI_SCALE)

/* ------------------------------------------------------------------------- */
/* Animation records                                                          */
/* ------------------------------------------------------------------------- */
#define Q2_MFRAME_SIZE 3        /* NOT 4 — the record is unpadded */
#define Q2_MMOVE_SIZE 16

/* ai >= this indexes the creature's own verb table rather than the shared one */
#define Q2_AI_LOCAL_FLAG 0x80

/* Where the local verbs start in a class's 32-entry method table. Read from
 * the `lw v0, 104(v0)` at 0x800618D8 — 104 bytes is 26 pointers. */
#define Q2_CLASS_METHOD_COUNT 32
#define Q2_CLASS_VERB_BASE    26
#define Q2_CLASS_COUNT        256

typedef struct q2_mframe {
    u8 ai;
    s8 dist;
    u8 think;
} q2_mframe;

struct q2_monster;

/* A move's end callback. Returning is enough; it may install a new move. */
typedef void (*q2_endfunc)(struct q2_monster *m);

typedef struct q2_mmove {
    s32 first_frame;
    s32 last_frame;
    const q2_mframe *frames;
    q2_endfunc endfunc;         /* NULL when the move just loops */

    /* Kept so a move decoded from a module image can still be traced back. */
    u32 image_offset;
    u32 frames_offset;
    u32 endfunc_offset;
} q2_mmove;

/* The six shared movement verbs, in the executable's own table order
 * (0x800D561C, filled at 0x80061D98). Slot 0 is null, and there is
 * deliberately no turn verb — a precise negative, since the PC lineage has one
 * and its absence would otherwise look like an oversight. */
typedef enum q2_ai_verb {
    Q2_AI_NONE = 0,
    Q2_AI_STAND,        /* 0x8005CDA8 */
    Q2_AI_WALK,         /* 0x8005ED6C */
    Q2_AI_RUN,          /* 0x8005E350 */
    Q2_AI_CHARGE,       /* 0x8005EE84 */
    Q2_AI_MOVE,         /* 0x8005ED44 */
    Q2_AI_VERB_COUNT
} q2_ai_verb;

extern const char *const q2_ai_verb_names[Q2_AI_VERB_COUNT];

/* ------------------------------------------------------------------------- */
/* aiflags — entity+0xDC. Values are PC Quake II's, confirmed one by one       */
/* against the branches that test them.                                       */
/* ------------------------------------------------------------------------- */
enum {
    Q2_AI_STAND_GROUND      = 0x00000001,  /* ai_stand   0x8005CDD8 */
    Q2_AI_TEMP_STAND_GROUND = 0x00000002,  /* ai_stand   0x8005CE5C */
    Q2_AI_SOUND_TARGET      = 0x00000004,  /* ai_run     0x8005E398 */
    Q2_AI_LOST_SIGHT        = 0x00000008,  /* ai_run     0x8005E638 */
    Q2_AI_PURSUIT_LAST_SEEN = 0x00000010,  /* ai_run     0x8005E6C0 */
    Q2_AI_PURSUE_NEXT       = 0x00000020,  /* ai_run     0x8005E660 */
    Q2_AI_PURSUE_TEMP       = 0x00000040,  /* ai_run     0x8005E688 */
    Q2_AI_HOLD_FRAME        = 0x00000080,  /* M_MoveFrame 0x8006184C */
    Q2_AI_GOOD_GUY          = 0x00000100,  /* FindTarget 0x8005D354 */
    Q2_AI_BRUTAL            = 0x00000200,  /* checkattack 0x8005DDE8 */
    Q2_AI_NOSTEP            = 0x00000400,  /* SV_movestep 0x8005FF0C */
    Q2_AI_DUCKED            = 0x00000800,
    Q2_AI_COMBAT_POINT      = 0x00001000,  /* ai_run     0x8005E390 */
    Q2_AI_MEDIC             = 0x00002000,  /* checkattack 0x8005DDE0 */
    Q2_AI_RESURRECTING      = 0x00004000,
    /* Not in vanilla 3.20's header but plainly present here: while it is set,
     * ai_charge and ai_stand leave the facing alone. 0x8005EEF4. */
    Q2_AI_MANUAL_STEERING   = 0x00010000
};

/* ------------------------------------------------------------------------- */
/* Entity flags — entity+0x20, a u16. id's FL_* values exactly.               */
/* ------------------------------------------------------------------------- */
enum {
    Q2_FL_FLY            = 0x0001,
    Q2_FL_SWIM           = 0x0002,
    Q2_FL_IMMUNE_LASER   = 0x0004,
    Q2_FL_INWATER        = 0x0008,
    Q2_FL_GODMODE        = 0x0010,
    Q2_FL_NOTARGET       = 0x0020,  /* FindTarget 0x8005D50C */
    Q2_FL_IMMUNE_SLIME   = 0x0040,
    Q2_FL_IMMUNE_LAVA    = 0x0080,
    Q2_FL_PARTIALGROUND  = 0x0100,  /* SV_NewChaseDir 0x800607C8 */
    /*
     * Both of these are read out of T_Damage rather than assumed from id's
     * header, and both land on id's own bit anyway: 0x8006291C tests 0x800 and
     * forces knockback to zero, and 0x80062994 SETS it on anything that dies
     * with SVF_MONSTER or a client block. 0x200 and 0x400 sit between and have
     * no reader found, so they stay unnamed.
     */
    Q2_FL_NO_KNOCKBACK   = 0x0800   /* T_Damage 0x8006291C, 0x80062994 */
};

/* spawnflags — entity+0x1C. Only the two the AI reads are named. */
#define Q2_SPAWNFLAG_AMBUSH  0x00040000   /* bit 18, FindTarget 0x8005D3D4 */
#define Q2_SVFLAG_INUSE      0x20000000   /* bit 29, tested everywhere      */

/* svflags — entity+0x40. */
/*
 * Bit 1 is id's SVF_DEADMONSTER, and it went unnamed here for as long as
 * nothing wrote it. Five of the seven transcribed modules do — every `*_dead`
 * and every gib arm sets it, and `q2_M_MoveFrame` was already TESTING the bare
 * literal 2. A body with it set stops being solid to other monsters, which is
 * how a corpse in a doorway does not block the creature behind it.
 */
#define Q2_SVF_DEADMONSTER   0x00000002
#define Q2_SVF_MONSTER       0x00000004   /* FindTarget 0x8005D4D0 */

/* ------------------------------------------------------------------------- */
/*
 * entity+0x20 IS A PACKED WORD, not a `u16 flags`
 * ------------------------------------------------------------------------- *
 * Every one of the seven creature modules writes it as a bitfield, and the
 * layout came out the same from seven independent reads:
 *
 *     bits  0..15   flags       FL_* — what this header already names
 *     bits 16..17   solid       SOLID_BBOX 2 at spawn
 *     bits 18..21   movetype    MOVETYPE_STEP 5 at spawn, MOVETYPE_TOSS 7 dead
 *     bits 22..23   deadflag    DEAD_DEAD 2, which every `die` tests to refuse
 *                               a second entry
 *
 * and entity+0x1C carries one more above the SVFLAG_INUSE bit this header
 * already names:
 *
 *     bits 30..31   takedamage  DAMAGE_YES 1, DAMAGE_AIM 2
 *
 * Six values, and all six are id's own enumerants — SOLID_BBOX 2,
 * MOVETYPE_STEP 5, MOVETYPE_TOSS 7, DEAD_DEAD 2, DAMAGE_YES 1, DAMAGE_AIM 2.
 * Nothing in the reads was arranged to produce that; the bit POSITIONS were
 * found first, from the shift-and-mask each module uses, and the values fell
 * out afterwards. `T_Damage` reads two of them independently (0x800629D8 for
 * deadflag, 0x80062A34 for movetype) and agrees.
 */
#define Q2_SOLID_SHIFT       16
#define Q2_SOLID_MASK        0x3u
#define Q2_MOVETYPE_SHIFT    18
#define Q2_MOVETYPE_MASK     0xFu
#define Q2_DEADFLAG_SHIFT    22
#define Q2_DEADFLAG_MASK     0x3u
#define Q2_TAKEDAMAGE_SHIFT  30
#define Q2_TAKEDAMAGE_MASK   0x3u

enum { Q2_SOLID_NOT = 0, Q2_SOLID_TRIGGER = 1, Q2_SOLID_BBOX = 2,
       Q2_SOLID_BSP = 3 };
enum { Q2_MOVETYPE_NONE = 0, Q2_MOVETYPE_NOCLIP = 1, Q2_MOVETYPE_PUSH = 2,
       Q2_MOVETYPE_STOP = 3, Q2_MOVETYPE_WALK = 4, Q2_MOVETYPE_STEP = 5,
       Q2_MOVETYPE_FLY = 6, Q2_MOVETYPE_TOSS = 7 };
enum { Q2_DEAD_NO = 0, Q2_DEAD_DYING = 1, Q2_DEAD_DEAD = 2,
       Q2_DEAD_RESPAWNABLE = 3 };
enum { Q2_DAMAGE_NO = 0, Q2_DAMAGE_YES = 1, Q2_DAMAGE_AIM = 2 };

/* attack_state, using the PC enum's values. Stored as a 3-bit field at bits
 * 18..20 of the word at entity+0x138 (0x8005E254). */
typedef enum q2_attack_state {
    Q2_AS_NONE     = 0,
    Q2_AS_STRAIGHT = 1,
    Q2_AS_SLIDING  = 2,
    Q2_AS_MELEE    = 3,
    Q2_AS_MISSILE  = 4
} q2_attack_state;

/* Content masks, read from the trace call sites. Both are id's own values. */
#define Q2_MASK_PLAYERSOLID   0x02010003u   /* ai_run corner peek 0x8005E808 */
#define Q2_MASK_MONSTERSOLID  0x02020003u   /* SV_movestep        0x8005FE7C */

/*
 * The bit both of them share, and the only one the port reads: it is what
 * turns the ENTITY clip on. A trace whose mask carries it is clipped against
 * the entity list at 0x800544EC after the hull walk, which is how a creature
 * is stopped by a door rather than walking through it. Both AI masks set it,
 * so in practice every AI trace asks for entities — the gate exists so a
 * caller that does not is still answered correctly.
 */
#define Q2_MASK_ENTITY_BIT    0x02000000u

/* ------------------------------------------------------------------------- */
/* Range bands                                                                */
/* ------------------------------------------------------------------------- */
/*
 * Compared against SQUARED distance, which is why the executable's immediates
 * are one less than a perfect square: the compiler turned `d2 < k*k` into
 * `k*k-1 < d2`. Read at 0x8005D56C and again, identically, at 0x8005E19C.
 */
#define Q2_MELEE_DISTANCE       1020   /* 0x000FE00F + 1 = 1020^2 */
#define Q2_MELEE_DISTANCE_BIG   2040   /* 0x003F803F + 1 = 2040^2 */
#define Q2_RANGE_NEAR_DIST      6000   /* 0x022550FF + 1 = 6000^2, id's 500 */
#define Q2_RANGE_MID_DIST      12000   /* 0x089543FF + 1 = 12000^2, id's 1000 */

/* The one class that gets the doubled melee reach. Read as an equality against
 * the class id byte at entity+0x23. */
#define Q2_CLASS_LONG_MELEE      68

/* ------------------------------------------------------------------------- */
/* Class bytes — entity+0x23                                                  */
/* ------------------------------------------------------------------------- */
/*
 * The class byte is a THIRD numbering, distinct from the class-table row id
 * (18, 19, 20 for the three Soldiers) and from the module's own name. It is the
 * index into `classMethods[256]`, and it is what the engine compares when it
 * wants to know which creature it is holding.
 *
 * Where it comes from was not written down anywhere until now: each 48-byte
 * record of the class descriptor table at **0x800A3518** carries it at **+0x20**,
 * and the module loader copies it into the entity with `lbu v0, 0x20(s4)` /
 * `sb v0, 0x23(s0)` at 0x8007E660. Reading that column out gives the whole map,
 * and the seven bytes the disc's own modules register — Arachner 64, Berserk 70,
 * Gunner 79, Infantry 81, Soldier 87/88/89, Tankcomm 91, Insane 94 — all land on
 * it exactly, which is the check that says the column is the right one.
 *
 * Named here are only the bytes some engine path actually tests for.
 */
/*
 * 47 IS THE CORPSE. `0x8007F0F0` writes it into the actor's class field
 * (+0xD2) at detach and saves the creature's own class to +0xDA, so a body has
 * its own class for the rest of its life and can be told from the thing it was.
 */
#define Q2_CLASS_CORPSE          47

#define Q2_CLASS_JORG            82   /* health 3000 — id's monster_jorg      */
#define Q2_CLASS_RIDER           83   /* health 3000 — id's monster_makron    */
#define Q2_CLASS_MEDIC           84   /* health  310 — id's monster_medic     */
#define Q2_CLASS_BOSS1           90   /* health 1500 — id's monster_supertank */
#define Q2_CLASS_TANKCOMM        91   /* health  750 — id's monster_tank      */

typedef enum q2_range_band {
    Q2_RANGE_MELEE = 0,
    Q2_RANGE_NEAR,
    Q2_RANGE_MID,
    Q2_RANGE_FAR
} q2_range_band;

/* The forward-cone threshold: a dot product of 1230/4096, about 0.30, so the
 * cone is roughly 72 degrees either side. That is id's own 0.3 and is why
 * monsters notice you from well off to the side. 0x8005D76C. */
#define Q2_INFRONT_DOT 1230

/* ------------------------------------------------------------------------- */
/* A live creature                                                            */
/* ------------------------------------------------------------------------- */
/*
 * One structure serves for creatures and for whatever they are chasing, which
 * is how the original does it too — `enemy`, `goalentity` and `movetarget` are
 * all plain entity pointers and the code never asks what kind of thing it has.
 * A player is an entity with `client` set.
 *
 * Offsets in the comments are the executable's, so a field can be checked
 * against a disassembly without hunting for it.
 */
typedef struct q2_monster {
    /* --- placement ------------------------------------------------------- */
    s32 pos[3];             /* +0x00  origin; index 1 is up, and points DOWN */
    s32 velocity[3];
    s16 angles[3];          /* +0x10  yaw is angles[2] (+0x14)               */
    s16 mins[3];            /* the movement hull, from 0x8005CCF4            */
    s16 maxs[3];
    s16 view_height;        /* +0x4C  added to origin[1] by visible()        */

    /* --- identity -------------------------------------------------------- */
    u32 spawnflags;         /* +0x1C  bit 18 ambush, bit 29 in use           */
    u16 flags;              /* +0x20  FL_*, the low half of the packed word  */

    /*
     * The other three fields of that same word, and the one above spawnflags.
     * Kept as separate members rather than packed back into one, because the
     * port has no reason to reproduce the packing and every reason to make a
     * `movetype` read like a movetype. See the layout note above for where
     * each one lives on the console and why the values are id's.
     *
     * `deadflag` is the field the console's `die` handlers test to refuse a
     * second entry; this port's `dead` bool is the same guard and the two are
     * kept in step by `q2_monster_damage_reaction`.
     */
    u8  solid;              /* +0x20 bits 16..17 */
    u8  movetype;           /* +0x20 bits 18..21 */
    u8  deadflag;           /* +0x20 bits 22..23 */
    u8  takedamage;         /* +0x1C bits 30..31 */

    /*
     * +0x4E, and it was decoded per creature and then dropped on the floor.
     * `q2_creature` has carried `mass` since the module decoder was written —
     * Soldier 100, Infantry 200, Gunner 200, Berserk 250, Arachner 400,
     * Tankcomm 500, all id's own numbers — and there was nowhere on the live
     * entity to put it.
     */
    s16 mass;               /* +0x4E */

    /*
     * +0x3A. `hurt` is only its LOW BIT (the wounded half of a skin pair); the
     * base index is per creature, and a module can set the whole field —
     * the Tank Commander's second class byte spawns with `skinnum = 2`.
     */
    u8  skinnum;            /* +0x3A */
    u8  class_id;           /* +0x23  indexes the class method table         */
    u8  class_byte;         /* the local descriptor key, 64..94 for monsters */

    /*
     * THE CLASS TABLE ROW this creature was placed from — 18, 19, 20 for the
     * three Soldiers — kept because `class_id` does not survive the spawn.
     *
     * `q2_creature_spawn` overwrites `class_id` with the module's own local
     * descriptor key (crebind.c), and a module that registers several class
     * bytes against ONE shared method table then has no way back to the row.
     * The Soldier is exactly that case: its spawn function dispatches on the
     * class-table id at 0x80101604 — `lh v1, 0xD2(entity+0x24)`, branching on
     * 19 / 18 / 20 — and each arm writes a different skinnum and health. Losing
     * the row and reconstructing the variant from `class_byte` instead
     * permuted all three: see soldier_skin.
     */
    u8  pop_class_id;

    /*
     * WHICH POPULATION GROUP PLACED THIS CREATURE.
     *
     * A group is not spawned because it exists; it is spawned because a script
     * SELECTED it — `0x80056C60` takes a name and sets bit 0 of the group's
     * flags, and the spawn pass runs only the selected ones (population.h).
     * The flags word is zero on disc for all 222 groups of all 49 maps, so
     * nothing is standing there at load until something asks.
     *
     * `CREBATCH` is that ask, and 58 of the disc's 89 resolvable calls name a
     * group claiming NO zone — `ShotgunRoom`, `BerserkHide` — which is an
     * ambush waiting to be summoned. Keeping the group index on the creature is
     * what lets one be held back and then released.
     */
    u16 group;
    u32 svflags;            /* +0x40  SVF_MONSTER and the freed bit          */
    bool client;            /* +0x3C  non-NULL for a player                  */

    /* --- health ---------------------------------------------------------- */
    s16 health;             /* the original keeps this in the link object    */
    s16 max_health;
    s16 gib_health;         /* negative; below this the body is destroyed    */

    /* --- animation ------------------------------------------------------- */
    s16 frame;              /* +0x38                                         */
    s32 next_think;         /* +0x90  on the 10 Hz AI clock                  */

    /*
     * The pain handler's own clock, on the same 10 Hz tick as `next_think`.
     *
     * A monster shot twice in a frame must not restart its flinch twice, and
     * without this every hit of a burst re-entered the pain move from frame
     * zero — a soldier under machinegun fire never advanced past the first
     * pose and never got back to running. Three seconds is the interval the
     * class uses, which is 30 of these ticks.
     */
    s32 pain_debounce;

    /*
     * BLOODIED. `skinnum |= 1` at half health: every class ships its skins in
     * pairs, clean on the even index and wounded on the odd one, and the pain
     * handler sets the low bit rather than choosing a skin. Kept as a flag
     * because the base index is derived from the class row per creature
     * (soldier_skin), so the two cannot be folded into one stored number.
     */
    bool hurt;
    void (*think)(struct q2_monster *m);   /* +0x94 — M_MoveFrame when awake */

    /* --- targeting ------------------------------------------------------- */
    struct q2_monster *enemy;        /* +0xBC                                */
    struct q2_monster *oldenemy;     /* +0xC0                                */
    struct q2_monster *goalentity;   /* +0x84                                */
    struct q2_monster *movetarget;   /* +0x88                                */

    /*
     * +0x48, and the ONE thing that writes it on a creature is the kill
     * bookkeeping: `Killed` sets `targ->owner = attacker` when the attacker's
     * class byte is 84 (0x80062A18), which is id's "medics won't heal monsters
     * that they kill themselves". Unreachable on this disc — the Medic has a
     * class-table row and no CreAI module ships for it — and transcribed
     * anyway, because a branch that cannot fire here is still the branch the
     * original has.
     */
    struct q2_monster *owner;        /* +0x48                                */
    s32 show_hostile;                /* +0xB0                                */
    s32 teleport_time;               /* +0xD0                                */
    s16 targetname;                  /* +0x18  script name id, 0 when none   */
    s16 target;                      /* +0x16                                */
    s16 combattarget;                /* +0x56                                */
    s16 yaw_speed;                   /* +0x8C  degrees per tick, 4096/turn   */
    s16 ideal_yaw;                   /* +0x8E                                */
    s32 blind_target[3];             /* +0x5C  kept when the blindfire bit is set */

    /* --- monsterinfo, entity+0xD8 onward --------------------------------- */
    const q2_mmove *currentmove;     /* +0xD8                                */
    u32 aiflags;                     /* +0xDC                                */

    void (*stand)(struct q2_monster *m);                       /* +0xE0 */
    void (*idle)(struct q2_monster *m);                        /* +0xE4 */
    void (*search)(struct q2_monster *m);                      /* +0xE8 */
    void (*walk)(struct q2_monster *m);                        /* +0xEC */
    void (*run)(struct q2_monster *m);                         /* +0xF0 */
    void (*dodge)(struct q2_monster *m, struct q2_monster *other, s32 eta);
                                                               /* +0xF4 */
    void (*attack)(struct q2_monster *m);                      /* +0xF8 */
    void (*melee)(struct q2_monster *m);                       /* +0xFC */
    void (*sight)(struct q2_monster *m, struct q2_monster *other);
                                                               /* +0x100 */
    bool (*checkattack)(struct q2_monster *m);                 /* +0x104 */

    /*
     * PAIN and DIE — entity+0xA0 and +0xA4, written by the Soldier's module at
     * 0x80101684 and 0x80101690.
     *
     * These had no home at all, so `crebind`'s install list skipped slots 11
     * and 12 and `soldier_pain` / `soldier_die` were dead code: damage reached
     * a creature through `q2_actor_to_monster`, which sets health and a bool
     * and calls nothing. No pain animation, no pain sound, no death sound, no
     * random choice among the death moves.
     *
     * The original passes T_Damage's attacker, inflictor, damage and point to
     * both.
     *
     * AND THE DAMAGE IS NOW PASSED, because four of the seven transcriptions
     * cannot work without it. This used to say "a `soldier_pain` that wanted
     * the damage to pick pain4 would need the argument added here first", and
     * that turned out to understate it:
     *
     *     Tankcomm  pain   `damage <= 10` returns outright, `damage <= 30`
     *                      adds a roll, and 30 / 60 pick among three moves
     *     Gunner    pain   three flinch animations split at 10 and 25
     *     Berserk   pain   `damage < 20 || random() < 0.5` gates the flinch
     *     Berserk   die    `damage >= 50` picks the long death
     *
     * Every one of those was reaching a sentinel and taking one arm forever.
     * The attacker, the inflictor and the point are still dropped: nothing
     * reconstructed reads them, and adding a parameter nothing consumes is how
     * a signature stops meaning anything.
     */
    void (*pain)(struct q2_monster *m, s16 damage);            /* +0xA0 */
    void (*die)(struct q2_monster *m, s16 damage);             /* +0xA4 */
    /*
     * +0x108, with its threshold at +0x140. Not in the PC lineage: M_ChangeYaw
     * and SV_StepDirection both call it INSTEAD of turning when the turn they
     * were about to make exceeds the threshold, so a creature that needs to
     * swing a long way plays an animation rather than spinning on the spot.
     * Zero threshold disables it, which is what most creatures use.
     */
    void (*bigturn)(struct q2_monster *m);                     /* +0x108 */
    s32  bigturn_threshold;                                    /* +0x140 */

    s32 pausetime;          /* +0x10C */
    s32 attack_finished;    /* +0x110 */
    s32 saved_goal[3];      /* +0x114 */
    s32 search_time;        /* +0x120 */
    s32 trail_time;         /* +0x124 */
    s32 last_sighting[3];   /* +0x128 */
    s32 idle_time;          /* +0x134 */

    u16 nextframe;          /* +0x138 */
    u8  attack_state;       /* +0x13A bits 2..4                             */
    bool lefty;             /* +0x13A bit 5                                 */
    bool blindfire;         /* +0x13A bit 1                                 */
    u8  speed_scale;        /* +0x13B multiplies every frame's dist         */

    /* --- port bookkeeping ------------------------------------------------ */
    bool in_use;
    bool dead;

    /*
     * Blown apart rather than killed. Damage past `gib_health` destroys the
     * body outright: no death animation, because there is nothing left to
     * animate. Separate from `dead` because a body already dead can still be
     * gibbed by a later explosion, and the two arms play different sounds.
     */
    bool gibbed;

    /*
     * DETACHED — the body is a corpse and no longer a creature. See
     * `q2_monster_corpse_detach`.
     */
    bool corpse;
    u8   corpse_was_class;      /* actor+0xDA: what it was before */

    bool on_ground;
    s32  ground_height;     /* the floor the stand-in world puts under it    */
} q2_monster;

/* The registry is 38 slots, which is exactly Population's class_id range. */
#define Q2_MONSTER_CLASS_COUNT 38

/* ------------------------------------------------------------------------- */
/* Level state — 0x800E46D8 onward                                            */
/* ------------------------------------------------------------------------- */
/*
 * Nine consecutive words in the original, in this exact order, which is id's
 * own `level_locals_t` tail. FindTarget reads all of them (0x8005D3AC..
 * 0x8005D478) and that ordering is what identifies them.
 */
typedef struct q2_level {
    s32 framenum;                   /* 0x800E46D8 */
    s32 time;                       /* 0x800E46DC — the 10 Hz clock */
    q2_monster *sight_client;       /* 0x800E46E0 */
    q2_monster *sight_entity;       /* 0x800E46E4 */
    s32 sight_entity_framenum;      /* 0x800E46E8 */
    q2_monster *sound_entity;       /* 0x800E46EC */
    s32 sound_entity_framenum;      /* 0x800E46F0 */
    q2_monster *sound2_entity;      /* 0x800E46F4 */
    s32 sound2_entity_framenum;     /* 0x800E46F8 */

    /*
     * 0x800E46FC — `level.total_monsters`, and it is the counter's other half:
     * `monster_start` increments it for every creature it spawns that is not
     * AI_GOOD_GUY, which is the same exclusion `Killed` applies to the kill
     * count below. id keeps the pair in exactly this order.
     */
    s32 total_monsters;

    /*
     * 0x800E4700 — `level.killed_monsters`, incremented by `Killed` at
     * 0x80062A10 for any creature that is not a good guy.
     *
     * The MISSION screen's kill column used to be a scan for bodies with the
     * `dead` flag set, which is a reconstruction of the effect rather than of
     * the counter: it counts a good guy, it counts a creature killed twice
     * differently from the console, and it cannot count one that has been
     * removed. This is the number the original keeps.
     */
    s32 killed_monsters;
} q2_level;

extern q2_level q2_level_state;

void q2_level_reset(void);

void q2_monster_init(q2_monster *m);

/* True while the entity is a live participant: the original's
 * `flags & 0x20000000` test, which every AI path opens with. */
static inline bool q2_ent_inuse(const q2_monster *m)
{
    return m != NULL && m->in_use && (m->spawnflags & Q2_SVFLAG_INUSE) != 0;
}

/* Positional advance for one animation frame, with the speed scale and the
 * hold-frame flag applied. 0x80061930. */
s32 q2_monster_frame_dist(const q2_monster *m, const q2_mframe *frame);

/* Squared distance between two entities, and the band it falls in. */
s64 q2_monster_dist_sq(const q2_monster *m, const s32 target[3]);
q2_range_band q2_range(const q2_monster *self, const q2_monster *other);

/* True when `other` lies inside `self`'s forward cone. 0x8005D608. */
bool q2_infront(const q2_monster *self, const q2_monster *other);

/* Apply damage. Returns true when this killed the creature. */
bool q2_monster_damage(q2_monster *m, s16 amount);

/*
 * Skill, which the pain path gates on. Defined in cre_soldier.c and declared
 * again here because the damage reaction needs it and `crebind.h` — where it
 * otherwise lives — sits ABOVE this header and cannot be included from it.
 */
s32 q2_cre_skill(void);

/*
 * The tail of T_Damage, 0x80062940..0x80062B54 — everything the original does
 * AFTER the health subtraction, which is where a creature's reaction lives.
 *
 * `combat.c` already carries the arithmetic half (the takedamage gate, the
 * friendly-fire halving, the surprise bonus, godmode and the knockback
 * suppression). What had no port at all was this half: which handler is called,
 * with what guards, and what the world records about the death. Call it once
 * per creature per frame in which its health changed, with the health already
 * applied and `dead` not yet raised.
 *
 * `damage` is the amount that actually landed — zero is meaningful, because
 * the original still runs M_ReactToDamage for it and then skips the flinch.
 */
void q2_monster_damage_reaction(q2_monster *targ, q2_monster *attacker,
                                s16 damage);

/*
 * monster_death_use — 0x800622E8. Run once, from the kill path, before the
 * creature's own `die`.
 */
void q2_monster_death_use(q2_monster *self);

/* ------------------------------------------------------------------------- */
/* Corpses — the detach at 0x8007F098 and the handler at 0x8007F71C           */
/* ------------------------------------------------------------------------- */
/*
 * A BODY STOPS BEING A CREATURE, and this port had no such step at all.
 *
 * The console keeps two linked structures: the ACTOR (health at +0x108,
 * gib_health at +0x44, collision volume at +0x54, per-tick handler at +0x3C,
 * entity at +0x2EC) and the ENTITY. When a creature's own `*_dead` raises
 * SVF_DEADMONSTER, `0x8007EC28` calls `0x8007F098` and the two come apart:
 *
 *     0x8007F0A8/B0  gib_health  is copied from entity+0x52 onto actor+0x44
 *     0x8007F0B4/BC  max_health  is copied from entity+0x50 onto actor+0x48
 *     0x8007F0C8     entity+0x24 = 0            — the entity loses its actor
 *     0x8007F0D0     entity+0x1C &= 0xDFFFFFFF  — bit 29, the edict is FREED
 *     0x8007F0DC     actor+0x2EC = 0            — the actor loses its entity
 *     0x8007F0E4     actor+0x3C  = 0x8007F71C   — the per-tick handler becomes
 *                                                 the CORPSE handler
 *     0x8007F0E0     the collision volume is rescaled (below)
 *     0x8007F0F4     actor+0xD2 = 47, with the old class saved to +0xDA
 *
 * From that tick on there is no edict, which is why `movetype` is written on
 * death by every module and read by nothing: there is nothing left to read it
 * off. A console corpse does not fall because a console corpse is not an
 * entity any more.
 *
 * THE VOLUME RESCALE, 0x8007F77C, is two divides on the actor's own box:
 * `+0x96 /= 4` (0x8007F794, an arithmetic shift with the usual +3 bias) and
 * `+0x94 = (3 * +0x94) / 2` (0x8007F79C..0x8007F7B4). So a body becomes half as
 * tall again as it is wide and a quarter as tall as it was — id's
 * `self->maxs[2] = -8` generalised. That is what stops a corpse blocking a
 * doorway at head height.
 *
 * THE HANDLER, 0x8007F71C, does four things in order: honour a queued clip
 * transition (0x8005B2A8, and returns if it took one), advance the animation
 * (0x8005B880), append to the draw list (0x800552B4), and then
 * `if (health <= gib_health)` hand the body to the destruction dispatcher
 * (0x8007CEB4). So a settled corpse is still checked for gibbing every tick,
 * against the threshold it copied at detach.
 */
void q2_monster_corpse_detach(q2_monster *m);

/*
 * One tick of a corpse — 0x8007F71C. Returns true when the body was destroyed
 * this tick, which is the caller's cue to stop drawing it.
 */
bool q2_monster_corpse_tick(q2_monster *m);

/* ------------------------------------------------------------------------- */
/* The class method table                                                     */
/* ------------------------------------------------------------------------- */
/*
 * Two levels, exactly as 0x80061D10 builds it: a 256-entry table of pointers
 * to 32-entry method tables, every slot starting out at the shared inert one.
 * Registering a method clones the shared table for that class on first write,
 * which is the port's way of getting the same observable behaviour without the
 * original's fixed BSS arena.
 */
typedef void (*q2_class_method)(q2_monster *m);

void q2_class_table_reset(void);

/* The setter the loader hands a module as import +0x118 (0x80061DE4). */
void q2_class_method_set(u32 class_id, u32 index, q2_class_method fn);
q2_class_method q2_class_method_get(u32 class_id, u32 index);

/* Convenience for the two halves, so callers do not open-code the +26. */
void q2_class_think_set(u32 class_id, u32 think_index, q2_class_method fn);
void q2_class_verb_set(u32 class_id, u32 verb_index, q2_class_method fn);

/* ------------------------------------------------------------------------- */
/* The frame driver — M_MoveFrame, 0x8006175C                                 */
/* ------------------------------------------------------------------------- */
void q2_M_MoveFrame(q2_monster *m);

/* monster_start_go, 0x80061BA4: wake a placed creature up. */
void q2_monster_start_go(q2_monster *m);

/*
 * G_PickTarget, 0x8005F708 — resolve a script `targetname` to an entity.
 *
 * The AI needs it in two places (a creature's path target at spawn and its
 * combat point on first contact) and neither should drag the script namespace
 * into this module, so the resolver is installed from outside. With none
 * installed every lookup fails, which leaves creatures standing where they
 * were placed rather than pathing — the same thing the original does with an
 * unresolvable target.
 */
void q2_ai_set_pick_target(q2_monster *(*fn)(s16 targetname, void *user),
                           void *user);
q2_monster *q2_pick_target(s16 targetname);

/* Decode a move and a frame out of a relocated module image. `offset` is
 * module-relative. Returns false when out of range. */
bool q2_mmove_read(const u8 *image, size_t size, u32 offset, q2_mmove *out);
bool q2_mframe_read(const u8 *image, size_t size, u32 offset, q2_mframe *out);

#endif /* Q2PSX_MONSTER_H */
