#include "trig.h"

#include <math.h>

static s16  g_sin_table[Q2_ANGLE_360];
static bool g_table_ready;

static void build_table(void)
{
    int i;

    if (g_table_ready)
        return;

    for (i = 0; i < Q2_ANGLE_360; i++) {
        double radians = (double)i * (2.0 * 3.14159265358979323846) / (double)Q2_ANGLE_360;
        double value   = sin(radians) * (double)Q2_ONE_12;

        /* Round half away from zero, then clamp: sin(90 degrees) must land on
         * exactly +4096 rather than 4095, or an identity rotation would shrink
         * geometry very slightly on every frame it is applied. */
        value = value < 0.0 ? value - 0.5 : value + 0.5;

        if (value >  32767.0) value =  32767.0;
        if (value < -32768.0) value = -32768.0;

        g_sin_table[i] = (s16)value;
    }

    g_table_ready = true;
}

static s32 wrap_angle(s32 angle)
{
    angle %= Q2_ANGLE_360;
    if (angle < 0)
        angle += Q2_ANGLE_360;
    return angle;
}

s32 q2_sin12(s32 angle)
{
    build_table();
    return g_sin_table[wrap_angle(angle)];
}

s32 q2_cos12(s32 angle)
{
    build_table();
    return g_sin_table[wrap_angle(angle + Q2_ANGLE_90)];
}

s32 q2_acos12(s32 cos12)
{
    double radians, angle;

    if (cos12 >  Q2_ONE_12) cos12 =  Q2_ONE_12;
    if (cos12 < -Q2_ONE_12) cos12 = -Q2_ONE_12;

    radians = acos((double)cos12 / (double)Q2_ONE_12);
    angle   = radians * (double)Q2_ANGLE_360 /
              (2.0 * 3.14159265358979323846);

    /* Truncate rather than round: the original's table does, visibly so at the
     * ends — its entry for cos = 4095/4096 is 20 where rounding gives 21. */
    return (s32)angle;
}

void q2_rotation_yaw_pitch(s16 m[3][3], s32 yaw, s32 pitch)
{
    s32 sy = q2_sin12(yaw),   cy = q2_cos12(yaw);
    s32 sp = q2_sin12(pitch), cp = q2_cos12(pitch);

    /* R = Rx(pitch) * Ry(yaw). Products of two 1.3.12 values are shifted back
     * down by 12 to stay in 1.3.12. */
    m[0][0] = (s16)cy;
    m[0][1] = 0;
    m[0][2] = (s16)(-sy);

    m[1][0] = (s16)(((s64)sp * sy) >> Q2_FRAC_12);
    m[1][1] = (s16)cp;
    m[1][2] = (s16)(((s64)sp * cy) >> Q2_FRAC_12);

    m[2][0] = (s16)(((s64)cp * sy) >> Q2_FRAC_12);
    m[2][1] = (s16)(-sp);
    m[2][2] = (s16)(((s64)cp * cy) >> Q2_FRAC_12);
}

void q2_rotation_view(s16 m[3][3], s32 yaw, s32 pitch, s32 roll)
{
    s16 base[3][3];
    s32 sr, cr;
    int c;

    q2_rotation_yaw_pitch(base, yaw, pitch);

    if (roll == 0) {
        int r;
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                m[r][c] = base[r][c];
        return;
    }

    sr = q2_sin12(roll);
    cr = q2_cos12(roll);

    /*
     * Rz(roll) * base. Only the first two rows change: a rotation about the view
     * axis mixes screen X and screen Y and leaves depth alone, which is exactly
     * why the roll can be applied here instead of inside the basis and still be
     * the console's.
     */
    for (c = 0; c < 3; c++) {
        s32 x = base[0][c];
        s32 y = base[1][c];

        m[0][c] = (s16)((( (s64)cr * x) - ((s64)sr * y)) >> Q2_FRAC_12);
        m[1][c] = (s16)((( (s64)sr * x) + ((s64)cr * y)) >> Q2_FRAC_12);
        m[2][c] = base[2][c];
    }
}

