/*
 * ai.h — the shared creature AI: noticing, chasing, attacking.
 *
 * The per-creature modules hold each monster's own think functions — which
 * sound it makes, which projectile it throws. Those are MIPS code and have to
 * be reimplemented one creature at a time.
 *
 * But the behaviour that makes a monster a monster is NOT per-creature. Target
 * acquisition, the movement verbs, the state transitions and the frame advance
 * are all shared engine code, and reimplementing that once makes every spawned
 * creature act: stand, notice the player, turn, close the distance, and strike
 * when near enough.
 *
 * So this is the difference between monsters that exist and monsters that
 * behave. What is still missing afterwards is flavour — the specific attack a
 * given creature performs — not agency.
 *
 * ---------------------------------------------------------------------------
 * The state model
 * ---------------------------------------------------------------------------
 * A creature's state is its current move plus the aiflags word, exactly as in
 * the lineage this was ported from. The callback block is in the original's
 * order: stand, idle, search, walk, run, dodge, attack, melee, sight,
 * checkattack.
 *
 * Transitions come from target acquisition. A creature with no enemy stands or
 * idles; one that acquires an enemy runs at it; one within melee range attacks.
 *
 * ---------------------------------------------------------------------------
 * Range tiers
 * ---------------------------------------------------------------------------
 * The original classifies distance into bands and picks behaviour from the
 * band rather than the raw number. The thresholds here are in world units and
 * derived from the established scale, and they are marked INFERRED — the
 * executable's own values have not been read.
 *
 * ---------------------------------------------------------------------------
 * Sight
 * ---------------------------------------------------------------------------
 * A creature notices the player when the player is within its sight range AND
 * inside its forward cone. The cone is wide — about 72 degrees off-axis — which
 * is why monsters in this game spot you from well to the side rather than only
 * head-on. See monster.h for the threshold.
 *
 * There is deliberately no line-of-sight trace yet. Adding one needs the
 * collision hull walk to run from an arbitrary point rather than the player's
 * cell, and doing it badly would make monsters either blind or omniscient. Its
 * absence is visible as creatures noticing through walls, which is the honest
 * failure to have while it is missing.
 */
#ifndef Q2PSX_AI_H
#define Q2PSX_AI_H

#include "monster.h"
#include "q2psx.h"

/* Range bands, in world units. INFERRED from the established scale, not read
 * from the executable. */
#define Q2_RANGE_MELEE_UNITS   800     /* about 80 PC units  */
#define Q2_RANGE_NEAR_UNITS   5000
#define Q2_RANGE_MID_UNITS   10000
#define Q2_SIGHT_UNITS       20000

typedef enum q2_range_band {
    Q2_RANGE_MELEE = 0,
    Q2_RANGE_NEAR,
    Q2_RANGE_MID,
    Q2_RANGE_FAR
} q2_range_band;

q2_range_band q2_ai_range(const q2_monster *m, const s32 target[3]);

/* Turn `m` toward its ideal yaw by at most `max_step`, taking the short way
 * round. Returns true once it is facing within one step. */
bool q2_ai_turn_toward(q2_monster *m, s32 max_step);

/* Point the creature's ideal yaw at a position. */
void q2_ai_face(q2_monster *m, const s32 target[3]);

/*
 * Look for a target. Sets `enemy` and returns true on acquisition.
 *
 * `index` is what gets stored in `enemy`, so a caller with several potential
 * targets can identify which was found.
 */
bool q2_ai_find_target(q2_monster *m, const s32 target[3], s32 index);

/* The shared movement verbs. `dist` is the frame's advance, already scaled. */
void q2_ai_stand(q2_monster *m, s32 dist);
void q2_ai_walk(q2_monster *m, s32 dist);
void q2_ai_run(q2_monster *m, s32 dist, const s32 target[3]);
void q2_ai_charge(q2_monster *m, s32 dist, const s32 target[3]);
void q2_ai_move(q2_monster *m, s32 dist);

/* What a creature decided to do this tick, so the caller can act on it without
 * the AI needing to know about combat or sound. */
typedef enum q2_ai_action {
    Q2_AI_ACT_NONE = 0,
    Q2_AI_ACT_SIGHTED,     /* just noticed the player   */
    Q2_AI_ACT_MELEE,       /* struck at close range     */
    Q2_AI_ACT_ATTACK       /* attacked at range         */
} q2_ai_action;

/*
 * Run one AI tick for a creature.
 *
 * `now` is on the 10 Hz AI clock. `target` is the player's position, and
 * `target_index` identifies it. Returns what the creature did.
 */
q2_ai_action q2_ai_think(q2_monster *m, const s32 target[3], s32 target_index,
                         s32 now);

#endif /* Q2PSX_AI_H */
