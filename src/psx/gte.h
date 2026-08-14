/*
 * gte.h — the PlayStation Geometry Transformation Engine, reimplemented exactly.
 *
 * This is the single most important file for visual fidelity.
 *
 * The GTE is COP2 on the R3000A: a fixed-point vector coprocessor that does the
 * perspective transform, backface test, depth cueing and per-vertex lighting. Its
 * output screen coordinates are *16-bit integers with no fractional part*, and its
 * perspective divide uses a Newton-Raphson reciprocal with a deliberately small
 * lookup table. Those two facts together produce the PlayStation's signature
 * vertex wobble: as a model moves smoothly through the world, its projected
 * vertices snap between whole pixels, and they snap slightly *wrong* because the
 * reciprocal is approximate.
 *
 * You cannot get that look by transforming in float and rounding at the end — the
 * error distribution is different. So the whole pipeline is reproduced here,
 * including saturation flags, so that a vertex fed through this code lands on the
 * same pixel it landed on in 1999.
 *
 * All of this is derived from publicly documented hardware behaviour and written
 * from scratch; no Sony or Hammerhead code is present.
 */
#ifndef Q2PSX_GTE_H
#define Q2PSX_GTE_H

#include "fixed.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Vector types as the GTE sees them.                                         */
/* ------------------------------------------------------------------------- */
typedef struct gte_vec16 {   /* V0..V2 — model-space vertex, s16 components */
    s16 x, y, z;
    s16 pad;
} gte_vec16;

typedef struct gte_vec32 {   /* translation vectors, MAC results */
    s32 x, y, z;
} gte_vec32;

typedef struct gte_matrix {  /* 1.3.12 rotation / light / colour matrix */
    s16 m[3][3];
} gte_matrix;

typedef struct gte_sxy {     /* projected screen coordinate — integer pixels */
    s16 x, y;
} gte_sxy;

typedef struct gte_rgbc {
    u8 r, g, b, c;
} gte_rgbc;

/* ------------------------------------------------------------------------- */
/* Saturation / overflow flag bits (control register 31).                      */
/* Bit 31 is the sticky "any error" summary, set from the mask below.          */
/* ------------------------------------------------------------------------- */
enum {
    GTE_FLAG_IR0_SAT       = 1u << 12,
    GTE_FLAG_SY2_SAT       = 1u << 13,
    GTE_FLAG_SX2_SAT       = 1u << 14,
    GTE_FLAG_MAC0_NEG      = 1u << 15,
    GTE_FLAG_MAC0_POS      = 1u << 16,
    GTE_FLAG_DIV_OVERFLOW  = 1u << 17,
    GTE_FLAG_SZ3_SAT       = 1u << 18,
    GTE_FLAG_COLOR_B_SAT   = 1u << 19,
    GTE_FLAG_COLOR_G_SAT   = 1u << 20,
    GTE_FLAG_COLOR_R_SAT   = 1u << 21,
    GTE_FLAG_IR3_SAT       = 1u << 22,
    GTE_FLAG_IR2_SAT       = 1u << 23,
    GTE_FLAG_IR1_SAT       = 1u << 24,
    GTE_FLAG_MAC3_NEG      = 1u << 25,
    GTE_FLAG_MAC2_NEG      = 1u << 26,
    GTE_FLAG_MAC1_NEG      = 1u << 27,
    GTE_FLAG_MAC3_POS      = 1u << 28,
    GTE_FLAG_MAC2_POS      = 1u << 29,
    GTE_FLAG_MAC1_POS      = 1u << 30,
    GTE_FLAG_ERROR         = 1u << 31,

    /* Bits that feed the sticky error summary. */
    GTE_FLAG_ERROR_MASK    = 0x7F87E000u
};

/* ------------------------------------------------------------------------- */
/* Machine state. One instance per rendering context.                         */
/* ------------------------------------------------------------------------- */
typedef struct gte_state {
    /* --- data registers ------------------------------------------------- */
    gte_vec16  v[3];        /* VXY0/VZ0 .. VXY2/VZ2                          */
    gte_rgbc   rgbc;        /* RGBC                                          */
    u16        otz;         /* average Z, 1/8 scale — the OT bucket index    */
    s16        ir0;         /* 0.8.8 depth-cue interpolation factor          */
    s16        ir[3];       /* IR1..IR3, 1.3.12                              */
    gte_sxy    sxy[3];      /* SXY0..SXY2 — 3-deep FIFO, integer pixels      */
    u16        sz[4];       /* SZ0..SZ3 — 4-deep FIFO                        */
    gte_rgbc   rgb_fifo[3]; /* RGB0..RGB2                                    */
    s32        res1;        /* unused hardware register, preserved           */
    s32        mac0;
    s32        mac[3];      /* MAC1..MAC3                                    */
    s32        lzcs, lzcr;

    /* --- control registers ---------------------------------------------- */
    gte_matrix rot;         /* R11..R33, 1.3.12                              */
    gte_vec32  tr;          /* TRX/TRY/TRZ, 1.19.12                          */
    gte_matrix light;       /* L11..L33                                      */
    gte_vec32  bk;          /* RBK/GBK/BBK background colour                 */
    gte_matrix colour;      /* LR1..LB3                                      */
    gte_vec32  fc;          /* RFC/GFC/BFC far colour                        */
    s32        ofx, ofy;    /* screen offset, 1.15.16                        */
    u16        h;           /* projection plane distance                     */
    s16        dqa;         /* depth-cue scale                               */
    s32        dqb;         /* depth-cue offset, 1.15.16                     */
    s16        zsf3, zsf4;  /* average-Z scale factors                       */
    u32        flag;        /* FLAG                                          */
} gte_state;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * What `InitGeom` leaves in the average-Z scale factors — 341 and 256, written
 * by the `ctc2` pair at 0x8008E4C8 and 0x8008E4D4, and never overridden
 * anywhere in the image. Both work out to a quarter of the mean vertex depth,
 * which is libgte's default for a 1024-entry table and is far coarser than this
 * game's 51-entry viewport slice — one of the reasons to believe the world's
 * draw order comes from `SortData` rather than from the GTE.
 */
