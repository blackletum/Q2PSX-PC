/*
 * crebind.h — joining a decoded creature to a C implementation of it.
 *
 * `creature.[ch]` reads a module: its class bytes, its speed scale and mass,
 * its callbacks, and every move and frame it owns. None of that needs writing
 * by hand. What does need writing is the body of each think index — the sound,
 * the shot, the claw — and this is where the two meet.
 *
 * ---------------------------------------------------------------------------
 * A move is named by its first frame, not by an index
 * ---------------------------------------------------------------------------
 * The obvious binding is positional: move 0 is stand, move 1 is walk. It is
 * also fragile, because the decoder's order depends on which callback reached
 * a move first, and that changes the moment the decoder improves.
 *
 * A move's FIRST FRAME NUMBER is stable, is meaningful, and is what id's own
 * source names — `FRAME_stand101`, `FRAME_attak101`. So an implementation asks
 * for the move starting at frame 146 and gets the Soldier's stand1 whatever
 * order it was found in. If the disc does not have such a move the request
 * fails visibly rather than silently installing the wrong animation.
 *
 * ---------------------------------------------------------------------------
 * End callbacks resolve themselves
 * ---------------------------------------------------------------------------
 * A move's end callback is a module address, and the C side has no way to call
 * it. It does not need one: that address is almost always also a monsterinfo
 * callback or a method-table entry, both of which the implementation already
 * supplies. So the binder resolves an endfunc by looking the address up in
 * those two sets. An address in neither leaves the move looping, which is what
 * a move with no end callback does anyway.
 */
#ifndef Q2PSX_CREBIND_H
#define Q2PSX_CREBIND_H

#include "creature.h"
#include "monster.h"
#include "q2psx.h"

/*
 * The hand-written half of a creature.
 *
 * `callback` is indexed exactly as `q2_creature.callback`: stand, idle,
 * search, walk, run, dodge, attack, melee, sight, checkattack, bigturn, pain,
 * die. `method` is indexed by the think byte in an animation frame.
 *
 * A NULL entry means "the module installs one here and this port does not
 * implement it yet" — which is a visible gap, not a crash: the frame driver
 * skips a NULL method and the AI skips a NULL callback.
 */
/*
 * FOUR OF THE THIRTEEN SLOTS DO NOT HAVE THIS SIGNATURE, and the table stores
 * them anyway.
 *
 *     5  dodge        void (*)(q2_monster *, q2_monster *other, s32 eta)
 *     8  sight        void (*)(q2_monster *, q2_monster *other)
 *     9  checkattack  bool (*)(q2_monster *)
 *    11  pain         void (*)(q2_monster *, s16 damage)
 *    12  die          void (*)(q2_monster *, s16 damage)
 *
 * `q2_creature_spawn` installs each of those through its own signature rather
 * than through the flat table, so the entry here must be written with an
 * explicit cast — `(q2_class_method)(void *)soldier_pain`. Writing the bare
 * function name is a compile error, which is the right outcome: the alternative
 * is five separate tables for thirteen slots.
 */
typedef struct q2_cre_impl {
    const char *name;                       /* the module name it matches */
    q2_class_method callback[13];
    q2_class_method method[Q2_CLASS_METHOD_COUNT];

    /* Called after the framework's own spawn setup, for anything the module's
     * spawn function does beyond the callbacks — a Soldier picks its weapon
     * variant here. `variant` is the entity's skinnum. */
    void (*spawn)(q2_monster *m);
} q2_cre_impl;

/* A decoded creature joined to an implementation and ready to spawn. */
typedef struct q2_cre_bind {
    const q2_creature *cre;
    const q2_cre_impl *impl;
    q2_mmove           move[Q2_CRE_MAX_MOVES];
    u32                move_count;

    /* The module's name for each move, parallel to `move[]`. NULL entries are
     * normal: not every move is named. See q2_creature_bind_move_names. */
    const char *const *move_name;
    u32                move_name_count;
    bool               ready;

    /*
     * What each think index does, decoded from the module. Supplied by the
     * caller because decoding needs the relocated image, which the bind does
     * not keep. A creature with these bound performs its real per-frame
     * actions; one without them animates and does nothing.
     */
    const q2_cre_think *think;
    u32                 think_count;
} q2_cre_bind;

