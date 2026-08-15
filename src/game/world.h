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
#include "surface.h"

struct q2_mover_set;
struct q2_rotator_set;

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

    /*
     * Where movement enters the picture. A door does not have geometry of its
     * own to animate — every node in a zone shares one origin — so the original
     * keeps the displacement in the mover's runtime object and ADDS IT WHEN
     * DRAWING: the zone draw at 0x800678EC reads the object's s16 triple at
     * +0x12 and adds it to the node's camera-space position.
     *
     * This is the same mechanism, with the object reached through the mover set
     * instead of through the engine's 48-entry array. NULL draws everything at
     * rest, which is what a static render wants.
     */
    const struct q2_mover_set *movers;

    /*
     * Nodes a script has hidden — one byte per Scene node, or NULL.
     *
     * `flags08` bit 15 IS the hide flag and the draw already honours it, but it
     * is clear on every node on the disc: `OBJDRAWOFF` sets it at RUN TIME, on
     * the runtime object rather than on the chunk. This port does not build the
     * console's 48-entry object array (mover.h says why), so the runtime state
     * lives beside the zone instead, and the draw checks both.
     *
     * Owned by the caller and expected to outlive the zone.
     */
    const u8 *node_hidden;
    u32       node_hidden_count;

    /*
     * The AUTHORED draw order, from the zone's SortData chunk.
     *
     * When this is NULL the renderer walks nodes in index order and derives an
     * ordering-table bucket per quad from its depth, which is what the port has
     * always done. When it is set, the node order and the bucket both come from
     * the stream instead — which is what the console does, and the difference is
     * not cosmetic: the original's world is not depth-sorted at all (sortdata.h).
     *
     * It is opt-in because of open question 7a. A chunk holds many streams and
     * which one a viewport uses comes from a runtime record at its `+28`, not
     * from the disc, so the port can decode every stream but cannot yet say
     * which is the right one.
     *
     * `sort_offset` names it by BYTE offset. Prefer q2_sortdata_stream_offset()
     * to turn a stream INDEX into one: the streams are self-delimiting and
     * therefore enumerable, so an index is a stable disc-derived handle where a
     * raw byte offset is not. Stream 0 is what a single-stream chunk holds.
     */
    const struct q2_sortdata *sort;
    u32                       sort_offset;

    /*
     * Brush geometry that TURNS — ROTHATCH, SIMROT, SIMROT2, ROTBUTTON.
     *
     * A node bound to a runtime object picks up a rotate-about-pivot transform
     * as well as a displacement; see rotator.h for the integrator and for why
     * obj+0x18 is a pivot rather than a second offset. NULL draws every node
     * unrotated, which is what a static render wants.
     */
    const struct q2_rotator_set *rotators;

    /*
     * The zone's lights, for the flare pass. NULL draws no flares, which is what
     * every caller wanted before they existed.
     *
     * `light_node` is the SecondaryCol node the viewer is standing in, because
     * that is what selects the static lights (see spacelights.h). Negative means
     * only the dynamic lights are considered — and that is the original's own
     * behaviour, not a fallback: a flare is visible from inside the cells the
     * level's build tool assigned its light to, and nowhere else.
     */
    const struct q2_light_world *lights;
    s32                          light_node;
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

    /*
     * The strafe lean, entity+0xEA. Derived from the wish velocity rather than
     * from input (0x8003AB18), applied by the camera as the outermost rotation
     * (0x8004F40C hands RotMatrix the whole triple), and zero for every camera
     * that is not a player's — which is why it defaults to zero and costs
     * nothing to carry.
     */
    s32 roll;

    u16 projection;   /* GTE H — the field of view control */

    /*
     * The geometry offset — view+266/+268, `SetGeomOffset` at 0x80076B78. It is
     * where the projection's centre lands in the framebuffer, and it is a
     * viewport's property rather than a buffer's: the quad split puts four
     * different centres in one 512x248 frame.
     *
     * Every layout happens to set it to the viewport's own middle, halved toward
     * zero, so leaving BOTH at zero means "the middle of whatever buffer you are
     * drawing into" and is what a caller with no viewport gets. Zero is a safe
     * sentinel precisely because no layout ever asks for a centre at the corner.
     */
    s32 ofs_x, ofs_y;

    /*
     * The viewport's far distance — view+264, which the per-viewport draw parks
     * at 0x800B2CCC for the polygon emitter. It is 6400 for every layout except
     * the quad split, which uses 4000.
     *
     * It is what turns a camera-space depth into an ordering-table bucket here.
     * The console does not do that: its world sort comes from the `SortData`
     * chunk (open question #7), and `InitGeom` leaves ZSF3/ZSF4 at 341/256
     * (0x8008E4C4), which would make the GTE's own AVSZ index run off the end of
     * a 51-entry slice past a depth of about 200 — corroborating that the
     * hardware's average-Z index is not what orders the world. Until #7 falls
     * the port sorts per quad, and this is the range it sorts across.
     */
    s32 far_z;
} q2_camera;

/* view+264 for the four layouts that use it (0x80077E58, 0x80077674,
 * 0x80077A20, 0x80077C20); the quad split's 4000 is at 0x80077818. */
#define Q2_CAMERA_FAR_DEFAULT 6400

/*
 * A camera for a buffer that is NOT a console viewport — an offline render, a
 * model viewer, a test. It deliberately does not pretend to be the console's:
 * the projection distance is the buffer's own width, which frames a square-pixel
 * image sensibly and is what every offline path here has always used.
 *
 * The console's own distances are 160, 175 and 320 (screen.h and FORMATS §12.7),
 * they live in the view record at +262, and they are paired with a 512x248
 * framebuffer whose pixels are two thirds as wide as they are tall. Anything
 * that means to show what the television showed must take `projection` and
 * `ofs_x`/`ofs_y` from a viewport instead of calling this.
 */
