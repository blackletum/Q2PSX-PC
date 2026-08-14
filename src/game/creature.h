/*
 * creature.h — reading a creature out of its own module, deterministically.
 *
 * The AI framework in `ai.[ch]` is the half of a monster that is shared. This
 * is the half that is not: which animations a given creature has, which frames
 * of them do something, how fast it moves, how far it can turn. All of that is
 * inside the creature's `CreAIBin` module, and all of it is DATA — so it is
 * read off the disc rather than transcribed.
 *
 * What is left over, and is genuinely code, is the body of each think
 * function: the sound, the shot, the claw. Those are small and are written by
 * hand per creature in `creatures/`.
 *
 * ---------------------------------------------------------------------------
 * There are only seven creature modules on the whole disc
 * ---------------------------------------------------------------------------
 * Walking every map's `CreAIBin` chain gives, in full:
 *
 *     Soldier    BASE0 BASE1 BASE2 BASE3 JAIL5 MAGDEMO WASTE1
 *     Tankcomm   COMMAND WASTE4
 *     Gunner     POWER1 WASTE3
 *     Arachner   POWER1
 *     Infantry   POWER2
 *     Berserk    WASTE4
 *     Insane     LAB
 *
 * Thirteen modules over the disc, seven distinct. The class table names 26
 * creatures, so most of them are placed models with no brain shipped — which
 * is worth knowing before anyone goes looking for the other nineteen.
 *
 * ---------------------------------------------------------------------------
 * How a module is decoded, and why none of it is guesswork
 * ---------------------------------------------------------------------------
 * Earlier passes scanned module images for anything shaped like a move record
 * and over-matched badly — 251 candidates where a disassembly census expected
 * 160. The fix is not a better filter. Every structure is REACHABLE, and this
 * follows the chain instead of searching it:
 *
 *   1. **Export 2** is the module init. It builds a 32-entry method table in
 *      its own data and hands it to the engine with
 *      `classMethods[class_byte] = table` through import +0x118
 *      (`0x80061DE4`), once per class byte the module serves. Reading its
 *      `sw` stream gives the table address and every class byte.
 *
 *   2. **Export 0** is the spawn function. It writes the monsterinfo callbacks
 *      straight into the entity, so its `sw rX, imm(rS)` stream with `imm` in
 *      0xE0…0x108 IS the callback set; `sb rX, 0x13B` is the animation speed
 *      scale and `sh rX, 0x4E` is the mass.
 *
 *   3. Each callback installs moves with `sw rX, 0xD8(rS)`, where `rX` was
 *      materialised by a `lui`/`addiu` pair. Following those gives the move
 *      records, and a move record's own frame pointer gives the frames.
 *
 * Every address involved is one the relocation stream already fixed up, so a
 * decode that lands outside the image is a decode that is wrong, and is
 * rejected rather than reported.
 */
#ifndef Q2PSX_CREATURE_H
#define Q2PSX_CREATURE_H

#include "monster.h"
#include "q2psx.h"
#include "reloc.h"

/* The entity offsets the spawn function writes, which are what identify it. */
/* The entity's own pain and die hooks, which the spawn function writes FIRST
 * — id's `self->pain` and `self->die`, ahead of the monsterinfo block. They
 * are where every pain and death animation is installed, so a decoder that
 * only follows monsterinfo finds a creature that cannot be hurt. */
#define Q2_ENT_OFS_PAIN    0xA0
#define Q2_ENT_OFS_DIE     0xA4

#define Q2_ENT_OFS_STAND   0xE0
#define Q2_ENT_OFS_IDLE    0xE4
#define Q2_ENT_OFS_SEARCH  0xE8
#define Q2_ENT_OFS_WALK    0xEC
#define Q2_ENT_OFS_RUN     0xF0
#define Q2_ENT_OFS_DODGE   0xF4
#define Q2_ENT_OFS_ATTACK  0xF8
#define Q2_ENT_OFS_MELEE   0xFC
#define Q2_ENT_OFS_SIGHT   0x100
#define Q2_ENT_OFS_CHECK   0x104
#define Q2_ENT_OFS_BIGTURN 0x108
#define Q2_ENT_OFS_SCALE   0x13B
#define Q2_ENT_OFS_MASS    0x4E
#define Q2_ENT_OFS_MOVE    0xD8

#define Q2_CRE_MAX_MOVES    64
#define Q2_CRE_MAX_CLASSES   8
#define Q2_CRE_MAX_FRAMES 1024

/* One move recovered by following a callback. */
typedef struct q2_cre_move {
    u32 addr;               /* module address of the record        */
    s32 first_frame;
    s32 last_frame;
    u32 frames_addr;
    u32 endfunc_addr;       /* 0 when the move loops               */
    u32 frame_index;        /* into the owner's frame pool         */
    u32 frame_count;
    /* Which callback installed it, as an offset from Q2_ENT_OFS_STAND / 4,
     * or -1 when it was reached only through another move's endfunc. */
    s32 via;
} q2_cre_move;

