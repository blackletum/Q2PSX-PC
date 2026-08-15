/*
 * creworld.h — a level's live creatures.
 *
 * Every piece of this existed and nothing joined them up. `reloc.[ch]`
 * relocates a `CreAIBin` module, `creature.[ch]` decodes one, `crebind.[ch]`
 * joins it to an implementation, `spawn.[ch]` turns Population records into
 * monsters and `ai.[ch]` runs them — but the only caller was the inspector,
 * which decodes a module to report on it and draws a creature at its spawn
 * point without ever ticking one. The client had no monsters at all: a level
 * was its geometry, its items and nothing that moves.
 *
 * This is the join. Give it a map's COMMON.DAT and it produces a set of live
 * creatures with their AI running.
 *
 * ---------------------------------------------------------------------------
 * Three numbers name a creature, and they are not the same number
 * ---------------------------------------------------------------------------
 * This is the part that has to be got right, so it is written down.
 *
 *   - a Population spawn record carries a **class id**, 0..37, which indexes
 *     the engine's module registry (spawn.h);
 *   - the **class table** in the executable turns that id into a NAME and a
 *     health (classtable.h) — and names are not unique: ids 18, 19 and 20 are
 *     all `Soldier`, with 30, 20 and 40 health;
 *   - a module serves one or more **class bytes**, 64..94, and `Soldier`'s
 *     three are 87, 89 and 88 in that order (creature.h).
 *
 * The module is found by matching its 12-byte header name against the class
 * table's name, which is the direction the engine's own lookup runs
 * (`q2_class_find_name`). The VARIANT — which of the module's class bytes a
 * given spawn is — is taken from the position of that id among the class table
 * entries sharing the name: the first `Soldier` record gets class_byte[0], the
 * second class_byte[1], and so on. Three ids, three class bytes, in the order
 * both tables state them. That correspondence is inferred from the two tables
 * agreeing in length and order rather than read out of code, and it is the one
 * inference in this file.
 *
 * ---------------------------------------------------------------------------
 * What a live creature does and does not do here
 * ---------------------------------------------------------------------------
 * It notices the player, turns, chases, remembers where it last saw them, and
 * plays its own animations off the disc. Its health is the class table's.
 *
 * It collides with the world too, through `aiworld.[ch]` bound to the zone's own
 * `PrimaryColl` — sight, stepping and the ground probe all. That was not true
 * when this file was written: the binding read `q2_coll_move`'s "stopped" return
 * as "could not start", so every creature was told it was buried in the floor it
 * stood on and none of them moved. See openquestions #48.
 *
 * What it still cannot do is anything its module's think functions have not been
 * transcribed for. The Soldier's are written; the other six run on the generic
 * implementation, which animates and chases correctly and performs no per-frame
 * action, so a Tank Commander hunts you down and swings without firing.
 */
#ifndef Q2PSX_CREWORLD_H
#define Q2PSX_CREWORLD_H

#include "classtable.h"
#include "crebind.h"
#include "creature.h"
#include "dat.h"
#include "disc.h"
#include "ident.h"
#include "level.h"
#include "monster.h"
#include "population.h"
#include "q2psx.h"
#include "spawn.h"

/* The disc has seven distinct modules; a map carries at most a few of them. */
#define Q2_CREWORLD_MAX_MODULES 16

/*
 * Model-clip ticks per AI animation frame.
 *
 * Not read out of code — measured. Every one of the Soldier's 31 CastList clip
 * lengths is divisible by three, and dividing by three turns them into the
 * lengths of its module's moves: clips 1..4 are exactly the four consecutive
 * moves 50-54, 55-61, 62-79 and 80-96. `Q2_MODEL_TICKS_PER_FRAME` is 10 and is
 * the view weapon's rate, which is a different thing.
 *
 * How the engine picks a clip is no longer a question: it does not pick one.
 * `0x8006B924` walks the clip list subtracting lengths from a running position,
 * so the clips are one timeline and this scale is what puts an AI frame on it.
 * See `q2_model_anim_at` and openquestions #47.
 */
#define Q2_CRE_TICKS_PER_FRAME 3

/*
 * Where a module image is relocated to. Far from the executable's own address
 * space so a pointer that escaped the module is obvious rather than plausible —
 * the same base the inspector uses, so an address printed by one means the
 * same thing in the other.
 */
#define Q2_CREWORLD_BASE 0x80100000u

typedef struct q2_creature_module {
    char         name[13];
    u8          *image;                       /* relocated body, owned here */
    size_t       size;
    q2_creature  cre;
    q2_cre_bind  bind;
    q2_cre_think think[Q2_CLASS_METHOD_COUNT];

    /*
     * The module's own name for each move, pointing into `image`. Kept here
     * because the bind does not own the image and the names must outlive the
     * call that reads them. They are how the ENGINE reaches an animation --
     * 0x8006D330 walks block D comparing 12-byte names -- so the draw path
     * needs them to pose a creature the way the disc does.
     */
    const char  *move_name[Q2_CRE_MAX_MOVES];

    bool         ready;
} q2_creature_module;

