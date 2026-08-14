#include "crebind.h"

#include <string.h>

/* One bind per class byte, which is how the engine's own table is indexed. */
static q2_cre_bind *g_bind[Q2_CLASS_COUNT];

void q2_cre_bind_reset(void)
{
    memset(g_bind, 0, sizeof(g_bind));
}

q2_cre_bind *q2_cre_bind_for(const q2_monster *m)
{
    if (!m)
        return NULL;
    return g_bind[m->class_byte];
}

/*
 * Resolve a move's end callback.
 *
 * The address is the module's, so it is looked up in the two sets the
 * implementation already covers: the monsterinfo callbacks and the method
 * table. Anything else leaves the move looping.
 */
static q2_endfunc resolve_endfunc(const q2_creature *c, const q2_cre_impl *impl,
                                  u32 addr)
{
    u32 i;

    if (!addr)
        return NULL;

    for (i = 0; i < 13; i++)
        if (c->callback[i] == addr)
            return (q2_endfunc)impl->callback[i];

    for (i = 0; i < c->method_count; i++)
        if (c->method[i] == addr)
            return (q2_endfunc)impl->method[i];

    return NULL;
}

bool q2_creature_bind(q2_cre_bind *b, const q2_creature *c,
                      const q2_cre_impl *impl)
{
    u32 i;

    if (!b || !c || !impl)
        return false;

    memset(b, 0, sizeof(*b));
    b->cre  = c;
    b->impl = impl;

    for (i = 0; i < c->move_count && i < Q2_CRE_MAX_MOVES; i++) {
        q2_creature_mmove(c, &c->move[i],
                          resolve_endfunc(c, impl, c->move[i].endfunc_addr),
                          &b->move[i]);
        b->move_count++;
    }

    /* Register the think handlers for every class byte this module serves,
     * exactly as the module's own init does through import +0x118. */
    for (i = 0; i < c->class_count; i++) {
        u32 k;
        for (k = 0; k < Q2_CLASS_METHOD_COUNT; k++)
            q2_class_method_set(c->class_byte[i], k, impl->method[k]);
        g_bind[c->class_byte[i]] = b;
    }

    b->ready = true;
    return true;
}

void q2_creature_bind_thinks(q2_cre_bind *b, const q2_cre_think *think,
                             u32 count)
{
    if (!b)
        return;
    b->think       = think;
    b->think_count = count;
}

void q2_creature_spawn(q2_cre_bind *b, q2_monster *m, u32 class_index)
{
    const q2_creature *c;

    if (!b || !b->ready || !m)
        return;

    c = b->cre;

    if (class_index >= c->class_count)
        class_index = 0;

    m->class_byte = c->class_byte[class_index];
    m->class_id   = c->class_byte[class_index];

    if (c->speed_scale > 0)
        m->speed_scale = (u8)c->speed_scale;

    m->stand       = (void (*)(q2_monster *))b->impl->callback[0];
    m->idle        = (void (*)(q2_monster *))b->impl->callback[1];
    m->search      = (void (*)(q2_monster *))b->impl->callback[2];
    m->walk        = (void (*)(q2_monster *))b->impl->callback[3];
    m->run         = (void (*)(q2_monster *))b->impl->callback[4];
    m->attack      = (void (*)(q2_monster *))b->impl->callback[6];
    m->melee       = (void (*)(q2_monster *))b->impl->callback[7];
    m->bigturn     = (void (*)(q2_monster *))b->impl->callback[10];

    /* dodge, sight and checkattack take extra arguments, so they are wired
     * through their own signatures rather than the flat table. */
    if (b->impl->callback[8])
        m->sight = (void (*)(q2_monster *, q2_monster *))
                   (void *)b->impl->callback[8];
    if (b->impl->callback[9])
        m->checkattack = (bool (*)(q2_monster *))(void *)b->impl->callback[9];

    m->in_use      = true;
    m->spawnflags |= Q2_SVFLAG_INUSE;
    m->svflags    |= Q2_SVF_MONSTER;
}

const q2_mmove *q2_cre_find_move(const q2_monster *m, s32 first_frame)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b)
        return NULL;

    for (i = 0; i < b->move_count; i++)
        if (b->move[i].first_frame == first_frame)
            return &b->move[i];

    return NULL;
}

bool q2_cre_set_move(q2_monster *m, s32 first_frame)
{
    const q2_mmove *mv = q2_cre_find_move(m, first_frame);

    if (!mv || !m)
        return false;

    m->currentmove = mv;
    return true;
}

/* ------------------------------------------------------------------------- */
/* The registry of built-in implementations                                   */
/* ------------------------------------------------------------------------- */
extern const q2_cre_impl q2_cre_soldier;
extern const q2_cre_impl q2_cre_generic;

static const q2_cre_impl *const g_impls[] = {
    &q2_cre_soldier,
    NULL
};

const q2_cre_impl *const *q2_cre_impls(void)
{
    return g_impls;
}

const q2_cre_impl *q2_cre_impl_find(const char *module_name)
{
    u32 i;

    if (!module_name)
        return NULL;

    for (i = 0; g_impls[i]; i++)
        if (g_impls[i]->name && strcmp(g_impls[i]->name, module_name) == 0)
            return g_impls[i];

    /*
     * No hand-written implementation. The generic one gives the creature its
     * decoded animations, its callbacks and the shared AI — everything except
     * the per-frame actions, which it simply does not perform. That is a
     * creature that walks, chases and dies correctly but never fires, which is
     * an honest partial rather than a wrong guess.
     */
    return &q2_cre_generic;
}
