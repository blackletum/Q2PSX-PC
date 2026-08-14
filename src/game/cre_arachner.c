/*
 * cre_arachner.c — the Arachner's attack, transcribed.
 *
 * One callback; everything else runs on the generic handlers and the decoded
 * actions.
 *
 * ---------------------------------------------------------------------------
 * The original, at module+0x12CC
 * ---------------------------------------------------------------------------
 *     v    = self->origin - enemy->origin        ; note the order: self first
 *     dist = import[+0xB8](v)                    ; the vector's length
 *     if (dist < 1053) return;                   ; installs nothing at all
 *
 *     self->[0x5C..0x64] = enemy->origin;        ; remembered, see below
 *     self->[0x60]      += enemy->[0x4C];
 *
 *     move = (|v.x| + |v.z| < |v.y|) ? M_94_109 : M_130_132;
 *
 * Two things worth stating.
 *
 * **It can decline.** Below 1053 units the function returns without touching
 * `currentmove`, so the creature carries on with whatever it was playing. That
 * is not a case the generic handler could express — `set_via` always installs
 * something — and it is why a close Arachner should not snap into an attack.
 *
 * **The choice is vertical.** `|dx| + |dz| < |dy|` asks whether the target is
 * further away up-or-down than it is along the floor, which for a creature that
 * walks walls is the question that decides which attack it uses. It is not a
 * range band and not a roll.
 *
 * From the census, in frame order:
 *
 *     94-109   0*5 3 0*5 4 0*4        thinks 3 and 4, both `call(+0x8C)` — the rail
 *     130-132  0*3                    no think at all
 *
 * So the vertical case shoots and the horizontal case is a three-frame gesture.
 *
 * Not written: the `self->[0x5C]` block the original fills with the enemy's
 * origin before choosing, and the accumulation into `self->[0x60]`. What reads
 * them is not established, and neither affects which move is installed.
 */
#include "ai.h"
#include "crebind.h"
#include "monster.h"

#define ARACH_MOVE_RAIL    94    /* module+0x1900 */
#define ARACH_MOVE_GESTURE 130   /* module+0x18C0 */

/* Below this the callback installs nothing. Squared, so no root is taken. */
#define ARACH_MIN_DIST_SQ (1053 * 1053)

static void arachner_attack(q2_monster *m)
{
    s32 dx, dy, dz;

    if (!m)
        return;

    if (!m->enemy) {
        q2_cre_generic_attack(m);
        return;
    }

    if (q2_monster_dist_sq(m, m->enemy->pos) < ARACH_MIN_DIST_SQ)
        return;                     /* too close: it declines, as the original does */

    dx = m->pos[0] - m->enemy->pos[0];
    dy = m->pos[1] - m->enemy->pos[1];
    dz = m->pos[2] - m->enemy->pos[2];

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dz < 0) dz = -dz;

    q2_cre_set_move(m, (dx + dz < dy) ? ARACH_MOVE_RAIL : ARACH_MOVE_GESTURE);
}

const q2_cre_impl q2_cre_arachner = {
    "Arachner",
    {
        q2_cre_generic_stand, q2_cre_generic_idle, q2_cre_generic_search,
        q2_cre_generic_walk, q2_cre_generic_run, NULL,
        arachner_attack, q2_cre_generic_melee,
        NULL, NULL, NULL, q2_cre_generic_pain, q2_cre_generic_die
    },
    { NULL },
    NULL
};
