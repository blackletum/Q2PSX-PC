/*
 * scene.h — the Scene and MapMod chunks: the zone's drawable world.
 *
 * A zone is a flat list of convex nodes. Each node owns a group of vertices in
 * the Points chunk and a record of quads in MapMod, and the three are indexed in
 * lockstep: node i uses Points group i and the MapMod record at Scene[i].mapmod_offset.
 * The node count equals the Points group count on all 115 zone files.
 *
 * Verified independently over the whole disc: 17,035 nodes, 17,035 MapMod
 * records, 274,936 quads, zero violations of the size identities below.
 *
 * ---------------------------------------------------------------------------
 * Scene — 52 bytes per node
 * ---------------------------------------------------------------------------
 *   0x00  u32   mapmod_offset   byte offset of this node's record in MapMod.
 *                               offs[0]==0, strictly increasing, no duplicates,
 *                               so node i's record spans [off[i], off[i+1]) and
 *                               the last runs to the end of the chunk.
 *   0x04  u32   reserved        always 0
 *   0x08  u16   flags           bitfield; observed {0,0x400,0x800,0x1000,0x1400,
 *                               0x4000,0x4400,0x4800}. Meaning unknown.
 *   0x0A  u16   pad             always 0
 *   0x0C  u8    unk0C           0..4, nonzero in 6 nodes disc-wide
 *   0x0D  u8    unk0D           0..3, nonzero in 5 nodes
 *   0x0E  u8    unk0E           0..197
 *   0x0F  u8    reserved        always 0
 *   0x10  s32[3] bbox_min
 *   0x1C  s32[3] bbox_max
 *   0x28  s32[3] origin         world vertex = Points.xyz + origin
 *
 * The stored AABB does NOT reliably contain the node's own geometry. Across
 * 51,105 axis tests, the node's points fall outside the stored box on at least
 * one side in 43,696 of them, by 0..3 units. Cull with an inflated box or
 * geometry will pop — q2_scene_node_bounds() applies that margin for you.
 *
 * ---------------------------------------------------------------------------
 * MapMod — one variable-length record per node
 * ---------------------------------------------------------------------------
 *   0x00  u16   num_polys       0..63, never >= 64. Five nodes have 0.
 *   0x02  u16   colour_offset   == 8 + num_polys*12, on 17035/17035
 *   0x04  u32   uv_offset       colour_offset <= uv_offset <= record end
 *   0x08        poly[num_polys] 12 bytes each
 *         then  u8 rgb[][3]     size == align4(3*(max_colour_index+1))
 *         then  u8 uv[n][8]     n = (record_end - uv_offset)/8, always exact
 *
 * A poly is 12 bytes and maps directly onto a PSX POLY_GT4:
 *   0x00  u8[4] vtx     indices into this node's Points group, PSX quad order
 *   0x04  u8[4] col     per-corner index into this record's RGB table (Gouraud)
 *   0x08  u16   clut    PSX CLUT id: VRAM x = (clut & 0x3F)*16, y = clut >> 6
 *   0x0A  u8    tpage   PSX texture page attribute, 0..11
 *   0x0B  u8    uv_idx  bits 0-5 index the UV table; bits 6-7 are render flags
 *                       of unknown meaning (0x00 88.3%, 0x80 4.6%, 0x40 3.6%,
 *                       0xC0 3.5%)
 *
 * Vertex indices are u8, which is safe because a Points group never holds more
 * than 117 vertices.
 *
 * CORRECTION — the quad corners are a WINDING, not the PSX "Z" order.
 *
 * A libgpu POLY_GT4 packet uses Z order (0,1,2 then 1,3,2), and assuming
 * MapMod matched it is the obvious mistake to make. It does not: these indices
 * run around the perimeter, so the triangles are (0,1,2) and (0,2,3).
 *
 * The symptom of getting it wrong is distinctive and worth recognising. Every
 * quad renders as a bowtie, so a wall becomes a regular lattice of diamond
 * holes with the background showing through. It looks like a near-plane or
 * culling problem rather than an index-order one, which is what makes it worth
 * writing down.
 *
 * Presumably the engine reorders when it builds the GPU packet. For a port that
 * rasterises these directly, winding order is what the data says.
 */
#ifndef Q2PSX_SCENE_H
#define Q2PSX_SCENE_H

#include "level.h"
#include "points.h"
#include "q2psx.h"

#define Q2_SCENE_NODE_SIZE 52
#define Q2_MAPMOD_POLY_SIZE 12

/* How far the stored AABB must be grown to actually contain its geometry. */
#define Q2_SCENE_BBOX_SLOP 4

typedef struct q2_scene_node {
    u32 mapmod_offset;
    u16 flags;
    s32 bbox_min[3];
    s32 bbox_max[3];
    s32 origin[3];
} q2_scene_node;

typedef struct q2_mapmod_poly {
    u8  vtx[4];
    u8  col[4];
    u16 clut;
    u8  tpage;
    u8  uv_idx;      /* low 6 bits index the UV table */
    u8  flags;       /* high 2 bits of uv_idx, shifted down */
} q2_mapmod_poly;

/* One node's polygon record, with its colour and UV tables located. */
typedef struct q2_mapmod_rec {
    u32       num_polys;
    const u8 *polys;        /* num_polys * 12 bytes            */
    const u8 *rgb;          /* 3 bytes per colour entry        */
    u32       rgb_count;
    const u8 *uv;           /* 8 bytes per UV set              */
    u32       uv_count;
} q2_mapmod_rec;

typedef struct q2_scene {
    const u8      *nodes;        /* borrowed, 52 bytes each */
    u32            node_count;
    const u8      *mapmod;       /* borrowed                */
    u32            mapmod_size;
} q2_scene;

/* Parse the Scene chunk and locate MapMod. Borrows the zone's buffer. */
q2_result q2_scene_parse(q2_scene *out, const q2_zone_file *zone);

/* Decode node `index`. Returns false if out of range. */
bool q2_scene_get_node(const q2_scene *s, u32 index, q2_scene_node *out);

/* Locate node `index`'s polygon record. Returns false if malformed. */
bool q2_scene_get_mapmod(const q2_scene *s, u32 index, q2_mapmod_rec *out);

/* Decode polygon `poly` of a record. */
bool q2_mapmod_get_poly(const q2_mapmod_rec *rec, u32 poly, q2_mapmod_poly *out);

/* The node's AABB, grown by Q2_SCENE_BBOX_SLOP so it actually contains its
 * geometry. Use this for culling, not the raw stored box. */
void q2_scene_node_bounds(const q2_scene_node *n, s32 min_out[3], s32 max_out[3]);

#endif /* Q2PSX_SCENE_H */
