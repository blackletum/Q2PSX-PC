/*
 * screen.h — the screen: display environments, double buffering, viewports,
 * the ordering table's shape, and the order a frame is put together in.
 *
 * ---------------------------------------------------------------------------
 * What this module is
 * ---------------------------------------------------------------------------
 * Everything between "the game has decided what to draw" and "a field is on the
 * television". On the console that is four cooperating pieces:
 *
 *   - a DISPENV / DRAWENV pair per buffer, describing which rectangle of VRAM
 *     is being shown and which is being drawn into;
 *   - one ordering table, carved into fixed slices — one per viewport, plus a
 *     background slice and an overlay slice — so that split screen is expressed
 *     as *where in the OT* a primitive lands rather than as a second pass;
 *   - a set of viewport layouts, one per player count, each of which is a table
 *     of literal constants in the executable rather than anything computed;
 *   - a frame lock: DrawSync(0), VSync(2), PutDispEnv, DrawOTag, in that order.
 *
 * All of it was read out of `SLES_015.34` and is transcribed here with the
 * address it came from. `q2psx-inspect screen <disc>` reads the same values
 * back off a disc and compares them to this file, so a wrong constant here is
 * a test failure rather than a subtly wrong picture.
 *
 * ---------------------------------------------------------------------------
 * The one thing that is NOT modelled, and why
 * ---------------------------------------------------------------------------
 * On hardware the two framebuffers are rectangles *inside* VRAM — (0,0) and
 * (512,0), with the texture pages living from y = 256 down. This module keeps
 * two standalone `psx_framebuffer`s instead and records where each one would
 * have been (`q2_screen_buffer_origin`). Nothing that has been reconstructed so
 * far reads the framebuffer back as a texture, so the distinction is currently
 * unobservable; if a screen wipe or a feedback effect ever turns up, this is
 * the seam it will need.
 */
#ifndef Q2PSX_SCREEN_H
#define Q2PSX_SCREEN_H

#include "gpu.h"
#include "gte.h"
#include "ident.h"
#include "q2psx.h"
#include "raster.h"

/* ------------------------------------------------------------------------- */
/* The display state — 0x800764DC, the display-init function called from main  */
/* ------------------------------------------------------------------------- */
/*
 * Framebuffer dimensions are runtime state, not constants: exactly one writer
 * (this function) and 42 readers. `SetVideoMode(1)` four instructions earlier
 * is the only call site in the image, which is what makes the video standard a
 * property the executable states rather than one a port infers from a serial.
 *
 * NTSC values are NOT known and must not be guessed — PAL turned out to be 248
 * lines rather than the widely repeated 256, so the folklore 240 is less
 * trustworthy now, not more. Asking for an NTSC screen yields the PAL geometry
 * with `height_is_inferred` set, so a caller can tell.
 */
#define Q2_SCREEN_PAL_WIDTH   512   /* 0x800B2DA0, stored at 0x800764F0 */
#define Q2_SCREEN_PAL_HEIGHT  248   /* 0x800B2DA2, stored at 0x800764FC */

/*
 * VRAM origins of the two buffers, the {s16 x, s16 y} table at 0x800B2EF4.
 * Buffer 0 draws at x = 0 while buffer 1 is displayed, and vice versa — the
 * draw env takes its origin from this table indexed by the *draw* buffer, and
 * the display env of the *other* buffer is what reaches PutDispEnv.
 */
typedef struct q2_screen_buf_origin {
    s16 x, y;
} q2_screen_buf_origin;

typedef struct q2_screen_display {
    u16 width, height;          /* 0x800B2DA0 / 0x800B2DA2                    */
    u32 video_mode;             /* 0x800A9E70 — 1 == MODE_PAL                 */
    u16 field_hz;               /* 50 on PAL, from SetVideoMode(1)            */
    u16 vsync_divisor;          /* the literal 2 at 0x80018974                */

    q2_screen_buf_origin buf[2];/* 0x800B2EF4, stride 4                       */

    u8  draw_buffer;            /* 0x800B2738                                 */
    u8  disp_buffer;            /* 0x800B2739 — always 1 - draw_buffer        */
    u8  rotate[3];              /* 0x800B273C..0x800B273E, see q2_screen_swap */

    u8  bg_rgb[3];              /* gp+1604 (0x800AEC44) — the clear colour    */
    u8  bg_enable;              /* gp+18732 (0x800B2F2C) — the clear's isbg   */

    bool height_is_inferred;    /* true when asked for a build we cannot read */
} q2_screen_display;