void q2_rotation_euler(s16 m[3][3], s32 rx, s32 ry, s32 rz)
{
    s32 sx = q2_sin12(rx), cx = q2_cos12(rx);
    s32 sy = q2_sin12(ry), cy = q2_cos12(ry);
    s32 sz = q2_sin12(rz), cz = q2_cos12(rz);

    /* R = Rz * Ry * Rx, expanded so each entry costs at most two 1.3.12
     * products. Written out rather than composed from three matrices because
     * the intermediate rounding of a composed form differs from the original's
     * and this is meant to match it. */
    s32 sxsy = ((s64)sx * sy) >> Q2_FRAC_12;
    s32 cxsy = ((s64)cx * sy) >> Q2_FRAC_12;

    m[0][0] = (s16)(((s64)cz * cy) >> Q2_FRAC_12);
    m[0][1] = (s16)((((s64)cz * sxsy) >> Q2_FRAC_12) - (((s64)sz * cx) >> Q2_FRAC_12));
    m[0][2] = (s16)((((s64)cz * cxsy) >> Q2_FRAC_12) + (((s64)sz * sx) >> Q2_FRAC_12));

    m[1][0] = (s16)(((s64)sz * cy) >> Q2_FRAC_12);
    m[1][1] = (s16)((((s64)sz * sxsy) >> Q2_FRAC_12) + (((s64)cz * cx) >> Q2_FRAC_12));
    m[1][2] = (s16)((((s64)sz * cxsy) >> Q2_FRAC_12) - (((s64)cz * sx) >> Q2_FRAC_12));

    m[2][0] = (s16)(-sy);
    m[2][1] = (s16)(((s64)cy * sx) >> Q2_FRAC_12);
    m[2][2] = (s16)(((s64)cy * cx) >> Q2_FRAC_12);
}

void q2_quat_to_matrix(s16 m[3][3], const s16 q[4])
{
    /* Each product of two 1.3.12 values is brought back to 1.3.12 by >> 12
     * before the quaternion form's doubling, so no term can overflow s16 for a
     * unit quaternion. */
    s32 x = q[0], y = q[1], z = q[2], w = q[3];

    s32 xx = (x * x) >> Q2_FRAC_12, yy = (y * y) >> Q2_FRAC_12;
    s32 zz = (z * z) >> Q2_FRAC_12;
    s32 xy = (x * y) >> Q2_FRAC_12, xz = (x * z) >> Q2_FRAC_12;
    s32 yz = (y * z) >> Q2_FRAC_12;
    s32 wx = (w * x) >> Q2_FRAC_12, wy = (w * y) >> Q2_FRAC_12;
    s32 wz = (w * z) >> Q2_FRAC_12;

    m[0][0] = (s16)(Q2_ONE_12 - 2 * (yy + zz));
    m[0][1] = (s16)(2 * (xy - wz));
    m[0][2] = (s16)(2 * (xz + wy));

    m[1][0] = (s16)(2 * (xy + wz));
    m[1][1] = (s16)(Q2_ONE_12 - 2 * (xx + zz));
    m[1][2] = (s16)(2 * (yz - wx));

    m[2][0] = (s16)(2 * (xz - wy));
    m[2][1] = (s16)(2 * (yz + wx));
    m[2][2] = (s16)(Q2_ONE_12 - 2 * (xx + yy));
}

/* ------------------------------------------------------------------------- */
void q2_rotation_aspect_x(s16 m[3][3], s32 scale_12)
{
    int c;

    if (!m || scale_12 == 4096)
        return;

    for (c = 0; c < 3; c++) {
        s32 v = ((s32)m[0][c] * scale_12) >> 12;

        /* The row stays s16 because the GTE's registers are. At the console's
         * own 1.5484 the largest entry reaches 6342, well inside the range; the
         * clamp is here so an absurd viewport cannot corrupt the matrix. */
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        m[0][c] = (s16)v;
    }
}