typedef struct q2_creature {
    char name[13];          /* the module header's own 12-byte name */

    /* From export 2 — the class bytes this module serves and the method table
     * it registers for them. */
    u8  class_byte[Q2_CRE_MAX_CLASSES];
    u32 class_count;
    u32 method_table_addr;
    u32 method[Q2_CLASS_METHOD_COUNT];
    u32 method_count;       /* highest populated slot + 1 */

    /* From export 0 — the spawn function. Indexed the same way the entity is:
     * slot 0 is stand, 1 idle, 2 search, 3 walk, 4 run, 5 dodge, 6 attack,
     * 7 melee, 8 sight, 9 checkattack, 10 bigturn, 11 pain, 12 die. Zero when
     * not installed. */
    u32 callback[13];
    s32 speed_scale;        /* -1 when the spawn function did not set one */
    s32 mass;

    /* Moves, and the frame pool they point into. */
    q2_cre_move move[Q2_CRE_MAX_MOVES];
    u32         move_count;
    q2_mframe   frames[Q2_CRE_MAX_FRAMES];
    u32         frame_count;

    u32 base;               /* the address the image was relocated to */
} q2_creature;

extern const char *const q2_cre_callback_names[13];

/*
 * Decode one module of a relocated `CreAIBin` image.
 *
 * `image`/`size` is the module BODY — past its 16-byte `{name, next}` header —
 * already relocated to `base`. `name` is that header's name.
 *
 * Returns false only when the image is unusable; a module whose spawn function
 * installs fewer callbacks than expected still decodes, with the missing slots
 * zero, because that is a real and common shape (the Soldier has no melee).
 */
bool q2_creature_decode(q2_creature *out, const u8 *image, size_t size,
                        u32 base, const char *name);

/*
 * The module's own name for each move.
 *
 * Every creature module carries three tables of text, and the third is the
 * interesting one: its NAME at about `+0x168`, a 12-byte sound-name table at
 * about `+0x1C4`, and a **20-byte move-name table** further in — "Start Walk",
 * "Attak 1 Loop", "Stand N", "Runshoot", "Fire Chain".
 *
 * The link to the moves is the COUNT: the Tank's table has exactly sixteen
 * slots and the Tank has exactly sixteen moves, and the same holds for every
 * module on the disc. So the table is located by scanning for the first run of
 * `move_count` consecutive 20-byte slots that each start printable and contain
 * a terminator — an exact-count match, not a resemblance.
 *
 * Not every module has one: the Soldier's carries its sound names and no move
 * names at all, so this returns NULL there rather than inventing labels.
 *
 * `out[i]` points into the module image and is valid while it is.
 */
u32 q2_creature_move_names(const q2_creature *c, const u8 *image, size_t size,
                           const char **out, u32 out_count);

/* Look a move up by the callback that installs it, or by its module address. */
const q2_cre_move *q2_creature_move_via(const q2_creature *c, u32 callback_slot);
const q2_cre_move *q2_creature_move_at(const q2_creature *c, u32 addr);

/*
 * Build a `q2_mmove` the frame driver can run from a decoded move.
 *
 * `endfunc` is supplied by the caller because the module's own end callback is
 * MIPS and has to be reimplemented; passing NULL makes the move loop, which is
 * what a move with no end callback does anyway.
 */
void q2_creature_mmove(const q2_creature *c, const q2_cre_move *m,
                       q2_endfunc endfunc, q2_mmove *out);

/* ------------------------------------------------------------------------- */
/* What a think function DOES                                                 */
/* ------------------------------------------------------------------------- */
/*
 * The move tables are data and come across for free. A think function is code,
 * and there are 37 of them across the six creatures the Soldier does not
 * cover — too many to transcribe one at a time, and transcribing them by
 * pattern rather than by reading each one would be invention.
 *
 * So they are decoded instead, the same way the moves were: by following what
 * the function actually does rather than guessing what it might. A think
 * function is a short, stereotyped thing. It calls into the engine through the
 * module's import table, and the arguments it passes are constants the
 * compiler materialised — which is exactly what the register tracker already
 * recovers.
 *
 * Five kinds cover every think function on the disc:
 *
 *   SOUND      import +0x20 with a sound handle from the module's own data
 *   MELEE      import +0xEC — `fire_hit` — with the aim vector built on the
 *              stack and the damage and kick at sp+16 and sp+20
 *   NEXTFRAME  a halfword store to entity+0x138, the refire jump
 *   MOVE       a word store to entity+0xD8, installing an animation
 *   AIFLAG     a word store to entity+0xDC, which is how the three shared
 *              duck helpers work — `monster_duck_down` sets AI_DUCKED,
 *              `monster_duck_hold` holds or releases AI_HOLD_FRAME, and
 *              `monster_duck_up` clears AI_DUCKED again. The Gunner and the
 *              Infantry both carry all three, byte for byte identical
 *   CALL       any other import, recorded by its slot so nothing is silently
 *              dropped — a creature whose action is not understood reports
 *              which engine function it wanted rather than doing nothing
 *
 * The decode is checked against the Soldier, whose think functions were read
 * by hand first: `q2psx-inspect creatures` reports where the two disagree.
 */
