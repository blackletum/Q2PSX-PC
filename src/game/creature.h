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

    /*
     * TRUE for a range this decoder INVENTED by cutting a module mmove at the
     * model's clip-name boundaries, rather than one the module declares.
     *
     * Both matter, and they are not the same thing. The clip pieces are what
     * the animation census counts — summing a creature's distinct move lengths
     * and tripling gives its model's total clip frames, which is how the
     * move-to-clip correspondence was established in the first place. The
     * module's own mmove records are what the AI INSTALLS, and their ranges and
     * endfuncs are load-bearing: the Soldier's two attack moves are
     * {first 0, last 11, endfunc soldier_run} at 0x80102E74 and
     * {first 12, last 29, endfunc soldier_run} at 0x80102EBC, and its refire
     * thinks jump to the absolute frames 1, 9, 15 and 27.
     *
     * Cutting the parents down to their first piece and clearing their
     * endfuncs — which is what this decoder used to do — turned those into a
     * ONE-frame and a THREE-frame move with no endfunc. q2_M_MoveFrame wraps a
     * move with no endfunc back to its first frame forever, and every
     * `nextframe` the refire chain asks for lands outside the piece and is
     * discarded. A Soldier that decided to attack was frozen for the rest of
     * the level, playing a dist-0 charge frame and never calling SV_movestep
     * again. The Soldier ships on seven of the thirteen AI maps.
     *
     * So both live in the array — index-parallel, because crebind.c's
     * chain_endfunc and the client's move-name lookup both walk it by index —
     * and q2_cre_find_move skips the pieces, so only a real mmove is ever
     * installed on a creature.
     */
    bool clip_piece;
    u32 frames_addr;
    u32 endfunc_addr;       /* 0 when the move loops               */

    /*
     * The move `endfunc_addr` installs, when it is nothing but an installer —
     * three instructions that materialise a move record and store it to
     * entity+0xD8. Zero when the endfunc does more than that, or does not exist.
     *
     * This matters because `resolve_endfunc` can only map an address to one of
     * the thirteen callbacks or thirty-two methods an implementation carries. A
     * standalone installer is neither, so it resolved to NULL and the chain died
     * — which is why sixteen of the disc's moves have `via == -1` and nothing
     * reaches them. The Gunner's whole hitscan attack is on the far side of one.
     */
    u32 endfunc_move;
    u32 frame_index;        /* into the owner's frame pool         */
    u32 frame_count;
    /* Which callback installed it, as an offset from Q2_ENT_OFS_STAND / 4,
     * or -1 when it was reached only through another move's endfunc. */
    s32 via;
} q2_cre_move;

/* How many callback slots a creature carries. 0 stand, 1 idle, 2 search,
 * 3 walk, 4 run, 6 attack, 7 melee, 11 pain, 12 die. */
#define Q2_CRE_CALLBACK_SLOTS 13

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
    u32 callback[Q2_CRE_CALLBACK_SLOTS];
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
/*
 * The module's own sound names, indexed by sound number.
 *
 * A run of 12-byte slots past the module's name, one per sound, in the order
 * the sound numbers use — checked against the Soldier, whose numbering was read
 * out of its code: idle 0, sight 1, pain 2, death 5, shotgun cock 9, and its
 * slots are in exactly that order. Returns how many were found.
 */
u32 q2_creature_sound_names(const q2_creature *c, const u8 *image, size_t size,
                            const char **out, u32 out_count);

