/*
 * viewweapon.h — the weapon in the player's hands.
 *
 * ---------------------------------------------------------------------------
 * It is a model, not an overlay
 * ---------------------------------------------------------------------------
 * The console does not paste a sprite over the frame. The view weapon is a
 * `CastList` model — the bank names it "Blaster G", "Supershot G" — placed at
 * the player's eye and drawn through the same GTE, into the same ordering
 * table, as everything else. That is why it sorts against the world instead of
 * always winning, and it is why this module produces primitives rather than
 * pixels.
 *
 * ---------------------------------------------------------------------------
 * The state machine — 0x8004F87C
 * ---------------------------------------------------------------------------
 * Four states, and every transition is an animation event rather than a timer:
 *
 *        RAISE ──clip ends──> IDLE ──fire pressed──> FIRE
 *          ^                   │                      │
 *          │                   └──selection changed───┤ clip ends
 *          │                            │             v
 *          └── model swapped <── LOWER <┘            IDLE
 *
 * The whole "has the animation finished" test is `clip_start + 20*frame ==
 * clip_end` (0x8004F93C) — the clip bank stores no length, only neighbouring
 * pointers, so the end IS the next clip's start (vmtables.h).
 *
 * Two details that a port gets wrong by simplifying:
 *
 *   - **LOWER holds rather than loops.** When the lower clip runs out, the
 *     70-tick switch countdown at weapon-block +222 is checked (0x8004F944); if
 *     it has not expired the frame index is *rewound by one* and the state
 *     stays, so the weapon hangs at the bottom of its arc. The model is only
 *     swapped when the countdown reaches zero.
 *   - **Running dry does not just stop the gun.** If the fire function reports
 *     it could not fire, the machine clears the fire latch and recomputes the
 *     next and previous weapons (0x800506C4 / 0x80050758 at 0x8004FB5C), which
 *     is what makes the console auto-switch off an empty weapon.
 *
 * ---------------------------------------------------------------------------
 * Where it sits, which is the part ports get visibly wrong
 * ---------------------------------------------------------------------------
 * Read at 0x8004F5E8…0x8004F640:
 *
 *     pos.x = player.x + rotated_offset.x
 *     pos.y = player.y + rotated_offset.y + 286 - view_offset
 *     pos.z = player.z + rotated_offset.z
 *
 * The `286 - view_offset` is not a fudge — it is the *same expression the
 * camera uses* (FORMATS §9.12: `eye.y = pos.y + 286 - viewOffset`). The weapon
 * hangs off the eye, so it rises and falls with a crouch exactly as the view
 * does, and no separate bob is needed to make it feel attached.
 *
 * The rotation is `RotMatrix(angles)` (0x80089E38 at 0x8004F464) where the
 * angles are the player's aim and the second angle triple, with **x negated**
 * (0x8004F41C) — the animation's own rotation is NOT in that matrix; it reaches
 * the model separately (0x8004F474's MulMatrix). Then the offset from the
 * animation is rotated before the eye is added, so the weapon swings with the
 * view rather than sliding across it.
 *
 * ---------------------------------------------------------------------------
 * The model is authored in VIEW space, and that is the whole trick
 * ---------------------------------------------------------------------------
 * `Blaster G` has bounds `[-45 -105 0] .. [67 70 482]`: the grip sits on the
 * origin and the barrel runs out along +Z. Its own axes ARE the camera's, so
 * the matrix it must be drawn with is the camera's **inverse**, composed with
 * the clip's rotation:
 *
 *     camera * (camera^T * R_clip)  ==  R_clip
 *
 * which is the port's form of the original composing the view rotation into the
 * entity's matrix and letting the camera undo it. Two mistakes follow from
 * missing this, and both were made here before they were fixed: rotating the
 * offset by the camera's FORWARD matrix sends the weapon out along a world axis
 * and puts it behind the eye, where every face is rejected at the projection
 * plane; and folding the clip's angles into that same matrix does it again,
 * because several clips carry a half-circle component.
 *
 * ---------------------------------------------------------------------------
 * What is NOT established
 * ---------------------------------------------------------------------------
 * The angle sum at 0x8004F40C adds a triple at player+230 to the triple the
 * view-angle helper at 0x80038260 returns. One of those is the aim and the
 * other is the shot kick; which is which was not separated, because both are
 * three signed angles added to the same matrix and no call site distinguishes
 * them. This module therefore takes aim and kick as two inputs and adds them
 * the way the original adds them — if the attribution is ever swapped, nothing
 * here changes.
 */
