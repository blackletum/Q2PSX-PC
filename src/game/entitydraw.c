#include "entitydraw.h"

#include "fxtables.h"     /* Q2_FX_ABR_ADD, the blend the bolt draws in */
#include "projectile.h"
#include "weapontables.h"

#include <string.h>

/* The engine's scratch window bound; a model needing more is rejected by the
 * loader, so this is a ceiling rather than a guess. */
#define POSE_MAX 256

bool q2_entity_resolve_model(q2_entity *e, const q2_model_bank *bank)
{
    s32 index;

    if (!e)
        return false;

    /*
     * Already resolved stays resolved, and against the bank it was resolved
     * AGAINST — a map's models are spread over COMMON's CastList and up to
     * eight zones', and an index means nothing against a different bank. A
     * caller with several banks calls this once per bank; the first that has
     * the name wins and the rest are no-ops.
     */
    if (e->model_index >= 0 && e->model_bank)
        return true;

    if (!bank || !e->model[0])
        return false;

    index = q2_model_bank_find(bank, e->model);
    if (index < 0)
        return false;           /* another bank may still have it */

    e->model_index = index;
    e->model_bank  = bank;

    /*
     * The clip length the think wraps the frame against. The engine reads it
     * from the CastList node (`lh model[+2]` at 0x80059404), which is the
     * duration of the clip the entity is playing. The item table names up to two
     * clips in its `extra` list; the first is what a spawned item plays, and an
     * item with no clip named holds its rest pose.
     */
    e->clip_length = 0;
    if (e->clip_count > 0) {
        q2_model m;
        q2_model_anim clip;

        if (q2_model_get(bank, (u32)index, &m) == Q2_OK &&
            q2_model_anim_get(&m, e->clip[0], &clip))
            e->clip_length = clip.frames;
    }

    return true;
}

