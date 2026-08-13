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
#include "model.h"
#include "q2psx.h"
#include "world.h"

typedef struct q2_model_draw_stats {
    u32 parts;
    u32 faces_total;
    u32 faces_emitted;
    u32 faces_rejected_near;   /* GTE divide overflow: at or behind the plane */
    u32 faces_rejected_bad;    /* index outside the scratch window            */
    u32 ot_overflow;
} q2_model_draw_stats;

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
    u32                  clut4_count_a;
} q2_model_instance;

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
