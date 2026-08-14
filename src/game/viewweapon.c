#include "viewweapon.h"

#include "trig.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Keys                                                                       */
/* ------------------------------------------------------------------------- */
static const q2_vm_key *current_key(const q2_viewweapon *vw)
{
    const q2_vm_clip *c = q2_vm_clip_get(vw->tab, vw->weapon, vw->state);

    if (!c || vw->frame >= c->count)
        return NULL;
    return &c->key[vw->frame];
}

static u32 clip_length(const q2_viewweapon *vw, q2_vm_state state)
{
    const q2_vm_clip *c = q2_vm_clip_get(vw->tab, vw->weapon, state);
    return c ? c->count : 0;
}

/*
 * 0x8004F1F8. A key with a non-zero jitter takes `((rand() * duration) >> 15 +
 * 1) * 10`; every other key takes its duration as written. The multiply by ten
 * is spelled out in the original as a shift-add chain, and it is why a jittered
 * key is an order of magnitude longer than its stored value rather than a small
 * perturbation of it.
 */
static s32 key_duration(const q2_vm_key *k)
{
    if (!k)
        return 0;
    if (k->jitter == 0)
        return k->duration;

    {
        s32 r = (s32)(rand() & 0x7FFF);
        s32 n = ((r * (s32)k->duration) >> 15) + 1;
        return n * 10;
    }
}

/* Latch the interpolation's starting point and load the new key's clock. */
static void begin_key(q2_viewweapon *vw)
{
    const q2_vm_key *k = current_key(vw);
    int i;

    for (i = 0; i < 3; i++) {
        vw->from_t[i] = vw->cur_t[i];
        vw->from_r[i] = vw->cur_r[i];
    }

    vw->total = key_duration(k);
    vw->left  = vw->total;

    if (k && k->event != Q2_VM_EVENT_NONE) {
        vw->event = k->event;
        vw->event_pending = true;
    }
}

/*
 * `v = from + (key - from) * (total - left) / total`, which is 0x8004EF80 and
 * its five siblings. A zero-duration key is legal — the original's divide would
 * trap, so the engine never reaches it with total == 0, and here it simply
 * snaps.
 */
static s16 lerp_field(s16 from, s16 to, s32 total, s32 left)
{
    s32 elapsed;

    if (total <= 0)
        return to;

    elapsed = total - left;
    if (elapsed <= 0)
        return from;
    if (elapsed >= total)
        return to;

    return (s16)((s32)from + ((s32)to - (s32)from) * elapsed / total);
}

