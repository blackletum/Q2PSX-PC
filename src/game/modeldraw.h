/*
 * modeldraw.h — turning a CastList model into PlayStation GPU primitives.
 *
 * The world renderer in world.c draws brush geometry, which is already in world
 * space and needs no articulation. A model is different in three ways, and all
 * three come straight from how the original does it (0x800B1F90):
 *
 *   - Vertices are PART-LOCAL. Each part carries its own rotation and
 *     translation from the animation, and parts do not inherit from each other,
 *     so the transform is v' = R(q) * v + t applied once per part.
 *   - Faces do not index the vertex array. They index a shared per-model
 *     SCRATCH WINDOW into which parts write their transformed vertices at
 *     vert_base — which is why a face can legitimately reference a vertex a
 *     different part put there.
 *   - Texturing is the world's rule with one difference: a face's CLUT index is
 *     offset by the map's clut4_count_a, because model palettes live in the
 *     second section of the CLUT array.
 *
 * This module deliberately does the transform in the same order as the original
 * rather than the order that would be natural on a PC: per part, matrix first,
 * translation into the GTE's translation registers, vertices through RTPT. The
 * point of the project is that the wobble and the sort order come out of the
 * same arithmetic, and that only holds if the arithmetic is arranged the same
 * way.
 */
#ifndef Q2PSX_MODELDRAW_H
#define Q2PSX_MODELDRAW_H

#include "gpu.h"
#include "gte.h"
#include "lighting.h"
#include "model.h"
#include "q2psx.h"
#include "world.h"

typedef struct q2_model_draw_stats {
    u32 parts;
    u32 faces_total;
    u32 faces_emitted;
    u32 faces_rejected_near;   /* GTE divide overflow: at or behind the plane */
    u32 faces_rejected_bad;    /* index outside the scratch window            */
    u32 faces_rejected_back;   /* both NCLIP halves faced away                */
    u32 ot_overflow;
    u32 faces_semi;            /* blend selector 1..4: drawn with ABE         */
} q2_model_draw_stats;

/*
 * BACKFACE REJECTION for a model face, and why its sign is the WORLD'S
 * INVERTED.
 *
 * The model linker at 0x800B2410 loads the projected corners into the GTE in
 * the same shape the world linker does, and runs the same pair of NCLIPs:
 *
 *     SXY0,SXY1,SXY2 = v0,v1,v3 ; NCLIP ; MAC0 <= 0 -> draw   (0x800B24A0)
 *     SXY0           = v2       ; NCLIP ; MAC0 >  0 -> draw   (0x800B24D0)
 *                                        otherwise   -> drop
 *
 * Compare `q2_world_quad_faces_camera`, whose two tests are `MAC0 > 0 -> draw`
 * and `MAC0 < 0 -> draw` (0x800AF8A8 / 0x800AF8C8). The comparisons are exactly
 * exchanged, so MODEL QUADS ARE WOUND THE OPPOSITE WAY ROUND FROM WORLD QUADS.
 * That is not an inference from how it looks: it is `bgtz` in one linker and
 * `blez` in the other, on the identical register.
 *
 * It is also measurable, which is what settles it independently of the sign
 * convention this port's GTE happens to use. Averaging the projected depth of
 * each group over the Soldier: the faces this rule KEEPS mean 1174 and the ones
 * it drops mean 1233, and the same split holds on all fourteen of its parts.
 * The kept set is the nearer one, which is what "front facing" means.
 *
 * The corner order is the file's — the perimeter — exactly as the world's is;
 * the linker's 2/3 swap is the file-to-hardware Z-order conversion and applies
 * to the UVs in the same breath (0x8006A3E4 stores face uv3 into POLY uv2).
 *
 * RESIDUE. The linker has an escape: `bltz t3` at 0x800B2498 skips the test
 * entirely, and `t3` is a 32-bit mask shifted left once per face, so it is one
 * FORCE-DRAW BIT PER FACE, MSB first, over the same 32-face batches the
 * attribute emitter walks in. Nothing here builds that mask, so this port culls
 * unconditionally; what sets a bit is unread. See openquestions #10b.
 *
 * Clobbers the GTE's SXY registers and MAC0.
 */
Q2PSX_INLINE bool q2_model_quad_faces_camera(gte_state *g, const gte_sxy screen[4])
{
    g->sxy[0] = screen[0];
    g->sxy[1] = screen[1];
    g->sxy[2] = screen[3];
    gte_nclip(g);
    if (g->mac0 <= 0)
        return true;

    g->sxy[0] = screen[2];
    gte_nclip(g);
    return g->mac0 > 0;
}

/*
 * Where and how a model instance sits in the world.
 *
 * `pose` holds one entry per part, or NULL to draw the model unposed — which is
 * what the 1,324 static models want, and what an articulated model falls back
 * to when its animation has not been selected yet.
 */