#ifndef Q2PSX_VIEWWEAPON_H
#define Q2PSX_VIEWWEAPON_H

#include "gpu.h"
#include "gte.h"
#include "model.h"
#include "modeldraw.h"
#include "q2psx.h"
#include "vmtables.h"
#include "world.h"

/* 0x8004ECEC — the switch countdown the LOWER state waits on. */
#define Q2_VW_SWITCH_TICKS  70

/* FORMATS §9.12. The weapon hangs off the eye, so it uses the eye's own base. */
#define Q2_VW_EYE_BASE     286

/*
 * The fire function's verdict, as the state machine consumes it. The original's
 * fire functions return a small integer; 2 is the "could not fire" the machine
 * compares against (0x8004FB44), and it is what makes an empty weapon
 * auto-switch rather than click.
 */
typedef enum q2_vw_fire_result {
    Q2_VW_FIRED = 0,
    Q2_VW_FIRE_DENIED = 2
} q2_vw_fire_result;

typedef struct q2_viewweapon {
    const q2_vm_tables *tab;

    /* What is in the hands now, and what has been asked for. Both are 1-based
     * weapon ids; they differ exactly while the weapon is being swapped. */
    int         weapon;          /* view model +212 */
    int         pending;         /* view model +214 */

    q2_vm_state state;           /* +72  */
    u32         frame;           /* +76  */

    /*
     * The key clock. `total` is the current key's duration and `left` counts
     * down by dt; the interpolation runs on (total - left) / total, so a key
     * with a zero duration is consumed in one step rather than dividing by
     * zero.
     */
    s32         total;           /* +48  */
    s32         left;            /* +80  */

    /* Where the interpolation started — the value the previous key left. */
    s16         from_t[3];       /* +224 / +226 / +228 */
    s16         from_r[3];       /* +236 / +238 / +240 */

    /* The interpolated present, which is what the transform uses. */
    s16         cur_t[3];
    s16         cur_r[3];

    bool        fire_latch;      /* +216: a shot is in flight this state      */

    /*
     * Set on the pass that releases the latch, which is also the pass on which
     * the original recomputes the player's next and previous weapons
     * (0x8004FB5C). A caller drains it to do the auto-switch off an empty gun —
     * the thing that makes running dry feel like the console rather than like a
     * click.
     */
    bool        refire;
    s16         switch_ticks;    /* weapon block +222, the 70-tick countdown  */

    /* The last event a key raised, and the frame it happened on. A caller
     * drains this to make a muzzle flash or a shell eject happen on the frame
     * the animation says, not on the frame the trigger was pressed. */
    s16         event;
    bool        event_pending;

    /* The model, resolved from the bank by name when the weapon is swapped. */
    const q2_model *model;

    /* Diagnostics: how many keys have been consumed since the last reset. */
    u32         keys_played;
} q2_viewweapon;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * Start with `weapon` in hand, already raised — which is what a level start
 * does. Pass 0 for "no weapon", which is a live state rather than an error:
 * slot 0 aliases slot 1 in the clip table exactly as it does in the fire-
 * function table.
 */
void q2_vw_init(q2_viewweapon *vw, const q2_vm_tables *tab, int weapon);

/* Ask for a different weapon. Takes effect through LOWER; the model does not
 * change until the lower clip has run and the countdown has expired. */
void q2_vw_select(q2_viewweapon *vw, int weapon);

/* Bind the resolved model for the weapon currently in hand. A caller does this
 * after `q2_vw_take_model_name` reports the swap. */
