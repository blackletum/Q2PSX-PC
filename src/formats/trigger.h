/*
 * trigger.h — TrigBounds: the volumes that connect the player to the script.
 *
 * This is the join between physics and gameplay. A trigger is an AABB plus a
 * set of planes, and an `event_offset` naming the Events record it fires. Walk
 * into one and the trigger graph runs — which is how doors open, lifts start,
 * and levels end.
 *
 * ---------------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------------
 *     u16 trigger_count
 *     u16 plane_count
 *     trigger[trigger_count + 1]     the last is a sentinel
 *     plane[plane_count]             ONE shared pool
 *
 * with `size == 4 + 36*(n+1) + 12*plane_count` holding on all 49 maps.
 *
 * A trigger is 36 bytes:
 *     0x00  s32[3]  min          AABB, world units
 *     0x0C  s32[3]  max
 *     0x18  u16     plane_start  index of this trigger's first plane
 *     0x1A  u16     event_offset byte offset into Events; 0xFFFF means none
 *     0x1C  u32     zero
 *     0x20  u16     id           9..75 plus 255; 255 (NOT 0xFFFF) means none
 *     0x22  u16     flags        14 distinct values, meaning unknown
 *
 * A plane is 12 bytes:
 *     0x00  s16[3]  point   RELATIVE to the owning trigger's min[]
 *     0x06  s16[3]  normal  outward, 1.3.12, |n| is 4095 or 4096 on all 8,228
 *
 * NEVER compute a trigger's plane range as [6i, 6i+6). It is tempting because
 * most triggers are boxes, but plane_count != 6*trigger_count on eleven maps,
 * and two further maps contain non-box triggers whose totals coincidentally
 * equal 6*trigger_count — so the shortcut passes a naive check and then reads
 * the wrong planes. Always use [plane_start[i], plane_start[i+1]).
 *
 * Note the plane points are relative to the trigger's own min corner, unlike
 * collision planes which are relative to their node's bbox_min. Same idea,
 * different base.
 */
#ifndef Q2PSX_TRIGGER_H
#define Q2PSX_TRIGGER_H

#include "level.h"
#include "q2psx.h"

#define Q2_TRIGGER_SIZE       36
#define Q2_TRIGGER_PLANE_SIZE 12
#define Q2_TRIGGER_NO_EVENT   0xFFFFu
#define Q2_TRIGGER_NO_ID      255u

typedef struct q2_trigger {
    s32 min[3];
    s32 max[3];
    u16 plane_start;
    u16 event_offset;   /* into the Events chunk; Q2_TRIGGER_NO_EVENT if none */
    u16 id;             /* Q2_TRIGGER_NO_ID if none */
    u16 flags;          /* meaning unknown */
} q2_trigger;

typedef struct q2_trigger_plane {
    s16 point[3];       /* relative to the trigger's min[] */
    s16 normal[3];      /* 1.3.12 outward */
} q2_trigger_plane;

typedef struct q2_triggers {
    const u8 *data;
    u32       count;        /* excludes the sentinel */
    u32       plane_count;
    const u8 *planes;
} q2_triggers;

q2_result q2_triggers_parse(q2_triggers *out, const q2_common_file *common);

bool q2_trigger_get(const q2_triggers *t, u32 index, q2_trigger *out);
bool q2_trigger_get_plane(const q2_triggers *t, u32 index, q2_trigger_plane *out);

/* Number of planes belonging to trigger `index`, from the successor's start. */
u32 q2_trigger_plane_count(const q2_triggers *t, u32 index);

/* Cheap AABB test — the right first pass, since most triggers are boxes. */
bool q2_trigger_point_in_box(const q2_trigger *trig, const s32 point[3]);

/*
 * Exact test: inside the AABB and on the inner side of every plane. Falls back
 * to the box result when the trigger has no planes, which is a real case.
 */
bool q2_trigger_contains(const q2_triggers *t, u32 index, const s32 point[3]);

#endif /* Q2PSX_TRIGGER_H */
