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

    /*
     * The lit colour of this slot. The original's window holds it in the same
     * record — "normals run through the lighting op into the same slots"
     * (0x800B23F0's NCT writes the colour FIFO alongside the RTPT results) —
     * and keeping it here rather than recomputing per face is what makes a face
     * that borrows another part's vertex borrow that part's LIGHT too. That is
     * the format's behaviour, not an approximation of it.
     */
    psx_rgb rgb;
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

/*
 * The same, through the matrix's TRANSPOSE — which for a rotation is its
 * inverse.
 *
 * A model's normals are part-LOCAL and the light directions are in WORLD space,
 * so one of the two has to cross into the other's frame before they can be
 * dotted. The instance matrix carries local -> world, so the directions need
 * its inverse. Rotating them FORWARDS instead, which is what this module used
 * to do, mirrors every model's shading about its own facing: normals pointing
 * at a lamp came out black and normals pointing away came out fully lit.
 */
static void matrix_apply_t(const s16 m[3][3], const s32 v[3], s32 out[3])
{
    int r;

    for (r = 0; r < 3; r++) {
        s64 sum = (s64)m[0][r] * v[0] + (s64)m[1][r] * v[1] +
                  (s64)m[2][r] * v[2];
        out[r] = (s32)(sum >> Q2_FRAC_12);
    }
}

void q2_model_instance_init(q2_model_instance *inst)
{
    if (!inst)
        return;
    memset(inst, 0, sizeof(*inst));
    inst->scale   = Q2_ONE_12;
    inst->tint[0] = inst->tint[1] = inst->tint[2] = 128;
    inst->bucket_override = -1;    /* sort per face unless a caller says not to */
}