/* ------------------------------------------------------------------------- */
/* A creature's sounds, decoded from the module's own registrations           */
/* ------------------------------------------------------------------------- */
/*
 * `q2_creature_sound_names` finds a RUN of 12-byte name slots and calls the run
 * the sound table. That works for five of seven creatures and is wrong in
 * principle for all of them: the run it finds is a string POOL, not a table.
 * The Berserk's proves it — the "table" comes back as
 * `Attack1 Attack2 Attack3 ber_pain2 inf_pain1 ber_deth2 inf_deth2 ...`, which
 * is three of its own MOVE names followed by a mix of its sounds and the
 * Infantry's. Slot index is the sound number, so a pool read as a table
 * misnumbers everything after the first wrong entry. #61 records three attempts
 * to pin the run's start, all reverted, and the third one's conclusion was that
 * the model of the problem was wrong.
 *
 * It was. The module does not index a table — it REGISTERS each sound by name
 * at init and keeps the returned handle in its own BSS:
 *
 *     801008D4  addiu t0, v1, 420      ; t0 = the name, module+0x1A4
 *     801008D8  lbu   v0, 1(t0)        ; ...packed into a0..a3 a byte at a
 *     801008DC  lbu   v1, 420(v1)      ;    time, because it is unaligned
 *     801008B8  lw    v0, 36(s2)       ; import slot 9, the sound loader
 *     801008C0  jalr  v0
 *     801008CC  sw    v0, 5976(v1)     ; the HANDLE, into module+0x1758
 *
 * and the code that plays a sound names that BSS address — which is exactly
 * what the decoder already reports as `sound(8010175C)` and what #61 found to
 * be zero in the image. Zero because it is filled at load; the NAME that fills
 * it is right there in the instruction stream.
 *
 * So the mapping is address -> name, recovered rather than guessed, and it does
 * not depend on any run, any anchor, or any ordering. It is the same decode the
 * LevelBin group selector needs (levelbin.h) and for the same reason: a
 * 12-byte string crossing a call boundary in this codebase is passed BY VALUE.
 */
typedef struct q2_cre_sound_bind {
    u32  addr;              /* the BSS word the handle is stored in */
    char name[13];          /* the name registered for it           */
} q2_cre_sound_bind;

/*
 * Decode a module's sound registrations. `load_base` is where the image was
 * relocated to. Returns how many were found; `max` bounds what is written.
 */
u32 q2_creature_sound_bindings(const u8 *image, size_t size, u32 load_base,
                               q2_cre_sound_bind *out, u32 max);

/* Which name a play site's BSS address was registered with, or NULL. */
const char *q2_creature_sound_for_addr(const q2_cre_sound_bind *binds,
                                       u32 count, u32 addr);


u32 q2_creature_move_names(const q2_creature *c, const u8 *image, size_t size,
                           const char **out, u32 out_count);

/*
 * ONE RECORD OF THE MODULE'S NAME TABLE — {u16 first; u16 last; char name[16]}.
 *
 * `q2_creature_move_names` pairs these with MOVES, and a move is not the right
 * key: the table indexes FRAME RANGES and a move can span several of them. The
 * Soldier's attack1 (0-11) covers four records — Fire 1 Ready, Aim, Shoot,
 * Done — and its walk1 (215-247) covers two. Asking for "the name of this
 * move" therefore returns nothing for exactly the moves a creature spends most
 * of its time in, and the draw then falls back to guessing a clip by length.
 *
 * The engine reaches an animation by NAME per FRAME (0x8006D330 from the frame,
 * with 0x8007EA44 turning the record into a timeline position), so the draw
 * wants this table whole and indexed by frame.
 */
typedef struct q2_cre_frame_name {
    s32 first;
    s32 last;
    const char *name;       /* into the module image, which the caller owns */
} q2_cre_frame_name;

/*
 * Read the module's name table straight through, one entry per record.
 *
 * Anchored rather than scanned: the table begins immediately past the module's
 * last instruction and runs at a fixed 20-byte stride, so it is walked from
 * there until a record fails validation. Returns the number written.
 */
u32 q2_creature_frame_names(const q2_creature *c, const u8 *image, size_t size,
                            q2_cre_frame_name *out, u32 out_count);

/* The record whose range contains `frame`, or NULL. */
const q2_cre_frame_name *q2_creature_frame_name_at(const q2_cre_frame_name *tab,
                                                   u32 count, s32 frame);

/* True when a think index's function is `jr ra` immediately -- the disc saying
 * this frame does nothing, which is an answer rather than a failed decode. */
