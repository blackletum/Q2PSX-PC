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
 * The plane POINT is only INFERRED, at 95.6%. Reading a/b/c as an unsigned
 * offset from the owning node's bbox_min puts 46,968 of 49,148 tested planes
 * inside their node, and makes 91% of nodes convex-consistent. The residual
 * 4.4% is unexplained — possibly a different base, a sign convention, or a
 * second class of plane. Player movement must not be built on this until it
 * reaches 100%; see docs/openquestions.md.
 *
 * q2_coll_plane_point() therefore reports whether the decoded point actually
 * landed inside its node, so callers can refuse to move a player through a
 * plane they cannot vouch for rather than silently mis-colliding.
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
 * Decode a plane's point into node-relative world coordinates.
 *
 * Returns true only if the point lands inside the owning node's bounding box.
 * A false return is not necessarily a parse error — 4.4% of planes disc-wide
 * fail this test under the current interpretation — but it does mean this
 * particular plane's position is not trustworthy.
 */
bool q2_coll_plane_point(const q2_collision *c, u32 node_index, u32 plane_index,
                         s32 out_point[3]);

/* Squared length of a normal, for validating that it really is unit length. */
s32 q2_coll_normal_len_sq(const q2_coll_plane *p);

#endif /* Q2PSX_COLLISION_H */