#define GTE_ZSF3_INIT 341
#define GTE_ZSF4_INIT 256

void gte_init(gte_state *g);

/* Load the identity transform with a given projection distance and screen
 * centre. `h` is the projection plane distance — on PSX Quake II this is
 * effectively the field of view control. */
void gte_set_projection(gte_state *g, u16 h, s32 centre_x, s32 centre_y);

/* Set the rotation matrix / translation used by RTPS and RTPT. */
void gte_set_rotation(gte_state *g, const gte_matrix *m);
void gte_set_translation(gte_state *g, s32 x, s32 y, s32 z);

/*
 * The three lighting control registers, named after the libgte calls the
 * original makes: SetLightMatrix (0x8008A1A8), SetColorMatrix (0x8008AD2C) and
 * SetBackColor (0x8008AD0C).
 *
 * The light matrix holds one light DIRECTION per row; the colour matrix holds
 * one light COLOUR per column, so row 0 is the three lights' red components.
 * That transposition is the hardware's, not a convention chosen here — see
 * lighting.c, which fills both from the original's own layout.
 *
 * gte_set_back_colour takes 0..255 components and shifts them left by 4, which
 * is what libgte's SetBackColor does, so that a back colour of 255 contributes
 * the same 4080 that a fully-lit light does.
 */
void gte_set_light_matrix(gte_state *g, const gte_matrix *m);
void gte_set_colour_matrix(gte_state *g, const gte_matrix *m);
void gte_set_back_colour(gte_state *g, s32 r, s32 gr, s32 b);

/* ------------------------------------------------------------------------- */
/* Operations. Names match the hardware mnemonics.                            */
/* ------------------------------------------------------------------------- */

/* RTPS — perspective-transform V0. Pushes one result through the SXY/SZ FIFOs. */
void gte_rtps(gte_state *g, bool set_mac0_from_depth_cue);

/* RTPT — perspective-transform V0, V1 and V2 (one triangle) in one go. */
void gte_rtpt(gte_state *g);

/* NCLIP — signed area of the triangle in SXY0..SXY2, into MAC0.
 * Negative means back-facing under the PSX's winding convention. */
void gte_nclip(gte_state *g);

/* AVSZ3 / AVSZ4 — average the SZ FIFO into OTZ, the ordering-table index.
 * This is how the PlayStation sorts polygons: there is no depth buffer, so the
 * OT bucket a primitive lands in *is* its depth. */
void gte_avsz3(gte_state *g);
void gte_avsz4(gte_state *g);

/* MVMVA — general matrix/vector multiply-accumulate. `sf` selects the 12-bit
 * shift; the mx/vx/tx selectors match the hardware's encoding. */
void gte_mvmva(gte_state *g, int sf, int mx, int vx, int tx, int lm);

/* ------------------------------------------------------------------------- */
/* The lighting operations.                                                   */
/*                                                                            */
/* Every one of them starts the same way: the vertex register is read as a     */
/* NORMAL, the light matrix turns it into three N-dot-L terms (negatives       */
/* clamped away, which is the hardware's `lm` bit doing the "facing away means */
/* unlit" test for free), and the colour matrix turns those three terms into   */
/* an RGB triple around the back colour. The result is pushed through the      */
/* colour FIFO, so RGB2 always holds the newest vertex.                        */
/*                                                                            */
/* They differ only in what happens afterwards:                                */
/*                                                                            */
/*   NC    nothing — the light IS the colour. This is what Quake II PSX uses   */
/*         for models (NCT at 0x800B23F0), which is why a model's texture is   */
/*         modulated purely by the light reaching it.                         */
/*   NCC   the result is multiplied by the primitive colour in RGBC.           */
/*   NCD   the result is multiplied by RGBC and then depth-cued toward the far */
/*         colour by IR0.                                                     */
/*                                                                            */
/* The S forms shade V0; the T forms shade V0, V1 and V2 in one operation.     */
/* ------------------------------------------------------------------------- */
void gte_ncs(gte_state *g);
void gte_nct(gte_state *g);
void gte_nccs(gte_state *g);
void gte_ncct(gte_state *g);
void gte_ncds(gte_state *g);
void gte_ncdt(gte_state *g);

/* CC / CDP — the same colour stages without the light-matrix stage, taking the
 * N-dot-L terms already sitting in IR1..IR3. */
void gte_cc(gte_state *g);
void gte_cdp(gte_state *g);

/* SQR — square IR1..IR3. */
void gte_sqr(gte_state *g, int sf);

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

/* The hardware's perspective divide, exposed for testing. Returns the clamped
 * quotient (H*0x10000/SZ3 rounded), saturating at 0x1FFFF and raising
 * GTE_FLAG_DIV_OVERFLOW. This approximation is a visible part of the look. */
u32 gte_divide(gte_state *g, u16 h, u16 sz3);

/* Convenience: transform one world-space vertex and return the integer screen
 * position plus the depth used for OT bucketing. Returns false if the vertex is
 * behind the projection plane. */
bool gte_project_point(gte_state *g, s32 wx, s32 wy, s32 wz,
                       gte_sxy *out_screen, u16 *out_z);

#endif /* Q2PSX_GTE_H */
