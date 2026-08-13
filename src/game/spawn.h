/*
 * spawn.h — placing creatures in a level from the Population records.
 *
 * The last link in the chain. Population's spawn list gives a position, an
 * angle and a class_id; the class_id indexes the engine's 38-entry module
 * registry; the module holds that creature's AI. So a spawn record plus a
 * relocated module is a live monster.
 *
 * ---------------------------------------------------------------------------
 * Why class_id can be trusted
 * ---------------------------------------------------------------------------
 * It was an open question for a long time: 25 distinct values in 0..37 whose
 * meaning was unknown, and which demonstrably was NOT a ModelNames index (15 of
 * 673 records exceeded the map's name count, and resolving them by name gave
 * nonsense).
 *
 * The registry settles it. The engine stores each loaded module into a table
 * indexed by the instance's class id, and that table has exactly 38 slots —
 * the same range the spawn records use, bound-checked at the read site. Two
 * independent numbers agreeing at 38 is what makes this a link rather than a
 * coincidence.
 *
 * ---------------------------------------------------------------------------
 * What a spawned creature can and cannot do
 * ---------------------------------------------------------------------------
 * It exists, occupies a position, faces a direction, has health, and can be
 * shot and killed. It does not yet ACT, because acting means running the
 * per-frame think functions that live as MIPS code inside the module, and a
 * native port has to reimplement those creature by creature.
 *
 * That is a real and visible halfway state, not a hidden one: monsters stand
 * still. Which is why q2_spawn_stats reports how many were placed and how many
 * had no module — the difference between "no monsters here" and "monsters we
 * could not resolve" is worth being able to see.
 */
#ifndef Q2PSX_SPAWN_H
#define Q2PSX_SPAWN_H

#include "monster.h"
#include "population.h"
#include "q2psx.h"
#include "reloc.h"

/* Default health when a creature's own value is not known. Deliberately not a
 * guess at any particular monster's health — it is a placeholder that makes a
 * spawned creature killable so the rest of the chain can be exercised. */
#define Q2_SPAWN_DEFAULT_HEALTH 100

typedef struct q2_spawn_stats {
    u32 records;        /* spawn records seen                    */
    u32 placed;         /* creatures created                     */
    u32 no_module;      /* class_id with no registered module    */
    u32 out_of_range;   /* class_id >= 38, which should never happen */
} q2_spawn_stats;

typedef struct q2_monster_set {
    q2_monster *monsters;
    u32         count;
    u32         capacity;

    /* Which class ids have a module. Indexed by class_id. */
    bool        class_present[Q2_MONSTER_CLASS_COUNT];
} q2_monster_set;

void q2_monster_set_free(q2_monster_set *set);

/* Record that a class has a module available, so spawning can tell a missing
 * creature from an unregistered one. */
void q2_monster_set_register(q2_monster_set *set, u32 class_id);

/*
 * Create creatures from every non-path spawn group in the population.
 *
 * Records whose class_id has no registered module are still counted but not
 * placed, so the caller can see the shortfall rather than silently getting
 * fewer monsters than the level intends.
 */
q2_result q2_spawn_from_population(q2_monster_set *out, const q2_population *pop,
                                   q2_spawn_stats *stats);

#endif /* Q2PSX_SPAWN_H */
