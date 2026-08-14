/*
 * cre_infantry.c — the Infantry's attack, transcribed.
 *
 * One callback; everything else runs on the generic handlers and the decoded
 * actions.
 *
 * ---------------------------------------------------------------------------
 * The original, at module+0x1848
 * ---------------------------------------------------------------------------
 *     v0 = import[+0xB4](self, self->enemy)      ; q2_range
 *     move = (v0 != 0) ? M_184_198 : M_199_206;
 *
 * The shortest of the three read so far: one range test and no roll. Melee
 * range takes 199-206, everything else takes 184-198.
 *
 * From the census, in frame order:
 *
 *     199-206  0*2 10 0*2 11 0*2      thinks 10 and 11
 *     184-198  0*3 8 0*6 9 0*4        thinks 8 and 9
 *
 * Think 11 is the one carrying a melee and `call(+0xFC)`, the spread; think 8
 * carries `call(+0x84)`, the hitscan. So both moves shoot, and which one is a
 * question of range rather than of luck — the generic handler took 199-206
 * every time, which is the close-quarters attack used at any distance.
 */
#include "ai.h"
#include "crebind.h"
#include "monster.h"

#define INFANTRY_MOVE_CLOSE 199   /* module+0x1B98 */
#define INFANTRY_MOVE_FAR   184   /* module+0x1B70 */

static void infantry_attack(q2_monster *m)
{
    if (!m)
        return;

    if (!m->enemy) {
        q2_cre_generic_attack(m);
        return;
    }

    q2_cre_set_move(m, q2_range(m, m->enemy) == Q2_RANGE_MELEE
                           ? INFANTRY_MOVE_CLOSE : INFANTRY_MOVE_FAR);
}

const q2_cre_impl q2_cre_infantry = {
    "Infantry",
    {
        q2_cre_generic_stand, q2_cre_generic_idle, q2_cre_generic_search,
        q2_cre_generic_walk, q2_cre_generic_run, NULL,
        infantry_attack, q2_cre_generic_melee,
        NULL, NULL, NULL, q2_cre_generic_pain, q2_cre_generic_die
    },
    { NULL },
    NULL
};