typedef struct q2_model_instance {
    const q2_model      *model;
    const q2_model_pose *pose;        /* num_parts entries, or NULL */
    s32                  origin[3];
    s32                  yaw;         /* 4096-step circle */

    /*
     * The other two angles, for the one thing on this disc that needs them.
     *
     * Nothing placed in the world tilts: the rotation integrator writes exactly
     * one axis and the zone draw's own RotMatrix call is effectively yaw-only,
     * which is why `yaw` was enough for every caller until now. The VIEW WEAPON
     * is the exception — it is rotated by `RotMatrix(view angles + the
     * animation's own)` at 0x8004F464, all three of them, with pitch negated at
     * the sum. Leaving these zero reproduces the previous behaviour exactly.
     */
    s32                  pitch;
    s32                  roll;

    /*
     * An explicit 3x3 rotation, 1.3.12, overriding the three angles above.
     *
     * The view weapon needs it and nothing else does. Its model is authored in
     * VIEW space — `Blaster G` spans z 0..482 with the grip at the origin — so
     * the matrix it wants is the camera's own inverse composed with the clip's
     * rotation, and that is not expressible as three Euler angles the camera
     * will then re-rotate. NULL keeps the angle path exactly as it was.
     */
    const s16          (*rot)[3];
    u32                  clut4_count_a;

    /*
     * Uniform scale, 1.0.12 — entity+0xFC. The engine applies it by scaling the
     * rows of the entity's own rotation matrix before handing them to the GTE
     * (0x8006B298: `ScaleMatrix(m, (a * b) >> 11)` three times), so it is a
     * property of the transform rather than of the vertices, and it is how an
     * item grows out of nothing when it materialises.
     *
     * Q2_ONE_12 is unscaled. Use q2_model_instance_init to get that default.
     */
    s32                  scale;

    /*
     * Per-vertex colour, entity+0x2AC. The world and model renderers both drive
     * the GPU's modulate path, and 128 is neutral — which is what every caller
     * used before items needed a tint, and what the engine writes at spawn
     * (0x80058944 stores the three bytes of "000", i.e. 0x30, and the
     * materialise ramp resets them to 127 at full size).
     */
    u8                   tint[3];

    /*
     * The lights reaching this instance, or NULL to draw it at `tint`.
     *
     * When it is present the vertices shade through the GTE's NCT path exactly
     * as the original's do: the three directions become the light matrix (
     * composed with the instance's own matrix and the part's, per part), the
     * three colours become the colour matrix, and the entity's glow becomes the
     * back colour. NCT does NOT multiply by the primitive's colour, so `tint`
     * is unused in that case — the light IS the colour, and the ambient reaches
     * the vertex through the back colour instead.
     *
     * Build one with q2_light_gather + q2_light_env_build.
     */
    const struct q2_light_env *light;

    /*
     * The engine's texture-page table, or NULL for a private one at ABR 0.
     *
     * Models and the world share it (0x800B36D8 is indexed by both emitters),
     * and the world's opaque path mutates it, so a model drawn into a world
     * frame should be handed the same q2_world_render's table. A model face's
     * own blend selector is OR-ed on top without writing back (0x8006DE50).
     */
    const q2_tpage_table *tpage;

    /*
     * ONE BUCKET FOR THE WHOLE MODEL, or < 0 to sort each face on its own
     * depth.
     *
     * The original never sorts a model's faces against each other. Its vertex
     * loop (0x800B23A0) stores SXY and RGB and no SZ at all, so no per-face
     * depth exists to sort by; the face loop appends packet pointers to a flat
     * array at 0x800DDDCC, and the tail chains every one of them into a SINGLE
     * ordering-table link point taken from the entity's own projected origin
     * (0x8006BF34 stores SZ3 of the origin at record+84). A model is one thing
     * in the table, not N.
     *
     * This port buckets per face, which for most models is invisible — they are
     * small and land in one bucket anyway — but the VIEW WEAPON spans a large
     * part of the near range, so its 62 faces landed in three different buckets
     * and any wall polygon whose own depth fell between them was painted over
     * the gun. That is the "view weapon clips into world geometry" report:
     * measured at 9-17% of the weapon's pixels eaten, concentrated on the
     * muzzle.
     */
    s32                  bucket_override;
} q2_model_instance;

/* Zero the instance and set the fields whose neutral value is not zero. */
void q2_model_instance_init(q2_model_instance *inst);

/*
 * Transform one model instance and append its primitives to `ot`.
 *
 * The ordering table is NOT cleared and the GTE's projection is NOT
 * reconfigured, so a caller can build the world first and then drop models into
 * the same table — which is the only way the sort can interleave them with the
 * geometry they stand in front of.
 */
u32 q2_model_build_ot(const q2_model_instance *inst,
                      const q2_camera *cam,
                      psx_ot *ot,
                      gte_state *gte,
                      q2_model_draw_stats *stats);

#endif /* Q2PSX_MODELDRAW_H */