/* ------------------------------------------------------------------------- */
/* The double-buffer block — 0x800B3730, stride 32408                          */
/* ------------------------------------------------------------------------- */
/*
 * Offsets into one buffer's block. A port does not need to lay memory out this
 * way, but the numbers are how every "db + N" in the disassembly reads, and the
 * OT offsets in particular are load-bearing: the slice arithmetic below is what
 * makes split screen work.
 */
#define Q2_SCREEN_DB_BASE          0x800B3730u
#define Q2_SCREEN_DB_STRIDE            32408u
#define Q2_SCREEN_DRAWENV_SIZE            92u
#define Q2_SCREEN_DISPENV_SIZE            20u
#define Q2_SCREEN_DB_DRAWENV_BG          436u  /* the full-screen background   */
#define Q2_SCREEN_DB_DRAWENV_VIEW        528u  /* + 92*p, four of them         */
#define Q2_SCREEN_DB_DRAWENV_OVERLAY     896u  /* 528 + 4*92                   */
#define Q2_SCREEN_DB_DISPENV           10940u
#define Q2_SCREEN_DB_OT                10984u

/* ------------------------------------------------------------------------- */
/* The ordering table                                                          */
/* ------------------------------------------------------------------------- */
/*
 * `ClearOTag(db + 10984, 217)` at 0x80018398 — forward order, so bucket 0 is
 * drawn first and higher buckets land on top of it. The 217 is not arbitrary;
 * it is exactly what the slices below add up to, which is the strongest
 * evidence that the slicing is real:
 *
 *      1   root                                   OT[0]
 *      1   the full-screen background env         OT[1]
 *    204   four viewport slices of 51             OT[2 .. 205]
 *     11   the overlay slice                      OT[206 .. 216]
 *      = 217
 *
 * The 51 is confirmed independently: the boot display path clears exactly 51
 * entries at db + 10992 (0x8006E188 / 0x8006E194).
 *
 * Within a slice the draw env sits at slice bucket 1, not 0 — the env packet is
 * AddPrim'd last (0x80076D44) so it draws first inside its bucket, and slice
 * bucket 0 is left for whatever wants to precede the clip change.
 */
#define Q2_SCREEN_OT_ENTRIES        217
#define Q2_SCREEN_OT_BACKGROUND       1
#define Q2_SCREEN_OT_VIEW_BASE        2
#define Q2_SCREEN_OT_VIEW_STRIDE     51
#define Q2_SCREEN_OT_VIEW_ENV         1   /* env bucket within a slice        */
#define Q2_SCREEN_OT_OVERLAY        206
#define Q2_SCREEN_OT_OVERLAY_LEN     11
#define Q2_SCREEN_MAX_VIEWS           4

/* ------------------------------------------------------------------------- */
/* A viewport                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * The fields the screen owns inside the 784-byte per-player view record at
 * 0x800D5C30. Offsets are kept in the comments because they are how the layout
 * functions and the per-frame draw at 0x80076A74 talk to each other.
 */
typedef struct q2_screen_view {
    s16 x, y;            /* +270 / +272  origin inside the framebuffer       */
    s16 w, h;            /* +274 / +276  size                                */
    s16 vw, vh;          /* +278 / +280  the 2D extent; differs on 2P-H      */
    s16 ofs_x, ofs_y;    /* +266 / +268  GTE SetGeomOffset, always (w/2,h/2) */
    s16 proj;            /* +262         GTE SetGeomScreen — the FOV control */
    s16 far_z;           /* +264         parked in 0x800B2CCC, /4 in ..2CC8  */
    s16 aspect;          /* +282         (w << 9) / fb_w or fb_h, see below  */
    s16 depth_scale;     /* +156         6144 split, 8192 full screen        */
    s16 pad_a, pad_b;    /* +304 / +306                                      */

    u8  bg_rgb[3];       /* +256..258    the viewport's own clear colour     */
    u8  bg_enable;       /* +260         isbg; cleared when the full-screen
                          *              background env is doing the clear   */

    s16 shake_x, shake_y;/* 0x800B2C34 / 0x800B2C36, copied from view+780    */
} q2_screen_view;

/* ------------------------------------------------------------------------- */
/* Layouts                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * One per setup function. Which one runs is chosen by the session mode at
 * 0x800B3356 through the jump table at 0x800AC90C (0x8003F8D8):
 *
 *   mode 0, 1 -> ONE          (0x80077D0C)
 *   mode 2    -> TWO_H or TWO_V, on HORIZONTAL SPLIT at 0x800B333C
 *   mode 3    -> QUAD with the view count forced to 3 (0x8003FAE4)
 *   mode 4    -> QUAD         (0x8007771C)
 *
 * FULL_SINGLE (0x80077540) is not a session layout: it is the boot-time
 * full-screen setup, and it is the only one that leaves both display envs
 * pointing at VRAM (0,0), i.e. single-buffered.
 */
