/*
 * cre_berserk.c — the Berserk's melee, transcribed.
 *
 * The Berserk has no attack callback at all: it is a melee creature, and the
 * census lists stand, search, walk, run, melee, sight, pain and die. So the one
 * function worth writing here is the melee, and it is the simplest of the five
 * read so far.
 *
 * ---------------------------------------------------------------------------
 * The original, at module+0x11D8
 * ---------------------------------------------------------------------------
 *     r    = import[+0x14]();               ; the random, 0..32767
 *     move = (r & 1) ? M_84_95 : M_76_83;
 *
 * A coin, and nothing else — no range test, no health test. From the census, in
 * frame order:
 *
 *     76-83  0*2 3 4 0*4      thinks 3 and 4
 *     84-95  0*4 3 6 0*6      thinks 3 and 6
 *
 * Both are swings; they differ in which second think follows the shared one. So
 * the generic handler taking the first was half right, and what this adds is
 * the other half of the coin — the creature alternates its two swings instead
 * of always throwing the same one.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "monster.h"

#define BERSERK_SWING_A 76   /* module+0x15FC */
#define BERSERK_SWING_B 84   /* module+0x1630 */

static void berserk_melee(q2_monster *m)
{
    if (!m)
        return;

    q2_cre_set_move(m, (rand() & 1) ? BERSERK_SWING_B : BERSERK_SWING_A);
}

const q2_cre_impl q2_cre_berserk = {
    "Berserk",
    {
        q2_cre_generic_stand, q2_cre_generic_idle, q2_cre_generic_search,
        q2_cre_generic_walk, q2_cre_generic_run, NULL,
        q2_cre_generic_attack, berserk_melee,
        NULL, NULL, NULL, q2_cre_generic_pain, q2_cre_generic_die
    },
    { NULL },
    NULL
};
