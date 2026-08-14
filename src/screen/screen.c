#include "screen.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */
/*
 * The layout functions all halve a dimension with the compiler's signed
 * shift-and-correct idiom (`sra 16; srl 31; addu; sra 1`), which is C's
 * division truncating toward zero. Reproduced literally so that an odd width —
 * the 255 of the vertical split — halves the way the console halves it.
 */
static s16 half(s16 v)
{
    return (s16)(v / 2);
}

/*
 * `(w << 9) / divisor`, the field at view+282. The divisor is the framebuffer
 * HEIGHT for the two full-screen layouts and the framebuffer WIDTH for the
 * three split ones — that asymmetry is in the code, not a transcription slip
 * (0x8007761C and 0x80077E0C read 0x800B2DA2; 0x80077824, 0x80077A30 and
 * 0x80077C2C read 0x800B2DA0).
 */
static s16 aspect_of(s16 w, u16 divisor)
{
    if (divisor == 0)
        return 0;
    return (s16)(((s32)w << 9) / (s32)divisor);
}

const char *q2_screen_layout_name(q2_screen_layout l)
{
    switch (l) {
    case Q2_SCREEN_LAYOUT_ONE:         return "one";
    case Q2_SCREEN_LAYOUT_TWO_H:       return "two-horizontal";
    case Q2_SCREEN_LAYOUT_TWO_V:       return "two-vertical";
    case Q2_SCREEN_LAYOUT_QUAD:        return "quad";
    case Q2_SCREEN_LAYOUT_FULL_SINGLE: return "full-single";
    default:                           return "?";
    }
}

/* ------------------------------------------------------------------------- */
/* Layouts                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Every one of these is a transcription of one setup function. The shared tail
 * — geometry offset at half the viewport, the aspect divide, the clear colour
 * copied out of gp+1604 and the isbg out of gp+18732 — is factored out here
 * because all five do it identically; everything that differs is spelled out at
 * the call site so it can be read against the disassembly.
 */
static void view_common(q2_screen *s, q2_screen_view *v, u16 aspect_divisor)
{
    v->ofs_x = half(v->w);
    v->ofs_y = half(v->h);
    v->aspect = aspect_of(v->w, aspect_divisor);

    v->bg_rgb[0] = s->disp.bg_rgb[0];
    v->bg_rgb[1] = s->disp.bg_rgb[1];
    v->bg_rgb[2] = s->disp.bg_rgb[2];
    v->bg_enable = s->disp.bg_enable;

    v->shake_x = 0;
    v->shake_y = 0;
}

/* 0x80077D0C — one player, the whole framebuffer, double buffered. */
static void layout_one(q2_screen *s)
{
    q2_screen_view *v = &s->view[0];

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;               s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    v->x = 0;  v->y = 0;
    v->w = (s16)s->disp.width;
    v->h = (s16)s->disp.height;
    v->vw = v->w; v->vh = v->h;
    v->depth_scale = 8192;              /* 0x80077D9C */
    v->proj   = 160;                    /* 0x80077E50 */
    v->far_z  = 6400;                   /* 0x80077E58 */
    v->pad_a  = 93;                     /* 0x80077E60 */
    v->pad_b  = 201;                    /* 0x80077E6C */
    view_common(s, v, s->disp.height);

    s->view_count = 1;
}

/*
 * 0x80077540 — the boot-time full-screen view. Two things make it its own
 * layout rather than a duplicate of ONE: the projection distance is 320 rather
 * than 160, and it leaves BOTH buffer origins at (0,0) and writes both display
 * envs to (0,0) as well, i.e. it is single buffered. That is the state the
 * front end runs in.
 */
