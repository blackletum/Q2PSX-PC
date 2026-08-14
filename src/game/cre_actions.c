/*
 * cre_actions.c — running a think function that was decoded rather than written.
 *
 * Six of the seven creature modules have no hand transcription. They do not
 * need one to act: `q2_creature_decode_thinks` reads what each of their think
 * functions DOES — the sound it plays, the claw it swings, the frame it jumps
 * to — and this executes that.
 *
 * ---------------------------------------------------------------------------
 * What is faithful and what is not, stated rather than glossed
 * ---------------------------------------------------------------------------
 * Faithful: which sound, which frame, which animation, and a melee's aim
 * vector, damage base, damage spread and kick. Those are constants in the
 * module and are read exactly. The Berserk's two attacks come out as
 * `15 + rand()%3, kick 400` and `5 + rand()%3, kick 400` against aim vectors
 * of (1020, ±48, 0), which is PC Quake II's berserker to the unit on damage
 * and kick.
 *
 * Not faithful: a step the decoder marks `gated` sits behind a branch whose
 * condition is not modelled. Two rules apply, and both are chosen to fail
 * safely rather than to look right:
 *
 *   - a gated SOUND or MOVE runs anyway. A creature that grunts every time
 *     instead of one time in five is wrong in a way you can hear and cannot be
 *     hurt by.
 *   - a gated NEXTFRAME runs only while the enemy is alive. Every refire
 *     function on the disc opens with `if (self->enemy->health <= 0) return;`,
 *     so that guard is read rather than invented — and without it a creature
 *     jumps back into its firing frame forever.
 *
 * An unclassified import call does nothing and is counted. `q2psx-inspect
 * creatures` prints which import it wanted, so the gap has an address.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "creature.h"
#include "monster.h"

/* The hooks, shared with the hand-written creatures. */
extern void (*q2_cre_sound_fn)(q2_monster *m, int which, void *user);
extern void  *q2_cre_sound_user;
extern void (*q2_cre_melee_fn)(q2_monster *m, const s32 aim[3],
                               s32 damage, s32 kick, void *user);
extern void  *q2_cre_melee_user;

static void run_step(q2_monster *m, const q2_cre_step *s)
{
    switch (s->op) {
    case Q2_CRE_OP_SOUND:
        /* The module address of the handle identifies the sample; the host
         * maps it to whatever it has loaded. */
        if (q2_cre_sound_fn)
            q2_cre_sound_fn(m, (int)s->addr, q2_cre_sound_user);
        break;

    case Q2_CRE_OP_MELEE: {
        s32 damage = s->damage_base;
        if (s->damage_rand > 1)
            damage += rand() % s->damage_rand;
        if (q2_cre_melee_fn)
            q2_cre_melee_fn(m, s->aim, damage, s->kick, q2_cre_melee_user);
        break;
    }

    case Q2_CRE_OP_NEXTFRAME:
        if (s->gated && (!m->enemy || m->enemy->health <= 0))
            break;
        m->nextframe = (u16)s->frame;
        break;

    case Q2_CRE_OP_MOVE: {
        q2_cre_bind *b = q2_cre_bind_for(m);
        u32 i;
        if (!b || !b->cre)
            break;
        for (i = 0; i < b->cre->move_count; i++)
            if (b->cre->move[i].addr == s->addr) {
                q2_cre_set_move(m, b->cre->move[i].first_frame);
                break;
            }
        break;
    }

    case Q2_CRE_OP_AIFLAG:
        /*
         * Not gated on anything: a duck helper's stores are the whole of what
         * it does, and both arms of `monster_duck_hold` are stores, so running
         * every one it decoded reproduces the function rather than half of it.
         */
        m->aiflags &= ~s->flag_clear;
        m->aiflags |=  s->flag_set;
        break;

    case Q2_CRE_OP_CALL:
    default:
        break;
    }
}

void q2_cre_run_think(q2_monster *m, u32 index)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    const q2_cre_think *t;
    u32 i;

    if (!b || !b->think || index >= b->think_count)
        return;

    t = &b->think[index];
    for (i = 0; i < t->step_count; i++)
        run_step(m, &t->step[i]);
}
