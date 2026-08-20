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
    /*
     * DODGE — slot 5, and the reason it was never installed is not an
     * oversight in this port. It is an oversight in the ORIGINAL.
     *
     * `cre_soldier.c` used to carry a comment calling the empty slot "A GAP,
     * not a decision", which was the right instinct applied to the wrong side
     * of the boundary. Three of the disc's seven modules install a dodge
     * handler — the Soldier writes `module+0x22F0` to entity+0xF4 in all three
     * of its variant arms (0x801016C0, 0x801017C8, 0x801018CC), and the Gunner
     * and Infantry do the same — so the slot is plainly meant to be live.
     *
     * It is not. Sweeping all 632,832 bytes of `SLES_015.34` for a word load
     * off entity+0xF4 finds **zero** on any base but `sp`, and zero
     * `addiu rX, rY, 0xF4` that could reach it indirectly. Every neighbouring
     * monsterinfo slot has readers and they are easy to find — stand +0xE0 has
     * eleven (0x8005D228, 0x80061BFC, …), run +0xF0 has five, attack +0xF8 has
     * two (0x8005DB64 and 0x8005E298, both inside `M_CheckAttack`), melee +0xFC
     * has seven, sight +0x100 one, checkattack +0x104 two. Dodge has none.
     * id calls it from `check_dodge`, which the weapon fire path invokes when a
     * shot is aimed at a monster; that call site does not exist on this build.
     *
     * So the handler is installed here because the module writes it and a
     * faithful entity carries what the module wrote — and it is deliberately
     * given no caller, because the console has none. A creature on this disc
     * cannot dodge, and that is the disc's behaviour rather than this port's
     * shortfall.
     */
    if (b->impl->callback[5] && c->callback[5])
        m->dodge = (void (*)(q2_monster *, q2_monster *, s32))
                   (void *)b->impl->callback[5];

    if (b->impl->callback[8] && c->callback[8])
        m->sight = (void (*)(q2_monster *, q2_monster *))
                   (void *)b->impl->callback[8];
    if (b->impl->callback[9] && c->callback[9])
        m->checkattack = (bool (*)(q2_monster *))(void *)b->impl->callback[9];

    /*
     * Slots 11 and 12 — pain and die — which the install list simply did not
     * cover, so both were dead code in every creature on the disc. The module
     * writes them to entity+0xA0 and +0xA4 (the Soldier at 0x80101684 and
     * 0x80101690), and they take extra arguments like sight does.
     */
    if (b->impl->callback[11] && c->callback[11])
        m->pain = (void (*)(q2_monster *, s16))(void *)b->impl->callback[11];
    if (b->impl->callback[12] && c->callback[12])
        m->die = (void (*)(q2_monster *, s16))(void *)b->impl->callback[12];

    m->in_use      = true;
    m->spawnflags |= Q2_SVFLAG_INUSE;
    m->svflags    |= Q2_SVF_MONSTER;

    /*
     * THE DENOMINATOR, and it is the console's own rather than a headcount.
     *
     * `monster_start` increments `level.total_monsters` at 0x80061A64 for every
     * creature it starts that is not a good guy — the same exclusion `Killed`
     * applies to the kill counter — and this is the port's monster_start: the
     * one place an entity becomes a creature. The pair now moves together, so
     * "kills 3/9" is two numbers the original keeps rather than two scans of a
     * live array.
     */
    if (!(m->aiflags & Q2_AI_GOOD_GUY))
        q2_level_state.total_monsters++;

    /*
     * AND THE MODULE'S OWN SPAWN FUNCTION, WHICH HAD NO CALLER AT ALL.
     *
     * `q2_cre_impl.spawn` has been declared in crebind.h since the binder was
     * written, with a comment saying it is "for anything the module's spawn
     * function does beyond the callbacks". Nothing ever ran it —
     * `grep -rn 'impl->spawn' src/` returned nothing — and four of the seven
     * transcriptions now supply one.
     *
     * The Arachner's is the one that shows: its module ends export 0 by
     * writing `currentmove = Stand`, and without that a placed Arachner starts
     * with a NULL currentmove, which `q2_M_MoveFrame` returns on immediately.
     * It stood inert until an AI callback happened to install something.
     *
     * Run LAST, after the class byte, the scale, the mass and the callbacks,
     * because that is the order the loader uses: 0x8007E68C and 0x8007E698
     * write health and gib_health from the class row and only then does
     * 0x8007E6AC call the module's export 0, which is free to overwrite them.
     */
    if (b->impl && b->impl->spawn)
        b->impl->spawn(m);
}