void q2_camera_default(q2_camera *cam, int screen_w, int screen_h);

/*
 * BACKFACE REJECTION — the NCLIP pair every world quad goes through.
 *
 * All three quad linkers carry the identical sequence: 0x800AF8A8/0x800AF8C8
 * (variant 0), 0x800AFB08/0x800AFB28 (variant 2), 0x800AFD6C/0x800AFD8C
 * (variant 1). Culling is therefore not one of the things SETWIBBLE selects
 * between — it is what linking a quad means, and the variants differ only in
 * when they subdivide.
 *
 *     SXY0,SXY1,SXY2 = v0,v1,v3 ; NCLIP ; MAC0 >  0 -> draw
 *     SXY0           = v2       ; NCLIP ; MAC0 >= 0 -> drop
 *
 * `screen` is in the perimeter order MapMod stores, so v1-v3 is a diagonal and
 * the two tests are the quad's two halves. Either half facing the camera is
 * enough, which is what stops a quad that has folded through the near plane
 * from disappearing — and it is why the second test's comparison is >= rather
 * than > : a degenerate half is not evidence of visibility.
 *
 * Clobbers the GTE's SXY registers and MAC0, so call it once the projected
 * corners have been copied out.
 */
Q2PSX_INLINE bool q2_world_quad_faces_camera(gte_state *g, const gte_sxy screen[4])
{
    g->sxy[0] = screen[0];
    g->sxy[1] = screen[1];
    g->sxy[2] = screen[3];
    gte_nclip(g);
    if (g->mac0 > 0)
        return true;

    g->sxy[0] = screen[2];
    gte_nclip(g);
    return g->mac0 < 0;
}

/* Statistics from one build, so callers can tell "drew nothing" apart from
 * "drew everything off-screen" — a distinction that matters a great deal when
 * bringing a renderer up. */
typedef struct q2_world_stats {
    u32 nodes_visited;
    u32 quads_total;
    u32 quads_emitted;
    u32 quads_rejected_near;   /* GTE reported a divide overflow  */
    u32 quads_rejected_back;   /* both NCLIP halves faced away    */
    u32 quads_rejected_bad;    /* malformed record or bad indices */
    u32 quads_no_uv;           /* UV lookup failed; drawn untextured */
    u32 ot_overflow;           /* primitive pool was full         */

    /* The depth range the frame actually covered, in GTE SZ units, and how many
     * distinct ordering-table buckets it reached. A sort that collapses into
     * one bucket draws in emission order, which looks like a texture or
     * clipping fault rather than a sorting one — so it is measured. */
    u32 depth_min, depth_max;
    u32 buckets_used;

    /* Surface flags and blending — see surface.h for what each field means. */
    u32 nodes_hidden;          /* flags08 bit 15, or draw variant 3       */
    u32 nodes_sealing;         /* all-CLUT-0 node, never in a draw stream */
    u32 nodes_deferred;        /* flags08 bit 14                          */
    u32 quads_semi;            /* clut & 3 non-zero: drawn with ABE       */
    u32 quads_rejected_flat;   /* variant 1's zero-depth corner rejection */
    u32 quads_subdivided;      /* replaced by a 4x4 mesh                  */
} q2_world_stats;

/*
 * Render state that has to survive between frames.
 *
 * The texture-page table is why this exists. The world's opaque path does not
 * merely read `tpageTable[page] | blendTable[2]` — it writes the result back
 * (0x80068320), so a page is permanently promoted from ABR 0 to ABR 1 the first
 * time an opaque polygon on it is drawn, and every later transparent polygon on
 * that page inherits the promotion. Holding the table in caller-owned state is
 * what lets that happen once per level instead of once per frame.
 *
 * Passing NULL gives a fresh table for the call. That is what the offline tools
 * want, and it costs one frame of half-blended transparency that nothing
 * offline can observe.
 */
typedef struct q2_world_render {
    q2_tpage_table tpage;

    /*
     * Near-quad subdivision — the affine texture-warp control.
     *
     * `subdiv_threshold` is the summed depth of two opposite corners below which
     * a quad is subdivided into a 4x4 mesh. The original takes it from the
     * viewport record at +264 and parks it at 0x800B2CCC, which is the same
     * number as q2_camera.far_z. Zero disables subdivision.
     *
     * `pressure` is the packet-pool budget consulted at 0x800698A0: subdivide
     * freely, only for very near quads, or not at all.
     */
    s32                   subdiv_threshold;
    q2_surf_pool_pressure pressure;
} q2_world_render;

/* Pages at ABR 0, subdivision off. */
void q2_world_render_init(q2_world_render *r);

/*
 * Transform the zone and fill the ordering table.
 *
 * `ot` is cleared first. The GTE state is configured from `cam` and the
 * framebuffer size. `render` may be NULL, in which case a fresh render state is
 * used for the call. Returns the number of primitives emitted.
 */
u32 q2_world_build_ot(const q2_world_zone *z,
                      const q2_camera *cam,
                      int screen_w, int screen_h,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_world_render *render,
                      q2_world_stats *stats);

/* World-space bounds of the zone, computed properly: every node's local points
 * offset by that node's origin. Useful for placing a camera that can actually
 * see something. */
void q2_world_bounds(const q2_world_zone *z, s32 min_out[3], s32 max_out[3]);

#endif /* Q2PSX_WORLD_H */
