/*
 * trig.h — fixed-point sine and cosine on the PlayStation's angle convention.
 *
 * Angles are a full circle in 4096 steps, not radians and not degrees. That is
 * what the console's libraries used, what the game's stored angles are in, and
 * what keeps rotation matrices in the 1.3.12 format the GTE expects. Converting
 * to radians and back would introduce rounding the original never had.
 *
 * Results are 1.3.12: +4096 is +1.0, -4096 is -1.0.
 *
 * The table is built once at first use from the host's libm. That is a
 * deliberate trade: the *table* is computed in floating point, but every
 * subsequent use is exact integer arithmetic, so results are reproducible across
 * platforms as long as the table is. A future change could hard-code it if a
 * platform's libm ever disagreed at the ulp level.
 */
#ifndef Q2PSX_TRIG_H
#define Q2PSX_TRIG_H

#include "fixed.h"
#include "q2psx.h"

#define Q2_ANGLE_360 4096
#define Q2_ANGLE_180 2048
#define Q2_ANGLE_90  1024

/* Angle may be any value; it is reduced modulo a full circle. */
s32 q2_sin12(s32 angle);
s32 q2_cos12(s32 angle);

/*
 * Inverse cosine: takes a 1.3.12 cosine and returns the angle, 0 for +1.0 and
 * Q2_ANGLE_180 for -1.0. Inputs outside ±4096 are clamped.
 *
 * The original has this as a 4096-entry table at 0x8009FC44 indexed by
 * cos/2 + 2048, used only by the quaternion interpolator. This computes the
 * same function rather than shipping the table; `q2psx-inspect anims` measures
 * the two against each other over all 4096 entries so the difference is a
 * number rather than an assumption.
 */
s32 q2_acos12(s32 cos12);

/* Build a 1.3.12 rotation matrix for yaw then pitch, in the order the camera
 * wants: yaw about Y, then pitch about X. `m` is row-major m[row][col]. */
void q2_rotation_yaw_pitch(s16 m[3][3], s32 yaw, s32 pitch);

/*
 * Three Euler angles to a 1.3.12 rotation matrix, in libgte `RotMatrix`'s form:
 * R = Rz(rz) * Ry(ry) * Rx(rx), angles on the 4096-step circle.
 *
 * This is what the zone draw builds from a rotating node's runtime object
 * (0x800678B8 calls RotMatrix at 0x80089E38 on the object's +0x0C triple).
 *
 * The composition order is unobservable on this disc: the rotation integrator
 * at 0x8002F1A8 stores to obj[0x0C + 2*axis] and nothing ever clears the other
 * two slots, so exactly one angle is non-zero and the three orders agree. It is
 * spelled out here so the general case is defined rather than accidental.
 */
void q2_rotation_euler(s16 m[3][3], s32 rx, s32 ry, s32 rz);

/*
 * The view basis, with the strafe roll.
 *
 * `RotMatrix` (0x80089E38) composes Rz * Ry * Rx, and the camera at 0x8004F40C
 * hands it (pitch, yaw, roll) — so the roll is the OUTERMOST rotation, applied
 * in screen space after the view direction is decided. That is what makes it a
 * lean rather than a change of heading.
 *
 * Identical to q2_rotation_yaw_pitch when `roll` is zero, which is what keeps it
 * safe to use on every camera rather than only the player's.
 */
void q2_rotation_view(s16 m[3][3], s32 yaw, s32 pitch, s32 roll);

