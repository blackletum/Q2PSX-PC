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
 * It does not collide with the world: `ai.[ch]`'s movement runs against the
 * stand-in world in `aiworld.[ch]`, and giving it the zone's real hull is a
 * separate piece of work. So a creature can walk through a wall, which is
 * visible rather than hidden, and is why this reports what it placed.
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
 * See openquestions #47 for what is still missing, which is how the engine
 * picks WHICH clip rather than how long a frame lasts inside one.
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

void q2_creature_world_free(q2_creature_world *w);

#endif /* Q2PSX_CREWORLD_H */