static void layout_full_single(q2_screen *s)
{
    q2_screen_view *v = &s->view[0];

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0; s->disp.buf[0].y = 0;
    s->disp.buf[1].x = 0; s->disp.buf[1].y = 0;

    v->x = 0;  v->y = 0;
    v->w = (s16)s->disp.width;
    v->h = (s16)s->disp.height;
    v->vw = v->w; v->vh = v->h;
    v->depth_scale = 8192;              /* 0x800775C4 */
    v->proj   = 320;                    /* 0x8007766C */
    v->far_z  = 6400;                   /* 0x80077674 */
    v->pad_a  = 93;                     /* 0x8007767C */
    v->pad_b  = 201;                    /* 0x80077688 */
    view_common(s, v, s->disp.height);

    s->view_count = 1;
}

/*
 * 0x80077900 — two players stacked, which is what HORIZONTAL SPLIT selects.
 * Full framebuffer width; the height is `(fb_h + 1) / 2 - 1`, and the second
 * viewport starts at 121 rather than at 124, so the two overlap by three lines.
 * That overlap is in the constants (0x800779A0 and 0x80077ACC) and is kept.
 */
static void layout_two_h(q2_screen *s)
{
    int i;
    s16 h = (s16)(((s32)s->disp.height + 1) / 2 - 1);

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;                  s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    for (i = 0; i < 2; i++) {
        q2_screen_view *v = &s->view[i];
        v->w = (s16)s->disp.width;
        v->h = h;
        v->vw = 320;                    /* 0x800779BC — NOT the viewport size */
        v->vh = 160;                    /* 0x800779C4 */
        v->depth_scale = 6144;          /* 0x80077A6C */
        v->proj  = 160;                 /* 0x80077A24 */
        v->far_z = 6400;                /* 0x80077A28 */
        v->pad_a = 0;
        v->pad_b = 95;                  /* 0x80077A98 */
        view_common(s, v, s->disp.width);
    }

    s->view[0].x = 0;  s->view[0].y = 1;    /* 0x80077AC4 / 0x80077AC8 */
    s->view[1].x = 0;  s->view[1].y = 121;  /* 0x80077AD0 / 0x80077AD4 */

    s->view_count = 2;
}

/* 0x80077AEC — two players side by side. Width is `fb_w / 2 - 1`. */
static void layout_two_v(q2_screen *s)
{
    int i;
    s16 w = (s16)(half((s16)s->disp.width) - 1);

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;                  s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    for (i = 0; i < 2; i++) {
        q2_screen_view *v = &s->view[i];
        v->w = w;
        v->h = (s16)s->disp.height;
        v->vw = v->w; v->vh = v->h;
        v->depth_scale = 6144;          /* 0x80077C68 */
        v->proj  = 175;                 /* 0x80077C18 */
        v->far_z = 6400;                /* 0x80077C20 */
        v->pad_a = 0;
        v->pad_b = (s16)(s->disp.height - 32);  /* 0x80077C9C */
        view_common(s, v, s->disp.width);
    }

    s->view[0].x = 0;   s->view[0].y = 0;   /* memset at 0x80077CE4 */
    s->view[1].x = 256; s->view[1].y = 0;   /* 0x80077CF0 / 0x80077CF4 */

    s->view_count = 2;
}

/*
 * 0x8007771C — the 2x2 split, used for both three and four players. Note the
 * one-pixel inset: the quadrants start at 1 and 257 / 1 and 124, not at 0.
 */
static void layout_quad(q2_screen *s)
{
    int i;
    static const s16 qx[4] = { 1, 257, 1, 257 };
    static const s16 qy[4] = { 1, 1, 124, 124 };

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;                  s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    for (i = 0; i < 4; i++) {
        q2_screen_view *v = &s->view[i];
        v->w = 256;                     /* 0x80077794 */
        v->h = 123;                     /* 0x8007779C */
        v->vw = v->w; v->vh = v->h;
        v->depth_scale = 6144;          /* 0x8007786C */
        v->proj  = 160;                 /* 0x80077810 */
        v->far_z = 4000;                /* 0x80077818 */
        v->pad_a = 0;
        v->pad_b = 0;
        view_common(s, v, s->disp.width);
        v->x = qx[i];
        v->y = qy[i];
    }

    s->view_count = 4;
}

