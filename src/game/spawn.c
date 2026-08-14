#include "spawn.h"

#include <stdlib.h>
#include <string.h>

void q2_monster_set_free(q2_monster_set *set)
{
    if (!set)
        return;
    free(set->monsters);
    memset(set, 0, sizeof(*set));
}

void q2_monster_set_register(q2_monster_set *set, u32 class_id)
{
    if (!set || class_id >= Q2_MONSTER_CLASS_COUNT)
        return;
    set->class_present[class_id] = true;
}

static q2_monster *monster_push(q2_monster_set *set)
{
    if (set->count >= set->capacity) {
        u32 want = set->capacity ? set->capacity * 2 : 32;
        q2_monster *bigger =
            (q2_monster *)realloc(set->monsters, want * sizeof(q2_monster));
        if (!bigger)
            return NULL;
        set->monsters = bigger;
        set->capacity = want;
    }

    {
        q2_monster *m = &set->monsters[set->count++];
        q2_monster_init(m);
        return m;
    }
}

q2_result q2_spawn_from_population(q2_monster_set *out, const q2_population *pop,
                                   q2_spawn_stats *stats)
{
    q2_spawn_stats local;
    u32 gi;

    if (!out || !pop)
        return Q2_ERR_INVALID_ARG;

    memset(&local, 0, sizeof(local));

    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;
        u32 slot;

        if (!q2_pop_get_group(pop, gi, &g))
            continue;

        /* Path groups carry patrol nodes, not creatures: same stride, entirely
         * different fields. Walking them as spawns would produce monsters at
         * nonsense coordinates. */
        if (q2_pop_group_is_path(&g))
            continue;

        for (slot = 0; ; slot++) {
            q2_pop_spawn rec;
            q2_monster *m;

            if (!q2_pop_get_spawn(pop, &g, slot, &rec))
                break;

            local.records++;

            if (rec.class_id >= Q2_MONSTER_CLASS_COUNT) {
                local.out_of_range++;
                continue;
            }

            /* No module for this class means the creature cannot act. Counting
             * it separately keeps "this level has no monsters" distinguishable
             * from "we failed to resolve its monsters". */
            if (!out->class_present[rec.class_id]) {
                local.no_module++;
                continue;
            }

            m = monster_push(out);
            if (!m) {
                q2_monster_set_free(out);
                return Q2_ERR_NO_MEMORY;
            }

            m->pos[0] = rec.x;
            m->pos[1] = rec.y;
            m->pos[2] = rec.z;

            /* The stored angle is INFERRED to be on the 4096-step circle: the
             * observed range is 0..3958 and the four dominant values are close
             * to the quarter turns. */
            m->angles[2] = (s16)rec.angle;
            m->ideal_yaw = m->angles[2];

            m->class_id   = (u8)rec.class_id;
            m->health     = Q2_SPAWN_DEFAULT_HEALTH;
            m->max_health = Q2_SPAWN_DEFAULT_HEALTH;
            m->gib_health = -Q2_SPAWN_DEFAULT_HEALTH / 2;
            m->in_use     = true;
            m->spawnflags |= Q2_SVFLAG_INUSE;

            local.placed++;
        }
    }

    if (stats)
        *stats = local;

    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Driving the creatures                                                      */
/* ------------------------------------------------------------------------- */
void q2_monster_set_wake(q2_monster_set *set, q2_monster *player)
{
    u32 i;

    q2_level_state.sight_client = player;

    if (!set)
        return;

    for (i = 0; i < set->count; i++) {
        q2_monster *m = &set->monsters[i];
        if (!m->in_use || m->dead)
            continue;
        q2_monster_start_go(m);
    }
}

u32 q2_monster_set_tick(q2_monster_set *set)
{
    u32 i, ran = 0;

    /*
     * Both counters move together. `framenum` is what the sight and sound
     * alert windows compare against and `time` is what every timer is measured
     * in; the original increments them as one and separating them would make
     * FindTarget's one-tick window either always or never open.
     */
    q2_level_state.framenum++;
    q2_level_state.time++;

    if (!set)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_monster *m = &set->monsters[i];

        if (!m->in_use || m->dead || !m->think)
            continue;
        if (m->next_think > q2_level_state.time)
            continue;

        m->think(m);
        ran++;
    }

    return ran;
}