/* Attach a decoded action set. Call before spawning from this bind. */
/*
 * The module's own NAME for each decoded move, in move order.
 *
 * Supplied by the caller for the same reason the thinks are: reading them needs
 * the relocated image, which the bind does not keep. Names matter because they
 * are how the ENGINE reaches an animation — `0x8006D330` walks block D comparing
 * 12-byte names — so a move without one cannot be posed the way the disc does
 * it. `names[i]` may be NULL where the module names nothing.
 */
void q2_creature_bind_move_names(q2_cre_bind *b, const char *const *names,
                                 u32 count);

void q2_creature_bind_thinks(q2_cre_bind *b, const q2_cre_think *think,
                             u32 count);

/*
 * Bind and register.
 *
 * Installs `impl->method` into the class method table for every class byte the
 * module serves, and builds the runtime moves with their end callbacks
 * resolved. `b` must outlive every creature spawned from it.
 */
bool q2_creature_bind(q2_cre_bind *b, const q2_creature *c,
                      const q2_cre_impl *impl);

/* Set up `m` as one of this creature: class byte, scale, mass, callbacks. */
void q2_creature_spawn(q2_cre_bind *b, q2_monster *m, u32 class_index);

/*
 * The move whose animation starts at `first_frame`, or NULL.
 *
 * This is what an implementation's callbacks use, and it is deliberately a
 * lookup rather than an index: a missing animation is then a NULL that shows
 * up as a creature that does not play it, instead of a creature that plays
 * something else.
 */
const q2_mmove *q2_cre_find_move(const q2_monster *m, s32 first_frame);

/* Install that move on `m`. Returns false when the disc has no such move. */
bool q2_cre_set_move(q2_monster *m, s32 first_frame);

/*
 * The same, keyed on the move record's own MODULE ADDRESS.
 *
 * A first frame is the right key almost everywhere and it is not unique: the
 * Arachner's walk (module+0x17DC) and run (module+0x1808) both cover frames
 * 16-24 and differ only in the per-frame AI verb, and the Tank Commander's
 * walk (module+0x1BC0) and run (module+0x1C3C) both cover 34-49. Asking for
 * frame 16 gets whichever the decoder found first, so an Arachner told to run
 * would walk.
 *
 * Both transcriptions had grown a private copy of this before it was shared;
 * this is that copy, once. Use it only where the frame range is genuinely
 * ambiguous — `q2_cre_set_move` stays the readable form, and a module address
 * in a callback is a constant only the disassembly can justify.
 */
const q2_mmove *q2_cre_find_move_at(const q2_monster *m, u32 module_addr);
bool q2_cre_set_move_at(q2_monster *m, u32 module_addr);

/* Which bind a live creature belongs to, by its class byte. */
q2_cre_bind *q2_cre_bind_for(const q2_monster *m);
void         q2_cre_bind_reset(void);

/* The built-in implementations, NULL-terminated. */
const q2_cre_impl *const *q2_cre_impls(void);

/*
 * The two hooks a transcribed creature needs from outside itself: a sound and
 * a shot. Both are the engine's job in the original — the module reaches them
 * through its import table — and keeping them as hooks stops every creature
 * from having to know about combat.
 *
 * The fire hook receives a packed (weapon_table * 8 + flash_number), which is
 * how the Soldier's three muzzle-flash tables are distinguished.
 */
void q2_cre_set_sound_hook(void (*fn)(q2_monster *m, int which, void *user),
                           void *user);
void q2_cre_set_fire_hook(void (*fn)(q2_monster *m, int flash, void *user),
                          void *user);

/* ------------------------------------------------------------------------- */
/*
 * A CREATURE'S SHOT, with the figures its own module carries.
 *
 * The old hook took a single `int`, which is why every creature but the
 * Soldier fired into nothing: it could name the spawner and not the shot. The
 * arguments the modules pass are now read for all seven, and every one of them
 * is id's own number —
 *
 *     Tankcomm  rocket   damage 50, speed 550     blaster damage 16, speed 800
 *               bullet   damage 20, kick 4, spread 300/500
 *     Gunner    grenade  damage 50, speed 600
 *               bullet   damage  3, kick 4, spread 300/500
 *     Infantry  bullet   damage  3, kick 4, spread 300/500
 *     Arachner  railgun  damage 50, kick 100
 *     Soldier   blaster  damage  5, speed 600
 *               shotgun  damage  2, kick 1, spread 1000/500, 12 pellets
 *               bullet   damage  2, kick 4, spread 300/500
 *
 * — `DEFAULT_BULLET_HSPREAD` 300 and `VSPREAD` 500 recurring across four
 * unrelated modules is the check that says the argument positions are right.
 *
 * `slot` is one of the `Q2_IMP_FIRE_*` offsets, so the host can tell a rocket
 * from a rail without the creature having to. A field a given weapon does not
 * use is zero, and a hook that finds `damage == 0` should decline rather than
 * invent: that is the state a creature whose figures are not read would be in.
 */
