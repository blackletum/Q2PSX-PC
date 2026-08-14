#include "creworld.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
static int name_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static s32 module_index(const q2_creature_world *w, const char *name)
{
    u32 i;
    for (i = 0; i < w->mod_count; i++)
        if (name_eq_ci(w->mod[i].name, name))
            return (s32)i;
    return -1;
}

/* ------------------------------------------------------------------------- */
/*
 * Decode one module out of the CreAIBin/CreAIRel pair.
 *
 * The two chunks are parallel lists of `{char name[12]; u32 next}` headers: the
 * body of a module runs from its own header's end to the next header, and its
 * relocation stream is at the same slot of the other chunk. A duplicate name is
 * skipped rather than decoded twice, because a map that spawns three Soldiers
 * carries one module and the registry is keyed by name.
 */
static bool module_take(q2_creature_world *w, const u8 *bin, u32 boff, u32 bnext,
                        u32 bin_size, const u8 *rel, u32 roff, u32 rnext,
                        u32 rel_size)
{
    q2_creature_module *m;
    q2_reloc_stats rs;
    char nm[13];
    size_t body;

    memcpy(nm, bin + boff, 12);
    nm[12] = 0;

    if (module_index(w, nm) >= 0)
        return true;                    /* already have it */
    if (w->mod_count >= Q2_CREWORLD_MAX_MODULES)
        return false;

    m = &w->mod[w->mod_count];
    memset(m, 0, sizeof(*m));
    memcpy(m->name, nm, 13);

    body = (size_t)((bnext > boff ? bnext : bin_size) - boff -
                    Q2_RELOC_CREAI_PREAMBLE);
    m->image = (u8 *)malloc(body ? body : 1);
    if (!m->image)
        return false;
    m->size = body;
    memcpy(m->image, bin + boff + Q2_RELOC_CREAI_PREAMBLE, body);

    if (q2_reloc_apply(m->image, m->size,
                       rel + roff + Q2_RELOC_CREAI_PREAMBLE,
                       (size_t)((rnext > roff ? rnext : rel_size) - roff -
                                Q2_RELOC_CREAI_PREAMBLE),
                       Q2_CREWORLD_BASE, &rs) != Q2_OK) {
        Q2_WARN("creature module '%s' will not relocate", nm);
        free(m->image);
        m->image = NULL;
        return false;
    }

    if (!q2_creature_decode(&m->cre, m->image, m->size, Q2_CREWORLD_BASE, nm)) {
        Q2_WARN("creature module '%s' will not decode", nm);
        free(m->image);
        m->image = NULL;
        return false;
    }

    /*
     * The actions each think index performs, decoded from the module's own
     * code. Without these a creature animates and does nothing; with them the
     * six modules that have no hand transcription still make their sounds and
     * swing their claws.
     */
    q2_creature_decode_thinks(&m->cre, m->image, m->size, Q2_CREWORLD_BASE,
                              m->think, Q2_CLASS_METHOD_COUNT);
    q2_creature_bind_thinks(&m->bind, m->think, Q2_CLASS_METHOD_COUNT);

    m->ready = q2_creature_bind(&m->bind, &m->cre, q2_cre_impl_find(nm));
    if (!m->ready) {
        free(m->image);
        m->image = NULL;
        return false;
    }

    w->mod_count++;
    return true;
}

