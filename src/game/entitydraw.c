#include "entitydraw.h"

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