u32 q2_model_build_ot(const q2_model_instance *inst,
                      const q2_camera *cam,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_model_draw_stats *stats)
{
    scratch_vertex window[Q2_MODEL_SCRATCH_MAX];
    s16 view[3][3], world[3][3], spin[3][3], spin_unscaled[3][3];
    q2_tpage_table private_tpage;
    const q2_tpage_table *tpage;
    const q2_model *m;
    u8  tint[3];
    u32 part, face_index = 0, emitted = 0, vertex_cursor = 0;

    if (!inst || !inst->model || !cam || !ot || !gte)
        return 0;

    m = inst->model;

    if (stats)
        memset(stats, 0, sizeof(*stats));

    memset(window, 0, sizeof(window));

    /* Share the world's table when the caller has one; otherwise a private copy
     * at ABR 0, which is what a page starts at before any opaque world polygon
     * has promoted it. */
    tpage = inst->tpage;
    if (!tpage) {
        q2_tpage_table_init(&private_tpage);
        tpage = &private_tpage;
    }

    /* The same basis the world is drawn with, roll included — models and brush
     * geometry must lean together or the two separate as soon as you strafe. */
    q2_rotation_view(view, cam->yaw, cam->pitch, cam->roll);

    /*
     * The instance's own facing, composed into the camera's rotation once
     * rather than per part. Everything placed in the world is yaw-only — the
     * rotation integrator writes one axis — so that path is kept exactly as it
     * was; the three-angle form is taken only when a caller asks for it, which
     * today is the view weapon and its RotMatrix at 0x8004F464.
     */
    /*
     * AND THE YAW IS MIRRORED ON THE WAY IN, which is why every creature
     * moonwalked and shot through its own back.
     *
     * `q2_rotation_yaw_pitch` builds Ry with m[0][2] = -sin and m[2][0] = +sin,
     * which is Ry(-yaw) — the WORLD-TO-CAMERA form, and exactly right for the
     * view basis above. Used unchanged as an instance's MODEL-TO-WORLD matrix
     * it turns the mesh the wrong way, so the yaw is mirrored on the way in.
     *
     * THAT MIRROR IS THE WHOLE CORRECTION. This used to add a half turn on top
     * of it — `2048 - yaw` — on the grounds that the Soldier mesh is authored
     * facing -Z, and that second half is what was still turning every creature
     * round. It was measured through this very function, which is circular: the
     * function's own convention was both the instrument and the thing under
     * test.
     *
     * The mesh decides it, and it says +Z. BASE1's Soldier poses to Z bounds
     * -151..+266, asymmetric toward +Z by the length of the outstretched weapon
     * arm, which is the part of a humanoid that points where it is facing. In
     * the game the extra half turn shows plainly: a soldier that has the player
     * down to 8 hp is drawn back-to-camera, and without it the same frame draws
     * it facing the player with its gun across its body.
     *
     * The simulation was never the problem and is now measured rather than
     * assumed — `--trace-cre` reports the angle between the direction a
     * creature travels and the one it faces, and over a hunting soldier's run
     * that angle is 0 on every walking tick. It reads 2048 only while the
     * soldier is backing off under attack_state 1 and while a corpse slides,
     * both of which are meant to be backwards.
     */
    if (inst->rot)
        memcpy(spin, inst->rot, sizeof(s16) * 9);
    else if (inst->pitch == 0 && inst->roll == 0)
        q2_rotation_yaw_pitch(spin, -inst->yaw, 0);
    else
        q2_rotation_euler(spin, inst->pitch, -inst->yaw,
                          inst->roll);

    /*
     * Uniform scale, applied to the instance's own matrix — which is where the
     * engine applies it too (0x8006B298 scales the rows of entity+0x2C0). Doing
     * it here rather than to the vertices means a part's TRANSLATION scales with
     * it, so a half-size model's parts stay attached instead of drifting apart.
     *
     * Only scales down in practice: an item ramps 0 -> 4096 while it
     * materialises. A scale above 4096 would push the 1.3.12 matrix entries out
     * of s16, so it is clamped rather than allowed to wrap.
     */
    /*
     * The UNSCALED instance rotation, kept for the light basis.
     *
     * Scale belongs on the position path — a part's translation has to shrink
     * with the model or the parts drift apart — but a light DIRECTION is not a
     * position. Composing the scaled matrix into the light basis applied the
     * scale a second time (the caller already passes it as `intensity_a`), so a
     * half-size instance was lit at a quarter.
     */
    memcpy(spin_unscaled, spin, sizeof(spin_unscaled));

    if (inst->scale != Q2_ONE_12) {
        s32 s = inst->scale;
        int r, c;

        if (s < 0)
            s = 0;
        if (s > 8 * Q2_ONE_12)
            s = 8 * Q2_ONE_12;

        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                spin[r][c] = (s16)(((s32)spin[r][c] * s) >> Q2_FRAC_12);
    }

    matrix_mul(world, view, spin);

    tint[0] = inst->tint[0];
    tint[1] = inst->tint[1];
    tint[2] = inst->tint[2];

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

        /*
         * The light matrix, and why it is composed differently from the
         * rotation matrix.
         *
         * 0x800B1F90 builds both from the part's quaternion and one of the
         * caller's two matrices, and the two callers' matrices differ by
         * exactly the VIEW rotation: positions use view x entity (the draw
         * context's +140), normals use the entity's own matrix alone (its +96).
         * That asymmetry is correct rather than an oversight — the light
         * directions this module is handed are in WORLD space, so rotating them
         * into the camera as well would make a model's shading swing as the
         * player turned.
         *
         * So: rows of the light matrix are the world-space directions carried
         * through the instance matrix and then the part's, which is what
         * 0x800B21B8 does with three MVMVAs against the already-composed
         * matrix.
         */
        if (inst->light) {
            s16 basis[3][3];
            gte_matrix lm;
            u32 j;

            /* Unscaled, and INVERTED below: `basis` carries part-local to
             * world, and what a world-space light direction needs is the trip
             * the other way. See matrix_apply_t. */
            matrix_mul(basis, spin_unscaled, rot);
            memset(&lm, 0, sizeof(lm));

            for (j = 0; j < Q2_LIGHT_ACTIVE_MAX; j++) {
                s32 in[3], out[3];
                int c;

                if (j >= inst->light->active)
                    break;

                for (c = 0; c < 3; c++)
                    in[c] = inst->light->dir[j][c];
                matrix_apply_t(basis, in, out);
                for (c = 0; c < 3; c++)
                    lm.m[j][c] = (s16)out[c];
            }

            gte_set_light_matrix(gte, &lm);
            gte_set_colour_matrix(gte, &inst->light->colour);
            gte_set_back_colour(gte, inst->light->back[0],
                                inst->light->back[1], inst->light->back[2]);
        }

        /* The part's translation is in model space: rotate it by the model's
         * own matrix, offset by where the model stands, and express the result
         * in camera space, because the GTE adds translation after rotating. */
        {
            s32 model_space[3], offset[3];

            /* The same scaled instance matrix the vertices went through, so the
             * offset shrinks with the model. */
            matrix_apply(spin, local, model_space);

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

            /*
             * The normal through the same op the original uses. NCS is NCT's
             * one-vertex form: light matrix, colour matrix, back colour, and
             * NOTHING from the primitive colour — so a fully unlit vertex comes
             * out as the back colour alone and a fully lit one saturates at
             * 255, which on the GPU's modulate path is twice the texel.
             */
            if (inst->light) {
                gte->v[0].x = mv.nx;
                gte->v[0].y = mv.ny;
                gte->v[0].z = mv.nz;
                gte_ncs(gte);

                window[slot].rgb.r = gte->rgb_fifo[2].r;
                window[slot].rgb.g = gte->rgb_fifo[2].g;
                window[slot].rgb.b = gte->rgb_fifo[2].b;
            } else {
                window[slot].rgb.r = tint[0];
                window[slot].rgb.g = tint[1];
                window[slot].rgb.b = tint[2];
            }
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

            /*
             * The linker's own NCLIP pair, before anything is allocated. An
             * ordering table has no depth buffer, so a face that has been
             * emitted is a face that will paint over whatever the sort put
             * behind it — which on a closed mesh is the far side of the model
             * showing through the near side. See q2_model_quad_faces_camera.
             */
            {
                gte_sxy screen[4];
                for (i = 0; i < 4; i++)
                    screen[i] = window[f.v[i]].xy;
                /*
                 * ...unless the model's own block-A batch directory FORCES the
                 * face. `bltz t3` at 0x800B2498 skips the test outright, `t3`
                 * being the batch's `force` word shifted one bit per face. It
                 * is zero in all 13,784 entries of all 1,723 models on this
                 * disc, so this never fires here — it is honoured for the same
                 * reason the loader tolerates chunk names no file emits (#33):
                 * the engine supports it, and a build that uses it should not
                 * be silently mis-drawn.
                 */
                if (!q2_model_batch_forces_draw(m, face_index) &&
                    !q2_model_quad_faces_camera(gte, screen)) {
                    if (stats)
                        stats->faces_rejected_back++;
                    continue;
                }
            }

            if (inst->bucket_override >= 0) {
                /*
                 * AN ABSOLUTE BUCKET, not a depth.
                 *
                 * It used to be a depth, and 0 meant "the frontmost bucket of
                 * whichever window is installed". That was the same bucket the
                 * status bar names for as long as a viewport slice was 51
                 * entries — and when the table was subdivided the two came
                 * apart: the depth path reaches the very top of the subdivided
                 * window (real bucket 423 of viewport 0) while the status bar's
                 * own helper lands on 416, so the gun started drawing over the
                 * HUD.
                 *
                 * Naming the bucket outright is what a packet whose place is
                 * structural is supposed to do (gpu.h), and it lets the caller
                 * express the gun's position RELATIVE to the bar rather than
                 * hoping the two arithmetics agree.
                 */
                prim = psx_ot_add_bucket(ot, (u32)inst->bucket_override);
            } else {
                /*
                 * One link point per face, from its own mean depth. That mean
                 * is also handed over as the sort key, because a bucket is a
                 * hundred-unit slab and a face that shared one with the wall
                 * behind it would otherwise be ordered by which emitter ran
                 * first. See psx_ot_add_depth.
                 */
                for (i = 0; i < 4; i++)
                    otz += window[f.v[i]].z;
                otz /= 4;
                prim = psx_ot_add_depth(
                           ot, (u16)q2_ot_bucket_for_depth(ot, otz,
                                                           cam->sort_range),
                           otz);
            }

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

                /* Whatever the vertex pass left in the window: the light for a
                 * lit instance, the flat tint otherwise. */
                prim->rgb[i] = window[f.v[i]].rgb;
            }

            /*
             * A face's blend selector is `flags >> 5`, and it reaches the GPU
             * through the same two tables the world uses (0x8006DE40):
             *
             *     POLY.code  = codeTable[sel]  | 0x3C
             *     POLY.tpage = tpageTable[page] | blendTable[sel]
             *
             * so selectors 1..4 are B/2+F/2, B+F, B-F and B+F/4 respectively and
             * 0 and 5..7 are opaque. Reading `sel != 0` as "transparent" was
             * right for four of the eight and wrong for three, and gave every
             * transparent face the additive mode regardless of which one it
             * asked for. Unlike the world's, this path never writes the table
             * back.
             */
            {
                u32 sel  = q2_model_face_blend(&f);
                u32 page = q2_model_face_page(&f);

                prim->clut = q2_vram_clut_word(
                    q2_model_face_clut_index(&f, inst->clut4_count_a));
                prim->tpage            = q2_tpage_model(tpage, page, sel);
                prim->semi_transparent = q2_surf_selector_semi(sel);
                prim->textured_blend   = true;

                if (stats && prim->semi_transparent)
                    stats->faces_semi++;
            }

            emitted++;
            if (stats)
                stats->faces_emitted++;
        }
    }

    return emitted;
}