typedef enum q2_cre_op {
    Q2_CRE_OP_NONE = 0,
    Q2_CRE_OP_SOUND,
    Q2_CRE_OP_MELEE,
    Q2_CRE_OP_NEXTFRAME,
    Q2_CRE_OP_MOVE,
    Q2_CRE_OP_AIFLAG,
    Q2_CRE_OP_CALL
} q2_cre_op;

/* Import slots a think function reaches, named where they are identified. */
/*
 * ---------------------------------------------------------------------------
 * Naming an import slot
 * ---------------------------------------------------------------------------
 * The loader that fills a module's 71 import pointers is at `0x8007DA00`, and
 * it writes every slot individually — `sw v0, N(s0)` with the address
 * materialised two instructions above. So ANY slot can be named by grepping
 * that one function for its offset, which turns the census's `call(+XX)?`
 * reports from opaque numbers into named engine calls.
 *
 * The method checks out on the slot that was already known: `+0x28` resolves to
 * `0x8006FC1C`, the SVECTOR rotate, which is exactly `Q2_IMP_LOCAL2WLD`.
 *
 * Named this way so far, beyond the four below:
 *
 *     +0xC0  0x8005C460   a scaled vector add, `out = a + (s * d) >> 12`,
 *                         per component — so the three consecutive `call(+C0)`
 *                         at the end of every Soldier fire think are MUZZLE
 *                         ARITHMETIC and not three shots
 *     +0xC4  0x8005C634
 *     +0xC8  0x8005F934   `vectoangles` — ratan2 through 0x8008A358, the
 *                         horizontal length through 0x8008A7E8, and three
 *                         angles written out; 3072 and 1024 are the quarter
 *                         turns it returns looking straight down and up
 *     +0xD8  0x8005BB58   angles to vectors — three angles masked to 0xFFF,
 *                         each indexing the packed {sin, cos} table at
 *                         0x800A5430
 *     +0xEC  0x80061118   `fire_hit`, the melee — Q2_IMP_FIRE_HIT below
 *
 * Which settles something about the Soldier's SHOT by elimination: its fire
 * think calls +0xD8, +0x2C, +0x28, +0xC8, +0xD8, +0xC0 x3, and every one of
 * those is now read and every one is AIMING ARITHMETIC. There is no fire call
 * in it at all.
 */
#define Q2_IMP_RAND      0x14
#define Q2_IMP_SOUND     0x20
#define Q2_IMP_LOCAL2WLD 0x28
#define Q2_IMP_MUZZLE    0x2C
#define Q2_IMP_RANGE     0xB4
#define Q2_IMP_FIRE_HIT  0xEC

#define Q2_CRE_MAX_STEPS 8

typedef struct q2_cre_step {
    u8  op;
    u32 import_ofs;     /* CALL / SOUND / MELEE: which import          */
    u32 addr;           /* SOUND: the handle's address; MOVE: the move */
    s32 aim[3];         /* MELEE                                       */
    s32 damage_base;    /* MELEE: the constant added after the modulo  */
    s32 damage_rand;    /* MELEE: the modulo's divisor, 0 when none    */
    s32 kick;           /* MELEE                                       */
    s32 frame;          /* NEXTFRAME                                   */
    u32 flag_set;       /* AIFLAG: bits to set                         */
    u32 flag_clear;     /* AIFLAG: bits to clear                       */
    bool gated;         /* the step sits behind a branch, so it is not
                         * certain to run every time                   */
} q2_cre_step;

typedef struct q2_cre_think {
    q2_cre_step step[Q2_CRE_MAX_STEPS];
    u32         step_count;
    bool        decoded;        /* the walk reached a return cleanly */
} q2_cre_think;

/*
 * Decode every think index the creature's frames actually use.
 *
 * `out` is indexed by think byte. Returns how many indices were decoded to at
 * least one step; an index that decodes to none is a think function this
 * reader does not understand, and shows up as such rather than as a creature
 * that quietly does nothing.
 */
u32 q2_creature_decode_thinks(const q2_creature *c, const u8 *image,
                              size_t size, u32 base,
                              q2_cre_think *out, u32 out_count);

/* Every distinct think index any of this creature's frames actually uses.
 * Writes up to `cap` indices in ascending order and returns how many — which
 * is the list a reimplementation has to cover, rather than all 32 slots. */
u32 q2_creature_think_indices(const q2_creature *c, u8 *out, u32 cap);

/* Same, for the local AI verbs (the `ai` bytes with bit 7 set). */
u32 q2_creature_verb_indices(const q2_creature *c, u8 *out, u32 cap);

#endif /* Q2PSX_CREATURE_H */
