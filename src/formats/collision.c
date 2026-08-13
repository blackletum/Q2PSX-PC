#include "collision.h"

#include <string.h>

static void read_node(const u8 *rec, q2_coll_node *out)
{
    int i;

    for (i = 0; i < 3; i++) {
        out->bbox_min[i] = q2_rd_s32(rec + 0 + i * 4);
        out->bbox_max[i] = q2_rd_s32(rec + 12 + i * 4);
    }
    out->first_plane = q2_rd_u16(rec + 24);
    out->first_extra = q2_rd_u16(rec + 26);
    out->unk_c       = q2_rd_u32(rec + 28);
    out->unk_d       = q2_rd_u32(rec + 32);
}

q2_result q2_collision_parse(q2_collision *out, const q2_zone_file *zone,
                             q2_coll_which which)
{
    const dat_chunk *chunk;
    u32 num_nodes, num_planes, num_extra;
    size_t nodes_bytes, planes_bytes, fixed;
    const u8 *p;
    q2_coll_node sentinel;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = zone->chunk[which == Q2_COLL_PRIMARY
                        ? Q2_ZONE_PRIMARY_COLL
                        : Q2_ZONE_SECONDARY_COL];
    if (!chunk || chunk->size < 4)
        return Q2_ERR_BAD_FORMAT;

    p          = chunk->data;
    num_nodes  = q2_rd_u16(p + 0);
    num_planes = q2_rd_u16(p + 2);

    /* Guard every multiply before it can overflow or run past the chunk. */
    nodes_bytes  = (size_t)(num_nodes + 1) * Q2_COLL_NODE_SIZE;
    planes_bytes = (size_t)num_planes * Q2_COLL_PLANE_SIZE;
    fixed        = 4 + nodes_bytes + planes_bytes;

    if (fixed > chunk->size) {
        Q2_ERROR("collision: %u nodes + %u planes need %zu bytes, chunk has %u",
                 num_nodes, num_planes, fixed, chunk->size);
        return Q2_ERR_BAD_FORMAT;
    }

    if ((chunk->size - fixed) % 4 != 0)
        return Q2_ERR_BAD_FORMAT;

    num_extra = (u32)((chunk->size - fixed) / 4);

    /* The sentinel's totals must agree with the header, which is what turns the
     * size arithmetic from plausible into proven for this file. */
    read_node(p + 4 + (size_t)num_nodes * Q2_COLL_NODE_SIZE, &sentinel);

    if (sentinel.first_plane != num_planes || sentinel.first_extra != num_extra) {
        Q2_ERROR("collision: sentinel says %u planes / %u extra, header implies %u / %u",
                 sentinel.first_plane, sentinel.first_extra, num_planes, num_extra);
        return Q2_ERR_BAD_FORMAT;
    }

    out->nodes       = p + 4;
    out->planes      = p + 4 + nodes_bytes;
    out->extra       = p + fixed;
    out->node_count  = num_nodes;
    out->plane_count = num_planes;
    out->extra_count = num_extra;

    return Q2_OK;
}

bool q2_collision_get_node(const q2_collision *c, u32 index, q2_coll_node *out)
{
    if (!c || !out || index > c->node_count)   /* index == node_count is the sentinel */
        return false;

    read_node(c->nodes + (size_t)index * Q2_COLL_NODE_SIZE, out);
    return true;
}

bool q2_collision_get_plane(const q2_collision *c, u32 index, q2_coll_plane *out)
{
    const u8 *rec;

    if (!c || !out || index >= c->plane_count)
        return false;

    rec = c->planes + (size_t)index * Q2_COLL_PLANE_SIZE;

    out->a  = q2_rd_u16(rec + 0);
    out->b  = q2_rd_u16(rec + 2);
    out->c  = q2_rd_u16(rec + 4);
    out->nx = q2_rd_s16(rec + 6);
    out->ny = q2_rd_s16(rec + 8);
    out->nz = q2_rd_s16(rec + 10);

    return true;
}

u32 q2_collision_node_plane_count(const q2_collision *c, u32 index)
{
    q2_coll_node here, next;

    if (!c || index >= c->node_count)
        return 0;
    if (!q2_collision_get_node(c, index, &here))
        return 0;
    if (!q2_collision_get_node(c, index + 1, &next))
        return 0;

    if (next.first_plane < here.first_plane)
        return 0;

    return (u32)(next.first_plane - here.first_plane);
}

bool q2_coll_plane_point(const q2_collision *c, u32 node_index, u32 plane_index,
                         s32 out_point[3])
{
    q2_coll_node node;
    q2_coll_plane plane;
    s32 pt[3];
    int i;

    if (!c)
        return false;
    if (!q2_collision_get_node(c, node_index, &node))
        return false;
    if (!q2_collision_get_plane(c, plane_index, &plane))
        return false;

    /* The stored triple is an unsigned displacement from the node's minimum
     * corner. This is the part that is only 95.6% right disc-wide. */
    pt[0] = node.bbox_min[0] + (s32)plane.a;
    pt[1] = node.bbox_min[1] + (s32)plane.b;
    pt[2] = node.bbox_min[2] + (s32)plane.c;

    if (out_point) {
        out_point[0] = pt[0];
        out_point[1] = pt[1];
        out_point[2] = pt[2];
    }

    for (i = 0; i < 3; i++) {
        if (pt[i] < node.bbox_min[i] || pt[i] > node.bbox_max[i])
            return false;
    }
    return true;
}

s32 q2_coll_normal_len_sq(const q2_coll_plane *p)
{
    if (!p)
        return 0;
    return (s32)p->nx * p->nx + (s32)p->ny * p->ny + (s32)p->nz * p->nz;
}