typedef enum q2_screen_layout {
    Q2_SCREEN_LAYOUT_ONE = 0,     /* 0x80077D0C — 512x248, double buffered   */
    Q2_SCREEN_LAYOUT_TWO_H,       /* 0x80077900 — stacked, HORIZONTAL SPLIT  */
    Q2_SCREEN_LAYOUT_TWO_V,       /* 0x80077AEC — side by side               */
    Q2_SCREEN_LAYOUT_QUAD,        /* 0x8007771C — 2x2, used for 3 and 4      */
    Q2_SCREEN_LAYOUT_FULL_SINGLE, /* 0x80077540 — boot, single buffered      */
    Q2_SCREEN_LAYOUT_COUNT
} q2_screen_layout;

const char *q2_screen_layout_name(q2_screen_layout l);

/* ------------------------------------------------------------------------- */
/* Why the game loop stopped — 0x800B2E28, dispatched at 0x8001860C            */
/* ------------------------------------------------------------------------- */
/*
 * The in-game loop at 0x800182C8 runs until this halfword goes non-zero, then
 * returns it. Everything that leaves the world — dying, restarting, the mission
 * screen, quitting — is expressed as one of these, which is why the screen owns
 * it: it is the thing that decides what is on screen next.
 *
 * The names come from what each dispatch arm calls. Values with no arm of their
 * own (2, 3, 4, 9) are returned to the outer state machine at 0x80018868 and
 * are NOT identified here rather than guessed at.
 */
typedef enum q2_screen_exit {
    Q2_SCREEN_EXIT_NONE       = 0,   /* keep running                          */
    Q2_SCREEN_EXIT_LEVEL_DONE = 1,   /* 0x80041548 when the session is mode 2 */
    Q2_SCREEN_EXIT_5          = 5,   /* 0x800411C8                            */
    Q2_SCREEN_EXIT_FRONTEND   = 6,   /* 0x8007CCE0(0)                         */
    Q2_SCREEN_EXIT_7          = 7,   /* 0x80018ED8                            */
    Q2_SCREEN_EXIT_8          = 8,   /* 0x8004149C                            */
    Q2_SCREEN_EXIT_10         = 10,  /* 0x80041300; also forced at boot       */
    Q2_SCREEN_EXIT_11         = 11,  /* 0x800415F4                            */
    Q2_SCREEN_EXIT_FLAG_12    = 12,  /* sets 0x800B2A54                       */
    Q2_SCREEN_EXIT_FLAG_13    = 13,  /* sets 0x800B2A50                       */
    Q2_SCREEN_EXIT_FLAG_14    = 14,  /* sets 0x800B2A5C                       */
    Q2_SCREEN_EXIT_FLAG_15    = 15,  /* sets 0x800B2A88                       */
    Q2_SCREEN_EXIT_16         = 16,  /* 0x80041974                            */
    Q2_SCREEN_EXIT_FRONTEND_1 = 17,  /* 0x8007CCE0(1), then becomes 6         */
    Q2_SCREEN_EXIT_FRONTEND_2 = 18,  /* 0x8007CCE0(2), then becomes 6         */
    Q2_SCREEN_EXIT_19         = 19,  /* 0x80041958                            */
    Q2_SCREEN_EXIT_20         = 20,  /* 0x80040608(1)                         */
    Q2_SCREEN_EXIT_21         = 21   /* 0x80040608(0)                         */
} q2_screen_exit;

/* ------------------------------------------------------------------------- */
/* Timing — 0x800182C8                                                         */
/* ------------------------------------------------------------------------- */
/*
 * `dt` is in 1/300 s units and is clamped, not averaged: `slti 31` at
 * 0x800184B8 with 30 in the delay slot. VSync(2) at 50 Hz is 12 units, so the
 * clamp bites at two and a half dropped fields.
 */
#define Q2_SCREEN_DT_NOMINAL   12
#define Q2_SCREEN_DT_MAX       30

/* ------------------------------------------------------------------------- */
/* The screen                                                                  */
/* ------------------------------------------------------------------------- */
typedef struct q2_screen {
    q2_screen_display disp;

    q2_screen_view    view[Q2_SCREEN_MAX_VIEWS];
    int               view_count;      /* 0x800B2C2C                         */
    q2_screen_layout  layout;

    q2_screen_view    overlay;         /* the view at 0x800D6870             */
    bool              overlay_armed;   /* 0x80077230 ran this frame          */
    bool              background_armed;/* 0x800769A0 ran this frame          */

    /* The two buffers. `buf[disp.draw_buffer]` is the one being drawn. */
    psx_framebuffer   buf[2];

    q2_screen_exit    exit_code;       /* 0x800B2E28                         */
    s32               dt;              /* 0x800B2DB4, clamped                */
    u32               frame;
} q2_screen;

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Reproduces 0x800764DC: video mode, framebuffer size, buffer indices, the
 * three-slot rotation, the display envs from 0x80077454, and the boot-time
 * full-screen single-buffered view from 0x80077540 — which is the state the
 * front end runs in before a session picks a layout.
 *
 * Allocates the two framebuffers, so it can fail.
 */