void q2_vw_set_model(q2_viewweapon *vw, const q2_model *model);

/*
 * The 12-byte model name the weapon in hand wants, or NULL when the module has
 * no table. The name is not NUL-terminated at the source, so this returns the
 * table's own fixed-width copy.
 */
const char *q2_vw_model_name(const q2_viewweapon *vw);

/* ------------------------------------------------------------------------- */
/* Per tick                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * Advance by `dt` in the console's 1/300 s units, consuming as many keys as it
 * spans — the original's loop at 0x8004EEA4 takes `min(left, dt)` and repeats,
 * so a long frame plays a short clip through rather than skipping it.
 *
 * `fire_held` is the trigger. `fired` is what the caller's fire function
 * decided when the machine asked it to shoot; pass Q2_VW_FIRED normally and
 * Q2_VW_FIRE_DENIED when the shot could not happen, which is what triggers the
 * auto-switch.
 *
 * Returns true when the weapon in hand changed, which is a caller's cue to
 * resolve the new model name.
 */
bool q2_vw_advance(q2_viewweapon *vw, s32 dt, bool fire_held,
                   q2_vw_fire_result fired);

/*
 * Take the "the latch was released" signal — the pass on which the original
 * recomputes the player's next and previous weapons, and therefore the moment a
 * caller should auto-switch off an empty gun.
 */
bool q2_vw_take_refire(q2_viewweapon *vw);

/* Take the pending animation event, or false when there is none. */
bool q2_vw_take_event(q2_viewweapon *vw, s16 *event_out);

/* True while the machine wants the caller's fire function called this tick. */
bool q2_vw_wants_fire(const q2_viewweapon *vw);

/* ------------------------------------------------------------------------- */
/* Placement and drawing                                                      */
/* ------------------------------------------------------------------------- */
/*
 * Where the weapon is, given the player. `feet` is the player's own position
 * (not the eye), `view_offset` is the crouch easing value the camera uses, and
 * `aim` / `kick` are the two angle triples the original sums — see the header
 * note about which is which.
 *
 * Writes the world position and the three angles the model should be drawn at.
 */
/*
 * ATTRIBUTED, and it is no longer the open question #41 recorded.
 *
 * `aim` is `player+230`, which the call site at 0x8004F40C reads straight out
 * of the entity with `lhu` and uses. `kick` is what 0x80038260 returns, and
 * that function is three near-identical blocks, each loading a DEADLINE
 * (32/36/40 off obj+0xAC), subtracting the clock at 0x800B2BAC, dropping out if
 * it has passed, and otherwise scaling a stored angle pair by the remaining
 * fraction and accumulating into the SAME output. Three contributions, each
 * decaying over its own period, summed - which is a kick and nothing else is.
 * The periods are in the reciprocal constants: 0x88888889 is /30 (firing),
 * 0x1B4E81B5 is /150 (damage), 0xB60B60B7 is /90 (landing).
 */
void q2_vw_place(const q2_viewweapon *vw,
                 const s32 feet[3], s32 view_offset,
                 const s16 aim[3], const s16 kick[3],
                 s32 origin_out[3], s32 angles_out[3]);

/*
 * Emit the weapon into `ot`. The ordering table is not cleared and the GTE's
 * projection is not touched, so this drops into a frame the world has already
 * been built into — which is the only way the weapon can sort against a wall it
 * is pushed into.
 *
 * `proto` supplies the shared render state a caller already has (the texture
 * page table, the CLUT split, lighting); its origin, angles and model are
 * overwritten. Returns the number of primitives emitted.
 */
u32 q2_vw_build_ot(const q2_viewweapon *vw,
                   const q2_model_instance *proto,
                   const s32 feet[3], s32 view_offset,
                   const s16 aim[3], const s16 kick[3],
                   const q2_camera *cam,
                   psx_ot *ot, gte_state *gte,
                   q2_model_draw_stats *stats);

#endif /* Q2PSX_VIEWWEAPON_H */
