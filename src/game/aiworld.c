#include "aiworld.h"

#include "combat.h"   /* 0x800544EC's narrow phase, shared */

#include "worldscale.h"

#include <stddef.h>
#include <string.h>

#include "aimove.h"

/*
 * How much of the requested move actually happened, as a 1.12 fraction.
 *
 * The collision walk reports where it stopped, not how far it got, so the
 * fraction is recovered from the distance travelled along the dominant axis.
 * That is the same axis the walk itself clips on, so the answer agrees with
 * the original's to within the rounding of one divide — and the AI only ever
 * compares it against 4096 or against another fraction from the same source.
 */
static s32 travelled_fraction(const s32 start[3], const s32 end[3],
                              const s32 hit[3])
{
    s32 axis = 0;
    s32 best = 0;
    int i;
    s64 want, got;

    for (i = 0; i < 3; i++) {
        s32 d = end[i] - start[i];
        if (d < 0)
            d = -d;
        if (d > best) {
            best = d;
            axis = i;
        }
    }

    if (best == 0)
        return Q2_TRACE_ONE;

    want = (s64)end[axis] - start[axis];
    got  = (s64)hit[axis] - start[axis];

    if (want < 0) { want = -want; got = -got; }
    if (got <= 0)
        return 0;
    if (got >= want)
        return Q2_TRACE_ONE;

    return (s32)((got * Q2_TRACE_ONE) / want);
}

/* Is the caller's box a real one? 0x8005BDA8..0x8005BE30 compares all six
 * components against the shared zero vector at 0x8009FBE4; anything non-zero
 * takes the eroded hull. A NULL pointer is the same as a zero box. */
static bool box_is_real(const s16 mins[3], const s16 maxs[3])
{
    int i;

    if (!mins || !maxs)
        return false;
    for (i = 0; i < 3; i++) {
        if (mins[i] || maxs[i])
            return true;
    }
    return false;
}

/*
 * 0x800544EC, for a MOVE rather than for a shot — the same function, run
 * through combat.c's transcription (q2_combat_trace_cylinders), which the
 * projectile-contact fixture checks against the disc's own code.
 *
 * This used to run an older cylinder approximation of its own, with exact roots
 * where the console takes SquareRoot0's and its fraction divide, and a filter
 * of its own as well: it dropped every body that was not `solid`, so a corpse
 * never stopped a walker unless the caller's mask carried 0x04000000. Neither
 * is the console's. gitrace (0x8005BD3C) tests the mask for 0x02000000 alone
 * (0x8005BF18) and 0x800544EC skips the ignore entity alone (0x800545E8);
 * what decides whether a body is in the query at all is whether its handler
 * put it on the frame's actor list, which `listed` records. `solid` is the
 * separation pass's gate (0x800513F0) and stays there.
 *
 * The mover is a POINT here, and that is the console's: 0x800544EC reads the
 * candidate's radius and slab and nothing of the caller's own box. It works
 * because the radius it reads is 286, the player's own half-extent, so a
 * creature that stops 286 from a player's centre stops at their skin.
 */
typedef struct body_trace_list {
    const q2_move_body *list;
    s32                 ignore_id;
} body_trace_list;

static bool body_trace_get(void *user, u32 index, s32 origin[3],
                           s32 *radius, s16 *height)
{
    const body_trace_list *l = (const body_trace_list *)user;
    const q2_move_body *o = &l->list[index];

    if (!o->listed || o->id == l->ignore_id)
        return false;
    memcpy(origin, o->pos, sizeof(o->pos));
    *radius = o->sweep_radius;
    *height = o->height;
    return true;
}

