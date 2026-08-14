#include "world.h"

#include "mover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* The projection distance sets the field of view. Using the screen width is
     * the convention the console's own libraries used and gives roughly a
     * 90-degree horizontal view. */
    cam->projection = (u16)screen_w;
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

/* ------------------------------------------------------------------------- */
u32 q2_world_build_ot(const q2_world_zone *z,
                      const q2_camera *cam,
                      int screen_w, int screen_h,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_world_stats *stats)
{
    gte_matrix rot;
    u32 emitted = 0;
    u32 n;

    if (!z || !cam || !ot || !gte)
        return 0;

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
    gte_set_projection(gte, cam->projection, screen_w / 2, screen_h / 2);

    q2_rotation_yaw_pitch(rot.m, cam->yaw, cam->pitch);
    gte_set_rotation(gte, &rot);

    /* Scale the ordering table across the depth range the zone actually spans.
     * ZSF4 divides the summed Z of four vertices; picking it so that distant
     * geometry lands in the last bucket rather than all piling into bucket 0 is
     * what makes the sort do anything useful. */
    gte->zsf3 = (s16)(Q2_ONE_12 / 3);
    gte->zsf4 = (s16)(Q2_ONE_12 / 4);

    for (n = 0; n < z->scene.node_count; n++) {
        q2_scene_node node;
        q2_mapmod_rec rec;
        const q2_point_group *grp;
        u32 p;
        s32 translation[3];

        if (z->node_filter && n < z->node_filter_count && !z->node_filter[n])
            continue;

        if (!q2_scene_get_node(&z->scene, n, &node))
            continue;
        if (!q2_scene_get_mapmod(&z->scene, n, &rec)) {
            if (stats) stats->quads_rejected_bad++;
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

        /* The node's origin folds into the GTE translation, so each vertex stays
         * a plain s16 exactly as it is stored.
         *
         * A mover's displacement is added here rather than to the geometry,
         * because that is where the original adds it: the zone draw reads the
         * runtime object's s16 triple at +0x12 and offsets the node's
         * camera-space position by it. Nothing in the Scene chunk moves. */
        {
            s32 shift[3] = { 0, 0, 0 };

            if (z->movers)
                q2_movers_node_offset(z->movers, n, shift);

            translation[0] = node.origin[0] + shift[0] - cam->pos[0];
            translation[1] = node.origin[1] + shift[1] - cam->pos[1];
            translation[2] = node.origin[2] + shift[2] - cam->pos[2];
        }

        /* Translation is applied after rotation by the GTE, so it must be
         * expressed in camera space. */
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
                gte_sxy screen[4];
                u16 depth[4];
                u32 otz;

                screen[0] = gte->sxy[0];
                screen[1] = gte->sxy[1];
                screen[2] = gte->sxy[2];
                depth[0]  = gte->sz[1];
                depth[1]  = gte->sz[2];
                depth[2]  = gte->sz[3];

                if (gte->flag & GTE_FLAG_DIV_OVERFLOW) {
                    /* Behind or through the projection plane. The hardware could
                     * not clip, so the original rejected these outright — and so
                     * do we, because that popping is part of the look. */
                    if (stats) stats->quads_rejected_near++;
                    continue;
                }

                gte->v[0].x = pt[3].x; gte->v[0].y = pt[3].y; gte->v[0].z = pt[3].z;
                gte_rtps(gte, false);

                if (gte->flag & GTE_FLAG_DIV_OVERFLOW) {
                    if (stats) stats->quads_rejected_near++;
                    continue;
                }

                screen[3] = gte->sxy[2];
                depth[3]  = gte->sz[3];

                /* Average depth picks the ordering-table bucket. This is the
                 * whole depth-sorting mechanism: per primitive, not per pixel. */
                otz = ((u32)depth[0] + depth[1] + depth[2] + depth[3]) / 4;
                otz >>= 2;
                {
                    /*
                     * Fit the depth into whatever slice of the table this
                     * viewport owns — 51 buckets when the screen module has
                     * installed a window, the whole table otherwise.
                     *
                     * The console does NOT sort per polygon here: the zone draw
                     * links a whole scene node's quads into one bucket chosen
                     * from the `SortData` chunk, which is still undecoded
                     * (openquestions #7) — the emitter at 0x800AF9BC does a
                     * single AddPrim against one entry pointer per batch. So
                     * the port's sort is finer than the original's, not
                     * coarser, and the visible difference is that the
                     * original's sort artefacts are per node and these are per
                     * quad. When #7 falls this is the line that changes.
                     */
                    u32 span = psx_ot_bucket_span(ot);
                    if (span == 0)
                        span = 1;
                    if (otz >= span)
                        otz = span - 1;
                }

                prim = psx_ot_add(ot, (u16)otz);
                if (!prim) {
                    if (stats) stats->ot_overflow++;
                    continue;
                }

                prim->kind = PSX_PRIM_GT4;

                for (i = 0; i < 4; i++) {
                    prim->xy[i].x = screen[i].x;
                    prim->xy[i].y = screen[i].y;
                }

                /* Per-corner Gouraud colour, indexed through the record's RGB
                 * table. A missing table means flat white rather than a crash —
                 * five nodes on the disc legitimately have no polygons. */
                for (i = 0; i < 4; i++) {
                    u32 ci = poly.col[i];
                    if (rec.rgb && ci < rec.rgb_count) {
                        prim->rgb[i].r = rec.rgb[ci * 3 + 0];
                        prim->rgb[i].g = rec.rgb[ci * 3 + 1];
                        prim->rgb[i].b = rec.rgb[ci * 3 + 2];
                    } else {
                        prim->rgb[i].r = prim->rgb[i].g = prim->rgb[i].b = 128;
                    }
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
                        prim->uv[i].u = uv[c * 2 + 0];
                        prim->uv[i].v = uv[c * 2 + 1];
                    }
                } else {
                    prim->kind = PSX_PRIM_G4;   /* Gouraud, untextured. */
                    if (stats) stats->quads_no_uv++;
                }

                /*
                 * Translate the stored fields into what the GPU actually wants.
                 * All three rules below are the world renderer's own, read out
                 * of the executable rather than inferred:
                 *
                 *   clut >> 8    indexes the CLUT-id table the engine builds
                 *                at 0x80076378, one entry per 4bpp CLUT the
                 *                map uploads   (0x80068288)
                 *   clut & 3     non-zero sets the primitive's ABE bit, code
                 *                0x3E instead of 0x3C   (0x800682A8)
                 *   tpage & 0x1F indexes a table of GetTPage words built at
                 *                0x80078034 as GetTPage(0, 0, 64*(i+1), 256).
                 *                The literal 0 is the colour-mode argument, so
                 *                4bpp is the executable's own statement, not an
                 *                inference from CLUT geometry.
                 *
                 * The remaining six bits of the low byte are build-time residue
                 * the engine never reads; they are set on 251,872 of 274,936
                 * polygons, so a port that treats the u16 as a hardware CLUT
                 * word samples a nonsense VRAM address on 92% of the world.
                 *
                 * The blend mode is ADD, not HALF: the opaque path ORs 32 into
                 * the page's table entry at 0x80068320, which is ABR 1, and
                 * writes it back, so by the time any semi-transparent polygon
                 * on that page is drawn the mode is B+F.
                 */
                {
                    u32 clut_index = q2_mapmod_clut_index(poly.clut);
                    u32 semi       = q2_mapmod_clut_semi(poly.clut);

                    prim->clut  = q2_vram_clut_word(clut_index);
                    prim->tpage = psx_make_tpage((int)poly.tpage + 1, 1,
                                                 PSX_BLEND_ADD, PSX_TEX_4BIT);
                    prim->semi_transparent = (semi != 0);
                }
                prim->textured_blend  = true;

                emitted++;
                if (stats) stats->quads_emitted++;
            }
        }
    }

    return emitted;
}
