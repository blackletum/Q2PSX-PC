/*
 * cre_gunner.c — the Gunner's attack, transcribed.
 *
 * One callback, as with the Tank Commander. Everything else runs on the generic
 * handlers and the decoded actions.
 *
 * ---------------------------------------------------------------------------
 * The original, at module+0x1814
 * ---------------------------------------------------------------------------
 *     v0 = import[+0xB4](self, self->enemy)
 *     if (v0 == 0)                 move = M_137_143;    ; module+0x1C54
 *     else if (import[+0x14]() & 1) move = M_108_128;   ; module+0x1CF4
 *     else                          move = M_137_143;
 *
 * `import[+0xB4]` is `0x8005EF84`, and it is `q2_range` — which this port
 * already carries. The identification is not by shape alone: it subtracts the
 * two origins, takes `q2_vector_length_sq` (`0x8005C59C`, already named in
 * ai.h), and compares against 0x003F803F or 0x000FE00F depending on whether the
 * entity's class byte at +0x23 is 68 — which is 2040² - 1 and 1020² - 1, the
 * long-melee and ordinary melee bands, with the same off-by-one the port's own
 * `q2_range` reproduces. So the branch is `range == Q2_RANGE_MELEE`.
 *
 * `import[+0x14]` is the random the Tank Commander's table also rolls; here only
 * its low bit is used, so this is a coin and not a weighted table.
 *
 * ---------------------------------------------------------------------------
 * What each move is
 * ---------------------------------------------------------------------------
 * From the census, in frame order:
 *
 *     108-128  0*4 1 0*2 1 0*2 1 0*2 1 0*7    four spaced think 1s
 *     137-143  13 0*6                          think 13 on its first frame
 *
 * Both are firing animations, which is why the Gunner — unlike the Tank
 * Commander — was not silenced by the generic handler taking the first move.
 * `q2_creature_move_via` happened to pick one the original also picks. This
 * makes that agreement deliberate rather than lucky, and adds the melee-range
 * branch, which the generic handler had no way to know about.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "monster.h"

#define GUNNER_MOVE_BURST 108   /* module+0x1CF4 */
#define GUNNER_MOVE_SNAP  137   /* module+0x1C54 */

static void gunner_attack(q2_monster *m)
{
    if (!m)
        return;

    if (!m->enemy) {
        q2_cre_generic_attack(m);
        return;
    }

    /* Close enough to be in melee range, and it takes the short animation. */
    if (q2_range(m, m->enemy) == Q2_RANGE_MELEE) {
        q2_cre_set_move(m, GUNNER_MOVE_SNAP);
        return;
    }

    q2_cre_set_move(m, (rand() & 1) ? GUNNER_MOVE_BURST : GUNNER_MOVE_SNAP);
}

const q2_cre_impl q2_cre_gunner = {
    "Gunner",
    {
        q2_cre_generic_stand, q2_cre_generic_idle, q2_cre_generic_search,
        q2_cre_generic_walk, q2_cre_generic_run, NULL,
        gunner_attack, q2_cre_generic_melee,
        NULL, NULL, NULL, q2_cre_generic_pain, q2_cre_generic_die
    },
    { NULL },
    NULL
};