static void bound_trace(void *user, const s32 start[3], const s16 mins[3],
                        const s16 maxs[3], const s32 end[3],
                        const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    q2_collision *hull;
    s32 pos[3];
    s32 node = -1;
    bool walked;

    /*
     * `mask` is read for the one bit that matters — 0x02000000, which
     * SV_movestep sets (0x02020003 at 0x8005FE7C and 0x80060014) and which is
     * what turns the entity clip on. Behind it the console runs TWO passes:
     * the entity boxes (the doors and lifts, 0x80053974) and then the ACTOR
     * list (0x800544EC), and `ignore` belongs to the second. It used to be
     * dropped here with a note saying "CREATURES AND BODIES still do not clip:
     * the port has no per-creature box list to hand this"; it has one now, and
     * the second pass is at the end of this function.
     */

    s32 hint = q2_ai_trace_hint;

    q2_ai_trace_hint = -1;              /* consumed, whatever happens below */
    memset(out, 0, sizeof(*out));

    if (b)
        b->stats.traces++;

    if (!b || !b->coll) {
        out->fraction  = Q2_TRACE_ONE;
        out->endpos[0] = end[0];
        out->endpos[1] = end[1];
        out->endpos[2] = end[2];
        return;
    }

    /*
     * THE HULL SELECT — 0x8005BD3C. A real box goes to SecondaryCol, which is
     * already eroded by the body's half-extent, so the swept point IS the box.
     * A degenerate one goes to PrimaryColl, the un-eroded query hull.
     */
    hull = b->coll;
    if (box_is_real(mins, maxs) && b->move_hull) {
        hull = b->move_hull;
        b->stats.trace_boxed++;
    } else {
        /* The point arm refines the hint against the actor's position
         * (0x80054F44) before its walk; the port finds the cell instead. */
        hint = -1;
    }

    /*
     * `q2_coll_move` returns false when the move was STOPPED, not when it could
     * not begin — collision.h §0x80044C44 — and a stopped move is the normal,
     * useful answer for a walker's step trace, which exists precisely to be
     * stopped by the floor. `out_pos` is filled in either way; `out_node` is -1
     * only when the walk never found a cell at all, which is the real "started
     * outside the hull".
     *
     * Reading the false as start-solid is what made every creature in the game
     * stand still: `SV_movestep` bails on `allsolid` before it ever looks at
     * the fraction, so a creature standing on a floor was told it was buried in
     * one. Measured on BASE1, 1052 of 1052 traces took that arm.
     */
    walked = q2_coll_move(hull, start, end, hint, pos, &node);
    /* SecondaryCol's current node is what the next trace starts from. */
    if (hull == b->move_hull)
        q2_ai_trace_end = hull->current_node;
    if (!walked && node < 0) {
        /*
         * The eroded hull could not place the START. The original does not
         * retry — 0x8005BD3C hard-selects — but a creature whose body is
         * bigger than the erosion assumes (JAIL2's Gladiator misses the
         * SecondaryCol floor by two units) would otherwise freeze where it
         * previously walked. Fall back to the un-eroded hull and COUNT it, so
         * the placement failures stay visible instead of being papered over.
         */
        if (hull != b->coll &&
            (q2_coll_move(b->coll, start, end, -1, pos, &node) || node >= 0)) {
        } else {
            b->stats.trace_unplaced++;
            out->fraction   = 0;
            out->startsolid = true;
            out->allsolid   = true;
            out->endpos[0]  = start[0];
            out->endpos[1]  = start[1];
            out->endpos[2]  = start[2];
            return;
        }
    }

    /*
     * AND THE DOORS — the entity arm of the same sweep, which IS 0x80053C58:
     * 0x8005BE94 calls it with (start, where the hull left the move, the same,
     * 0x810), the very sweep the actor physics runs (q2_move_sweep_world). So
     * the boxes are inflated by 286 on every face (0x80053D8C..0x80053E5C),
     * not by the creature's own size, and the second walk's trigger VOLUMES
     * block too when their mask meets 0x810 — the monster clips a player
     * walks through. A segment clip that took only entity boxes, grown by the
     * creature's own extents, let the BASE3 attract run's ledge Infantry walk
     * through the clip the console stops it on at frame 31 (fraction 381
     * against the port's clear run), and from there every creature drew
     * another's numbers.
     *
     * The mask gate is the console's: a query that does not ask for entities
     * does not get them.
     */
    if (b->ents && (mask & Q2_MASK_ENTITY_BIT)) {
        q2_move_world mw = *b->ents;
        q2_move_contact mc;

        mw.half_extent = Q2_SWEEP_HALF_EXTENT;
        mw.mask        = 0x810u;
        if (!q2_move_sweep_world(&mw, start, pos, &mc)) {
            pos[0] = mc.pos[0];
            pos[1] = mc.pos[1];
            pos[2] = mc.pos[2];
            b->stats.trace_blocked_ent++;
        }
    }

    /*
     * AND THE OTHER BODIES — 0x8005BF4C, behind the same mask bit.
     *
     * Clipped from the START to wherever the hull and the doors left the move,
     * so the nearest of the three wins with no fractions to compare, exactly as
     * the door arm above does.
     *
     * `ignore` is honoured here and nowhere else in this function, which is why
     * it stopped being `(void)ignore`: 0x8005BF30 puts the caller's own entity
     * in a2 and 0x800545E8 skips it by pointer. Without that a creature's first
     * step would end on its own cylinder and no creature would ever move again.
     *
     * The cylinder's radius is the candidate's entity+0x94 — 286 for a player
     * — and NOT the +0x90 the separation pass reads. They are different fields
     * and the two routines read different ones: 0x800545F4 is `lh a0, 64(a2)`
     * with a2 = entity+84, which is +0x94, while 0x80051428 is `lh v1,
     * 144(a1)`, which is +0x90. 286 is the player's own half-extent, so a
     * creature that stops 286 from a player's centre stops at their skin.
     */
    if (b->bodies && b->bodies->list && (mask & Q2_MASK_ENTITY_BIT)) {
        body_trace_list l;
        s32 hit[3], idx;
        u32 i;

        l.list = b->bodies->list;
        l.ignore_id = -1;
        if (ignore && ignore == b->player_owner) l.ignore_id = -2;
        else if (ignore && b->body_owner_base && b->body_owner_count) {
            for (i = 0; i < b->body_owner_count; i++)
                if (ignore == &b->body_owner_base[i]) { l.ignore_id = (s32)i; break; }
        }

        /*
         * 0x8005BF4C hands 0x800544EC the frame's actor list and the caller's
         * own entity, and that is the whole filter: 0x800545E8 skips the
         * ignore entity and nothing else, so a corpse still on the list —
         * PrimaryQuakeDeadBody appends one each frame until its dissolve gate
         * answers (0x8007F728..0x8007F740) — stops a walker with its corpse
         * cylinder exactly as it stops a round. Run through the shot path's
         * own transcription, so the two cannot drift apart again.
         */
        idx = q2_combat_trace_cylinders(start, pos, body_trace_get, &l,
                                        b->bodies->count, hit);
        if (idx >= 0) {
            const q2_move_body *o = &b->bodies->list[idx];

            pos[0] = hit[0];
            pos[1] = hit[1];
            pos[2] = hit[2];
            out->ent = o->id == -2 ? b->player_owner :
                       (o->id >= 0 && (u32)o->id < b->body_owner_count)
                         ? &b->body_owner_base[o->id] : NULL;
            b->stats.trace_blocked_body++;
        }
    }

    out->endpos[0] = pos[0];
    out->endpos[1] = pos[1];
    out->endpos[2] = pos[2];
    out->fraction  = travelled_fraction(start, end, pos);
    if (out->fraction >= Q2_TRACE_ONE)
        b->stats.trace_clear++;
}