static void interpolate(q2_viewweapon *vw)
{
    const q2_vm_key *k = current_key(vw);
    int i;

    if (!k)
        return;

    for (i = 0; i < 3; i++) {
        vw->cur_t[i] = lerp_field(vw->from_t[i], k->t[i], vw->total, vw->left);
        vw->cur_r[i] = lerp_field(vw->from_r[i], k->r[i], vw->total, vw->left);
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
void q2_vw_init(q2_viewweapon *vw, const q2_vm_tables *tab, int weapon)
{
    if (!vw)
        return;

    memset(vw, 0, sizeof(*vw));
    vw->tab     = tab;
    vw->weapon  = (weapon >= 0 && weapon < Q2_VM_SLOTS) ? weapon : 0;
    vw->pending = vw->weapon;

    /* A level start has the weapon already up: the machine's own reset lands in
     * RAISE at frame 0 (0x8004FA48 sets state 0 after a swap). */
    vw->state = Q2_VM_RAISE;
    vw->frame = 0;
    begin_key(vw);
    interpolate(vw);
}

void q2_vw_select(q2_viewweapon *vw, int weapon)
{
    if (!vw || weapon < 0 || weapon >= Q2_VM_SLOTS)
        return;
    vw->pending = weapon;

    /*
     * 0x8004ECEC. Asking for a weapon arms the 70-tick countdown; LOWER holds
     * at the bottom of its arc until it expires, which is what stops a fast
     * double-tap from swapping the model twice in as many frames.
     */
    vw->switch_ticks = Q2_VW_SWITCH_TICKS;
}

void q2_vw_set_model(q2_viewweapon *vw, const q2_model *model)
{
    if (vw)
        vw->model = model;
}

const char *q2_vw_model_name(const q2_viewweapon *vw)
{
    if (!vw || !vw->tab)
        return NULL;
    return vw->tab->model_name[vw->weapon];
}

bool q2_vw_take_refire(q2_viewweapon *vw)
{
    bool r;

    if (!vw)
        return false;
    r = vw->refire;
    vw->refire = false;
    return r;
}

bool q2_vw_take_event(q2_viewweapon *vw, s16 *event_out)
{
    if (!vw || !vw->event_pending)
        return false;
    if (event_out)
        *event_out = vw->event;
    vw->event_pending = false;
    return true;
}

bool q2_vw_wants_fire(const q2_viewweapon *vw)
{
    return vw && vw->state == Q2_VM_IDLE && !vw->fire_latch;
}

/* ------------------------------------------------------------------------- */
/* The state machine — 0x8004F87C, called once per key boundary               */
/* ------------------------------------------------------------------------- */
/*
 * The fire check the idle state runs — 0x8004FB04…0x8004FB98.
 *
 * The order here is the original's and it is not the obvious one. The latch is
 * tested BEFORE the fire function is consulted, and a set latch does not fire:
 * it clears, and that clearing is the same pass that recomputes the player's
 * next and previous weapons. So one shot per press is not enforced by a timer,
 * it falls out of a latch that costs one pass to release. A port that clears
 * the latch when the fire clip ends instead fires a frame early, every time.
 */
static void q2_vw_idle_fire_check(q2_viewweapon *vw, bool fire_held,
                                  q2_vw_fire_result fired)
{
    if (!fire_held && !vw->fire_latch)
        return;                     /* 0x8004FB18: nothing to do */

    if (vw->fire_latch) {
        /*
         * 0x8004FB5C. The shot that set this has been made. Clearing it is
         * also what makes an empty weapon auto-switch, because the original
         * recomputes the neighbours on the same path — which is why a caller
         * is told about it through `refire`.
         */
        vw->fire_latch = false;
        vw->refire     = true;
        return;
    }

    if (fired == Q2_VW_FIRE_DENIED) {
        /* Running dry is the same path: no clip, and the neighbours are
         * recomputed so the console switches off the empty gun. */
        vw->refire = true;
        return;
    }

    vw->state      = Q2_VM_FIRE;
    vw->frame      = 0;
    vw->fire_latch = true;
    begin_key(vw);
}

/*
 * Returns true when the model in hand changed. `clip_done` is the original's
 * `clip_start + 20*frame == clip_end`, i.e. the frame index has walked off the
 * end of the clip.
 */
static bool machine_step(q2_viewweapon *vw, bool clip_done, bool fire_held,
                         q2_vw_fire_result fired)
{
    bool swapped = false;

    switch (vw->state) {
    case Q2_VM_LOWER:
        if (!clip_done)
            break;

        /*
         * 0x8004F944. The countdown gates the swap: while it runs the frame is
         * rewound by one and the weapon hangs at the bottom of its arc. Only
         * when it expires does the pending weapon become the real one.
         */
        if (vw->switch_ticks > 0) {
            if (vw->frame > 0)
                vw->frame--;
            break;
        }

        vw->weapon     = vw->pending;
        vw->state      = Q2_VM_RAISE;
        vw->frame      = 0;
        vw->fire_latch = false;
        swapped        = true;
        begin_key(vw);
        break;

    case Q2_VM_RAISE:
        /* 0x8004FA80: the raise ends in IDLE, and that is where the fire
         * function is bound for the weapon now in hand. */
        if (clip_done) {
            vw->state = Q2_VM_IDLE;
            vw->frame = 0;
            begin_key(vw);
        }
        break;

    case Q2_VM_FIRE:
        /*
         * 0x8004FC04: the fire clip always returns to idle — and it does NOT
         * clear the latch. That omission is the refire cadence: the shot stays
         * latched into the idle state, and it takes one further pass through
         * the fire check to clear it, so a held trigger fires, idles for one
         * pass, and fires again rather than restarting the clip every tick.
         */
        if (clip_done) {
            vw->state = Q2_VM_IDLE;
            vw->frame = 0;
            begin_key(vw);
        }
        break;

    case Q2_VM_IDLE:
    default:
        q2_vw_idle_fire_check(vw, fire_held, fired);
        if (vw->state == Q2_VM_IDLE && clip_done) {
            /* 0x8004FBD8: idle loops. */
            vw->frame = 0;
            begin_key(vw);
        }
        break;
    }

    /*
     * 0x8004FAB4. A selection that differs from what is in hand starts the
     * lower, from any state except the one already firing — which is why you
     * cannot cancel a shot by switching.
     */
    if (!swapped && vw->pending != vw->weapon &&
        vw->state != Q2_VM_FIRE && vw->state != Q2_VM_LOWER) {
        vw->state = Q2_VM_LOWER;
        vw->frame = 0;
        begin_key(vw);
    }

    return swapped;
}

bool q2_vw_advance(q2_viewweapon *vw, s32 dt, bool fire_held,
                   q2_vw_fire_result fired)
{
    bool swapped = false;
    int guard = 0;

    if (!vw || !vw->tab)
        return false;

    if (vw->switch_ticks > 0) {
        vw->switch_ticks = (s16)(vw->switch_ticks - dt);
        if (vw->switch_ticks < 0)
            vw->switch_ticks = 0;
    }

    /*
     * 0x8004EEA4. The original consumes dt in chunks of `min(left, dt)` and
     * loops, so a long host frame plays a short clip all the way through
     * instead of skipping it — which matters because the events on those keys
     * are what fire the shot.
     *
     * The guard is a port's own: the original cannot spin here because every
     * clip has at least one key with a non-zero duration, but a mis-read table
     * could, and hanging is a worse failure than a stalled animation.
     */
    while (dt > 0 && guard++ < 512) {
        s32 step = dt;

        if (vw->total > 0 && vw->left < step)
            step = vw->left;
        if (step < 0)
            step = 0;

        vw->left -= step;
        dt       -= step;

        interpolate(vw);

        if (vw->left > 0)
            break;

        /* The key is spent: advance, and let the machine see whether that ran
         * off the end of the clip. */
        vw->frame++;
        vw->keys_played++;

        {
            u32 len = clip_length(vw, vw->state);
            bool done = (vw->frame >= len);

            if (machine_step(vw, done, fire_held, fired))
                swapped = true;
            else if (done && vw->state != Q2_VM_LOWER)
                vw->frame = 0;
        }

        begin_key(vw);
        interpolate(vw);

        if (vw->total <= 0 && dt > 0) {
            /* A zero-length key would otherwise consume no time and spin. */
            continue;
        }
    }

    /*
     * The trigger is tested every tick, not every key boundary (0x8004FAF4), so
     * a press between two keys still fires on the tick it arrived.
     */
    if (vw->state == Q2_VM_IDLE) {
        q2_vw_idle_fire_check(vw, fire_held, fired);
        if (vw->state == Q2_VM_FIRE)
            interpolate(vw);
    }

    /* And a selection change likewise. */
    if (vw->pending != vw->weapon &&
        vw->state != Q2_VM_FIRE && vw->state != Q2_VM_LOWER) {
        vw->state = Q2_VM_LOWER;
        vw->frame = 0;
        begin_key(vw);
        interpolate(vw);
    }

    return swapped;
}

/* ------------------------------------------------------------------------- */
/* Placement                                                                  */
/* ------------------------------------------------------------------------- */
void q2_vw_place(const q2_viewweapon *vw,
                 const s32 feet[3], s32 view_offset,
                 const s16 aim[3], const s16 kick[3],
                 s32 origin_out[3], s32 angles_out[3])
{
    s16 rot[3][3];
    s32 ang[3];
    s32 local[3];
    int i;

    if (!vw || !feet)
        return;

    /*
     * 0x8004F40C. The three angles are the player's aim plus the second triple
     * the original adds, and the x component is NEGATED at the sum. The
     * animation's own rotation rides on top, which is what makes a weapon dip
     * as it is raised rather than snapping upright.
     */
    /*
     * `ang` is the VIEW matrix and nothing else. The clip's own rotation is NOT
     * in it: the original builds `sp+40` from the player's aim and the second
     * angle triple only (0x8004F40C…0x8004F448) and hands that to RotMatrix, and
     * the clip's rotation reaches the model through the separate chain at
     * 0x8004F284. Folding the clip's angles in here rotates the offset by them
     * too, and since several clips carry a half-circle component that puts the
     * weapon behind the camera — where every face is rejected at the projection
     * plane and nothing draws at all.
     */
    ang[0] = -((aim ? aim[0] : 0) + (kick ? kick[0] : 0));
    ang[1] =  ((aim ? aim[1] : 0) + (kick ? kick[1] : 0));
    ang[2] =  ((aim ? aim[2] : 0) + (kick ? kick[2] : 0));

    if (angles_out) {
        /* The model's own orientation does carry the clip's rotation, composed
         * on top of the view — 0x8004F474's MulMatrix. */
        angles_out[0] = ang[0] + vw->cur_r[0];
        angles_out[1] = ang[1] + vw->cur_r[1];
        angles_out[2] = ang[2] + vw->cur_r[2];
    }

    if (!origin_out)
        return;

    /*
     * 0x8004F5E0. The animation's translation is rotated before the eye is
     * added — rotate then translate, not the other way round, or the weapon
     * slides across the view instead of swinging with it.
     *
     * The matrix is the camera's INVERSE, and that is not a detail. The view
     * model is authored in view space: `Blaster G` runs from z = 0 at the grip
     * to z = 482 at the muzzle, so its own +Z is "away from the eye", not a
     * world direction. Rotating the offset by the camera's forward matrix
     * instead sends it out along a world axis — which is what put the weapon
     * behind the camera and had every face rejected at the projection plane.
     *
     * A rotation matrix's inverse is its transpose, so this is the same matrix
     * read down its columns.
     */
    q2_rotation_yaw_pitch(rot, ang[1], ang[0]);

    for (i = 0; i < 3; i++) {
        local[i] = ((s32)rot[0][i] * vw->cur_t[0]
                  + (s32)rot[1][i] * vw->cur_t[1]
                  + (s32)rot[2][i] * vw->cur_t[2]) >> Q2_FRAC_12;
    }

    /*
     * 0x8004F5E8…0x8004F640. The eye's own expression, not an approximation of
     * it: FORMATS §9.12 gives `eye.y = pos.y + 286 - viewOffset`, and +Y is
     * down, so a larger view offset raises both the camera and the weapon by
     * the same amount.
     */
    origin_out[0] = feet[0] + local[0];
    origin_out[1] = feet[1] + local[1] + Q2_VW_EYE_BASE - view_offset;
    origin_out[2] = feet[2] + local[2];
}

u32 q2_vw_build_ot(const q2_viewweapon *vw,
                   const q2_model_instance *proto,
                   const s32 feet[3], s32 view_offset,
                   const s16 aim[3], const s16 kick[3],
                   const q2_camera *cam,
                   psx_ot *ot, gte_state *gte,
                   q2_model_draw_stats *stats)
{
    q2_model_instance inst;
    s32 origin[3];
    s32 angles[3];
    s32 angles_view[3];
    s16 rot_out[3][3];

    if (!vw || !vw->model || !cam || !ot || !gte)
        return 0;

    if (proto)
        inst = *proto;
    else
        q2_model_instance_init(&inst);

    q2_vw_place(vw, feet, view_offset, aim, kick, origin, angles);

    /* The view part of those angles, without the clip's — the composition above
     * needs the camera's rotation on its own. */
    angles_view[0] = -((aim ? aim[0] : 0) + (kick ? kick[0] : 0));
    angles_view[1] =  ((aim ? aim[1] : 0) + (kick ? kick[1] : 0));
    angles_view[2] =  ((aim ? aim[2] : 0) + (kick ? kick[2] : 0));
    (void)angles;

    inst.model     = vw->model;
    inst.pose      = NULL;
    inst.origin[0] = origin[0];
    inst.origin[1] = origin[1];
    inst.origin[2] = origin[2];

    /*
     * The rotation the model is drawn with, and the reason it is a matrix
     * rather than three angles.
     *
     * `q2_model_build_ot` composes the CAMERA's rotation with the instance's,
     * because that is what the console does for anything standing in the world.
     * A view model is not standing in the world: it is authored in view space,
     * so what has to survive that composition is the clip's own wobble and
     * nothing else. The instance matrix is therefore the camera's inverse —
     * its transpose — with the clip's rotation on top:
     *
     *      camera * (camera^T * R_clip)  ==  R_clip
     *
     * which is the port's form of the original composing `RotMatrix(view
     * angles)` into the entity's own matrix at 0x8004F474 and letting the
     * camera undo it.
     */
    {
        s16 inv[3][3], clip[3][3];
        int r, c, k;

        q2_rotation_yaw_pitch(inv, angles_view[1], angles_view[0]);
        q2_rotation_euler(clip, vw->cur_r[0], vw->cur_r[1], vw->cur_r[2]);

        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                s32 acc = 0;
                for (k = 0; k < 3; k++)
                    acc += (s32)inv[k][r] * (s32)clip[k][c];   /* inv^T * clip */
                rot_out[r][c] = (s16)(acc >> Q2_FRAC_12);
            }
        }
        inst.rot = (const s16 (*)[3])rot_out;
    }

    return q2_model_build_ot(&inst, cam, ot, gte, stats);
}