/* 0x80077EAC — the overlay camera at 0x800D6870, run by every layout. */
static void layout_overlay(q2_screen *s)
{
    q2_screen_view *v = &s->overlay;

    memset(v, 0, sizeof(*v));

    v->x = 0; v->y = 0;
    v->w = (s16)s->disp.width;
    v->h = (s16)s->disp.height;
    v->vw = v->w; v->vh = v->h;
    v->depth_scale = 8192;              /* 0x80077EF8 */
    v->proj = 320;                      /* 0x80077F90 */
    view_common(s, v, s->disp.height);
    v->bg_enable = 0;                   /* 0x80077FB4 — the overlay never clears */
}

void q2_screen_set_layout(q2_screen *s, q2_screen_layout layout, int players)
{
    if (!s)
        return;

    layout_overlay(s);

    switch (layout) {
    case Q2_SCREEN_LAYOUT_ONE:         layout_one(s);         break;
    case Q2_SCREEN_LAYOUT_TWO_H:       layout_two_h(s);       break;
    case Q2_SCREEN_LAYOUT_TWO_V:       layout_two_v(s);       break;
    case Q2_SCREEN_LAYOUT_QUAD:        layout_quad(s);        break;
    case Q2_SCREEN_LAYOUT_FULL_SINGLE: layout_full_single(s); break;
    default:                           layout_one(s); layout = Q2_SCREEN_LAYOUT_ONE; break;
    }

    /*
     * Three players use the four-quadrant layout with the count overridden
     * afterwards (0x8003FAE4 calls 0x8007771C, which writes 4, then stores 3).
     * The fourth quadrant is simply never drawn into and shows the background.
     */
    if (layout == Q2_SCREEN_LAYOUT_QUAD && players == 3)
        s->view_count = 3;

    s->layout = layout;
}

q2_screen_layout q2_screen_layout_for(int players, bool horizontal_split)
{
    if (players <= 1)
        return Q2_SCREEN_LAYOUT_ONE;
    if (players == 2)
        return horizontal_split ? Q2_SCREEN_LAYOUT_TWO_H : Q2_SCREEN_LAYOUT_TWO_V;
    return Q2_SCREEN_LAYOUT_QUAD;
}

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                   */
/* ------------------------------------------------------------------------- */
q2_result q2_screen_init(q2_screen *s, q2_video_std video)
{
    q2_result r;

    if (!s)
        return Q2_ERR_INVALID_ARG;

    memset(s, 0, sizeof(*s));

    /*
     * 0x800764DC. SetVideoMode(1) then 512 and 248 into 0x800B2DA0/DA2. An NTSC
     * build's equivalents have not been read, and PAL's 248 already refuted the
     * commonly repeated 256 — so rather than substitute one folklore number for
     * another, an NTSC request gets the PAL geometry and says so.
     */
    s->disp.width         = Q2_SCREEN_PAL_WIDTH;
    s->disp.height        = Q2_SCREEN_PAL_HEIGHT;
    s->disp.video_mode    = 1;
    s->disp.field_hz      = 50;
    s->disp.vsync_divisor = 2;
    s->disp.height_is_inferred = (video != Q2_VIDEO_PAL);

    /* 0x8007657C..0x80076590: the draw/display indices and the three-slot
     * rotation the swap advances alongside them. */
    s->disp.draw_buffer = 0;
    s->disp.disp_buffer = 1;
    s->disp.rotate[0] = 0;
    s->disp.rotate[1] = 1;
    s->disp.rotate[2] = 2;

    /* 0x80077454 sets the display envs cross-paired, buffer 0 showing x = 512.
     * The background colour and its enable both start cleared. */
    s->disp.buf[0].x = 0;
    s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width;
    s->disp.buf[1].y = 0;
    s->disp.bg_rgb[0] = s->disp.bg_rgb[1] = s->disp.bg_rgb[2] = 0;
    s->disp.bg_enable = 0;

    r = psx_fb_init(&s->buf[0], s->disp.width, s->disp.height);
    if (r != Q2_OK)
        return r;
    r = psx_fb_init(&s->buf[1], s->disp.width, s->disp.height);
    if (r != Q2_OK) {
        psx_fb_free(&s->buf[0]);
        return r;
    }

    /* The state the front end boots into (0x8006DFB8 -> 0x80077540). */
    q2_screen_set_layout(s, Q2_SCREEN_LAYOUT_FULL_SINGLE, 1);

    s->exit_code = Q2_SCREEN_EXIT_NONE;
    s->dt        = Q2_SCREEN_DT_NOMINAL;
    return Q2_OK;
}

