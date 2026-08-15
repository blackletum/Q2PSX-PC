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

/*
 * The endfunc for a move whose own ends in "install this other move".
 *
 * It takes only the entity, so which move to install has to come from the
 * entity: `m->currentmove` is still the move that just finished, and its
 * decoded record carries the address the module's installer would have written.
 */
static void chain_endfunc(q2_monster *m)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b || !b->cre || !m->currentmove)
        return;

    for (i = 0; i < b->move_count && i < b->cre->move_count; i++) {
        if (&b->move[i] != m->currentmove)
            continue;

        {
            u32 target = b->cre->move[i].endfunc_move;
            u32 k;

            if (!target)
                return;

            for (k = 0; k < b->cre->move_count; k++)
                if (b->cre->move[k].addr == target) {
                    q2_cre_set_move(m, b->cre->move[k].first_frame);
                    return;
                }
        }
        return;
    }
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
        q2_endfunc fn = resolve_endfunc(c, impl, c->move[i].endfunc_addr);

        /*
         * An endfunc that is nothing but an installer is neither a callback nor
         * a think, so `resolve_endfunc` cannot name it and returned NULL — and
         * the move chain it exists to make simply did not happen. The decoder
         * records which move such a function installs; this runs it.
         *
         * The Gunner's hitscan is entirely on the far side of one of these: its
         * fire think lives in move 144-151, which no callback installs and
         * which `M_137_143`'s three-instruction endfunc at `module+0x11D8` is
         * the only route to.
         */
        if (!fn && c->move[i].endfunc_move)
            fn = chain_endfunc;

        q2_creature_mmove(c, &c->move[i], fn, &b->move[i]);
        b->move_count++;
    }

    /* Register the think handlers for every class byte this module serves,
     * exactly as the module's own init does through import +0x118. */
    for (i = 0; i < c->class_count; i++) {
        u32 k;
        for (k = 0; k < Q2_CLASS_METHOD_COUNT; k++) {
            /*
             * A think index the implementation does not write falls back to the
             * generic one, which runs the action the decoder read out of that
             * very function. That is what makes a PARTIAL transcription
             * worthwhile: a creature can have one callback written by hand and
             * keep the decoded actions for everything else, instead of the
             * choice being all thirty-two or none.
             */
            q2_class_method fn = impl->method[k];

            if (!fn && impl != &q2_cre_generic)
                fn = q2_cre_generic.method[k];

            q2_class_method_set(c->class_byte[i], k, fn);
        }
        g_bind[c->class_byte[i]] = b;
    }

    b->ready = true;
    return true;
}

void q2_creature_bind_move_names(q2_cre_bind *b, const char *const *names,
                                 u32 count)
{
    if (!b)
        return;
    b->move_name       = names;
    b->move_name_count = count;
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

    /*
     * A callback the MODULE does not have must stay NULL, even when the
     * implementation offers one.
     *
     * The generic implementation supplies a handler for every slot, because it
     * does not know which creature it is being used for. Installing all of them
     * unconditionally tells the AI things about the creature that are not true,
     * and one of them matters: `M_CheckAttack` picks melee over a missile only
     * when `m->melee` is set (0x8005DB04, and the transcription in ai.c). The
     * Tank Commander's module has no melee callback at all — the census lists
     * stand, idle, walk, run, attack, sight, pain and die — so it was being sent
     * to a melee it does not have, 43 times in a 400-frame capture, where the
     * generic handler found no move to play and it stood there. It never
     * reached a missile attack, which is why it never fired.
     *
     * `c->callback[i]` is the module's own address for that slot, zero when it
     * has none, so the module decides and the implementation only supplies.
     */
#define INSTALL(field, slot)     m->field = c->callback[slot]                    ? (void (*)(q2_monster *))b->impl->callback[slot] : NULL

    INSTALL(stand,   0);
    INSTALL(idle,    1);
    INSTALL(search,  2);
    INSTALL(walk,    3);
    INSTALL(run,     4);
    INSTALL(attack,  6);
    INSTALL(melee,   7);
    INSTALL(bigturn, 10);
#undef INSTALL

    /* dodge, sight and checkattack take extra arguments, so they are wired
     * through their own signatures rather than the flat table. */
    if (b->impl->callback[8] && c->callback[8])
        m->sight = (void (*)(q2_monster *, q2_monster *))
                   (void *)b->impl->callback[8];
    if (b->impl->callback[9] && c->callback[9])
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
extern const q2_cre_impl q2_cre_tankcomm;
extern const q2_cre_impl q2_cre_gunner;
extern const q2_cre_impl q2_cre_infantry;
extern const q2_cre_impl q2_cre_arachner;
extern const q2_cre_impl q2_cre_berserk;

static const q2_cre_impl *const g_impls[] = {
    &q2_cre_soldier,
    &q2_cre_tankcomm,
    &q2_cre_gunner,
    &q2_cre_infantry,
    &q2_cre_arachner,
    &q2_cre_berserk,
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
