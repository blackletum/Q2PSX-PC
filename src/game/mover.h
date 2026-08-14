/*
 * mover.h — doors, lifts and double doors.
 *
 * MOVER_A, MOVER_B and MOVER_C are not three motions. They are ONE motion —
 * an axis-aligned translation of a group of Scene nodes — with three
 * parameterisations. This integrator contains no rotation: no matrix, no angle
 * field, and no GTE call.
 *
 *     MOVER_A  1,006 uses. Vertical, axis hard-wired to Y. Lifts and most doors.
 *     MOVER_B     20 uses. Horizontal, axis taken from the payload.
 *     MOVER_C    292 uses. Double door: two leaves moving opposite ways.
 *
 * CORRECTION — that is true of these three and NOT of the engine.
 *
 * This file used to say "there is no rotation anywhere in the engine". The zone
 * draw refutes it: at 0x800678B4 it calls `RotMatrix` on three s16 Euler angles
 * at the node's runtime object +0x0C, and then adds TWO independent s16 triples,
 * +0x12 and +0x18, to the node's camera-space position. So a node carries a full
 * rotation and two translations, and `ROTHATCH`, `SIMROT`, `SIMROT2` and
 * `ROTBUTTON` — which userfuncs.c has always listed as rotating movers — drive
 * the rotation slots of the same object this module writes the translation of.
 *
 * What is implemented here is the linear family and its +0x12 triple. The
 * rotation slots and the second triple are decoded (see surface.h, which maps
 * the whole object binding out of Scene.flags08 bits 0-9) but no integrator
 * fills them yet, so rotating brush geometry stands still rather than turning.
 *
 * ---------------------------------------------------------------------------
 * Why this port does not reproduce the load-time pre-pass
 * ---------------------------------------------------------------------------
 * The original rewrites the script's s16 slots in place at load, turning Scene
 * node indices into indices into a fixed 48-entry runtime array. That array
 * exists because the console had 2 MB; it is an allocation strategy, not
 * meaning.
 *
 * A native port wants the opposite: read the DISC values, which are Scene node
 * indices, and build objects directly. So this module never performs the
 * rewrite, and the disc payload is the only input. That also sidesteps the
 * subtlety that made movers unimplementable for so long — the same bytes mean
 * different things before and after the pre-pass.
 *
 * ---------------------------------------------------------------------------
 * On-disc payloads
 * ---------------------------------------------------------------------------
 * Offsets are from the start of the event item (op at +0, len at +1).
 *
 *   MOVER_A, len 24:
 *     +2  s16  travel        target displacement is -travel
 *     +4  s16  speed         absolute value taken
 *     +6  s16  portal_node   Scene node carrying the visibility bit; -1 none
 *     +8  s16  node[4]       Scene nodes translated; -1 unused
 *     +16 u16  key_mask      required key bits; 0 means unlocked
 *     +18 u8   delay         seconds before opening
 *     +19 u8   wait          seconds held open; 0xFF never auto-closes
 *     +20 s16  touch         non-zero also opens on touch (65 of 1,006)
 *
 *   MOVER_B, len 24: as A but +8 is the axis (& 3; only 0 and 2 occur), the
 *     nodes move to +10, key_mask to +18, delay/wait to +20/+21, and the
 *     target is +travel when the axis field is 0, -travel otherwise.
 *
 *   MOVER_C, len 32: two leaves at +10 and +18, key_mask +26, delay/wait
 *     +28/+29. Leaf 1's target is the negation of leaf 0's, and leaf 1 owns the
 *     portal node and points at leaf 0 as its partner.
 *
 * ---------------------------------------------------------------------------
 * Timing
 * ---------------------------------------------------------------------------
 * delay and wait are bytes multiplied by 300 and counted down by the same dt
 * the simulation uses, so one byte is one second at the 25 Hz tick.
 *
 * Note this 300 is genuine, unlike TIMER's, which is x30 — see userfuncs.h.
 * The two use different instruction idioms and conflating them makes every
 * door ten times too slow.
 */
#ifndef Q2PSX_MOVER_H
#define Q2PSX_MOVER_H

#include "events.h"
#include "q2psx.h"
#include "scene.h"

#define Q2_MOVER_MAX_PARTS  4
#define Q2_MOVER_TIMEBASE 300
#define Q2_MOVER_WAIT_NEVER 0xFFFFu

/* obj+0x52 in the original. */
typedef enum q2_mover_state {
    Q2_MV_IDLE = 0,
    Q2_MV_OPENING,
    Q2_MV_ARRIVED,
    Q2_MV_CLOSING,
    Q2_MV_BLOCKED,
    Q2_MV_DELAY,
    Q2_MV_OPEN
} q2_mover_state;

enum {
    Q2_MV_BLK_IGNORE_OPENING = 1,
    Q2_MV_BLK_IGNORE_CLOSING = 2
};

typedef struct q2_mover {
    s16 node[Q2_MOVER_MAX_PARTS];   /* Scene nodes this group translates */
    u32 part_count;

    s16 portal_node;    /* -1 when the group has none */
    s16 target;         /* signed displacement along `axis` */
    s16 speed;
    u16 key_mask;

    u16 delay_timer;
    u16 wait_timer;     /* Q2_MOVER_WAIT_NEVER means it never auto-closes */

    u8  axis;           /* 0 = X, 1 = Y, 2 = Z */
    u8  state;
    u8  saved_state;
    u8  block_timer;
    u8  block_flags;
    u8  triggered;      /* latch, cleared each tick */
    u8  announced;      /* so a locked door only complains once */
    u8  touch_opens;

    s32 offset;         /* current displacement along the axis */

    s32 partner;        /* index of the other leaf, -1 when single */
} q2_mover;

typedef struct q2_mover_set {
    q2_mover *movers;
    u32       count;
    u32       capacity;
} q2_mover_set;

q2_result q2_movers_build(q2_mover_set *out, const q2_events *events);
void      q2_movers_free(q2_mover_set *set);

/* Latch a mover so it acts on the next tick. Reversing a closing door is
 * immediate, which is why this is not just a flag set. */
void q2_mover_trigger(q2_mover_set *set, u32 index);

/*
 * Advance every mover by `dt` in the simulation's own units.
 *
 * `player_keys` gates locked doors. Returns the number that moved, so a caller
 * can tell a static frame from a busy one.
 */
u32 q2_movers_tick(q2_mover_set *set, s32 dt, u16 player_keys);

/*
 * The displacement to add to a Scene node's origin when drawing it, or zero.
 *
 * This is how movement reaches the renderer, and it is the original's own
 * mechanism rather than a convenience: the per-frame handler at 0x80025658
 * accumulates the displacement in the mover's runtime object at +0x12, and the
 * zone draw at 0x800678EC adds that triple to the node's camera-space position
 * as it draws it. The geometry is never modified, which is why every node in a
 * zone can share one origin and doors still move.
 *
 * Linear scan because a zone has at most a few dozen movers and this is called
 * once per node per frame.
 */
void q2_movers_node_offset(const q2_mover_set *set, u32 scene_node, s32 out[3]);

#endif /* Q2PSX_MOVER_H */