void q2_screen_free(q2_screen *s)
{
    if (!s)
        return;
    psx_fb_free(&s->buf[0]);
    psx_fb_free(&s->buf[1]);
}

void q2_screen_buffer_origin(const q2_screen *s, int index, int *x, int *y)
{
    if (!s || index < 0 || index > 1) {
        if (x) *x = 0;
        if (y) *y = 0;
        return;
    }
    if (x) *x = s->disp.buf[index].x;
    if (y) *y = s->disp.buf[index].y;
}

/* ------------------------------------------------------------------------- */
/* The frame                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * 0x80018214. Two things happen: the draw/display indices flip, and a separate
 * three-slot ring rotates by one. The ring is initialised to {0,1,2} at
 * 0x8007657C and is not the buffer index — it survives here because it is part
 * of the swap's observable state and dropping it would quietly change whatever
 * consumes it.
 */
static void screen_swap(q2_screen *s)
{
    u8 a = s->disp.rotate[0];
    u8 b = s->disp.rotate[1];
    u8 c = s->disp.rotate[2];
    u8 previous;

    s->disp.rotate[0] = b;
    s->disp.rotate[1] = c;
    s->disp.rotate[2] = a;

    previous            = s->disp.draw_buffer;
    s->disp.draw_buffer = (u8)(1 - previous);
    s->disp.disp_buffer = previous;
}

void q2_screen_frame_begin(q2_screen *s, psx_ot *ot)
{
    if (!s)
        return;

    screen_swap(s);
    s->frame++;

    s->background_armed = false;
    s->overlay_armed    = false;

    if (ot) {
        psx_ot_clear(ot);
        psx_ot_set_window(ot, 0, 0);
    }
}

void q2_screen_background(q2_screen *s)
{
    int i;

    if (!s)
        return;

    s->background_armed = true;

    /*
     * 0x800780C0 clears every viewport's isbg immediately after arming the
     * full-screen env, so the frame is cleared once rather than once per
     * viewport. Without this the per-viewport clears would run *after* the
     * full-screen one and erase nothing visible, but the split-screen gutters
     * would then be the only thing the full-screen clear owns — which is
     * precisely what it is for.
     */
    for (i = 0; i < Q2_SCREEN_MAX_VIEWS; i++)
        s->view[i].bg_enable = 0;
}

bool q2_screen_view_begin(q2_screen *s, int p, psx_ot *ot, gte_state *gte)
{
    q2_screen_view *v;

    if (!s || p < 0 || p >= s->view_count)
        return false;

    v = &s->view[p];

    /* 0x80076B78 / 0x80076B90 — the GTE is reloaded per viewport, which is what
     * lets two players have different fields of view on the same frame. */
    if (gte)
        gte_set_projection(gte, (u16)v->proj, v->ofs_x, v->ofs_y);

    /* Geometry starts one bucket past the slice's draw env. */
    if (ot)
        psx_ot_set_window(ot,
                          (u32)Q2_SCREEN_OT_VIEW_BASE
                              + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                              + Q2_SCREEN_OT_VIEW_ENV + 1u,
                          (u32)Q2_SCREEN_OT_VIEW_STRIDE
                              - (Q2_SCREEN_OT_VIEW_ENV + 1u));

    return true;
}