typedef struct q2_creature_world {
    q2_creature_module mod[Q2_CREWORLD_MAX_MODULES];
    u32                mod_count;

    q2_class_table     classes;
    bool               classes_ready;

    /* class_id -> module, and which of that module's class bytes it is. */
    s8                 class_module[Q2_MONSTER_CLASS_COUNT];
    u8                 class_variant[Q2_MONSTER_CLASS_COUNT];

    q2_monster_set     set;
    q2_spawn_stats     stats;

    /* Kept rather than dropped, because CREBATCH names a GROUP and the names
     * live here. Borrows COMMON.DAT, which outlives the zone. */
    q2_population      pop;
    bool               pop_ready;

    /*
     * The class table name each placed creature resolved to, one per monster.
     *
     * It has to be kept, because `q2_creature_spawn` OVERWRITES `class_id` with
     * the module's class BYTE — 87 for a Soldier, not the Population id 18 —
     * since that byte is what `q2_cre_bind_for` keys the runtime bind on. After
     * spawning there is no longer anything on the monster that indexes the
     * class table, so the name is taken while it is still there.
     */
    char             (*model_name)[13];

    /*
     * The player, as the AI sees one. The sim owns the real player; this is the
     * entity FindTarget acquires, moved to the player's eye every tick. It is
     * held here because `q2_level_state.sight_client` is a pointer the AI keeps
     * across ticks and it must not dangle.
     */
    q2_monster         sight;

    bool               ready;
} q2_creature_world;

/*
 * Decode every creature module this map ships, and place every spawn record
 * whose class resolves to one of them.
 *
 * `common` must outlive the call but not the result — the modules are copied
 * and relocated into storage this owns. Returns Q2_OK even when a map places no
 * creatures; `w->stats` says what happened.
 */
q2_result q2_creature_world_load(q2_creature_world *w, const disc *d,
                                 const q2_build_id *id,
                                 const q2_common_file *common);

/*
 * Wake every creature and point the AI at the player. Call once the player is
 * standing where the level starts them.
 */
void q2_creature_world_wake(q2_creature_world *w, const s32 player_eye[3]);

/*
 * One AI tick. `player_eye` moves the sight client first, so a creature that
 * looks this tick looks at where the player is now.
 *
 * Returns how many creatures thought, which is zero on a level with none.
 */
u32 q2_creature_world_tick(q2_creature_world *w, const s32 player_eye[3]);

/* The model name for a live creature — the class table's name, which is also
 * the name its model carries in the map's CastList. NULL when unresolvable. */
const char *q2_creature_world_model_name(const q2_creature_world *w,
                                         const q2_monster *m);

/*
 * The first frame of this creature's death move, or -1 when its module carries
 * no named move that says death. Pair it with `q2_cre_set_move` to put a body
 * into its death animation instead of deleting it.
 */
s32 q2_creature_world_death_frame(const q2_creature_world *w,
                                  const q2_monster *m);

/*
 * The name of sound `index` for this creature, out of its own module's table,
 * or NULL. Every module carries one; only the Soldier's had been read.
 */
const char *q2_creature_world_sound_name(const q2_creature_world *w,
                                         const q2_monster *m, u32 index);

/*
 * SUMMON a Population group — what `CREBATCH` means.
 *
 * A group is not spawned because it exists. `0x80056C60` takes a twelve-byte
 * name, finds the group and sets bit 0 of its flags; the spawn pass runs only
 * the selected ones and sets bit 1 so a group cannot run twice
 * (population.h). The flags word is ZERO ON DISC for all 222 groups of all 49
 * maps, so at load nothing is selected.
 *
 * This port spawns every record up front and holds the ones a script owns
 * DORMANT — `in_use` clear — because the alternative is re-running the spawn
 * pass mid-level against a set other systems already hold pointers into. The
 * effect is the console's: an ambush is not standing there until it is called
 * for. Returns how many creatures this call woke, which is 0 for a group
 * already summoned — the bit-1 latch.
 */
u32 q2_creature_world_summon(q2_creature_world *w, const char *group);

/*
 * Hold back every creature whose group is a script's rather than the level's.
 *
 * WHICH groups stand there is the map's LevelBin's answer, and it is now read
 * rather than guessed at (levelbin.h): `q2_levelbin_selected` decodes the
 * module's calls to the engine's group selector and hands back the names. Pass
 * the module and it is used.
 *
 * Without one, the fallback is the naming rule #79 used — a group called
 * `Zone<N>` is that zone's own population and anything else is a batch. That
 * rule is not a guess any more either: across the disc the modules make 83
 * selector calls, 71 resolve to a real group, and **69 of those 71 claim a
 * zone**. The two that do not are JAIL3's `Jail4Return` and `Jail5Return` —
 * populations for coming BACK to a level, which the module selects
 * conditionally and a static scan cannot gate.
 *
 * Call after the world has loaded and before it wakes. Returns how many were
 * held.
 */
u32 q2_creature_world_hold_batches(q2_creature_world *w, int resident_zone,
                                   const u8 *levelbin, u32 levelbin_size);

void q2_creature_world_free(q2_creature_world *w);

#endif /* Q2PSX_CREWORLD_H */