/* ------------------------------------------------------------------------- */
/* The pixel-aspect correction                                                */
/* ------------------------------------------------------------------------- */
/*
 * The framebuffer's pixels are not square and the projection has only one
 * scale, so one of the two axes has to carry the difference.
 *
 * `gte_rtps` computes `sx = ofx + (h/z)*ir[0]` and `sy = ofy + (h/z)*ir[1]`
 * from a single `h`: that is the hardware, and there is no second register for
 * a horizontal field of view. A 512 x 248 buffer on a 4:3 screen therefore
 * comes out anamorphic — hFOV 116.0 against vFOV 75.6, where a correct picture
 * at that vFOV wants hFOV 91.9 — unless something scales view-space x before
 * the divide. The only thing upstream of `ir[0]` is the rotation matrix, so the
 * scale goes in its first ROW, which costs nothing at run time and is why no
 * second projection constant has ever turned up in the view record.
 *
 * ---------------------------------------------------------------------------
 * How this was measured, because eyeballing it got the sign wrong twice
 * ---------------------------------------------------------------------------
 * Against a DuckStation capture, calibrated by solving the framebuffer-to-image
 * mapping from the HUD — which is drawn in framebuffer pixels, so it pins the
 * scale and origin exactly and removes any argument about what aspect the
 * capture was displayed at (x 1.2809, y 1.8571, origin y 10).
 *
 * The ruler is the blaster's muzzle stripe: rigidly attached to the camera, so
 * unlike anything in the world its size does not depend on where the player is
 * standing. That distinction is load-bearing — a first attempt used the canyon's
 * sky gap, which said the opposite, because the reference frame was taken after
 * the player had walked forward and every world feature was therefore nearer.
 *
 * Sampled across a full idle cycle rather than one frame, since the weapon
 * sways: 22 frames, and the stripe's width/height sits at 0.67 for the settled
 * pose (range 0.41 .. 0.81). Retail's is 1.00. Our stripe never once reaches
 * retail's WIDTH while its height comfortably exceeds it, which is a horizontal
 * deficiency and not a scale error. 1/0.67 = 1.49 against the 1.5484 below.
 */
#define Q2_DISPLAY_ASPECT_NUM 4
#define Q2_DISPLAY_ASPECT_DEN 3

/*
 * The 1.0.12 factor for a `w` x `h` viewport on a Q2_DISPLAY_ASPECT screen:
 * 4096 * (w/h) / (4/3). Returns 4096 — unity — for a viewport that is already
 * square-pixelled, so applying it unconditionally is safe.
 */
Q2PSX_INLINE s32 q2_aspect_x_12(s32 w, s32 h)
{
    if (w <= 0 || h <= 0)
        return 4096;
    return (4096 * Q2_DISPLAY_ASPECT_DEN * w) / (Q2_DISPLAY_ASPECT_NUM * h);
}

/*
 * The same thing from the projection CENTRE, which is what a camera carries.
 * Every layout sets the centre to its viewport's own middle, so the viewport is
 * twice the offset — and taking it from here rather than from the framebuffer
 * is what gives a split viewport its own correction instead of the whole
 * frame's. Falls back to the buffer when the camera leaves the centre at zero,
 * which is the "no viewport, use the middle of whatever you are drawing into"
 * sentinel documented on q2_camera.ofs_x.
 */
Q2PSX_INLINE s32 q2_aspect_x_12_centre(s32 ofs_x, s32 ofs_y, s32 w, s32 h)
{
    if (ofs_x > 0 && ofs_y > 0)
        return q2_aspect_x_12(ofs_x * 2, ofs_y * 2);
    return q2_aspect_x_12(w, h);
}

/*
 * Scale a view matrix's first row, in place, by a 1.0.12 factor. Apply to the
 * matrix that is about to become the GTE's rotation — NOT to one being built to
 * invert, or the cancellation the view weapon depends on stops cancelling.
 */
void q2_rotation_aspect_x(s16 m[3][3], s32 scale_12);

/*
 * Rotation matrix for a quaternion, both in 1.3.12. `q` is x, y, z, w, which is
 * the order the model animation keys decode to. Row-major, and in the form the
 * GTE's rotation registers want.
 */
void q2_quat_to_matrix(s16 m[3][3], const s16 q[4]);

#endif /* Q2PSX_TRIG_H */