void q2_screen_overlay_begin(q2_screen *s, psx_ot *ot, gte_state *gte)
{
    if (!s)
        return;

    s->overlay_armed = true;

    if (gte)
        gte_set_projection(gte, (u16)s->overlay.proj,
                           s->overlay.ofs_x, s->overlay.ofs_y);

    if (ot)
        psx_ot_set_window(ot,
                          (u32)Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_VIEW_ENV + 1u,
                          (u32)Q2_SCREEN_OT_OVERLAY_LEN
                              - (Q2_SCREEN_OT_VIEW_ENV + 1u));
}

u16 q2_screen_view_otz(const q2_screen *s, int p, u32 z)
{
    u32 usable;

    (void)s;
    if (p < 0 || p >= Q2_SCREEN_MAX_VIEWS)
        p = 0;

    /* The env packet occupies slice bucket 1, so geometry starts at 2 and the
     * slice has 51 - 2 usable buckets. */
    usable = Q2_SCREEN_OT_VIEW_STRIDE - (Q2_SCREEN_OT_VIEW_ENV + 1);
    if (z >= usable)
        z = usable - 1;

    return (u16)(Q2_SCREEN_OT_VIEW_BASE + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                 + Q2_SCREEN_OT_VIEW_ENV + 1 + z);
}

u16 q2_screen_overlay_otz(const q2_screen *s, u32 z)
{
    u32 usable = Q2_SCREEN_OT_OVERLAY_LEN - (Q2_SCREEN_OT_VIEW_ENV + 1);

    (void)s;
    if (z >= usable)
        z = usable - 1;

    return (u16)(Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_VIEW_ENV + 1 + z);
}

/* ------------------------------------------------------------------------- */
/* Composition                                                                */
/* ------------------------------------------------------------------------- */
/*
 * One draw environment, as the walk below sees it: a clip rectangle inside the
 * buffer, a drawing offset, and an optional background fill. Built from a view
 * the way 0x80076A74 builds it, minus the VRAM buffer origin — this module's
 * framebuffer *is* the buffer, so the origin is already accounted for.
 */
typedef struct screen_env {
    s16  x, y, w, h;
    bool clear;
    u8   rgb[3];
} screen_env;

static void env_from_view(const q2_screen_view *v, screen_env *e)
{
    /*
     * 0x80076C18..0x80076C3C. The shake displaces the rectangle's origin and
     * shrinks its size by the same amount, so the far edge stays put and the
     * near edges bite into the viewport — a screen shake that never exposes
     * anything outside the viewport it belongs to.
     */
    e->x = (s16)(v->x + v->shake_x);
    e->y = (s16)(v->y + v->shake_y);
    e->w = (s16)(v->w - v->shake_x);
    e->h = (s16)(v->h - v->shake_y);
    e->clear  = (v->bg_enable != 0);
    e->rgb[0] = v->bg_rgb[0];
    e->rgb[1] = v->bg_rgb[1];
    e->rgb[2] = v->bg_rgb[2];
}

static void env_apply(psx_framebuffer *fb, const screen_env *e,
                      psx_raster_opts *opts)
{
    opts->clip_x = e->x;
    opts->clip_y = e->y;
    opts->clip_w = e->w;
    opts->clip_h = e->h;
    opts->ofs_x  = e->x;
    opts->ofs_y  = e->y;

    if (e->clear) {
        int py, px;
        int x1 = e->x + e->w;
        int y1 = e->y + e->h;
        u16 c  = psx_rgb555(e->rgb[0], e->rgb[1], e->rgb[2]);

        if (x1 > fb->width)  x1 = fb->width;
        if (y1 > fb->height) y1 = fb->height;

        for (py = e->y < 0 ? 0 : e->y; py < y1; py++)
            for (px = e->x < 0 ? 0 : e->x; px < x1; px++)
                fb->px[py * fb->width + px] = c;
    }
}