bool q2_creature_think_is_empty(const q2_creature *c, const u8 *image,
                                size_t size, u32 base, u32 index);

/* Name records in the module that no decoded move claims. Non-zero means the
 * module names behaviour the decoder never reached. */
u32 q2_creature_unclaimed_names(const q2_creature *c, const u8 *image,
                                size_t size, const char **out, u32 out_count);

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
 * ---------------------------------------------------------------------------
 * THE WHOLE TABLE, read out one target at a time
 * ---------------------------------------------------------------------------
 * All 71 slots are now named. A `?` marks one whose identification rests on
 * shape rather than on something decisive; everything else was settled by what
 * the target actually does — the constants it uses, the entity offsets it
 * touches, the tables it indexes.
 *
 *     +0x14  0x80089E28  rand
 *     +0x18  0x8008A4D8  bios_printf
 *     +0x1C  0x8005C8C8  link_entity
 *     +0x20  0x8007270C  play_sound
 *     +0x24  0x80073518  sound_index_by_name
 *     +0x28  0x8006FC1C  local_to_world_svector
 *     +0x2C  0x8006C6C8  attachment_local_offset
 *     +0x30  0x8006D608  pose_at_position
 *     +0x34  0x8006CA2C  pose_blend_two_positions
 *     +0x38  0x8006CC44  attachment_world_point
 *     +0x3C  0x80073AA4  sound_request_clone
 *     +0x40  0x800739E4  sound_voice_still_playing
 *     +0x44  0x80073A34  sound_request_set_params
 *     +0x48  0x800E46D8  DATA: level_locals
 *     +0x4C  0x800AECF8  DATA: cvar pointer table {skill, deathmatch, ?}
 *     +0x50  0x800CBA28  DATA: the object array
 *     +0x54  0x8005E350  ai_run
 *     +0x58  0x8005ED6C  ai_walk
 *     +0x5C  0x8005CDA8  ai_stand
 *     +0x60  0x8005ED44  ai_move
 *     +0x64  0x8005EE84  ai_charge
 *     +0x68  0x800565C4  spawn_entity_from_placement
 *     +0x6C  0x8006D280  free_object
 *     +0x70  0x80055400  radius_damage_at_attachment   ?
 *     +0x74  0x8006D330  find_move_by_name
 *     +0x78  0x8002F238  blur_trail_start
 *     +0x7C  0x8004E998  beam_attach
 *     +0x80  0x80062000  monster_fire_blaster
 *     +0x84  0x80061DFC  monster_fire_bullet
 *     +0x88  0x80061ED0  monster_fire_shotgun
 *     +0x8C  0x8006217C  monster_fire_railgun
 *     +0x90  0x800621BC  monster_fire_bfg
 *     +0x94  0x800614D4  monster_fire_grenade
 *     +0x98  0x8006210C  monster_fire_rocket
 *     +0x9C  0x800621F8  monster_fire_laser
 *     +0xA0  0x80031094  muzzle_flash_light
 *     +0xA4  0x8005FA28  findradius
 *     +0xA8  0x80068A1C  zone_unoccupied
 *     +0xAC  0x8005D104  FoundTarget
 *     +0xB0  0x8005F048  HuntTarget
 *     +0xB4  0x8005EF84  range
 *     +0xB8  0x8005C4E8  VectorLength
 *     +0xBC  0x8005C59C  VectorLengthSquared
 *     +0xC0  0x8005C460  VectorMA
 *     +0xC4  0x8005C634  VectorNormalize
 *     +0xC8  0x8005F934  vectoangles
 *     +0xCC  0x8005F8E8  vectoyaw
 *     +0xD0  0x8005F104  infront
 *     +0xD4  0x8005C7B8  anglemod
 *     +0xD8  0x8005BB58  AngleVectors
 *     +0xDC  0x8005F5FC  G_ProjectSource
 *     +0xE0  0x8005CB00  entity_absolute_bounds
 *     +0xE4  0x8005B950  visible
 *     +0xE8  0x8005D8C8  M_CheckAttack
 *     +0xEC  0x80061118  fire_hit
 *     +0xF0  0x80057D54  T_Damage
 *     +0xF4  0x8005BD3C  trace
 *     +0xF8  0x800D501C  DATA: the global trace result
 *     +0xFC  0x80062240  walkmonster_start
 *     +0x100 0x8006229C  flymonster_start
 *     +0x104 0x80062268  swimmonster_start
 *     +0x108 0x8005CC58  entity_maxs_x
 *     +0x10C 0x8005CC8C  entity_maxs_y
 *     +0x110 0x8005CBF0  entity_mins_y
 *     +0x114 0x8005CBBC  entity_mins_x
 *     +0x118 0x80061DE4  register_class_method
 *     +0x11C 0x8007F71C  corpse_think   ?
 *     +0x120 0x8005A778  spawn_explosion
 *     +0x124 0x8008A108  swap_stack_pointer
 *     +0x128 0x800B28D4  DATA: the saved stack pointer for +0x124
 *     +0x12C 0x80040800  player_proximity_effect
 *
 * ---------------------------------------------------------------------------
 * Three things the full read settled that a partial one could not
 * ---------------------------------------------------------------------------
 * **The fire family is eight, not three, and it is contiguous.** +0x80..+0x9C
 * is blaster, bullet, shotgun, railgun, bfg, grenade, rocket, laser — so a
 * think function's `call(+98)` is a ROCKET rather than an unknown, which is
 * what the Tank Commander's third attack has been waiting on.
 *
 * **The Soldier's fire think contains no fire call.** It calls +0xD8, +0x2C,
 * +0x28, +0xC8, +0xD8 and +0xC0 three times, and every one of those is now
 * read: AngleVectors, the attachment offset, the SVECTOR rotate, vectoangles,
 * AngleVectors again and three VectorMAs. All of it is AIMING ARITHMETIC. The
 * shot is reached through a fresh `lui` the action decoder does not follow.
 *
 * **One slot the loader writes differently, which is why a pattern dump misses
 * it.** 69 of the 71 stores are `sw v0, N(s0)` two instructions after the
 * address is materialised. 0x8007DB8C is `sw s6, 24(s0)`, and s6 was loaded
 * once at the top of the function — 0x8008A4D8, the BIOS printf stub A(3Fh).
 * The other two non-`sw v0` stores are the header the loader writes first:
 * `sh v0, 16(s0)` = 304, the record size, and `sh v0, 18(s0)` = 1, the version.
 * That is FORMATS §14.11's "304-byte, version 1" confirmed from the other side.
 */