static bool bound_los(void *user, const s32 a[3], const s32 b3[3])
{
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 pos[3];
    s32 node = -1;

    if (!b || !b->coll)
        return true;

    b->stats.los_calls++;

    if (!q2_coll_move(b->coll, a, b3, -1, pos, &node)) {
        b->stats.los_blocked++;
        return false;
    }

    /* Sight is all-or-nothing in the original: `visible` tests the fraction
     * against 1.0 and takes anything less as blocked. */
    if (pos[0] != b3[0] || pos[1] != b3[1] || pos[2] != b3[2]) {
        b->stats.los_blocked++;
        return false;
    }

    /*
     * AND THE DOORS. `visible` (0x8005B950) runs its sweep through the entity
     * list too, and a closed door is an entity — so a creature that could see
     * you through one was not being generous, it was asking the hull a
     * question the hull cannot answer.
     *
     * This is the line that fixes "creatures attack through doors": every
     * creature's fire hook gates each shot on `q2_visible`, so a sight line
     * that stops at the door stops the shooting with it. A POINT, not a box:
     * sight is the query the original hands a zero box, which is what sends it
     * to PrimaryColl in the first place.
     */
    if (b->ents) {
        q2_move_seg_hit mh;

        if (q2_move_clip_segment(b->ents, a, b3, NULL, &mh)) {
            b->stats.los_blocked++;
            b->stats.los_blocked_ent++;
            return false;
        }
    }

    return true;
}