void q2_screen_compose(q2_screen *s, const psx_ot *ot,
                       const psx_vram *vram, const psx_raster_opts *opts)
{
    psx_framebuffer *fb;
    psx_raster_opts  local;
    u32 buckets, b;
    int p;

    if (!s || !ot || !opts)
        return;

    fb = &s->buf[s->disp.draw_buffer];
    if (!fb->px)
        return;

    local = *opts;
    local.clip_x = local.clip_y = local.clip_w = local.clip_h = 0;
    local.ofs_x  = local.ofs_y  = 0;

    buckets = ot->bucket_count;
    if (buckets > Q2_SCREEN_OT_ENTRIES)
        buckets = Q2_SCREEN_OT_ENTRIES;

    for (b = 0; b < buckets; b++) {
        s32 idx;

        /*
         * The draw-env packets execute at the bucket they were linked into,
         * before that bucket's geometry: each is AddPrim'd after the geometry
         * that shares its bucket, and within a bucket the most recent link
         * draws first. Applying them here is that rule, not an approximation
         * of it.
         */
        if (b == Q2_SCREEN_OT_BACKGROUND && s->background_armed) {
            screen_env e;
            e.x = 0;
            e.y = 0;
            e.w = (s16)s->disp.width;
            e.h = (s16)s->disp.height;
            e.clear  = true;                  /* isbg = 1 at 0x800769F4 */
            e.rgb[0] = s->disp.bg_rgb[0];
            e.rgb[1] = s->disp.bg_rgb[1];
            e.rgb[2] = s->disp.bg_rgb[2];
            env_apply(fb, &e, &local);
        }

        for (p = 0; p < s->view_count; p++) {
            u32 env_bucket = Q2_SCREEN_OT_VIEW_BASE
                           + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                           + Q2_SCREEN_OT_VIEW_ENV;
            if (b == env_bucket) {
                screen_env e;
                env_from_view(&s->view[p], &e);
                env_apply(fb, &e, &local);
            }
        }

        if (b == Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_VIEW_ENV && s->overlay_armed) {
            screen_env e;
            env_from_view(&s->overlay, &e);
            env_apply(fb, &e, &local);
        }

        for (idx = ot->bucket_head[b]; idx >= 0; idx = ot->next[idx])
            psx_raster_prim(fb, &ot->prims[idx], vram, &local);
    }
}

void q2_screen_present(q2_screen *s)
{
    /*
     * PutDispEnv(db + 10940) then DrawOTag(db + 10984) at 0x800185AC and
     * 0x800185BC. The env that reaches the hardware belongs to the buffer that
     * has just been drawn, and it displays the *other* rectangle — the cross
     * pairing set up at 0x80077454. Here that reduces to: what was just drawn
     * is what is now shown.
     */
    if (!s)
        return;
    s->disp.disp_buffer = s->disp.draw_buffer;
}

const psx_framebuffer *q2_screen_front(const q2_screen *s)
{
    if (!s)
        return NULL;
    return &s->buf[s->disp.disp_buffer];
}

psx_framebuffer *q2_screen_back(q2_screen *s)
{
    if (!s)
        return NULL;
    return &s->buf[s->disp.draw_buffer];
}

s32 q2_screen_tick_dt(q2_screen *s, double seconds)
{
    s32 dt;

    if (!s)
        return Q2_SCREEN_DT_NOMINAL;

    /* 1/300 s units (§9.12). The console never sees a fractional field, but a
     * host frame loop does, so round rather than truncate. */
    dt = (s32)(seconds * 300.0 + 0.5);
    if (dt < 1)
        dt = 1;

    /* 0x800184B8: `slti 31` with 30 in the delay slot — a clamp, not an
     * average, so a long frame is simply lost time rather than a lurch. */
    if (dt >= Q2_SCREEN_DT_MAX + 1)
        dt = Q2_SCREEN_DT_MAX;

    s->dt = dt;
    return dt;
}