u32 q2_entity_build_ot(q2_entity_set *set, const q2_entity_draw_ctx *ctx,
                       const q2_camera *cam, psx_ot *ot, gte_state *gte,
                       q2_entity_draw_stats *stats)
{
    u32 i, emitted = 0;

    if (stats)
        memset(stats, 0, sizeof(*stats));

    if (!set || !ctx || !cam || !ot || !gte)
        return 0;

    for (i = 0; i < set->count; i++) {
        q2_entity *e = &set->ent[i];
        q2_model m;
        q2_model_pose pose[POSE_MAX];
        q2_model_instance inst;
        q2_model_draw_stats ms;
        bool posed = false;

        if (!e->in_use)
            continue;

        if (stats)
            stats->considered++;

        if (!q2_entity_visible(e, ctx->player)) {
            if (stats)
                stats->invisible++;
            continue;
        }

        /* A materialising item at zero scale collapses to a point. The engine
         * still draws the degenerate quads; skipping them costs nothing visible
         * and keeps the ordering table for primitives that cover a pixel. */
        if (e->scale <= 0) {
            if (stats)
                stats->invisible++;
            continue;
        }

        if (!q2_entity_resolve_model(e, ctx->bank)) {
            if (stats)
                stats->no_model++;
            continue;
        }

        /* Its OWN bank, which may not be the context's — see the resolve. */
        if (q2_model_get(e->model_bank, (u32)e->model_index, &m) != Q2_OK) {
            if (stats)
                stats->no_model++;
            continue;
        }

        /* Pose it on the frame its think left, when it has an animation and the
         * part count fits the engine's own buffer. */
        if (m.hdr.num_parts <= POSE_MAX && e->clip_count > 0) {
            q2_model_anim clip;
            if (q2_model_anim_get(&m, e->clip[0], &clip)) {
                s32 frame = clip.frames > 0 ? (e->frame % clip.frames) : 0;
                if (q2_model_pose_at(&m, &clip, (u32)frame, pose) == Q2_OK)
                    posed = true;
            }
        }

        q2_light_env env;

        q2_model_instance_init(&inst);
        inst.model         = &m;
        inst.pose          = posed ? pose : NULL;
        inst.origin[0]     = e->origin[0];
        inst.origin[1]     = e->origin[1];
        inst.origin[2]     = e->origin[2];
        inst.yaw           = e->angles[1];
        inst.scale         = e->scale;
        inst.clut4_count_a = ctx->clut4_count_a;
        inst.tpage         = ctx->tpage;

        /*
         * The glow tint. The engine writes zero into the three bytes while the
         * light block is running and 127 when a materialise finishes, so a lit
         * item is DARKENED by its own glow rather than brightened — the dynamic
         * light is what puts the colour back. Reproducing that means an item
         * with no glow keeps the neutral 128 and a glowing one modulates.
         */
        if (e->flags & Q2_ITEM_GLOW) {
            inst.tint[0] = e->glow[0];
            inst.tint[1] = e->glow[1];
            inst.tint[2] = e->glow[2];
        }

        /*
         * The lights, gathered per entity because the engine gathers per entity:
         * 0x8006BBCC sits inside the draw, not before the loop, so two items a
         * few units apart legitimately pick different lights.
         *
         * The glow becomes the BACK COLOUR here rather than a vertex tint,
         * because the shading op is NCT and NCT ignores the primitive colour.
         * That is the same "darkened by its own glow" behaviour the tint path
         * has, arriving through the register the hardware actually uses for it.
         */
        if (ctx->lights) {
            q2_light_set lit;

            q2_light_gather(&lit, ctx->lights, e->origin, ctx->coll_node, false);
            q2_light_env_build(&env, &lit, Q2_LIGHT_ONE, Q2_LIGHT_ONE,
                               (e->flags & Q2_ITEM_GLOW) ? e->glow : NULL);
            inst.light = &env;
        }

        emitted += q2_model_build_ot(&inst, cam, ot, gte, &ms);

        if (stats) {
            stats->drawn++;
            stats->faces_emitted += ms.faces_emitted;
            stats->ot_overflow   += ms.ot_overflow;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* Projectiles in flight — see the note in entitydraw.h                       */
/* ------------------------------------------------------------------------- */
/*
 * The six faces of the eight-corner box, in the corner order the table at
 * 0x8009DB1C stores them:
 *
 *     0 (-10,-10,-50)  1 ( 10,-10,-50)  2 (-10,-10, 50)  3 ( 10,-10, 50)
 *     4 (-10, 10,-50)  5 ( 10, 10,-50)  6 (-10, 10, 50)  7 ( 10, 10, 50)
 *
 * so x picks the low bit, z the second and y the third. Each row below is a
 * perimeter walk, which is the order the rasteriser wants; the table's own
 * order is the Z-order every mesh on this disc is stored in.
 */
static const u8 k_bolt_face[6][4] = {
    { 0, 1, 3, 2 },   /* y = -10 */
    { 4, 6, 7, 5 },   /* y = +10 */
    { 0, 4, 5, 1 },   /* z = -50 */
    { 2, 3, 7, 6 },   /* z = +50 */
    { 0, 2, 6, 4 },   /* x = -10 */
    { 1, 5, 7, 3 }    /* x = +10 */
};

/*
 * A rotation whose +Z axis lies along `dir`.
 *
 * The console builds this with RotMatrix from a Euler triple it derives from
 * the direction (0x8004D834). This port builds the basis directly from the
 * vector, which produces the same box along the same axis without a table of
 * arc-tangents the common layer does not have; the roll about the long axis is
 * arbitrary either way, because the section is square.
 *
 * Returns false for a direction of zero length, which a live projectile never
 * has — its velocity is what makes it live.
 */
static bool bolt_basis(const s32 dir[3], s16 m[3][3])
{
    static const s32 k_up[3] = { 0, -4096, 0 };  /* -Y is up in world space */
    s64 len2;
    s32 fwd[3], right[3], up[3];
    int k;

    len2 = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] + (s64)dir[2] * dir[2];
    if (len2 <= 0)
        return false;

    {
        s64 lo = 0, hi = 0x7FFFFFFF, len = 0;
        while (lo <= hi) {
            s64 mid = lo + (hi - lo) / 2;
            if (mid * mid <= len2) { len = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (len <= 0)
            return false;
        for (k = 0; k < 3; k++)
            fwd[k] = (s32)(((s64)dir[k] * 4096) / len);
    }

    /* right = up x fwd, and a fallback axis for a bolt fired straight up or
     * straight down, where the cross product degenerates. */
    right[0] = (s32)(((s64)k_up[1] * fwd[2] - (s64)k_up[2] * fwd[1]) >> 12);
    right[1] = (s32)(((s64)k_up[2] * fwd[0] - (s64)k_up[0] * fwd[2]) >> 12);
    right[2] = (s32)(((s64)k_up[0] * fwd[1] - (s64)k_up[1] * fwd[0]) >> 12);

    if (right[0] == 0 && right[1] == 0 && right[2] == 0) {
        right[0] = 4096;
        right[1] = 0;
        right[2] = 0;
    } else {
        s64 r2 = (s64)right[0] * right[0] + (s64)right[1] * right[1] +
                 (s64)right[2] * right[2];
        s64 lo = 0, hi = 0x7FFFFFFF, len = 0;
        while (lo <= hi) {
            s64 mid = lo + (hi - lo) / 2;
            if (mid * mid <= r2) { len = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (len <= 0)
            return false;
        for (k = 0; k < 3; k++)
            right[k] = (s32)(((s64)right[k] * 4096) / len);
    }

    up[0] = (s32)(((s64)fwd[1] * right[2] - (s64)fwd[2] * right[1]) >> 12);
    up[1] = (s32)(((s64)fwd[2] * right[0] - (s64)fwd[0] * right[2]) >> 12);
    up[2] = (s32)(((s64)fwd[0] * right[1] - (s64)fwd[1] * right[0]) >> 12);

    /* Columns are the local axes, so m * (x,y,z) = x*right + y*up + z*fwd. */
    for (k = 0; k < 3; k++) {
        m[k][0] = (s16)right[k];
        m[k][1] = (s16)up[k];
        m[k][2] = (s16)fwd[k];
    }
    return true;
}

u32 q2_projectiles_build_ot(const struct q2_projectiles *list,
                            const q2_camera *cam, psx_ot *ot, gte_state *gte)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    u32 i, emitted = 0;

    if (!list || !cam || !ot || !gte)
        return 0;

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        const q2_projectile *p = &list->p[i];
        s16     m[3][3];
        gte_sxy xy[8];
        u16     z[8];
        bool    ok[8];
        psx_rgb tint;
        u32     f;
        int     v;

        if (!p->in_use)
            continue;
        if (!bolt_basis(p->vel, m))
            continue;

        /* The kind's own glow, from the preset the dynamic light reads out of
         * 0x800AE954 — so the body and the light it casts agree. */
        tint.pad = 0;
        if (p->kind == Q2_PROJ_BFG) {
            tint.r = Q2_PROJ_BFG_LIGHT_R;
            tint.g = Q2_PROJ_BFG_LIGHT_G;
            tint.b = Q2_PROJ_BFG_LIGHT_B;
        } else {
            tint.r = Q2_PROJ_LIGHT_R;
            tint.g = Q2_PROJ_LIGHT_G;
            tint.b = Q2_PROJ_LIGHT_B;
        }

        for (v = 0; v < 8; v++) {
            s32 world[3];
            int k;

            for (k = 0; k < 3; k++) {
                s64 sum = (s64)m[k][0] * wt->bolt_shape[v][0] +
                          (s64)m[k][1] * wt->bolt_shape[v][1] +
                          (s64)m[k][2] * wt->bolt_shape[v][2];
                world[k] = p->pos[k] + (s32)(sum >> 12) - cam->pos[k];
            }

            ok[v] = gte_project_point(gte, world[0], world[1], world[2],
                                      &xy[v], &z[v]);
        }

        for (f = 0; f < 6; f++) {
            const u8 *idx = k_bolt_face[f];
            psx_prim *prim;
            u32 depth = 0;
            bool good = true;
            int c;

            for (c = 0; c < 4; c++) {
                if (!ok[idx[c]]) { good = false; break; }
                depth += z[idx[c]];
            }
            if (!good)
                continue;

            prim = psx_ot_add(ot, (u16)q2_ot_bucket_for_depth(ot, depth / 4u,
                                                              cam->far_z));
            if (!prim)
                break;

            /* Additive and flat: a bolt is a light source, and one that
             * occluded the wall behind it would read as a solid brick. */
            prim->kind             = PSX_PRIM_F4;
            prim->semi_transparent = true;
            prim->tpage            = Q2_FX_ABR_ADD;
            prim->rgb[0]           = tint;

            for (c = 0; c < 4; c++)
                prim->xy[c] = *(const psx_xy *)&xy[idx[c]];

            emitted++;
        }
    }

    return emitted;
}
