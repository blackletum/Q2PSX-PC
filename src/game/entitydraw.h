/*
 * entitydraw.h — putting the entity set into the ordering table.
 *
 * modeldraw.c draws ONE model instance. This walks a whole entity set, resolves
 * each entity's model out of the map's CastList, applies the state its think
 * left behind — the spin, the materialise scale, the glow tint — and appends
 * the lot to the same ordering table the world was built into.
 *
 * That last part is why this is a separate module and not something the client
 * does inline: the primitives have to land in the SAME table as the world
 * geometry, or the sort cannot interleave an item with the crate it sits behind.
 * The original has exactly one table per frame and so does this.
 *
 * ---------------------------------------------------------------------------
 * What is faithful here and what is not
 * ---------------------------------------------------------------------------
 * FAITHFUL: the scale reaches the GPU through the instance matrix, as it does in
 * the original (0x8006B298 scales the rows of entity+0x2C0); the yaw is the
 * entity's own +0xE8 on the 4096-step circle; the origin is +0xA4, which the
 * spawner already biased by 286 and the model's own offset.
 *
 * NOT YET: the engine also draws a translucent shadow quad and the sparkle
 * particles the materialise ramp emits. Neither is reconstructed, and neither is
 * faked — an item that has not finished materialising simply appears at its
 * current size with no sparkles around it.
 */
#ifndef Q2PSX_ENTITYDRAW_H
#define Q2PSX_ENTITYDRAW_H

#include "entity.h"
#include "gpu.h"
#include "gte.h"
#include "lighting.h"
#include "model.h"
#include "modeldraw.h"
#include "q2psx.h"
#include "world.h"

typedef struct q2_entity_draw_stats {
    u32 considered;
    u32 drawn;
    u32 no_model;      /* the bank does not carry the name the table gave    */
    u32 invisible;     /* hidden, collected, or scaled to nothing            */
    u32 faces_emitted;
    u32 ot_overflow;
} q2_entity_draw_stats;

typedef struct q2_entity_draw_ctx {
    const q2_model_bank *bank;      /* the map's CastList                    */
    u32                  clut4_count_a;  /* model palettes start after these */

    /*
     * The world's texture-page table, or NULL for a private one at ABR 0.
     *
     * An entity standing in a world frame has to share it: the world's opaque
     * path PROMOTES a page from ABR 0 to ABR 1 and writes the result back
     * (0x80068320), so an item on a page the world has already drawn on inherits
     * that promotion. A private table would blend it at the pre-promotion mode
     * — see the note on q2_model_instance.tpage in modeldraw.h. NULL is right
     * for an offline caller that draws no world.
     */
    const q2_tpage_table *tpage;

    /* Which player's view this is. An item already collected by that player is
     * invisible to them and to nobody else, which is the +0x118 per-player
     * block's whole purpose. */
    u32                  player;

    /*
     * The lights of the zone the entities stand in, or NULL to draw everything
     * at its flat tint as this module did before lighting existed.
     *
     * When it is present each entity gets its own gather — the engine does one
     * per entity per frame (0x8006BBCC, called from the draw itself), not one
     * per frame — and shades through the GTE. `coll_node` is the SecondaryCol
     * node to gather static lights from; the engine takes it from the entity's
     * own +0xA2, which this port does not track per item yet, so a caller that
     * knows the player's node can pass it and every entity in the zone will use
     * it. Negative means the fallback light, which is what an item away from
     * the player's own cell would otherwise get for free.
     */
    const q2_light_world *lights;
    s32                   coll_node;
} q2_entity_draw_ctx;

/*
 * Draw every visible entity. The ordering table is NOT cleared and the GTE's
 * projection is NOT reconfigured, so this composes after q2_world_build_ot.
 *
 * Returns the number of primitives emitted.
 */
u32 q2_entity_build_ot(q2_entity_set *set, const q2_entity_draw_ctx *ctx,
                       const q2_camera *cam, psx_ot *ot, gte_state *gte,
                       q2_entity_draw_stats *stats);

/*
 * Resolve one entity's model against the bank and cache the result, and fill in
 * the clip length its think wraps the frame against.
 *
 * Called by q2_entity_build_ot, and exposed because a caller that has the bank
 * open at spawn time should do it then: the engine resolves at spawn
 * (0x80058850) and an item whose name is not in the map's bank never spawns at
 * all. Returns false when the bank does not carry the name.
 */
bool q2_entity_resolve_model(q2_entity *e, const q2_model_bank *bank);

#endif /* Q2PSX_ENTITYDRAW_H */
