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
 * Rotation matrix for a quaternion, both in 1.3.12. `q` is x, y, z, w, which is
 * the order the model animation keys decode to. Row-major, and in the form the
 * GTE's rotation registers want.
 */
void q2_quat_to_matrix(s16 m[3][3], const s16 q[4]);

#endif /* Q2PSX_TRIG_H */