const q2_mmove *q2_cre_find_move(const q2_monster *m, s32 first_frame)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b)
        return NULL;

    /*
     * SKIP THE CLIP PIECES. `b->move[]` carries both the module's own mmove
     * records and the ranges the decoder cut out of them at the model's clip
     * boundaries (creature.h). Only the first kind is a move a creature can be
     * put into; a piece has no endfunc and a truncated range, and installing
     * one freezes the creature.
     *
     * They stay in the array rather than being filtered out of it because
     * `chain_endfunc` above and the client's move-name lookup both index
     * `b->move[]` and `b->cre->move[]` in lockstep.
     */
    for (i = 0; i < b->move_count; i++) {
        if (b->cre && i < b->cre->move_count && b->cre->move[i].clip_piece)
            continue;
        if (b->move[i].first_frame == first_frame)
            return &b->move[i];
    }

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

/*
 * By module address, for the two creatures whose first frames collide.
 *
 * `q2_cre_bind` keeps its runtime moves parallel to the decoded ones, so the
 * decoded record's `image_offset` — which is the module-relative address the
 * callback's `lui`/`addiu` pair materialised — identifies a move exactly where
 * a frame number cannot. See the note in crebind.h for which two.
 */
const q2_mmove *q2_cre_find_move_at(const q2_monster *m, u32 module_addr)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b || !b->cre)
        return NULL;

    for (i = 0; i < b->move_count && i < b->cre->move_count; i++) {
        /*
         * Clip pieces are skipped for the same reason `q2_cre_find_move`
         * skips them: a piece is a copy of its parent record and carries the
         * parent's address, so installing one freezes the creature on a
         * fragment of its own animation.
         */
        if (b->cre->move[i].clip_piece)
            continue;
        if (b->cre->move[i].addr == module_addr)
            return &b->move[i];
    }

    return NULL;
}

bool q2_cre_set_move_at(q2_monster *m, u32 module_addr)
{
    const q2_mmove *mv = q2_cre_find_move_at(m, module_addr);

    if (!mv || !m)
        return false;

    m->currentmove = mv;
    return true;
}

/* ------------------------------------------------------------------------- */
/* The shot hook — see q2_cre_shot in crebind.h                               */
/* ------------------------------------------------------------------------- */
static void (*g_shot_fn)(q2_monster *m, const q2_cre_shot *shot, void *user);
static void  *g_shot_user;

void q2_cre_set_shot_hook(void (*fn)(q2_monster *m, const q2_cre_shot *shot,
                                     void *user),
                          void *user)
{
    g_shot_fn   = fn;
    g_shot_user = user;
}

void q2_cre_fire_shot(q2_monster *m, const q2_cre_shot *shot)
{
    if (!m || !shot)
        return;

    q2_cre_actions.fire_calls++;

    if (!g_shot_fn) {
        q2_cre_actions.fire_no_hook++;
        return;
    }
    /*
     * The two guards are the ones every refire function on the disc opens
     * with, and they are checked here rather than in seven creature files.
     */
    if (!m->enemy) {
        q2_cre_actions.fire_no_enemy++;
        return;
    }
    if (m->enemy->health <= 0) {
        q2_cre_actions.fire_dead_enemy++;
        return;
    }

    q2_cre_actions.fire_sent++;
    g_shot_fn(m, shot, g_shot_user);
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
extern const q2_cre_impl q2_cre_insane;

static const q2_cre_impl *const g_impls[] = {
    &q2_cre_soldier,
    &q2_cre_tankcomm,
    &q2_cre_gunner,
    &q2_cre_infantry,
    &q2_cre_arachner,
    &q2_cre_berserk,
    &q2_cre_insane,
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
