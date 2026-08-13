/*
 * points.h — the "Points" chunk of ZONE*.DAT: the zone's vertex pool.
 *
 * Layout, confirmed against all 115 zone files on the PAL disc with no
 * mismatches:
 *
 *     u32  group_count
 *     struct { u32 byte_offset; u32 point_count; } groups[group_count];
 *     u8   point_data[];            // 12 bytes per point
 *
 * `byte_offset` is relative to the start of point_data, and the groups are
 * contiguous and in order: group[i].byte_offset == (sum of preceding counts)*12
 * held for every group of every file. The total point count always satisfies
 *
 *     sizeof(point_data) == total_points * 12
 *
 * exactly, which is what makes the layout safe to trust — a wrong stride would
 * not divide evenly across 115 files of wildly differing size.
 *
 * A point is 12 bytes:
 *
 *     offset  type      field
 *     0x00    s16       x        node-LOCAL, not world
 *     0x02    s16       y        vertical axis
 *     0x04    s16       z
 *     0x06    u8[6]     slot     reverse map into MapMod quad corners; 0xFF unused
 *
 * IMPORTANT — coordinates are node-local. The world position of a point is
 *
 *     world = point.xyz + Scene[group_index].origin
 *
 * where the Points group index and the Scene node index are the same thing: the
 * group count equals the Scene node count on all 115 zone files. Treating the
 * raw values as world coordinates gives a box that is the union of every node's
 * *local* extent, which is not any real volume. For BASE0 zone 0 the difference
 * is large: local bounds are [-17899,-3200,-15320]..[18581,3519,15000] while the
 * true world bounds are [-25600,-5120,-21361]..[10880,1599,8959].
 *
 * The `slot` bytes are a reverse index — slot = poly_index*4 + corner — letting a
 * point find the quads that use it. They hold for 99.971% of entries, with 317
 * exceptions across the disc, so they should be REBUILT FROM MapMod at load time
 * rather than trusted. They are exposed raw here for tooling.
 *
 * Coordinate scale relative to PC Quake II is UNRESOLVED and is a Tier 1 open
 * question — the authoring grid is visible (multiples of 640, 320, 160 and 80 are
 * heavily enriched) but the multiplier is not established. The loader applies no
 * scaling.
 */
#ifndef Q2PSX_POINTS_H
#define Q2PSX_POINTS_H

#include "level.h"
#include "q2psx.h"

#define Q2_POINT_SIZE 12

typedef struct q2_point {
    s16 x, y, z;     /* node-local; add the owning Scene node's origin */
    u8  slot[6];     /* reverse map into MapMod corners; 0xFF == unused */
} q2_point;

typedef struct q2_point_group {
    u32 first;   /* index of this group's first point, not a byte offset */
    u32 count;
} q2_point_group;

typedef struct q2_points {
    const u8            *data;        /* raw point records, borrowed         */
    u32                  count;       /* total points across all groups      */
    q2_point_group      *groups;      /* owned                               */
    u32                  group_count;
} q2_points;

/* Parse the Points chunk of an already-opened zone. Borrows the zone's buffer,
 * so the zone must outlive the result. */
q2_result q2_points_parse(q2_points *out, const q2_zone_file *zone);
void      q2_points_free(q2_points *p);

/* Decode one point. Returns false if `index` is out of range. */
bool q2_points_get(const q2_points *p, u32 index, q2_point *out);

/*
 * Axis-aligned bounds over every point, in NODE-LOCAL space.
 *
 * This is deliberately not called "the zone's bounding box", because it is not
 * one — it is the union of every node's local extent, and no geometry occupies
 * that volume. It is useful only as a corruption check (a misparse produces a
 * box filling the whole s16 range). For a real world-space box, add each node's
 * origin, which needs the Scene chunk.
 */
void q2_points_local_bounds(const q2_points *p, s16 min_out[3], s16 max_out[3]);

#endif /* Q2PSX_POINTS_H */
