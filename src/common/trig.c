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
