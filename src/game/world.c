#include "world.h"

#include "mover.h"
#include "rotator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* flare.h includes world.h, so it can only be reached from the .c side — which
 * is why q2_world_zone forward-declares the light world rather than including
 * it. */
#include "flare.h"
#include "sortdata.h"
#include "trig.h"
#include "vram.h"

q2_result q2_world_load_zone(q2_world_zone *out, const disc *d,
                             const char *map, int zone_index)
{
    char path[256];
    q2_buf buf;
    q2_result r;

    if (!out || !d || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/ZONE%d.DAT", map, zone_index);
    snprintf(out->name, sizeof(out->name), "%s/ZONE%d", map, zone_index);

    r = disc_read_file(d, path, &buf);
    if (r != Q2_OK) {
        /*
         * A MISSING zone file is how a caller learns where the zones stop.
         * Nothing on the disc says how many a map has, so the client counts by
         * probing until one is absent — which made 21 of the disc's 49 maps
         * report an ERROR on a perfectly normal load, and buried the four maps
         * that had a real one. Absent is INFO; anything else is still an error,
         * because a zone that exists and will not read is a fault.
         */
        if (r == Q2_ERR_NOT_FOUND)
            Q2_INFO("no %s — the map's zones end here", path);
        else
            Q2_ERROR("cannot read %s: %s", path, q2_result_str(r));
        return r;
    }

    r = q2_zone_open(&out->zone, &buf);
    if (r != Q2_OK) {
        q2_buf_free(&buf);
        return r;
    }

    r = q2_scene_parse(&out->scene, &out->zone);
    if (r != Q2_OK) {
        q2_zone_close(&out->zone);
        return r;
    }

    r = q2_points_parse(&out->points, &out->zone);
    if (r != Q2_OK) {
        q2_zone_close(&out->zone);
        return r;
    }

    /* The three chunks are indexed in lockstep; if they disagree we have
     * misparsed one of them and must not proceed on guesswork. */
    if (out->points.group_count != out->scene.node_count) {
        Q2_ERROR("%s: %u point groups but %u scene nodes",
                 out->name, out->points.group_count, out->scene.node_count);
        q2_points_free(&out->points);
        q2_zone_close(&out->zone);
        return Q2_ERR_BAD_FORMAT;
    }

    return Q2_OK;
}

void q2_world_free_zone(q2_world_zone *z)
{
    if (!z)
        return;
    q2_points_free(&z->points);
    q2_zone_close(&z->zone);
    memset(z, 0, sizeof(*z));
}

void q2_camera_default(q2_camera *cam, int screen_w, int screen_h)
{
    (void)screen_h;

    if (!cam)
        return;

    memset(cam, 0, sizeof(*cam));

    /*
     * The projection distance sets the field of view, and this one is the
     * PORT'S, not the console's — see the header. A distance equal to the
     * buffer's width puts the horizontal half-angle at atan(1/2), about 53
     * degrees across, which frames an offline square-pixel render without
     * claiming to be a viewport.
     *
     * The geometry offset is left at (0,0), which q2_world_build_ot reads as
     * "the middle of the buffer you hand me".
     */
    cam->projection = (u16)screen_w;
    cam->far_z      = Q2_CAMERA_FAR_DEFAULT;
}

void q2_world_bounds(const q2_world_zone *z, s32 min_out[3], s32 max_out[3])
{
    u32 n, i;
    s32 mn[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
    s32 mx[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
    bool any = false;

    if (!z)
        return;

    for (n = 0; n < z->scene.node_count; n++) {
        q2_scene_node node;
        const q2_point_group *grp;
        u32 k;

        if (!q2_scene_get_node(&z->scene, n, &node))
            continue;

        /* Scene node count and Points group count come from different headers
         * with nothing in the format tying them together. They agree on every
         * zone of the disc (17,035/17,035), but agreeing is not being
         * guaranteed, and this indexes one by the other. */
        if (n >= z->points.group_count)
            continue;

        grp = &z->points.groups[n];

        for (k = 0; k < grp->count; k++) {
            q2_point pt;
            s32 world[3];
            int c;

            if (!q2_points_get(&z->points, grp->first + k, &pt))
                continue;

            world[0] = pt.x + node.origin[0];
            world[1] = pt.y + node.origin[1];
            world[2] = pt.z + node.origin[2];

            for (c = 0; c < 3; c++) {
                if (world[c] < mn[c]) mn[c] = world[c];
                if (world[c] > mx[c]) mx[c] = world[c];
            }
            any = true;
        }
    }

    if (!any) {
        mn[0] = mn[1] = mn[2] = 0;
        mx[0] = mx[1] = mx[2] = 0;
    }

    for (i = 0; i < 3; i++) {
        if (min_out) min_out[i] = mn[i];
        if (max_out) max_out[i] = mx[i];
    }
}

void q2_world_render_init(q2_world_render *r)
{
    if (!r)
        return;

    memset(r, 0, sizeof(*r));
    q2_tpage_table_init(&r->tpage);
    r->subdiv_threshold = 0;
    r->pressure         = Q2_SURF_POOL_FREE;
}

/* ------------------------------------------------------------------------- */
/* One drawable quad, after transform and before it becomes a primitive.      */
/*                                                                            */
/* Split out because subdivision emits sixteen of these from one MapMod        */
/* polygon and every one of them has to go through the same colour, UV, CLUT   */
/* and blend rules as the original.                                            */
/* ------------------------------------------------------------------------- */
typedef struct world_quad {
    gte_sxy screen[4];
    u16     depth[4];
    psx_rgb rgb[4];
    psx_uv  uv[4];
    bool    textured;
} world_quad;

static psx_prim *emit_quad(psx_ot *ot,
                           const q2_camera *cam,
                           q2_world_render *render,
                           const world_quad *q,
                           const q2_mapmod_poly *poly,
                           s32 forced_bucket,
                           q2_world_stats *stats)
{
    psx_prim *prim;
    u32 otz;
    u32 span;
    s32 far;
    int i;

    otz = ((u32)q->depth[0] + q->depth[1] + q->depth[2] + q->depth[3]) / 4;

    if (stats) {
        if (stats->quads_emitted == 0 || otz < stats->depth_min)
            stats->depth_min = otz;
        if (otz > stats->depth_max)
            stats->depth_max = otz;
    }

    span = psx_ot_bucket_span(ot);
    far  = cam->far_z > 0 ? cam->far_z : Q2_CAMERA_FAR_DEFAULT;
    if (span == 0)
        span = 1;

    /*
     * A forced bucket is the authored one: the whole node goes into the bucket
     * SortData named, and the quad's own depth is not consulted at all. That is
     * the console's behaviour — see sortdata.h — and the depth mapping below is
     * the port's stand-in for when no order is supplied.
     */
    if (forced_bucket >= 0) {
        otz = (u32)forced_bucket;
        if (otz >= span)
            otz = span - 1;
        prim = psx_ot_add_bucket(ot, ot->window_len ? ot->window_base + otz : otz);
    } else {
        otz = (u32)(((u64)otz * span) / (u32)far);
        if (otz >= span)
            otz = span - 1;
        prim = psx_ot_add(ot, (u16)otz);
    }
    if (!prim) {
        if (stats) stats->ot_overflow++;
        return NULL;
    }

    prim->kind = q->textured ? PSX_PRIM_GT4 : PSX_PRIM_G4;

    for (i = 0; i < 4; i++) {
        prim->xy[i].x = q->screen[i].x;
        prim->xy[i].y = q->screen[i].y;
        prim->rgb[i]  = q->rgb[i];
        prim->uv[i]   = q->uv[i];
    }

    /*
     * Translate the stored fields into what the GPU actually wants. Every rule
     * here is the world renderer's own, read out of the executable:
     *
     *   clut >> 8    indexes the CLUT-id table the engine builds at 0x80076378,
     *                one entry per 4bpp CLUT the map uploads  (0x80068288)
     *   clut & 3     non-zero picks primitive code 0x3E over 0x3C, i.e. sets
     *                ABE                                      (0x800682A8)
     *   tpage & 0x1F indexes the table of GetTPage words built at 0x80078034 as
     *                GetTPage(0, 0, 64*(i+1), 256). The literal 0 is the colour
     *                mode, so 4bpp is the executable's statement, not ours.
     *
     * The two paths differ in more than the ABE bit, and that difference is the
     * whole of world transparency. The opaque path ORs blendTable[2] into the
     * page's table entry AND WRITES IT BACK (0x80068320), promoting the page
     * from ABR 0 to ABR 1; the transparent path reads the entry untouched. So a
     * transparent world surface is B/2+F/2 until an opaque polygon on its page
     * has been emitted, and B+F for the rest of the level. See surface.h.
     */
    {
        u32 clut_index = q2_mapmod_clut_index(poly->clut);
        bool semi      = q2_mapmod_clut_semi(poly->clut) != 0;
        u32 page       = (u32)poly->tpage & 0x1Fu;

        prim->clut = q2_vram_clut_word(clut_index);

        if (semi) {
            prim->tpage            = q2_tpage_world_semi(&render->tpage, page);
            prim->semi_transparent = true;
            if (stats) stats->quads_semi++;
        } else {
            prim->tpage            = q2_tpage_world_opaque(&render->tpage, page);
            prim->semi_transparent = false;
        }
    }
    prim->textured_blend = true;

    if (stats) stats->quads_emitted++;
    return prim;
}

/*
 * Replace one quad with the original's 4x4 mesh.
 *
 * 0x800B007C projects a 5x5 grid of vertices — 21 new ones, the four corners
 * arriving already transformed — and writes them into sixteen POLY_GT4 packets
 * at 52-byte stride. This does the same thing: bilinear interpolation in OBJECT
 * space (which is where the original's grid is built, since 0x800B007C loads
 * plain SVECTORs and runs RTPT over them) followed by one projection per point.
 *
 * The point of the exercise is the affine texture warp. UVs interpolate linearly
 * in screen space with no perspective divide, so the error over a quad grows
 * with how much perspective it spans; splitting a near quad sixteen ways cuts it
 * by the same factor. Interpolating the UV and Gouraud corners bilinearly is the
 * only thing that keeps the mesh identical to the quad it replaces, and is what
 * the packet writes require, though the interpolation itself was not traced
 * instruction by instruction.
 *
 * Returns the number of primitives emitted, or 0 if any grid point failed to
 * project — in which case the caller falls back to the flat quad rather than
 * dropping the surface.
 */
static u32 emit_subdivided(psx_ot *ot,
                           gte_state *gte,
                           const q2_camera *cam,
                           q2_world_render *render,
                           const q2_point pt[4],
                           const world_quad *flat,
                           const q2_mapmod_poly *poly,
                           s32 forced_bucket,
                           q2_world_stats *stats)
{
    enum { N = Q2_SURF_SUBDIV_STEPS, V = Q2_SURF_SUBDIV_STEPS + 1 };

    gte_sxy grid_xy[V][V];
    u16     grid_z[V][V];
    u32     emitted = 0;
    int     gx, gy, i;

    /*
     * The MapMod corners run around the perimeter, so p0-p1 and p3-p2 are the
     * two opposite edges and the surface parameter is
     *
     *     P(s,t) = lerp(lerp(p0,p1,s), lerp(p3,p2,s), t)
     *
     * with P(0,0)=p0, P(1,0)=p1, P(1,1)=p2, P(0,1)=p3.
     */
    for (gy = 0; gy < V; gy++) {
        for (gx = 0; gx < V; gx++) {
            s32 s = gx, t = gy;
            s32 top[3], bot[3], v[3];
            int c;

            /* Reuse the corners the caller already projected, exactly as the
             * original does — it is handed pointers to them rather than
             * re-transforming. */
            if ((gx == 0 || gx == N) && (gy == 0 || gy == N)) {
                int corner = (gy == 0) ? (gx == 0 ? 0 : 1)
                                       : (gx == 0 ? 3 : 2);
                grid_xy[gy][gx] = flat->screen[corner];
                grid_z[gy][gx]  = flat->depth[corner];
                continue;
            }

            top[0] = pt[0].x + ((pt[1].x - pt[0].x) * s) / N;
            top[1] = pt[0].y + ((pt[1].y - pt[0].y) * s) / N;
            top[2] = pt[0].z + ((pt[1].z - pt[0].z) * s) / N;
            bot[0] = pt[3].x + ((pt[2].x - pt[3].x) * s) / N;
            bot[1] = pt[3].y + ((pt[2].y - pt[3].y) * s) / N;
            bot[2] = pt[3].z + ((pt[2].z - pt[3].z) * s) / N;

            for (c = 0; c < 3; c++)
                v[c] = top[c] + ((bot[c] - top[c]) * t) / N;

            gte->v[0].x = (s16)v[0];
            gte->v[0].y = (s16)v[1];
            gte->v[0].z = (s16)v[2];
            gte_rtps(gte, false);

            if (gte->flag & GTE_FLAG_DIV_OVERFLOW)
                return 0;

            grid_xy[gy][gx] = gte->sxy[2];
            grid_z[gy][gx]  = gte->sz[3];
        }
    }

    for (gy = 0; gy < N; gy++) {
        for (gx = 0; gx < N; gx++) {
            world_quad sub = *flat;
            static const int dx[4] = { 0, 1, 1, 0 };
            static const int dy[4] = { 0, 0, 1, 1 };

            for (i = 0; i < 4; i++) {
                int cx = gx + dx[i];
                int cy = gy + dy[i];
                s32 fs = cx, ft = cy;

                sub.screen[i] = grid_xy[cy][cx];
                sub.depth[i]  = grid_z[cy][cx];

                /* The same bilinear weights, applied to the corner attributes. */
                {
                    s32 tu = flat->uv[0].u + ((s32)flat->uv[1].u - flat->uv[0].u) * fs / N;
                    s32 tv = flat->uv[0].v + ((s32)flat->uv[1].v - flat->uv[0].v) * fs / N;
                    s32 bu = flat->uv[3].u + ((s32)flat->uv[2].u - flat->uv[3].u) * fs / N;
                    s32 bv = flat->uv[3].v + ((s32)flat->uv[2].v - flat->uv[3].v) * fs / N;

                    sub.uv[i].u = (u8)(tu + (bu - tu) * ft / N);
                    sub.uv[i].v = (u8)(tv + (bv - tv) * ft / N);
                }
                {
                    s32 tr = flat->rgb[0].r + ((s32)flat->rgb[1].r - flat->rgb[0].r) * fs / N;
                    s32 tg = flat->rgb[0].g + ((s32)flat->rgb[1].g - flat->rgb[0].g) * fs / N;
                    s32 tb = flat->rgb[0].b + ((s32)flat->rgb[1].b - flat->rgb[0].b) * fs / N;
                    s32 br = flat->rgb[3].r + ((s32)flat->rgb[2].r - flat->rgb[3].r) * fs / N;
                    s32 bg = flat->rgb[3].g + ((s32)flat->rgb[2].g - flat->rgb[3].g) * fs / N;
                    s32 bb = flat->rgb[3].b + ((s32)flat->rgb[2].b - flat->rgb[3].b) * fs / N;

                    sub.rgb[i].r = (u8)(tr + (br - tr) * ft / N);
                    sub.rgb[i].g = (u8)(tg + (bg - tg) * ft / N);
                    sub.rgb[i].b = (u8)(tb + (bb - tb) * ft / N);
                }
            }

            if (emit_quad(ot, cam, render, &sub, poly, forced_bucket, stats))
                emitted++;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
u32 q2_world_build_ot(const q2_world_zone *z,
                      const q2_camera *cam,
                      int screen_w, int screen_h,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_world_render *render,
                      q2_world_stats *stats)
{
    gte_matrix rot, spin;
    q2_world_render local;
    q2_sort_reader order;
    bool have_order = false;
    u32 next_node = 0;
    u32 emitted = 0;
    u32 n;

    if (!z || !cam || !ot || !gte)
        return 0;

    if (!render) {
        q2_world_render_init(&local);
        render = &local;
    }

    if (stats)
        memset(stats, 0, sizeof(*stats));

    /*
     * An installed window means a caller owns the frame — the screen module has
     * already cleared the table and is filling one viewport's slice, so
     * clearing here would throw away the viewport drawn before this one. A
     * caller with no window keeps the old "hand me a table, I will manage it"
     * contract the offline tools use.
     */
    if (ot->window_len == 0)
        psx_ot_clear(ot);

    gte_init(gte);
    /*
     * The projection is the VIEWPORT's, not the buffer's. A caller driving this
     * through q2_screen has already had `SetGeomOffset(view+266, view+268)` and
     * `SetGeomScreen(view+262)` installed for it by q2_screen_view_begin
     * (0x80076B78 / 0x80076B90); reloading them here from the buffer's own
     * middle would agree by luck for the layouts whose viewport is the whole
     * frame and be wrong for the three splits, whose centres are not the
     * framebuffer's. So the camera carries the centre and the buffer is only the
     * fallback — see q2_camera.ofs_x.
     */
    gte_set_projection(gte, cam->projection,
                       (cam->ofs_x || cam->ofs_y) ? cam->ofs_x : screen_w / 2,
                       (cam->ofs_x || cam->ofs_y) ? cam->ofs_y : screen_h / 2);

    q2_rotation_view(rot.m, cam->yaw, cam->pitch, cam->roll);
    gte_set_rotation(gte, &rot);

    /*
     * `InitGeom` at 0x8008E4C4 leaves ZSF3 = 341 and ZSF4 = 256, and nothing in
     * the image overrides them, so the GTE's average-Z is a quarter of the mean
     * vertex depth. Those are the values the port runs with; the mapping from a
     * depth to a bucket is done below against the viewport's far distance
     * instead, because a quarter of a room-scale depth saturates a 51-entry
     * slice several times over.
     */
    gte->zsf3 = GTE_ZSF3_INIT;
    gte->zsf4 = GTE_ZSF4_INIT;

    /*
     * Two walks over the same body. Without a SortData stream the nodes go in
     * index order and each quad picks its own bucket from its depth; with one,
     * the stream dictates both the order and the bucket. See sortdata.h for why
     * those are genuinely different renderings rather than two granularities of
     * the same one.
     */
    if (z->sort && z->sort->data)
        have_order = q2_sort_begin(&order, z->sort, z->sort_offset,
                                   Q2_SORT_BUCKET_START);

    for (;;) {
        q2_scene_node node;
        q2_mapmod_rec rec;
        const q2_point_group *grp;
        q2_surf_variant variant;
        s16 spin_angles[3], spin_pivot[3];
        bool spin_active = false;
        s32 forced_bucket = -1;
        u32 p;
        s32 translation[3];

        if (have_order) {
            q2_sort_item it;

            if (!q2_sort_next(&order, &it))
                break;

            if (it.kind == Q2_SORT_ENTITY) {
                /* Models are drawn by their own emitter, so nothing is placed
                 * here — but the bucket must still step, because that stepping
                 * is what the world's own order is measured against. */
                q2_sort_entity_resolve(&order, true);
                continue;
            }

            n = it.node;
            if (n >= z->scene.node_count)
                continue;
            forced_bucket = (s32)it.bucket;
        } else {
            n = next_node++;
            if (n >= z->scene.node_count)
                break;
        }

        if (z->node_filter && n < z->node_filter_count && !z->node_filter[n])
            continue;

        if (!q2_scene_get_node(&z->scene, n, &node))
            continue;

        /*
         * The two node-level gates, both from the zone draw at 0x80067714.
         *
         * Bit 15 is a hide flag: `andi 0x8000` and straight on to the next node.
         * It is clear on every node on the disc — OBJDRAWOFF sets it at runtime.
         *
         * Draw variant 3 is the same outcome reached from the other direction:
         * the emitter at 0x80066740 dispatches on bits 10-11 and case 3 links
         * nothing. That is how a script hides a surface group with SETWIBBLE.
         */
        if (q2_scene_flags_nodraw(node.flags)) {
            if (stats) stats->nodes_hidden++;
            continue;
        }

        /* The same bit, set at run time rather than on the chunk — see
         * `node_hidden`. A script that has hidden this node hides it here. */
        if (z->node_hidden && n < z->node_hidden_count && z->node_hidden[n]) {
            if (stats) stats->nodes_hidden++;
            continue;
        }

        variant = q2_scene_flags_variant(node.flags);
        if (variant == Q2_SURF_VARIANT_HIDDEN) {
            if (stats) stats->nodes_hidden++;
            continue;
        }

        /*
         * Bit 14 puts the node on the deferred path (0x80066524): one depth for
         * the whole node from its projected origin, registered against a table
         * keyed on `unk0E`. The port draws it inline for now and counts it, so
         * the difference is visible rather than silent.
         */
        if (q2_scene_flags_deferred(node.flags) && stats)
            stats->nodes_deferred++;

        if (!q2_scene_get_mapmod(&z->scene, n, &rec)) {
            if (stats) stats->quads_rejected_bad++;
            continue;
        }

        /*
         * The third node-level gate, and the only one that is not a flag.
         *
         * A node whose every polygon binds CLUT index 0 is sealing geometry —
         * the flat planes the build tool hangs across doorways and openings.
         * Index 0 is a reserved all-0x8000 palette, so drawing one paints
         * opaque black over the room behind it. No SortData stream on the disc
         * names one; see q2_mapmod_rec_is_sealing for the measurement.
         *
         * Only applied on the index-order walk. When a stream IS supplied it
         * has already made this choice, and following it is more faithful than
         * second-guessing it — on this disc the two agree, and if some other
         * build's stream ever named such a node, the stream would be right.
         */
        if (!have_order && q2_mapmod_rec_is_sealing(&rec)) {
            if (stats) stats->nodes_sealing++;
            continue;
        }

        /* See the matching check in q2_world_bounds: these two counts come from
         * separate headers and nothing in the format ties them together. */
        if (n >= z->points.group_count) {
            if (stats) stats->quads_rejected_bad += rec.num_polys;
            continue;
        }

        grp = &z->points.groups[n];
        if (stats) {
            stats->nodes_visited++;
            stats->quads_total += rec.num_polys;
        }

        /*
         * The node's origin folds into the GTE translation, so each vertex stays
         * a plain s16 exactly as it is stored.
         *
         * A mover's displacement is added here rather than to the geometry,
         * because that is where the original adds it: the zone draw reads the
         * runtime object's s16 triple at +0x12 and offsets the node's
         * camera-space position by it. Nothing in the Scene chunk moves.
         *
         * Rotation arrives the same way and completes the transform
         * (0x800678B4 - 0x8006793C):
         *
         *     node position = origin - camera - (R . p) + p + d
         *
         * where R is RotMatrix of the object's three Euler angles at +0x0C, p is
         * the pivot at +0x18 relative to the node's origin, and d is the linear
         * displacement at +0x12. The `- R.p + p` is a rotation about p, so the
         * node's own vertices have to go through R as well — which is why the
         * rotation is composed into the GTE matrix below rather than being
         * folded into the translation on its own.
         */
        {
            s32 shift[3] = { 0, 0, 0 };

            if (z->movers)
                q2_movers_node_offset(z->movers, n, shift);

            spin_active = z->rotators &&
                          q2_rotators_node_transform(z->rotators, n,
                                                     spin_angles, spin_pivot);

            if (spin_active) {
                s32 rp[3];
                int c;

                q2_rotation_euler(spin.m, spin_angles[0], spin_angles[1],
                                  spin_angles[2]);

                /* R . p at 1.3.12, exactly as 0x8006FB18 computes it: sum the
                 * three products and shift once at the end. */
                for (c = 0; c < 3; c++) {
                    s64 s = (s64)spin.m[c][0] * spin_pivot[0]
                          + (s64)spin.m[c][1] * spin_pivot[1]
                          + (s64)spin.m[c][2] * spin_pivot[2];
                    rp[c] = (s32)(s >> Q2_FRAC_12);
                }

                for (c = 0; c < 3; c++)
                    shift[c] += spin_pivot[c] - rp[c];
            }

            translation[0] = node.origin[0] + shift[0] - cam->pos[0];
            translation[1] = node.origin[1] + shift[1] - cam->pos[1];
            translation[2] = node.origin[2] + shift[2] - cam->pos[2];
        }

        /*
         * A rotating node draws through camera_rot . R, so its own geometry
         * turns; a still one keeps the camera matrix untouched. Setting this per
         * node is what the original does too — RotMatrix is called inside the
         * node loop, not once per frame.
         */
        if (spin_active) {
            gte_matrix composed;
            int r_, c_;

            for (r_ = 0; r_ < 3; r_++) {
                for (c_ = 0; c_ < 3; c_++) {
                    s32 s = (s32)rot.m[r_][0] * spin.m[0][c_]
                          + (s32)rot.m[r_][1] * spin.m[1][c_]
                          + (s32)rot.m[r_][2] * spin.m[2][c_];
                    composed.m[r_][c_] = (s16)(s >> Q2_FRAC_12);
                }
            }
            gte_set_rotation(gte, &composed);
        } else {
            gte_set_rotation(gte, &rot);
        }

        /* Translation is applied after rotation by the GTE, so it must be
         * expressed in camera space. Note it uses the CAMERA matrix, not the
         * composed one: the node's position is not rotated by its own spin, only
         * its vertices are. */
        {
            s64 tx = (s64)rot.m[0][0] * translation[0]
                   + (s64)rot.m[0][1] * translation[1]
                   + (s64)rot.m[0][2] * translation[2];
            s64 ty = (s64)rot.m[1][0] * translation[0]
                   + (s64)rot.m[1][1] * translation[1]
                   + (s64)rot.m[1][2] * translation[2];
            s64 tz = (s64)rot.m[2][0] * translation[0]
                   + (s64)rot.m[2][1] * translation[1]
                   + (s64)rot.m[2][2] * translation[2];

            gte_set_translation(gte,
                                (s32)(tx >> Q2_FRAC_12),
                                (s32)(ty >> Q2_FRAC_12),
                                (s32)(tz >> Q2_FRAC_12));
        }

        for (p = 0; p < rec.num_polys; p++) {
            q2_mapmod_poly poly;
            q2_point pt[4];
            psx_prim *prim;
            bool ok = true;
            int i;

            if (!q2_mapmod_get_poly(&rec, p, &poly)) {
                if (stats) stats->quads_rejected_bad++;
                continue;
            }

            for (i = 0; i < 4; i++) {
                if (poly.vtx[i] >= grp->count ||
                    !q2_points_get(&z->points, grp->first + poly.vtx[i], &pt[i])) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                if (stats) stats->quads_rejected_bad++;
                continue;
            }

            /* Transform the quad as two GTE triangles: RTPT handles three
             * vertices, so the fourth goes through on a second pass. */
            gte->v[0].x = pt[0].x; gte->v[0].y = pt[0].y; gte->v[0].z = pt[0].z;
            gte->v[1].x = pt[1].x; gte->v[1].y = pt[1].y; gte->v[1].z = pt[1].z;
            gte->v[2].x = pt[2].x; gte->v[2].y = pt[2].y; gte->v[2].z = pt[2].z;
            gte_rtpt(gte);

            {
                world_quad q;
                gte_sxy screen[4];
                u16 depth[4];

                screen[0] = gte->sxy[0];
                screen[1] = gte->sxy[1];
                screen[2] = gte->sxy[2];
                depth[0]  = gte->sz[1];
                depth[1]  = gte->sz[2];
                depth[2]  = gte->sz[3];

                /*
                 * NO BLANKET NEAR REJECTION — this is what was eating the
                 * geometry at the edge of the viewport.
                 *
                 * Two `if (gte->flag & GTE_FLAG_DIV_OVERFLOW) continue;` blocks
                 * used to sit here, one after the RTPT and one after the RTPS,
                 * on the claim that "the original rejected these outright". The
                 * divide overflows for any corner nearer than H/2 — 80 units at
                 * the one-player viewport's projection distance of 160 — and
                 * that is not "behind the camera", it is *close*. A wall or a
                 * floor the player is standing next to has one corner inside 80
                 * units and the WHOLE quad went, so 1898 of 7364 quads were
                 * being dropped in an ordinary standing view.
                 *
                 * And near geometry projects to the frame's BORDER, which is why
                 * the holes always appeared at the outskirts of the viewport
                 * rather than in the middle of it.
                 *
                 * The original's near-plane treatment is not rejection, it is
                 * SUBDIVISION (0x800AF7CC and 0x800AFA2C, and see the emit
                 * below): a near quad is replaced by a 4x4 mesh whose pieces
                 * each project sanely. Only draw variant 1 rejects anything, and
                 * that test is its own, is address-backed, and is still applied
                 * a few lines down.
                 */
                gte->v[0].x = pt[3].x; gte->v[0].y = pt[3].y; gte->v[0].z = pt[3].z;
                gte_rtps(gte, false);

                screen[3] = gte->sxy[2];
                depth[3]  = gte->sz[3];

                /*
                 * A CORNER THAT OVERFLOWED THE DIVIDE HAS NO USABLE SCREEN
                 * POSITION, and the quad is rejected unless SUBDIVISION is
                 * going to rescue it.
                 *
                 * This is the near-plane rule, and getting it wrong in either
                 * direction is visible. Rejecting whenever ANY corner overflows
                 * — the sticky-flag test that used to be here — throws away a
                 * quarter of an ordinary view, and near geometry projects to
                 * the frame's border, so the holes appear at the outskirts.
                 * Keeping such a quad instead is worse: `gte_divide` clamps to
                 * 0x1FFFF on overflow, so that corner comes back SATURATED, and
                 * the quad is emitted stretched across the screen where it
                 * paints over everything behind it. That reads as "distant
                 * geometry is being culled" and is the opposite — it is near
                 * geometry being smeared over the distance.
                 *
                 * The original resolves it by SUBDIVIDING (0x800AF7CC,
                 * 0x800AFA2C): the 4x4 mesh's pieces each project sanely, so
                 * there is nothing to reject. Where subdivision does not apply —
                 * draw variant 1, which never subdivides, and variant 2's
                 * untagged polygons — the quad really is undrawable and goes.
                 * So the test is "any corner overflowed AND nothing is going to
                 * subdivide this", which is the same predicate consulted below,
                 * asked early.
                 */
                {
                    u32 h = (u32)cam->projection;
                    bool over = (h >= (u32)depth[0] * 2u) ||
                                (h >= (u32)depth[1] * 2u) ||
                                (h >= (u32)depth[2] * 2u) ||
                                (h >= (u32)depth[3] * 2u);

                    if (over &&
                        !(render->subdiv_threshold > 0 &&
                          q2_surf_should_subdivide(variant, poly.clut,
                                                   (s32)depth[1] + (s32)depth[3],
                                                   render->subdiv_threshold,
                                                   render->pressure))) {
                        if (stats) stats->quads_rejected_near++;
                        continue;
                    }
                }

                /*
                 * Draw variant 1's own rejection (0x800AFD20 / 0x800AFD34): if
                 * either of the two corners it looks up — vertex 1 and vertex 3,
                 * the packed byte pairs it extracts first — projected to depth
                 * zero, the quad is dropped without further testing. Variants 0
                 * and 2 have no such test, which is the only place the three
                 * linkers disagree about what is drawable at all.
                 */
                if (variant == Q2_SURF_VARIANT_FLAT &&
                    (depth[1] == 0 || depth[3] == 0)) {
                    if (stats) stats->quads_rejected_flat++;
                    continue;
                }

                /*
                 * Backface rejection, which the port did not have at all.
                 *
                 * The rule and its provenance are in world.h; what belongs here
                 * is what it was costing. Levels are sealed by brushes whose
                 * outward faces are never meant to be seen, and drawn without a
                 * cull those faces land in front of the rooms they enclose and
                 * black them out. It reads as a visibility fault — as if whole
                 * areas were failing to stream in — when the geometry behind
                 * them was being transformed and emitted correctly all along.
                 */
                if (!q2_world_quad_faces_camera(gte, screen)) {
                    if (stats) stats->quads_rejected_back++;
                    continue;
                }

                for (i = 0; i < 4; i++) {
                    q.screen[i] = screen[i];
                    q.depth[i]  = depth[i];
                }
                q.textured = true;

                /* Per-corner Gouraud colour, indexed through the record's RGB
                 * table. A missing table means flat white rather than a crash —
                 * five nodes on the disc legitimately have no polygons. */
                for (i = 0; i < 4; i++) {
                    u32 ci = poly.col[i];
                    if (rec.rgb && ci < rec.rgb_count) {
                        q.rgb[i].r = rec.rgb[ci * 3 + 0];
                        q.rgb[i].g = rec.rgb[ci * 3 + 1];
                        q.rgb[i].b = rec.rgb[ci * 3 + 2];
                    } else {
                        q.rgb[i].r = q.rgb[i].g = q.rgb[i].b = 128;
                    }
                    q.rgb[i].pad = 0;
                }

                /*
                 * UV corners are ROTATED AND REVERSED against the vertices, by
                 * an amount the polygon carries in the top two bits of its UV
                 * index byte:
                 *
                 *     vertex j takes uv[(3 - f - j) & 3],  f = uvIdxFlags >> 6
                 *
                 * This is transcribed from the world renderer at 0x80068118 -
                 * 0x800681D8, which writes the four packet corners from
                 * uv[(3-f)&3], uv[(2-f)&3], uv[(0-f)&3] and uv[(1-f)&3] in
                 * POLY_GT4 order, and whose colour writes in the same loop
                 * establish that packet corner 2 is vertex 3 and corner 3 is
                 * vertex 2. Undoing that Z-order shuffle leaves the rule above.
                 *
                 * Two earlier readings are dead. Straight-through (uv[i] to
                 * vertex i) is a mirror of the truth, and the "UV table is in
                 * libgpu Z order" reading was tested by rendering both ways and
                 * rejected on the evidence. Neither could have been settled
                 * from geometry alone: on a rectangular UV rect a reflection
                 * still tiles cleanly, and only text and asymmetric decals show
                 * it. f is non-zero on 11.7% of polygons.
                 *
                 * The else is not dead code even though the lookup currently
                 * succeeds for all 274,936 polygons on the disc: prim comes from
                 * a zeroed pool, so a silent failure would collapse the quad
                 * onto texel (0,0) and read as a flat blob rather than as a
                 * fault. Mirror what the RGB path above does and be explicit.
                 */
                if (rec.uv && poly.uv_idx < rec.uv_count) {
                    const u8 *uv = rec.uv + (size_t)poly.uv_idx * 8;
                    for (i = 0; i < 4; i++) {
                        u32 c = (u32)((3 - poly.flags - i) & 3);
                        q.uv[i].u = uv[c * 2 + 0];
                        q.uv[i].v = uv[c * 2 + 1];
                    }
                } else {
                    q.textured = false;   /* Gouraud, untextured. */
                    q.uv[0].u = q.uv[0].v = 0;
                    q.uv[1] = q.uv[2] = q.uv[3] = q.uv[0];
                    if (stats) stats->quads_no_uv++;
                }

                /*
                 * Subdivision — the affine texture-warp control, selected per
                 * node by SETWIBBLE and gated per polygon on variant 2. The
                 * depth the original tests is the sum of two opposite corners
                 * (0x800AF874), not the average of four, so use exactly that.
                 *
                 * A grid point that fails to project falls back to the flat
                 * quad rather than dropping the surface; the original cannot
                 * reach that case because it never subdivides a quad the near
                 * test already rejected.
                 */
                if (render->subdiv_threshold > 0 &&
                    q2_surf_should_subdivide(variant, poly.clut,
                                             (s32)depth[1] + (s32)depth[3],
                                             render->subdiv_threshold,
                                             render->pressure)) {
                    u32 sub = emit_subdivided(ot, gte, cam, render,
                                              pt, &q, &poly, forced_bucket,
                                              stats);
                    if (sub) {
                        emitted += sub;
                        if (stats) stats->quads_subdivided++;
                        continue;
                    }
                }

                prim = emit_quad(ot, cam, render, &q, &poly, forced_bucket, stats);
                if (prim)
                    emitted++;
            }
        }
    }

    /*
     * The flares, last, because they are additive and the original draws them
     * after the world for the same reason: 0x800759F0 is its own per-viewport
     * pass, run once the geometry is in the table, and it links every flare into
     * a single entry rather than sorting them. §17.3.
     */
    if (z->lights) {
        q2_flare_view view;

        memset(&view, 0, sizeof(view));

        /*
         * The centre is view+266/+268 — the SAME geometry offset the projection
         * above was given, not a bare half-extent. On a split screen the two
         * part company, and an element positioned against the wrong one slides
         * along its own centre-to-light line by the difference.
         */
        view.centre[0] = (s16)((cam->ofs_x || cam->ofs_y) ? cam->ofs_x
                                                          : screen_w / 2);
        view.centre[1] = (s16)((cam->ofs_x || cam->ofs_y) ? cam->ofs_y
                                                          : screen_h / 2);
        /* The 2D EXTENT, not the size — see q2_camera. Paired sentinel, so a
         * layout with a legitimately-zero component on one axis does not fall
         * back on that axis alone. */
        view.extent[0] = (s16)((cam->ext_w || cam->ext_h) ? cam->ext_w
                                                          : screen_w);
        view.extent[1] = (s16)((cam->ext_w || cam->ext_h) ? cam->ext_h
                                                          : screen_h);

        /*
         * One bucket for all of them, as the original has. Nearest in the
         * viewport's slice, so a flare paints over the geometry it is a
         * reflection of rather than being occluded by it — which is what a lens
         * flare is.
         */
        {
            u32 base = ot->window_len ? ot->window_base : 0;
            u32 span = ot->window_len ? ot->window_len  : ot->bucket_count;
            view.bucket = (u16)(base + (span ? span - 1 : 0));
        }

        /* The rotation matrix the flare pass needs is the one the node loop has
         * been leaving behind; restore the view rotation on its own so a MVMVA
         * against it is the camera transform and nothing else. */
        gte_set_rotation(gte, &rot);
        gte_set_translation(gte, 0, 0, 0);

        {
            q2_flare_stats fs;

            memset(&fs, 0, sizeof(fs));
            emitted += q2_flare_draw_all(z->lights, cam, z->light_node, &view,
                                         ot, gte, &fs);

            if (stats) {
                stats->flare_lights += fs.lights_considered;
                stats->flare_styled += fs.lights_styled;
                stats->flare_near   += fs.rejected_near;
                stats->flare_dark   += fs.rejected_dark;
                stats->flare_drawn  += fs.flares_drawn;
                stats->flare_prims  += fs.prims_emitted;
                stats->ot_overflow  += fs.ot_overflow;
            }
        }
    }

    /* How much of the slice the sort actually reached. One bucket means no
     * sorting happened at all, however many quads went in. */
    if (stats) {
        u32 base = ot->window_len ? ot->window_base : 0;
        u32 span = ot->window_len ? ot->window_len  : ot->bucket_count;
        u32 b;

        for (b = base; b < base + span && b < ot->bucket_count; b++)
            if (ot->bucket_head[b] >= 0)
                stats->buckets_used++;
    }

    return emitted;
}
