/*
 * fixed.h — the fixed-point formats the PlayStation geometry pipeline uses.
 *
 * These are not a convenience layer. Reproducing the original look depends on
 * arithmetic being *identical* to the hardware's, including truncation direction
 * and saturation, so every quantity that was fixed-point on the PSX stays
 * fixed-point here. Floating point is confined to the host presentation layer.
 *
 * Formats, in the notation "sign.integer.fraction":
 *
 *   1.3.12   — rotation/light/colour matrix coefficients, and IR1..IR3 results.
 *              Range [-8, +8), step 1/4096.
 *   1.19.12  — translation vector, MAC accumulators. Range [-524288, +524288).
 *   1.15.16  — screen offsets OFX/OFY and the DQB depth-cue bias.
 *   0.8.8    — the interpolation factor IR0 uses in depth cueing.
 *
 * World coordinates in Quake II PSX are plain integers in the same unit system as
 * PC Quake II (player is 56 units tall); the fractional formats appear once the
 * GTE gets involved.
 */
#ifndef Q2PSX_FIXED_H
#define Q2PSX_FIXED_H

#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Shift counts                                                               */
/* ------------------------------------------------------------------------- */
#define Q2_FRAC_12   12
#define Q2_FRAC_16   16
#define Q2_ONE_12    (1 << Q2_FRAC_12)   /* 4096 */
#define Q2_ONE_16    (1 << Q2_FRAC_16)   /* 65536 */

typedef s32 q12_t;   /* 1.19.12 */
typedef s32 q16_t;   /* 1.15.16 */

Q2PSX_INLINE q12_t q2_q12_from_int(s32 v)  { return (q12_t)(v << Q2_FRAC_12); }
Q2PSX_INLINE s32   q2_q12_to_int(q12_t v)  { return v >> Q2_FRAC_12; }

/* Products of two 1.x.12 values need a 64-bit intermediate: the GTE's MAC
 * registers are 44-bit internally and only saturate on write-back. */
Q2PSX_INLINE q12_t q2_q12_mul(q12_t a, q12_t b)
{
    return (q12_t)(((s64)a * (s64)b) >> Q2_FRAC_12);
}

Q2PSX_INLINE q12_t q2_q12_div(q12_t a, q12_t b)
{
    if (b == 0)
        return (a < 0) ? INT32_MIN : INT32_MAX;
    return (q12_t)(((s64)a << Q2_FRAC_12) / b);
}

/* ------------------------------------------------------------------------- */
/* Saturation helpers — these mirror the GTE's clamp behaviour exactly.        */
/* ------------------------------------------------------------------------- */
Q2PSX_INLINE s32 q2_clamp_s32(s64 v, s32 lo, s32 hi)
{
    if (v < (s64)lo) return lo;
    if (v > (s64)hi) return hi;
    return (s32)v;
}

Q2PSX_INLINE s32 q2_clamp_s16(s64 v)  { return q2_clamp_s32(v, -32768, 32767); }
Q2PSX_INLINE s32 q2_clamp_u16(s64 v)  { return q2_clamp_s32(v, 0, 65535); }
Q2PSX_INLINE s32 q2_clamp_u8(s64 v)   { return q2_clamp_s32(v, 0, 255); }

/* Count leading zeros on a 32-bit value, matching the GTE's LZCS/LZCR op.
 * Portable rather than intrinsic: this is not hot enough to matter. */
Q2PSX_INLINE s32 q2_count_leading_zeros_u32(u32 v)
{
    s32 n = 0;
    if (v == 0)
        return 32;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        n++;
    }
    return n;
}

#endif /* Q2PSX_FIXED_H */