typedef struct q2_cre_shot {
    u32 slot;           /* Q2_IMP_FIRE_* — which engine spawner            */
    s32 flash;          /* the muzzle-flash index the module passes        */
    s32 damage;
    s32 speed;          /* projectiles: the bolt's, grenade's or rocket's  */
    s32 kick;
    s32 hspread;        /* hitscan spread, id's DEFAULT_* where it matches */
    s32 vspread;
    s32 count;          /* pellets; 1 for anything that is not a shotgun   */
} q2_cre_shot;

void q2_cre_set_shot_hook(void (*fn)(q2_monster *m, const q2_cre_shot *shot,
                                     void *user),
                          void *user);

/* Fire one, from a transcribed creature. Does nothing without a hook. */
void q2_cre_fire_shot(q2_monster *m, const q2_cre_shot *shot);

/* The melee hook the decoded creatures reach: `fire_hit` with the module's own
 * aim vector, damage and kick. */
void q2_cre_set_melee_hook(void (*fn)(q2_monster *m, const s32 aim[3],
                                      s32 damage, s32 kick, void *user),
                           void *user);

/* Run the decoded actions for one think index on `m`. */
/*
 * Where a decoded action went, so "it never fired" can be told apart from "it
 * never got that far". Cumulative; zero it before a run.
 */
typedef struct q2_cre_action_stats {
    u32 thinks_run;
    u32 thinks_unbound;      /* no decoded think for that index               */
    u32 calls_seen;          /* CALL steps reached                            */
    u32 calls_unclassified;  /* an import slot with no meaning yet            */
    u32 fire_calls;          /* CALL steps that named a projectile spawner    */
    u32 fire_sent;           /* ...and reached the hook                       */
    u32 fire_no_hook;
    u32 fire_no_enemy;
    u32 fire_dead_enemy;

    /*
     * Which callback slots the generic implementation could and could not find
     * a move for — 0 stand, 1 idle, 2 search, 3 walk, 4 run, 6 attack,
     * 7 melee, 11 pain, 12 die.
     *
     * Sized to the creature's own callback table (`u32 callback[13]`) and NOT
     * to eight, which is what it was. Pain and die live at 11 and 12, so the
     * recorder's `slot < 8` guard dropped them both on the floor and the stats
     * line then read two words PAST the array to print them. Two subsystems
     * disagreeing about how many slots there are is exactly the kind of thing
     * a fixed 8 hides.
     */
    u32 move_via_set[Q2_CRE_CALLBACK_SLOTS];
    u32 move_via_missing[Q2_CRE_CALLBACK_SLOTS];

    /* How often each think index actually ran. A move whose fire think sits
     * six frames in only reaches it if the animation is not cut short. */
    u32 think_hits[32];
} q2_cre_action_stats;

extern q2_cre_action_stats q2_cre_actions;

/* The generic implementation and its handlers, so a PARTIAL transcription can
 * write one callback and keep the decoded actions for the rest. */
extern const q2_cre_impl q2_cre_generic;

void q2_cre_generic_stand(q2_monster *m);
void q2_cre_generic_idle(q2_monster *m);
void q2_cre_generic_search(q2_monster *m);
void q2_cre_generic_walk(q2_monster *m);
void q2_cre_generic_run(q2_monster *m);
void q2_cre_generic_attack(q2_monster *m);
void q2_cre_generic_melee(q2_monster *m);
void q2_cre_generic_pain(q2_monster *m);
void q2_cre_generic_die(q2_monster *m);

void q2_cre_run_think(q2_monster *m, u32 index);

/* Skill, which several creatures gate their refire and opening shot on. */
void q2_cre_set_skill(s32 skill);
s32  q2_cre_skill(void);

/*
 * The bank name for one of the Soldier's eleven sounds, or NULL. The module
 * carries the names itself, 12-byte fields at `module+0x1D0` parallel to the
 * handle table at `+0x32A0`; see cre_soldier.c.
 */
const char *q2_cre_soldier_sound_name(int which);

/* Find the implementation for a module name, or NULL when the port has none. */
const q2_cre_impl *q2_cre_impl_find(const char *module_name);

#endif /* Q2PSX_CREBIND_H */
