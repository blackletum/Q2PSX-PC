#include "modeldraw.h"

#include "trig.h"
#include "vram.h"

#include <string.h>

/* A transformed vertex as it sits in the shared window: screen position and
 * depth, exactly what the GTE left behind. */
typedef struct scratch_vertex {
    gte_sxy xy;
    u16     z;
    bool    valid;
} scratch_vertex;

/* m = a * b, both 1.3.12, row-major. */
static void matrix_mul(s16 m[3][3], const s16 a[3][3], const s16 b[3][3])
{
    int r, c;

    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            s32 sum = (s32)a[r][0] * b[0][c] +
                      (s32)a[r][1] * b[1][c] +
                      (s32)a[r][2] * b[2][c];
            m[r][c] = (s16)(sum >> Q2_FRAC_12);
        }
    }
}

static void matrix_apply(const s16 m[3][3], const s32 v[3], s32 out[3])
{
    int r;

    for (r = 0; r < 3; r++) {
        s64 sum = (s64)m[r][0] * v[0] + (s64)m[r][1] * v[1] +
                  (s64)m[r][2] * v[2];
        out[r] = (s32)(sum >> Q2_FRAC_12);
    }
}

u32 q2_model_build_ot(const q2_model_instance *inst,
                      const q2_camera *cam,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_model_draw_stats *stats)
{
    scratch_vertex window[Q2_MODEL_SCRATCH_MAX];
    s16 view[3][3], world[3][3];
    const q2_model *m;
    u32 part, face_index = 0, emitted = 0, vertex_cursor = 0;

    if (!inst || !inst->model || !cam || !ot || !gte)
        return 0;

    m = inst->model;

    if (stats)
        memset(stats, 0, sizeof(*stats));

    memset(window, 0, sizeof(window));

    q2_rotation_yaw_pitch(view, cam->yaw, cam->pitch);

    /* The instance's own facing, composed into the camera's rotation once
     * rather than per part. Yaw only: nothing on this disc tilts a model. */
    {
        s16 spin[3][3];
        q2_rotation_yaw_pitch(spin, inst->yaw, 0);
        matrix_mul(world, view, spin);
    }

    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;
        s16 rot[3][3], composed[3][3];
        s32 local[3], camera_space[3];
        u32 v;

        if (!q2_model_get_part(m, part, &p))
            break;

        if (stats)
            stats->parts++;

        if (inst->pose) {
            s16 q[4];
            memcpy(q, inst->pose[part].q, sizeof(q));
            q2_quat_to_matrix(rot, q);
            local[0] = inst->pose[part].t[0];
            local[1] = inst->pose[part].t[1];
            local[2] = inst->pose[part].t[2];
        } else {
            memset(rot, 0, sizeof(rot));
            rot[0][0] = rot[1][1] = rot[2][2] = (s16)Q2_ONE_12;
            local[0] = local[1] = local[2] = 0;
        }

        /* Vertices are part-local, so the part's rotation applies first and the
         * camera's after it. */
        matrix_mul(composed, world, rot);
        {
            gte_matrix gm;
            memcpy(gm.m, composed, sizeof(gm.m));
            gte_set_rotation(gte, &gm);
        }

        /* The part's translation is in model space: rotate it by the model's
         * own matrix, offset by where the model stands, and express the result
         * in camera space, because the GTE adds translation after rotating. */
        {
            s32 model_space[3], offset[3];

            {
                s16 spin[3][3];
                q2_rotation_yaw_pitch(spin, inst->yaw, 0);
                matrix_apply(spin, local, model_space);
            }

            offset[0] = inst->origin[0] + model_space[0] - cam->pos[0];
            offset[1] = inst->origin[1] + model_space[1] - cam->pos[1];
            offset[2] = inst->origin[2] + model_space[2] - cam->pos[2];

            matrix_apply(view, offset, camera_space);
            gte_set_translation(gte, camera_space[0], camera_space[1],
                                camera_space[2]);
        }

        /*
         * Each part's vertices are the next run in the model's vertex array —
         * the part record carries the count, not an offset — and they land in
         * the shared window at vert_base. Later parts may overwrite slots
         * earlier ones wrote; that overlap is the format, not a fault.
         */
        for (v = 0; v < p.num_verts; v++) {
            q2_model_vertex mv;
            u32 slot = p.vert_base + v;

            if (slot >= Q2_MODEL_SCRATCH_MAX)
                break;
            if (!q2_model_get_vertex(m, vertex_cursor + v, &mv))
                break;

            gte->v[0].x = mv.x;
            gte->v[0].y = mv.y;
            gte->v[0].z = mv.z;
            gte_rtps(gte, false);

            window[slot].xy    = gte->sxy[2];
            window[slot].z     = gte->sz[3];
            window[slot].valid = (gte->flag & GTE_FLAG_DIV_OVERFLOW) == 0;
        }

        vertex_cursor += p.num_verts;

        /* Faces are stored per part in the same order, so this part's faces are
         * the next run — and they can only be drawn once its vertices are in
         * the window, which is exactly why the original interleaves the two. */
        for (v = 0; v < p.num_faces; v++, face_index++) {
            q2_model_face f;
            psx_prim *prim;
            u32 otz = 0;
            bool ok = true;
            int i;

            if (!q2_model_get_face(m, face_index, &f))
                break;

            if (stats)
                stats->faces_total++;

            for (i = 0; i < 4; i++) {
                if (f.v[i] >= Q2_MODEL_SCRATCH_MAX || !window[f.v[i]].valid) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                if (stats) {
                    /* Distinguish "never written" from "behind the camera":
                     * the first is a decode fault, the second is the original's
                     * own near-plane behaviour. */
                    if (f.v[0] < Q2_MODEL_SCRATCH_MAX &&
                        f.v[1] < Q2_MODEL_SCRATCH_MAX &&
                        f.v[2] < Q2_MODEL_SCRATCH_MAX &&
                        f.v[3] < Q2_MODEL_SCRATCH_MAX)
                        stats->faces_rejected_near++;
                    else
                        stats->faces_rejected_bad++;
                }
                continue;
            }

            for (i = 0; i < 4; i++)
                otz += window[f.v[i]].z;
            otz = (otz / 4) >> 2;
            if (otz >= ot->bucket_count)
                otz = ot->bucket_count - 1;

            prim = psx_ot_add(ot, (u16)otz);
            if (!prim) {
                if (stats)
                    stats->ot_overflow++;
                continue;
            }

            prim->kind = PSX_PRIM_GT4;

            for (i = 0; i < 4; i++) {
                prim->xy[i].x  = window[f.v[i]].xy.x;
                prim->xy[i].y  = window[f.v[i]].xy.y;
                prim->uv[i].u  = f.uv[i][0];
                prim->uv[i].v  = f.uv[i][1];

                /* Lighting is not wired up yet, so draw at full brightness
                 * rather than black. The vertex normals are decoded and
                 * correct; what is missing is the light source. */
                prim->rgb[i].r = 128;
                prim->rgb[i].g = 128;
                prim->rgb[i].b = 128;
            }

            prim->clut = q2_vram_clut_word(
                q2_model_face_clut_index(&f, inst->clut4_count_a));
            prim->tpage = psx_make_tpage((int)q2_model_face_page(&f) + 1, 1,
                                         PSX_BLEND_ADD, PSX_TEX_4BIT);
            prim->semi_transparent = (q2_model_face_blend(&f) != 0);
            prim->textured_blend   = true;

            emitted++;
            if (stats)
                stats->faces_emitted++;
        }
    }

    return emitted;
}