/*
 * M_CheckBottom — 0x8005FB24, and it is FOUR corners, not one centre.
 *
 * 0x8005FB44..0x8005FC00 writes four stack triples,
 *
 *     (x - 143, y + 502, z - 143)   (x - 143, y + 502, z + 143)
 *     (x + 143, y + 502, z + 143)   (x + 143, y + 502, z - 143)
 *
 * and sweeps each of them FROM THE ENTITY ORIGIN — a0 = 0x800C8E90,
 * PrimaryColl, a1 = the entity, loop bound `slti v0, s2, 4` at 0x8005FC48. The
 * sweeps are DIAGONAL: the start stays at the centre and only the end moves to
 * the corner, so a wall between the two stops the sweep and counts as ground.
 * Four vertical drops at the corners would miss that, which is why the corner
 * offsets go on the END only.
 *
 * The two constants: 143 is half the body's 286 half-extent, and 502 is
 * Q2_EYE_BASE + Q2_STEPSIZE — one step below the FEET, measured from the
 * origin. That 502 is independent proof of the origin convention, since it
 * only lands on the floor if the origin is 286 above the feet.
 *
 * A corner whose sweep COMPLETES is not yet a ledge. 0x8005FC28 sends it to
 * 0x8005FC34 `jal 0x80053974` (a2 = 0 in the delay slot), the clip against the
 * 48-slot entity-box table at 0x800CAE10, which returns 0 when a box stops the
 * segment; 0x8005FC3C `bne v0, zero` then returns 0 — a ledge — only when no
 * box did. The port's table is `b->ents`, the sim's `move_world`, and its
 * entity half (q2_move_clip_segment skips the trigger volumes) holds movers,
 * GLASS, SHOOTTHEN, ROTHATCH, OBJDRAWOFF and intact FXGROUP parts. The last
 * three are registered by scenesolid.c, including health-zero explosives
 * which are solid without having damage callbacks. Freed boxes cease to
 * support a creature in the same call that destroys or hides their object.
 * Creatures, corpses and items use the separate body list, not this Scene-box
 * table, so this probe does not treat another creature as ground.
 */
#define AI_BOTTOM_CORNER  (Q2_EYE_BASE / 2)             /* 143 */

