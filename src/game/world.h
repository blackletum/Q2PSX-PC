/*
 * world.h — loading a zone and turning it into PlayStation GPU primitives.
 *
 * This is the join between the data layer and the fidelity layer. It reads
 * Scene, Points and MapMod, walks every node's quads, pushes each vertex through
 * the GTE, and emits a POLY_GT4 into the ordering table. From there the backend
 * has exactly the primitive stream the original hardware saw.
 *
 * Nothing here converts to floating point. Vertices go in as s16, come out of
 * the GTE as integer screen positions, and land in the OT bucket the GTE's
 * AVSZ4 chose. That is the whole point: the wobble and the sort artefacts are
 * produced here, not simulated later.
 */
#ifndef Q2PSX_WORLD_H
#define Q2PSX_WORLD_H

#include "disc.h"
#include "gpu.h"
#include "gte.h"
#include "level.h"
#include "points.h"
#include "q2psx.h"
#include "scene.h"

/* One loaded zone, with its three geometry chunks resolved. */
typedef struct q2_world_zone {
    q2_zone_file zone;
    q2_scene     scene;
    q2_points    points;
    char         name[64];

    /*
     * Optional diagnostic: when non-NULL, only nodes whose byte is non-zero are
     * drawn. It exists so a single piece of geometry — one door, one lift — can
     * be isolated from the 17,000 nodes around it, which is the only practical
     * way to tell a fault in one entity apart from a fault in the renderer.
     * Owned by the caller; NULL means draw everything.
     */
    const u8    *node_filter;
    u32          node_filter_count;
} q2_world_zone;

/* Load "<map>/ZONE<index>.DAT" from the disc. */
q2_result q2_world_load_zone(q2_world_zone *out, const disc *d,
                             const char *map, int zone_index);
void      q2_world_free_zone(q2_world_zone *z);

/* Where the camera is and which way it faces. Angles use the 4096-step circle. */
typedef struct q2_camera {
    s32 pos[3];       /* world position          */
    s32 yaw;
    s32 pitch;
    u16 projection;   /* GTE H — the field of view control */
} q2_camera;

void q2_camera_default(q2_camera *cam, int screen_w, int screen_h);

/* Statistics from one build, so callers can tell "drew nothing" apart from
 * "drew everything off-screen" — a distinction that matters a great deal when
 * bringing a renderer up. */
typedef struct q2_world_stats {
    u32 nodes_visited;
    u32 quads_total;
    u32 quads_emitted;
    u32 quads_rejected_near;   /* GTE reported a divide overflow  */
    u32 quads_rejected_bad;    /* malformed record or bad indices */
    u32 quads_no_uv;           /* UV lookup failed; drawn untextured */
    u32 ot_overflow;           /* primitive pool was full         */
} q2_world_stats;

/*
 * Transform the zone and fill the ordering table.
 *
 * `ot` is cleared first. The GTE state is configured from `cam` and the
 * framebuffer size. Returns the number of primitives emitted.
 */
u32 q2_world_build_ot(const q2_world_zone *z,
                      const q2_camera *cam,
                      int screen_w, int screen_h,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_world_stats *stats);

/* World-space bounds of the zone, computed properly: every node's local points
 * offset by that node's origin. Useful for placing a camera that can actually
 * see something. */
void q2_world_bounds(const q2_world_zone *z, s32 min_out[3], s32 max_out[3]);

#endif /* Q2PSX_WORLD_H */
