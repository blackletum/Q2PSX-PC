/*
 * cre_tankcomm.c — the Tank Commander's attack, transcribed.
 *
 * Everything else this creature does runs on the decoded actions, exactly as it
 * did before: it stands, notices, turns, walks, runs, is hurt and dies on the
 * generic handlers, which install the move the module's own callback installs
 * and execute the steps the decoder read out of its think functions. Only ONE
 * callback is written by hand here, because only one had to be.
 *
 * ---------------------------------------------------------------------------
 * Why: the generic handler was playing the after-the-kill animation
 * ---------------------------------------------------------------------------
 * `q2_creature_move_via` returns the FIRST move a callback installs, in decode
 * order. This creature's attack callback installs four, and the first one —
 * 77-114 — is the one it plays when the enemy's health has gone NEGATIVE. It is
 * what a Tank Commander does standing over a corpse: a proximity call and two
 * sounds, and no shot anywhere in its thirty-eight frames.
 *
 * So the creature attacked, the animation played through to the end, nothing
 * was ever fired, and every counter said the AI was working. It was: it was
 * being handed the wrong move.
 *
 * ---------------------------------------------------------------------------
 * The original, at module+0x11BC
 * ---------------------------------------------------------------------------
 *     v1 = self->enemy                        ; entity+0xBC
 *     v0 = v1->[0x24]->[0x108]                ; the enemy's health
 *     if (v0 < 0) {                           ; bgez v0, +0x54
 *         self->currentmove = M_77_114;       ; module+0x1DE4
 *         self->aiflags &= ~0x200;
 *         return;
 *     }
 *     v    = enemy->origin - self->origin;    ; three subtractions onto the stack
 *     dist = import[+0xB8](v);                ; the vector's length
 *     r    = import[+0x14]();                 ; 0..32767
 *
 *     if (dist < 1501)       move = (r < 13106) ? M_168_196 : M_55_70;
 *     else if (dist < 3001)  move = (r < 16384) ? M_168_196 : M_55_70;
 *     else if (r < 10813)    move = M_168_196;
 *     else if (r < 21626)  { move = M_115_135; self->[0xA8] = g + 50; }
 *     else                   move = M_55_70;
 *
 * 13106, 16384, 10813 and 21626 are 0.4, 0.5, 0.33 and 0.66 of 32768 — the same
 * shape as `M_CheckAttack`'s odds and as id's `tank_attack`.
 *
 * ---------------------------------------------------------------------------
 * What each move is
 * ---------------------------------------------------------------------------
 * From the census, in frame order:
 *
 *     77-114   0*6 6 0*3 6 0*7 11 0*6 9 0*11 6    the corpse animation
 *     168-196  0*5 13*19 0*5                      nineteen frames of think 13,
 *                                                 a sustained burst
 *     115-135  0*15 6 0*5
 *     55-70    0*9 8 0*2 8 0*2 8                  three separate shots
 *
 * Think 8 and think 13 are the ones that reach a projectile import; the moves
 * that carry them are the ones this picks whenever the enemy is alive.
 *
 * ---------------------------------------------------------------------------
 * Two departures, both stated rather than hidden
 * ---------------------------------------------------------------------------
 *  - the distance is compared as a SQUARE, against 1501 and 3001 squared, so no
 *    square root is taken. The bands are the module's own numbers; only the
 *    comparison is rearranged, and it orders identically.
 *  - `self->[0xA8]` is a timer the long-range branch sets to a global plus 50.
 *    Which global and what reads it are not established, so it is not written.
 *    The move it accompanies is installed either way, which is the part that
 *    shows.
 */
#include <stdlib.h>

#include "ai.h"
#include "crebind.h"
#include "monster.h"

/* The four moves, by the first frame each starts on — which is how every other
 * caller in this port names a move (crebind.h). */
#define TANK_MOVE_CORPSE   77
#define TANK_MOVE_BURST   168
#define TANK_MOVE_LONG    115
#define TANK_MOVE_SHOTS    55

/* The module's range bands, squared. */
#define TANK_NEAR_SQ  (1501 * 1501)
#define TANK_MID_SQ   (3001 * 3001)

/* 0.4, 0.5, 0.33 and 0.66 of 32768, as the module writes them. */
#define TANK_ROLL_NEAR  13106
#define TANK_ROLL_MID   16384
#define TANK_ROLL_FAR1  10813
#define TANK_ROLL_FAR2  21626

/* aiflags bit 9, cleared by the corpse branch. */
#define TANK_AIFLAG_ATTACKING 0x200u

static void tankcomm_attack(q2_monster *m)
{
    s64 d2;
    s32 roll;

    if (!m)
        return;

    /*
     * No enemy at all is not a case the module handles — it is only ever
     * called with one — so the generic handler is the honest fallback rather
     * than a guessed branch.
     */
    if (!m->enemy) {
        q2_cre_generic_attack(m);
        return;
    }

    /* The corpse branch. `health < 0`, not `<= 0`: a creature killed exactly to
     * zero is still shot at, which is the original's own boundary. */
    if (m->enemy->health < 0) {
        m->aiflags &= ~TANK_AIFLAG_ATTACKING;
        q2_cre_set_move(m, TANK_MOVE_CORPSE);
        return;
    }

    d2   = q2_monster_dist_sq(m, m->enemy->pos);
    roll = rand() & 0x7FFF;

    if (d2 < TANK_NEAR_SQ) {
        q2_cre_set_move(m, roll < TANK_ROLL_NEAR ? TANK_MOVE_BURST
                                                 : TANK_MOVE_SHOTS);
    } else if (d2 < TANK_MID_SQ) {
        q2_cre_set_move(m, roll < TANK_ROLL_MID ? TANK_MOVE_BURST
                                                : TANK_MOVE_SHOTS);
    } else if (roll < TANK_ROLL_FAR1) {
        q2_cre_set_move(m, TANK_MOVE_BURST);
    } else if (roll < TANK_ROLL_FAR2) {
        q2_cre_set_move(m, TANK_MOVE_LONG);
    } else {
        q2_cre_set_move(m, TANK_MOVE_SHOTS);
    }
}

/*
 * Every slot but `attack` is the generic handler, and every think index is the
 * decoded action — so this is a transcription of one function, not of a
 * creature, and it says so.
 */
const q2_cre_impl q2_cre_tankcomm = {
    "Tankcomm",
    {
        q2_cre_generic_stand, q2_cre_generic_idle, q2_cre_generic_search,
        q2_cre_generic_walk, q2_cre_generic_run, NULL,
        tankcomm_attack, q2_cre_generic_melee,
        NULL, NULL, NULL, q2_cre_generic_pain, q2_cre_generic_die
    },
    { NULL },
    NULL
};
