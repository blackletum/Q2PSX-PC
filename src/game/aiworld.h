/*
 * aiworld.h — binding the creature AI to the real collision model.
 *
 * The AI asks the world three questions and nothing else: can this creature
 * see that one, where does a box move end up, and is there ground under my
 * feet. `ai.h` takes those as function pointers so the behaviour can be
 * exercised without a disc; this is the implementation that answers them out
 * of a level's own hull.
 *
 * The three map onto routines the original already has, which is why the
 * binding is thin rather than a reimplementation:
 *
 *   line_of_sight   `visible` (0x8005B950) ends in a swept move through the
 *                   collision graph at 0x80044C44 — `q2_coll_move` here.
 *   trace           the same walk, because the movement hull `SecondaryCol` is
 *                   already `PrimaryColl` eroded by the mover's own box. The
 *                   AI's mins/maxs are therefore advisory: the erosion has
 *                   done the work, exactly as it does for the player.
 *   check_bottom    M_CheckBottom (0x8005FB24), a downward probe of a step
 *                   height plus a margin under the creature's centre.
 *
 * A creature whose zone has no collision loaded falls back to the open world,
 * which is the same failure the stand-in has and is visible rather than silent.
 */
#ifndef Q2PSX_AIWORLD_H
#define Q2PSX_AIWORLD_H

#include "ai.h"
#include "collision.h"
#include "q2psx.h"

/*
 * What the binding did, so a creature that will not move is a measurement
 * rather than a guess.
 *
 * `trace_unplaced` is the one that matters: `q2_coll_move` reports failure when
 * it cannot find a cell for the START point, and a walker's step trace starts
 * from a point the creature is not standing at. If nearly every trace lands
 * there, the fault is in placing the start, not in the geometry.
 */
typedef struct q2_ai_world_stats {
    u32 traces;
    u32 trace_unplaced;     /* the start point resolved to no cell        */
    u32 trace_clear;        /* ran the whole way: "walked off an edge"    */
    u32 bottom_calls;
    u32 bottom_fail;
    u32 los_calls;
    u32 los_blocked;
} q2_ai_world_stats;

typedef struct q2_ai_world_bind {
    q2_collision *coll;

    /* How far below a creature ground may be and still count. The step height,
     * because a creature that can climb a step can also stand off one. */
    s32 bottom_reach;

    q2_ai_world_stats stats;

    /* Filled in by the binder; pass this to q2_ai_set_world. */
    q2_ai_world world;
} q2_ai_world_bind;

/*
 * Point `bind` at a zone's hull and install it as the AI's world.
 *
 * The binding borrows `coll`; the caller keeps ownership and must not free it
 * while creatures are thinking. Passing a NULL hull installs the open world.
 */
void q2_ai_world_bind_init(q2_ai_world_bind *bind, q2_collision *coll);
void q2_ai_world_bind_install(q2_ai_world_bind *bind);

#endif /* Q2PSX_AIWORLD_H */