static bool bound_bottom(void *user, const q2_monster *m)
{
    static const s32 k_corner[4][2] = {
        { -AI_BOTTOM_CORNER, -AI_BOTTOM_CORNER },
        { -AI_BOTTOM_CORNER,  AI_BOTTOM_CORNER },
        {  AI_BOTTOM_CORNER,  AI_BOTTOM_CORNER },
        {  AI_BOTTOM_CORNER, -AI_BOTTOM_CORNER }
    };
    q2_ai_world_bind *b = (q2_ai_world_bind *)user;
    s32 start[3];
    int i;

    if (!b || !b->coll || !m)
        return true;

    b->stats.bottom_calls++;

    start[0] = m->pos[0];
    start[1] = m->pos[1];
    start[2] = m->pos[2];

    for (i = 0; i < 4; i++) {
        s32 end[3], pos[3];
        s32 node = -1;

        end[0] = start[0] + k_corner[i][0];
        /* Y points down, so probing for ground adds. From the ORIGIN, which is
         * why the reach is a step below the feet rather than a step. */
        end[1] = start[1] + Q2_EYE_BASE + b->bottom_reach;
        end[2] = start[2] + k_corner[i][1];

        /*
         * A probe that is STOPPED has found ground; one that runs the whole way
         * has found a drop. So the return of `q2_coll_move` is the answer,
         * inverted — the previous reading had it the right way round only by
         * accident, because it also treated "stopped" as failure and so
         * reported a drop under every creature standing on a floor.
         */
        if (!q2_coll_move(b->coll, start, end, -1, pos, &node)) {
            if (node < 0) {
                /* Never found a cell: the creature is outside the hull. */
                b->stats.bottom_fail++;
                return false;
            }
            continue;                   /* stopped short: this corner has ground */
        }

        /*
         * THE CORNER THAT REACHED THE END.
         *
         * 0x8005FB24 does not fail on it either: it hands the completed sweep
         * to 0x80053974, the ENTITY clip, and only that finding nothing makes
         * it a ledge. The entity list here is the sim's entity boxes — mover
         * parts, hatches and intact breakables, see above — so a creature standing
         * on a LIFT, in a doorway whose floor is the door itself, or on a pane
         * is standing on something instead of being told it is at the edge of
         * the world and refusing to walk.
         */
        if (b->ents) {
            q2_move_seg_hit mh;

            if (q2_move_clip_segment(b->ents, start, end, NULL, &mh)) {
                b->stats.bottom_on_ent++;
                continue;
            }
        }

        b->stats.bottom_fail++;
        return false;                   /* ran the whole way: a drop */
    }

    return true;
}

void q2_ai_world_bind_init(q2_ai_world_bind *bind, q2_collision *coll,
                           q2_collision *move_hull)
{
    if (!bind)
        return;

    memset(bind, 0, sizeof(*bind));

    bind->coll         = coll;
    bind->move_hull    = move_hull;
    bind->bottom_reach = Q2_STEPSIZE;

    bind->world.user          = bind;
    bind->world.trace         = bound_trace;
    bind->world.line_of_sight = bound_los;
    bind->world.check_bottom  = bound_bottom;
}

void q2_ai_world_bind_entities(q2_ai_world_bind *bind,
                               const q2_move_world *ents)
{
    if (!bind)
        return;

    /*
     * Stored as handed over, not tested for emptiness: `q2_sim_attach_movers`
     * REALLOCATES the target array and rewrites the count, so a binding that
     * decided at this moment that the world was empty would stay empty for the
     * rest of the level. The clip copes with a count of zero on its own.
     */
    bind->ents = ents;
}

void q2_ai_world_bind_bodies(q2_ai_world_bind *bind,
                             const q2_move_bodies *bodies,
                             struct q2_monster *owner_base,
                             u32 owner_count)
{
    if (!bind)
        return;

    /* Stored as handed over, for the reason `_bind_entities` gives: the sim
     * rebuilds this list every tick and its count moves with the level. */
    bind->bodies           = bodies;
    bind->body_owner_base  = owner_base;
    bind->body_owner_count = owner_count;
}

void q2_ai_world_bind_install(q2_ai_world_bind *bind)
{
    if (!bind || !bind->coll) {
        q2_ai_set_world(NULL);
        return;
    }
    q2_ai_set_world(&bind->world);
}
