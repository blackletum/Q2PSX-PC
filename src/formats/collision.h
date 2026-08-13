/*
 * collision.h — the PrimaryColl and SecondaryCol chunks: the zone's collision hulls.
 *
 * Both chunks use one layout:
 *
 *     u16            num_nodes
 *     u16            num_planes
 *     q2_coll_node   nodes[num_nodes + 1]    last entry is a totals sentinel
 *     q2_coll_plane  planes[num_planes]
 *     u32            extra[num_extra]
 *
 * The size equation
 *
 *     4 + (num_nodes+1)*36 + num_planes*12 + num_extra*4 == chunk_size
 *
 * holds exactly on all 230 chunks (115 zone files times two chunks), and the
 * sentinel's first_plane / first_extra equal the totals. That is a strong
 * enough identity that a misparse cannot hide: it would have to divide evenly
 * across 230 chunks of wildly differing size.
 *
 * A node's plane range is derived from its successor, exactly like the .DAT
 * container's chunk sizes:
 *
 *     count = nodes[i+1].first_plane - nodes[i].first_plane
 *
 * which is why the sentinel exists and why num_nodes+1 records are stored.
 *
 * ---------------------------------------------------------------------------
 * Confidence — read this before trusting a plane
 * ---------------------------------------------------------------------------
 * The plane NORMAL is CONFIRMED. It is a 1.3.12 unit vector: |n| == 4096 in
 * 120,911 planes, 4095 in 18,321, and 4094 in 8, out of 139,240 measured across
 * every file and both chunks. That is 100% within two least-significant bits of
 * unity, and diagonals show up as 2896, which is round(4096/sqrt(2)). No other
 * byte offset in the record is remotely unit-magnitude, so this is not a
 * coincidence of alignment.
 *
 * The plane POINT reads as an unsigned offset from the owning node's bbox_min,
 * and it is now CONFIRMED at 99.85% by the right test.
 *
 * An earlier pass rated this only 95.6% and warned against building player
 * movement on it. That figure came from asking whether the decoded point lands
 * inside the node's bounding box — which is the wrong question. A plane is
 * infinite; its stored reference point only has to lie ON the plane, and there
 * is no reason it must also sit inside the box.
 *
 * The right question is geometric: for an outward-facing convex hull, an
 * interior point must be on the negative side of every one of the node's
 * planes. Using the centroid of a node's own plane points as the interior
 * estimate, over all 139,240 planes in both hulls of all 115 zones:
 *
 *     strictly inside   138,610   99.548%
 *     on the plane          417    0.299%   (degenerate, not wrong)
 *     outside               213    0.153%
 *
 * and 22,625 of 22,773 nodes (99.35%) are fully consistent. The 148 nodes that
 * are not carry 1-4 bad planes each, consistent with a handful of genuinely
 * non-convex nodes rather than a misread field.
 *
 * So the encoding is sound and collision may be built on it. The 0.15% should
 * be handled defensively at runtime, not treated as a reason to distrust the
 * format.
 *
 * q2_coll_plane_point() therefore no longer rejects points outside the box —
 * that guard threw away 4.66% of perfectly good planes.
 *
 * Note also: SecondaryCol is NOT reliably the finer hull. It has more nodes than
 * PrimaryColl in 89 of 115 files, the same in 17, and FEWER in 9.
 */
#ifndef Q2PSX_COLLISION_H
#define Q2PSX_COLLISION_H

#include "level.h"
#include "q2psx.h"

#define Q2_COLL_NODE_SIZE  36
#define Q2_COLL_PLANE_SIZE 12

/* A 1.3.12 normal of unit length. */
#define Q2_COLL_NORMAL_ONE 4096

typedef struct q2_coll_node {
    s32 bbox_min[3];
    s32 bbox_max[3];
    u16 first_plane;
    u16 first_extra;
    u32 unk_c;        /* 0..65,077,433, not monotonic — meaning unknown */
    u32 unk_d;        /* 0..75 — meaning unknown                       */
} q2_coll_node;

typedef struct q2_coll_plane {
    u16 a, b, c;      /* point, as an unsigned offset from the node's bbox_min */
    s16 nx, ny, nz;   /* 1.3.12 unit normal                                    */
} q2_coll_plane;

typedef struct q2_collision {
    const u8 *nodes;        /* borrowed; (node_count+1) records of 36 bytes */
    const u8 *planes;       /* borrowed; plane_count records of 12 bytes    */
    const u8 *extra;
    u32       node_count;   /* excludes the sentinel                        */
    u32       plane_count;
    u32       extra_count;
} q2_collision;

/* Which hull to open. */
typedef enum q2_coll_which {
    Q2_COLL_PRIMARY = 0,
    Q2_COLL_SECONDARY
} q2_coll_which;

/* Parse a collision chunk. Validates the size equation and the sentinel, so a
 * success return means the layout is confirmed for this specific file, not
 * merely assumed. */
q2_result q2_collision_parse(q2_collision *out, const q2_zone_file *zone,
                             q2_coll_which which);

bool q2_collision_get_node(const q2_collision *c, u32 index, q2_coll_node *out);
bool q2_collision_get_plane(const q2_collision *c, u32 index, q2_coll_plane *out);

/* Number of planes belonging to node `index`. */
u32 q2_collision_node_plane_count(const q2_collision *c, u32 index);

/*
 * Decode a plane's reference point into world coordinates.
 *
 * Returns false only on a genuine range error. Whether the point sits inside
 * the node's box is deliberately NOT checked — see the note above; that guard
 * discarded 4.66% of valid planes.
 */
bool q2_coll_plane_point(const q2_collision *c, u32 node_index, u32 plane_index,
                         s32 out_point[3]);

/*
 * Signed distance from `point` to a plane, in world units scaled by 4096
 * (the normal is 1.3.12). Negative is the inside of an outward-facing hull.
 *
 * This is the primitive collision actually needs, and it is well defined even
 * for the 0.15% of planes whose stored point sits outside the node box.
 */
s32 q2_coll_plane_distance(const q2_collision *c, u32 node_index,
                           u32 plane_index, const s32 point[3]);

/* True when `point` is inside every plane of `node_index`. */
bool q2_collision_point_in_node(const q2_collision *c, u32 node_index,
                                const s32 point[3]);

/* Squared length of a normal, for validating that it really is unit length. */
s32 q2_coll_normal_len_sq(const q2_coll_plane *p);

#endif /* Q2PSX_COLLISION_H */