q2_result q2_screen_init(q2_screen *s, q2_video_std video);
void      q2_screen_free(q2_screen *s);

/* Install one of the layouts. `players` is honoured only for QUAD, where the
 * session code sets 3 after the layout has written 4 (0x8003FAE4). */
void q2_screen_set_layout(q2_screen *s, q2_screen_layout layout, int players);

/* Pick the layout the session mode would pick. `horizontal_split` is the menu
 * setting at 0x800B333C and is consulted only for two players. */
q2_screen_layout q2_screen_layout_for(int players, bool horizontal_split);

/* Where in VRAM buffer `index` lives — the model this module does not itself
 * enforce. See the header comment. */
void q2_screen_buffer_origin(const q2_screen *s, int index, int *x, int *y);

/* ------------------------------------------------------------------------- */
/* One frame                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * The order below is the order in 0x800182C8's loop body, and it matters: the
 * swap happens *before* the table is cleared and the world is built, so a frame
 * is always built into the buffer that is not being shown.
 *
 *      q2_screen_frame_begin(s, ot);            swap + ClearOTag(.., 217)
 *      q2_screen_background(s);                 optional, see below
 *      for each viewport p:
 *          q2_screen_view_begin(s, p, gte);     GTE offset/screen, env armed
 *          ... build geometry into q2_screen_view_otz(s, p, z) ...
 *      q2_screen_overlay_begin(s, gte);         the HUD/menu camera
 *      ... build the overlay into q2_screen_overlay_otz(s, z) ...
 *      q2_screen_compose(s, ot, vram, opts);    walk the table, apply envs
 *      q2_screen_present(s);                    flip
 */

/* Rotate the buffers (0x80018214) and clear the ordering table. */
void q2_screen_frame_begin(q2_screen *s, psx_ot *ot);

/*
 * Arm the single full-screen background clear (0x800769A0) and, exactly as
 * 0x800780C0 does immediately afterwards, turn every viewport's own clear off
 * so the screen is cleared once rather than once per viewport.
 */
void q2_screen_background(q2_screen *s);

/*
 * Prepare viewport `p`: load the GTE's projection distance and screen offset
 * from the view, and restrict the ordering table to that viewport's slice so an
 * emitter can add primitives without knowing the slicing — which is exactly the
 * arrangement on the console, where the world renderer is handed the slice base
 * in 0x800B2D60. Returns false when `p` is not a live viewport.
 */
bool q2_screen_view_begin(q2_screen *s, int p, psx_ot *ot, gte_state *gte);

/* Same for the overlay camera at 0x800D6870 (0x80077230 / 0x80077EAC). */
void q2_screen_overlay_begin(q2_screen *s, psx_ot *ot, gte_state *gte);

/*
 * Map a depth bucket into a viewport's slice. `z` is clamped into
 * [0, 51) and biased past the slice's env bucket, so a caller can hand over a
 * raw GTE otz without having to know the slicing.
 */
u16 q2_screen_view_otz(const q2_screen *s, int p, u32 z);
u16 q2_screen_overlay_otz(const q2_screen *s, u32 z);

/*
 * Walk the ordering table exactly as DrawOTag would: bucket 0 upwards, and
 * within a bucket most-recently-added first. The draw-env packets sitting in
 * the table change the clip rectangle and, when their isbg is set, clear it —
 * so this one walk produces the background, every viewport and the overlay,
 * each correctly clipped, with no second pass.
 */
void q2_screen_compose(q2_screen *s, const psx_ot *ot,
                       const psx_vram *vram, const psx_raster_opts *opts);

/* Flip: the buffer just drawn becomes the buffer being shown. */
void q2_screen_present(q2_screen *s);

/* The buffer a host window should be showing. */
const psx_framebuffer *q2_screen_front(const q2_screen *s);
psx_framebuffer       *q2_screen_back(q2_screen *s);

/* Fold a real elapsed time into the console's clamped dt (0x800184B8). */
s32 q2_screen_tick_dt(q2_screen *s, double seconds);

#endif /* Q2PSX_SCREEN_H */