#define Q2_IMP_RAND      0x14
#define Q2_IMP_SOUND     0x20
#define Q2_IMP_LOCAL2WLD 0x28
#define Q2_IMP_MUZZLE    0x2C
#define Q2_IMP_RANGE     0xB4
#define Q2_IMP_FIRE_HIT  0xEC

/*
 * The projectile spawners, +0x80..+0x9C — contiguous, in the order the loader
 * writes them, and the reason a decoded think function's `call(+98)` can now be
 * routed instead of counted as unclassified.
 */
#define Q2_IMP_FIRE_BLASTER  0x80
#define Q2_IMP_FIRE_BULLET   0x84
#define Q2_IMP_FIRE_SHOTGUN  0x88
#define Q2_IMP_FIRE_RAILGUN  0x8C
#define Q2_IMP_FIRE_BFG      0x90
#define Q2_IMP_FIRE_GRENADE  0x94
#define Q2_IMP_FIRE_ROCKET   0x98
#define Q2_IMP_FIRE_LASER    0x9C

/* True for the eight spawners above and nothing else. */
static inline bool q2_imp_is_fire(u32 slot)
{
    return slot >= Q2_IMP_FIRE_BLASTER && slot <= Q2_IMP_FIRE_LASER
           && ((slot & 3u) == 0);
}

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