static void modules_load(q2_creature_world *w, const q2_common_file *common)
{
    const dat_chunk *bin = common->chunk[q2_common_chunk_index("CreAIBin")];
    const dat_chunk *rel = common->chunk[q2_common_chunk_index("CreAIRel")];
    u32 boff = 0, roff = 0;

    if (!bin || !rel || bin->size <= Q2_RELOC_CREAI_PREAMBLE)
        return;

    while (boff + Q2_RELOC_CREAI_PREAMBLE < bin->size &&
           roff + Q2_RELOC_CREAI_PREAMBLE < rel->size) {
        u32 bnext = q2_rd_u32(bin->data + boff + 12);
        u32 rnext = q2_rd_u32(rel->data + roff + 12);

        module_take(w, bin->data, boff, bnext, bin->size,
                    rel->data, roff, rnext, rel->size);

        if (bnext <= boff || bnext >= bin->size)
            break;
        boff = bnext;
        roff = rnext;
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Bind every class id in the table to the module that serves it.
 *
 * The variant is the id's ordinal among the table entries sharing its name —
 * see the header for why that is the reading and what it rests on.
 */
static void classes_bind(q2_creature_world *w)
{
    u32 i;

    for (i = 0; i < Q2_MONSTER_CLASS_COUNT; i++) {
        w->class_module[i]  = -1;
        w->class_variant[i] = 0;
    }

    if (!w->classes_ready)
        return;

    for (i = 0; i < w->classes.count; i++) {
        const q2_class_entry *e = &w->classes.entries[i];
        s32 mi;
        u32 j, ordinal = 0;

        if (e->is_player || !e->name[0] || e->id >= Q2_MONSTER_CLASS_COUNT)
            continue;

        mi = module_index(w, e->name);
        if (mi < 0)
            continue;

        /* How many entries of this name came before this one. */
        for (j = 0; j < i; j++)
            if (!w->classes.entries[j].is_player &&
                name_eq_ci(w->classes.entries[j].name, e->name))
                ordinal++;

        if (ordinal >= w->mod[mi].cre.class_count)
            ordinal = w->mod[mi].cre.class_count ?
                      w->mod[mi].cre.class_count - 1 : 0;

        w->class_module[e->id]  = (s8)mi;
        w->class_variant[e->id] = (u8)ordinal;
        q2_monster_set_register(&w->set, e->id);
    }
}

/* ------------------------------------------------------------------------- */
q2_result q2_creature_world_load(q2_creature_world *w, const disc *d,
                                 const q2_build_id *id,
                                 const q2_common_file *common)
{
    q2_population pop;
    u32 i;

    if (!w || !common)
        return Q2_ERR_INVALID_ARG;

    q2_creature_world_free(w);
    memset(w, 0, sizeof(*w));

    /*
     * The method table and the level clock are global — the engine's are too —
     * so a second level must not inherit the first's. Resetting here rather
     * than in the caller keeps the two loads that matter next to each other.
     */
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_level_reset();

    w->classes_ready = (q2_class_table_load(&w->classes, d, id) == Q2_OK);
    if (!w->classes_ready)
        Q2_WARN("no entity class table for this build — no creatures");

    modules_load(w, common);
    classes_bind(w);

    if (q2_population_parse(&pop, common) != Q2_OK)
        return Q2_OK;                   /* a map with no population is fine */

    if (q2_spawn_from_population(&w->set, &pop, &w->stats) != Q2_OK)
        return Q2_ERR_NO_MEMORY;

    /*
     * Now that the set exists, give every creature its module: the class byte,
     * the speed scale, the mass and the thirteen callbacks. Health comes from
     * the class table, which is per ID and not per module — it is what makes a
     * 20-health Soldier different from a 40-health one.
     */
    if (w->set.count) {
        w->model_name = (char (*)[13])calloc(w->set.count, 13);
        if (!w->model_name)
            return Q2_ERR_NO_MEMORY;
    }

    for (i = 0; i < w->set.count; i++) {
        q2_monster *m = &w->set.monsters[i];
        u32 pop_class = m->class_id;      /* before q2_creature_spawn eats it */
        s32 mi = w->class_module[pop_class];
        const q2_class_entry *e;

        if (mi < 0)
            continue;

        e = q2_class_find(&w->classes, pop_class);
        if (e && w->model_name)
            memcpy(w->model_name[i], e->name, sizeof(e->name) < 13
                                              ? sizeof(e->name) : 13);

        q2_creature_spawn(&w->mod[mi].bind, m, w->class_variant[pop_class]);

        if (e && e->health > 0) {
            m->health     = e->health;
            m->max_health = e->health;
            m->gib_health = (s16)(e->offset ? e->offset : -e->health / 2);
        }
    }

    w->ready = true;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
static void sight_place(q2_creature_world *w, const s32 eye[3])
{
    if (!eye)
        return;
    w->sight.pos[0] = eye[0];
    w->sight.pos[1] = eye[1];
    w->sight.pos[2] = eye[2];
}

void q2_creature_world_wake(q2_creature_world *w, const s32 player_eye[3])
{
    if (!w)
        return;

    q2_monster_init(&w->sight);
    /*
     * Both halves of "in use". `q2_ent_inuse` is `in_use && (spawnflags &
     * INUSE)` — the port's own bool and the engine's bit — and FindTarget runs
     * it on the sight client before anything else. Setting only the bool made
     * every creature on every map look straight through the player.
     */
    w->sight.in_use      = true;
    w->sight.spawnflags |= Q2_SVFLAG_INUSE;
    w->sight.client      = true;     /* entity+0x0C != NULL: this is a player */
    w->sight.health      = 100;
    w->sight.max_health  = 100;
    sight_place(w, player_eye);

    q2_monster_set_wake(&w->set, &w->sight);
}

u32 q2_creature_world_tick(q2_creature_world *w, const s32 player_eye[3])
{
    if (!w || !w->ready)
        return 0;

    sight_place(w, player_eye);
    return q2_monster_set_tick(&w->set);
}

const char *q2_creature_world_model_name(const q2_creature_world *w,
                                         const q2_monster *m)
{
    size_t i;

    if (!w || !m || !w->model_name || !w->set.monsters)
        return NULL;
    if (m < w->set.monsters || m >= w->set.monsters + w->set.count)
        return NULL;

    i = (size_t)(m - w->set.monsters);
    return w->model_name[i][0] ? w->model_name[i] : NULL;
}

void q2_creature_world_free(q2_creature_world *w)
{
    u32 i;

    if (!w)
        return;

    for (i = 0; i < w->mod_count; i++) {
        free(w->mod[i].image);
        w->mod[i].image = NULL;
    }
    w->mod_count = 0;

    free(w->model_name);
    w->model_name = NULL;

    q2_monster_set_free(&w->set);

    if (w->classes_ready) {
        q2_class_table_free(&w->classes);
        w->classes_ready = false;
    }

    /* The AI keeps a pointer to the sight client across ticks; this set is
     * going away, so it must not be left pointing into it. */
    if (q2_level_state.sight_client == &w->sight)
        q2_level_state.sight_client = NULL;

    w->ready = false;
}
