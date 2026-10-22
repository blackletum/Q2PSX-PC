/*
 * launcher.c — the disc chooser shown before the game. See launcher.h.
 *
 * Everything is laid out on a 720 x 480 grid of layout units and drawn at
 * however many pixels a unit is on this display. Text is SDL's debug font,
 * whose 8x8 glyphs stay sharp only at whole-pixel scales, so every string is
 * placed on a whole pixel and drawn at a whole multiple; shapes are free to
 * land anywhere.
 */
#include "launcher.h"

#include "cddrive.h"
#include "disc.h"
#include "ident.h"
#include "launchcfg.h"
#include "version.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Layout, in units of the 720 x 480 grid                                     */
/* ------------------------------------------------------------------------- */
#define L_W         720.0f
#define L_H         480.0f
#define L_MARGIN     32.0f
#define L_LIST_Y    112.0f
#define L_LIST_H    280.0f       /* four cards and the gaps between them   */
#define L_CARD_H     62.0f
#define L_CARD_GAP    8.0f
#define L_BUTTON_Y  408.0f
#define L_BUTTON_W  128.0f
#define L_BUTTON_H   40.0f
#define L_GLYPH       8.0f       /* SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE     */

#define L_MAX_SOURCES 48
#define L_MAX_PADS    8
#define L_DRIVE_POLL_MS 2500u

/* ------------------------------------------------------------------------- */
/* Palette: a dark slate with Quake II's rust for the one thing to press      */
/* ------------------------------------------------------------------------- */
#define C_BG_TOP      0x0F1216
#define C_BG_BOTTOM   0x1A1E25
#define C_CARD        0x1C2027
#define C_CARD_HOVER  0x232831
#define C_CARD_SEL    0x272C36
#define C_BORDER      0x343B47
#define C_ACCENT      0xE07B2A
#define C_ACCENT_HI   0xF0913F
#define C_ACCENT_INK  0x1E1006
#define C_TEXT        0xE8EAED
#define C_TEXT_2      0xA3ABB5
#define C_TEXT_3      0x6B7480
#define C_ERROR       0xE5534B
#define C_OK          0x3FB950
#define C_DISC        0xC9CED6
#define C_DISC_RING   0x8B929C

typedef enum src_kind {
    SRC_IMAGE,
    SRC_DRIVE
} src_kind;

typedef enum src_state {
    SRC_READY,          /* identified as Quake II PSX                       */
    SRC_UNCHECKED,      /* a drive with a disc in it, read when played       */
    SRC_NO_DISC,        /* an empty drive                                    */
    SRC_NOT_Q2,         /* a disc, but not this game                         */
    SRC_UNREADABLE      /* not a disc image at all                           */
} src_state;

typedef struct src_entry {
    src_kind  kind;
    src_state state;
    bool      last;                     /* the launcher's saved choice      */
    char      path[Q2_LAUNCH_PATH_MAX];
    char      title[64];
    char      note[96];                 /* the second line, for a drive     */
    char      tag[48];                  /* "SLES-01534 - PAL"               */
} src_entry;

enum { BTN_NONE, BTN_BROWSE, BTN_PLAY };

typedef struct launcher {
    SDL_Window   *window;
    SDL_Renderer *r;
    float         ui, ox, oy;           /* pixels per unit, grid origin     */

    q2_launch_cfg cfg;
    const char   *cfg_path;

    src_entry     src[L_MAX_SOURCES];
    int           count;
    int           sel;
    int           hover;                /* card under the pointer, or -1    */
    int           hover_button;
    int           press_button;         /* pressed here; fires on release   */
    int           press_card;           /* double-clicked; plays on release */
    float         scroll;
    float         mx, my;               /* pointer, in units                */

    char          status[160];
    bool          status_error;

    Uint32        dialog_event;
    bool          dialog_open;
    SDL_Keycode   armed_key;            /* Enter/Space, waiting for release */
    bool          armed_pad;
    SDL_Gamepad  *pads[L_MAX_PADS];
    Uint64        drive_poll_at;
    u32           drive_signature;
} launcher;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (!dst || cap == 0)
        return;
    n = src ? strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *path_tail(const char *path)
{
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *t = a > b ? a : b;

    return t ? t + 1 : path;
}

static const char *path_ext(const char *path)
{
    const char *tail = path_tail(path);
    const char *dot = strrchr(tail, '.');

    return dot ? dot + 1 : "";
}

static bool ext_is(const char *path, const char *ext)
{
    return SDL_strcasecmp(path_ext(path), ext) == 0;
}

static bool file_exists(const char *path)
{
    SDL_PathInfo info;

    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static void set_status(launcher *l, bool error, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(l->status, sizeof(l->status), fmt, ap);
    va_end(ap);
    l->status_error = error;
}

/* ------------------------------------------------------------------------- */
/* The sources                                                                */
/* ------------------------------------------------------------------------- */
static int find_src(const launcher *l, const char *path)
{
    int i;

    for (i = 0; i < l->count; i++)
        if (q2_launch_path_same(l->src[i].path, path))
            return i;
    return -1;
}

/* Open it, ask what it is, close it. The disc layer reports a file that is
 * not a disc as an error, which is the point of asking, so only real faults
 * reach the console while a list is being built. */
static void identify(src_entry *s)
{
    q2_log_level level = q2_log_get_level();
    q2_build_id id;
    disc *d = NULL;

    q2_log_set_level(Q2_LOG_ERROR);
    if (disc_open(&d, s->path) != Q2_OK) {
        s->state = s->kind == SRC_DRIVE ? SRC_NO_DISC : SRC_UNREADABLE;
    } else if (q2_identify(d, &id) != Q2_OK) {
        s->state = SRC_NOT_Q2;
    } else {
        s->state = SRC_READY;
        if (s->kind == SRC_IMAGE)
            copy_str(s->title, sizeof(s->title),
                     id.desc ? id.desc->name : "Quake II (uncatalogued build)");
        snprintf(s->tag, sizeof(s->tag), "%s - %s", id.serial,
                 id.video == Q2_VIDEO_NTSC ? "NTSC" : "PAL");
    }
    disc_close(d);
    q2_log_set_level(level);
}

static src_entry *add_image(launcher *l, const char *path)
{
    src_entry *s;
    int at = find_src(l, path);

    if (at >= 0)
        return &l->src[at];
    if (l->count >= L_MAX_SOURCES || !file_exists(path))
        return NULL;

    s = &l->src[l->count++];
    memset(s, 0, sizeof(*s));
    s->kind = SRC_IMAGE;
    copy_str(s->path, sizeof(s->path), path);
    copy_str(s->title, sizeof(s->title), path_tail(path));
    identify(s);
    return s;
}

static src_entry *add_drive(launcher *l, const cd_drive_info *info)
{
    src_entry *s;
    int at = find_src(l, info->path);

    if (at >= 0)
        s = &l->src[at];
    else if (l->count < L_MAX_SOURCES)
        s = &l->src[l->count++];
    else
        return NULL;

    memset(s, 0, sizeof(*s));
    s->kind  = SRC_DRIVE;
    s->state = info->has_media ? SRC_UNCHECKED : SRC_NO_DISC;
    copy_str(s->path, sizeof(s->path), info->path);
    snprintf(s->title, sizeof(s->title), "CD drive %s", info->name);
    if (!info->has_media)
        copy_str(s->note, sizeof(s->note), "Empty - insert the Quake II disc");
    else if (info->label[0])
        snprintf(s->note, sizeof(s->note), "Disc in the drive: %s", info->label);
    else
        copy_str(s->note, sizeof(s->note), "A disc is in the drive");
    return s;
}

/* Images in one directory. A .bin or .img that sits beside a .cue is that
 * sheet's track and is not offered on its own. */
static void scan_dir(launcher *l, const char *dir)
{
    char **names;
    int n = 0, i;
    bool has_cue = false;
    size_t len;

    if (!dir || !dir[0])
        return;
    names = SDL_GlobDirectory(dir, "*", SDL_GLOB_CASEINSENSITIVE, &n);
    if (!names)
        return;

    for (i = 0; i < n; i++)
        has_cue |= ext_is(names[i], "cue");

    len = strlen(dir);
    for (i = 0; i < n; i++) {
        char full[Q2_LAUNCH_PATH_MAX];
        bool want = ext_is(names[i], "cue") || ext_is(names[i], "iso") ||
                    (!has_cue && (ext_is(names[i], "bin") ||
                                  ext_is(names[i], "img")));

        if (!want)
            continue;
        snprintf(full, sizeof(full), "%s%s%s", dir,
                 (len && (dir[len - 1] == '/' || dir[len - 1] == '\\'))
                     ? "" : "/",
                 names[i]);
        add_image(l, full);
    }
    SDL_free(names);
}

static u32 drives_signature(const cd_drive_info *d, int n)
{
    u32 h = 2166136261u;
    int i;
    const char *p;

    for (i = 0; i < n; i++) {
        for (p = d[i].path; *p; p++) h = (h ^ (u8)*p) * 16777619u;
        for (p = d[i].label; *p; p++) h = (h ^ (u8)*p) * 16777619u;
        h = (h ^ (d[i].has_media ? 1u : 2u)) * 16777619u;
    }
    return h;
}

/*
 * The whole list, in the order a player looks for it: the disc played last,
 * the others played before it, images beside the executable, then the
 * drives. `keep` is reselected if it is still there; otherwise the last disc
 * played, otherwise the first one that will play.
 */
static void build_sources(launcher *l, const char *keep)
{
    cd_drive_info drives[CD_DRIVE_LIST_MAX];
    char want[Q2_LAUNCH_PATH_MAX];
    int nd, i;

    copy_str(want, sizeof(want), keep ? keep : "");
    l->count = 0;
    nd = cd_drive_list(drives, CD_DRIVE_LIST_MAX);
    l->drive_signature = drives_signature(drives, nd);

    for (i = -1; i < l->cfg.recent_count; i++) {
        const char *p = i < 0 ? l->cfg.last : l->cfg.recent[i];
        int k;

        if (!p[0])
            continue;
        if (cd_drive_parse(p, NULL, 0, NULL, 0)) {
            for (k = 0; k < nd; k++)
                if (q2_launch_path_same(drives[k].path, p))
                    add_drive(l, &drives[k]);
        } else {
            add_image(l, p);
        }
    }

    {
        const char *base = SDL_GetBasePath();
        char *cwd = SDL_GetCurrentDirectory();
        char sub[Q2_LAUNCH_PATH_MAX];

        if (base) {
            snprintf(sub, sizeof(sub), "%sdisc", base);
            scan_dir(l, sub);
            scan_dir(l, base);
        }
        scan_dir(l, cwd);
        SDL_free(cwd);
    }

    for (i = 0; i < nd; i++)
        add_drive(l, &drives[i]);

    for (i = 0; i < l->count; i++)
        l->src[i].last = l->cfg.last[0] &&
                         q2_launch_path_same(l->src[i].path, l->cfg.last);

    l->sel = want[0] ? find_src(l, want) : -1;
    if (l->sel < 0 && l->cfg.last[0])
        l->sel = find_src(l, l->cfg.last);
    for (i = 0; l->sel < 0 && i < l->count; i++)
        if (l->src[i].state == SRC_READY || l->src[i].state == SRC_UNCHECKED)
            l->sel = i;
    if (l->sel < 0)
        l->sel = 0;
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
static SDL_FColor rgb(u32 hex, float a)
{
    SDL_FColor c;

    c.r = (float)((hex >> 16) & 0xFF) / 255.0f;
    c.g = (float)((hex >> 8) & 0xFF) / 255.0f;
    c.b = (float)(hex & 0xFF) / 255.0f;
    c.a = a;
    return c;
}

static float px_x(const launcher *l, float x) { return l->ox + x * l->ui; }
static float px_y(const launcher *l, float y) { return l->oy + y * l->ui; }

/* A filled shape as a fan around its centre. `pts` are in units. */
static void fan(launcher *l, const SDL_FPoint *pts, int n, SDL_FColor c)
{
    SDL_Vertex v[64];
    int idx[64 * 3];
    float cx = 0, cy = 0;
    int i;

    if (n < 3 || n > 62)
        return;
    for (i = 0; i < n; i++) {
        cx += pts[i].x;
        cy += pts[i].y;
    }
    memset(v, 0, sizeof(v));
    v[0].position.x = px_x(l, cx / (float)n);
    v[0].position.y = px_y(l, cy / (float)n);
    v[0].color = c;
    for (i = 0; i < n; i++) {
        v[i + 1].position.x = px_x(l, pts[i].x);
        v[i + 1].position.y = px_y(l, pts[i].y);
        v[i + 1].color = c;
        idx[i * 3 + 0] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = (i + 1) % n + 1;
    }
    SDL_RenderGeometry(l->r, NULL, v, n + 1, idx, n * 3);
}

static void round_rect(launcher *l, float x, float y, float w, float h,
                       float rad, SDL_FColor c)
{
    SDL_FPoint pts[4 * 7];
    int corner, i, n = 0;

    if (rad * 2.0f > w) rad = w * 0.5f;
    if (rad * 2.0f > h) rad = h * 0.5f;

    /* Clockwise on screen from the top-right corner: each corner's quarter
     * circle runs from the edge before it to the edge after it. */
    for (corner = 0; corner < 4; corner++) {
        float ccx = (corner == 0 || corner == 1) ? x + w - rad : x + rad;
        float ccy = (corner == 1 || corner == 2) ? y + h - rad : y + rad;
        float a0 = -1.5707963f + 1.5707963f * (float)corner;

        for (i = 0; i <= 6; i++) {
            float a = a0 + 1.5707963f * (float)i / 6.0f;
            pts[n].x = ccx + cosf(a) * rad;
            pts[n].y = ccy + sinf(a) * rad;
            n++;
        }
    }
    fan(l, pts, n, c);
}

static void frame_rect(launcher *l, float x, float y, float w, float h,
                       float rad, float line, SDL_FColor edge, SDL_FColor fill)
{
    round_rect(l, x, y, w, h, rad, edge);
    round_rect(l, x + line, y + line, w - 2 * line, h - 2 * line,
               rad > line ? rad - line : 0, fill);
}

static void circle(launcher *l, float cx, float cy, float rad, SDL_FColor c)
{
    SDL_FPoint pts[32];
    int i;

    for (i = 0; i < 32; i++) {
        float a = 6.2831853f * (float)i / 32.0f;
        pts[i].x = cx + cosf(a) * rad;
        pts[i].y = cy + sinf(a) * rad;
    }
    fan(l, pts, 32, c);
}

/* Whole pixels per glyph pixel for a text size, and the advance in units. */
static float glyph_scale(const launcher *l, int size)
{
    float s = floorf((float)size * l->ui + 0.5f);

    return s < 1.0f ? 1.0f : s;
}

static float text_w(const launcher *l, const char *s, int size, float track)
{
    float adv = L_GLYPH * glyph_scale(l, size) / l->ui + track;
    size_t n = strlen(s);

    return n ? (float)n * adv - track : 0.0f;
}

/* `track` is extra space after each glyph, in units, for the small capitals. */
static float text(launcher *l, float x, float y, const char *s, int size,
                  float track, SDL_FColor c)
{
    float g = glyph_scale(l, size);
    float px = floorf(px_x(l, x) + 0.5f), py = floorf(px_y(l, y) + 0.5f);
    bool clipped = SDL_RenderClipEnabled(l->r);
    SDL_Rect clip = { 0, 0, 0, 0 };

    SDL_SetRenderDrawColorFloat(l->r, c.r, c.g, c.b, c.a);
    /* SDL keeps the clip rectangle in render coordinates and multiplies it
     * by the scale, so the list's clip has to be divided down to match or
     * it lands three times further down the window than the list. */
    if (clipped)
        SDL_GetRenderClipRect(l->r, &clip);
    SDL_SetRenderScale(l->r, g, g);
    if (clipped) {
        SDL_Rect scaled;

        scaled.x = (int)floorf((float)clip.x / g);
        scaled.y = (int)floorf((float)clip.y / g);
        scaled.w = (int)ceilf((float)(clip.x + clip.w) / g) - scaled.x;
        scaled.h = (int)ceilf((float)(clip.y + clip.h) / g) - scaled.y;
        SDL_SetRenderClipRect(l->r, &scaled);
    }
    if (track == 0.0f) {
        SDL_RenderDebugText(l->r, px / g, py / g, s);
    } else {
        float step = floorf(L_GLYPH * g + track * l->ui + 0.5f);
        char one[2] = { 0, 0 };
        const char *p;

        for (p = s; *p; p++, px += step) {
            one[0] = *p;
            SDL_RenderDebugText(l->r, px / g, py / g, one);
        }
    }
    SDL_SetRenderScale(l->r, 1.0f, 1.0f);
    if (clipped)
        SDL_SetRenderClipRect(l->r, &clip);
    return text_w(l, s, size, track);
}

/* `s` cut to `max_w` units with its middle replaced by "...", which keeps a
 * path's drive and its file name — the two parts that say which one it is. */
static void fit_middle(const launcher *l, const char *s, int size, float max_w,
                       char *out, size_t cap)
{
    float adv = L_GLYPH * glyph_scale(l, size) / l->ui;
    size_t fit = adv > 0 ? (size_t)(max_w / adv) : 0;
    size_t n = strlen(s), head, tail;

    if (n <= fit || fit < 8) {
        copy_str(out, cap, s);
        if (n > fit && fit < cap)
            out[fit] = '\0';
        return;
    }
    head = (fit - 3) / 3;
    tail = fit - 3 - head;
    if (head + 3 + tail + 1 > cap)
        return;
    memcpy(out, s, head);
    memcpy(out + head, "...", 3);
    memcpy(out + head + 3, s + n - tail, tail);
    out[head + 3 + tail] = '\0';
}

static void pill(launcher *l, float right, float y, const char *label,
                 u32 fill, u32 ink)
{
    float w = text_w(l, label, 1, 1.0f) + 14.0f;

    round_rect(l, right - w, y, w, 16.0f, 8.0f, rgb(fill, 1.0f));
    text(l, right - w + 7.0f, y + 4.0f, label, 1, 1.0f, rgb(ink, 1.0f));
}

static void disc_icon(launcher *l, float cx, float cy, bool drive, bool lit)
{
    if (drive) {
        round_rect(l, cx - 15.0f, cy - 9.0f, 30.0f, 18.0f, 3.0f,
                   rgb(C_DISC_RING, 1.0f));
        round_rect(l, cx - 11.0f, cy - 1.0f, 22.0f, 3.0f, 1.0f,
                   rgb(C_CARD, 1.0f));
        circle(l, cx + 10.0f, cy + 5.0f, 1.6f,
               rgb(lit ? C_OK : C_TEXT_3, 1.0f));
        return;
    }
    circle(l, cx, cy, 14.0f, rgb(C_DISC, 1.0f));
    circle(l, cx, cy, 6.0f, rgb(C_DISC_RING, 1.0f));
    circle(l, cx, cy, 2.5f, rgb(C_CARD, 1.0f));
}

static float list_top(const launcher *l, int i)
{
    return L_LIST_Y + (float)i * (L_CARD_H + L_CARD_GAP) - l->scroll;
}

static float content_h(const launcher *l)
{
    return l->count ? (float)l->count * (L_CARD_H + L_CARD_GAP) - L_CARD_GAP
                    : 0.0f;
}

static bool can_play(const src_entry *s)
{
    return s->state == SRC_READY || s->state == SRC_UNCHECKED;
}

static void draw_card(launcher *l, int i)
{
    const src_entry *s = &l->src[i];
    float x = L_MARGIN, y = list_top(l, i), w = L_W - 2 * L_MARGIN;
    bool sel = i == l->sel, hov = i == l->hover;
    bool dim = !can_play(s);
    char line[160];
    float right = x + w - 16.0f, tag_w = 0.0f;

    if (sel)
        frame_rect(l, x, y, w, L_CARD_H, 10.0f, 2.0f, rgb(C_ACCENT, 1.0f),
                   rgb(C_CARD_SEL, 1.0f));
    else
        frame_rect(l, x, y, w, L_CARD_H, 10.0f, 1.0f, rgb(C_BORDER, 0.6f),
                   rgb(hov ? C_CARD_HOVER : C_CARD, 1.0f));

    disc_icon(l, x + 34.0f, y + L_CARD_H * 0.5f, s->kind == SRC_DRIVE,
              s->state != SRC_NO_DISC);

    /* The right-hand column first, so the title knows where to stop. */
    switch (s->state) {
    case SRC_NO_DISC:    pill(l, right, y + 12.0f, "NO DISC", C_BORDER, C_TEXT_2); break;
    case SRC_NOT_Q2:     pill(l, right, y + 12.0f, "NOT QUAKE II", C_ERROR, C_TEXT); break;
    case SRC_UNREADABLE: pill(l, right, y + 12.0f, "UNREADABLE", C_ERROR, C_TEXT); break;
    case SRC_READY:
    case SRC_UNCHECKED:
        if (s->last)
            pill(l, right, y + 12.0f, "LAST PLAYED", C_ACCENT, C_ACCENT_INK);
        break;
    }
    if (s->tag[0]) {
        tag_w = text_w(l, s->tag, 1, 0.0f) + 16.0f;
        text(l, right - tag_w + 16.0f, y + 38.0f, s->tag, 1, 0.0f,
             rgb(C_TEXT_2, 1.0f));
    }

    fit_middle(l, s->title, 2, right - (x + 64.0f) - 150.0f, line, sizeof(line));
    text(l, x + 64.0f, y + 12.0f, line, 2, 0.0f,
         rgb(dim ? C_TEXT_2 : C_TEXT, 1.0f));

    fit_middle(l, s->kind == SRC_DRIVE ? s->note : s->path, 1,
               right - tag_w - (x + 64.0f), line, sizeof(line));
    text(l, x + 64.0f, y + 38.0f, line, 1, 0.0f, rgb(C_TEXT_3, 1.0f));
}

static bool button_rect(int which, SDL_FRect *out)
{
    out->y = L_BUTTON_Y;
    out->w = L_BUTTON_W;
    out->h = L_BUTTON_H;
    if (which == BTN_PLAY)
        out->x = L_W - L_MARGIN - L_BUTTON_W;
    else if (which == BTN_BROWSE)
        out->x = L_W - L_MARGIN - 2 * L_BUTTON_W - 12.0f;
    else
        return false;
    return true;
}

static void draw_button(launcher *l, int which, const char *label,
                        bool primary, bool enabled)
{
    SDL_FRect b;
    bool hot = enabled && l->hover_button == which;
    float tw;

    button_rect(which, &b);
    if (primary)
        round_rect(l, b.x, b.y, b.w, b.h, 10.0f,
                   rgb(!enabled ? C_BORDER : hot ? C_ACCENT_HI : C_ACCENT, 1.0f));
    else
        frame_rect(l, b.x, b.y, b.w, b.h, 10.0f, 1.0f, rgb(C_BORDER, 1.0f),
                   rgb(hot ? C_CARD_HOVER : C_CARD, 1.0f));
    tw = text_w(l, label, 2, 0.0f);
    text(l, b.x + (b.w - tw) * 0.5f, b.y + (b.h - 16.0f) * 0.5f, label, 2, 0.0f,
         rgb(primary ? (enabled ? C_ACCENT_INK : C_TEXT_3) : C_TEXT, 1.0f));
}

static void draw(launcher *l)
{
    int ow = 0, oh = 0, i;
    SDL_Vertex bg[4];
    static const int quad[6] = { 0, 1, 2, 2, 1, 3 };
    char line[160];

    SDL_GetCurrentRenderOutputSize(l->r, &ow, &oh);
    l->ui = (float)ow / L_W < (float)oh / L_H ? (float)ow / L_W
                                              : (float)oh / L_H;
    l->ox = ((float)ow - L_W * l->ui) * 0.5f;
    l->oy = ((float)oh - L_H * l->ui) * 0.5f;

    SDL_SetRenderDrawBlendMode(l->r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(l->r, rgb(C_BG_TOP, 1).r, rgb(C_BG_TOP, 1).g,
                                rgb(C_BG_TOP, 1).b, 1.0f);
    SDL_RenderClear(l->r);

    memset(bg, 0, sizeof(bg));
    bg[0].position.x = 0;           bg[0].position.y = 0;
    bg[1].position.x = (float)ow;   bg[1].position.y = 0;
    bg[2].position.x = 0;           bg[2].position.y = (float)oh;
    bg[3].position.x = (float)ow;   bg[3].position.y = (float)oh;
    bg[0].color = bg[1].color = rgb(C_BG_TOP, 1.0f);
    bg[2].color = bg[3].color = rgb(C_BG_BOTTOM, 1.0f);
    SDL_RenderGeometry(l->r, NULL, bg, 4, quad, 6);

    /* The header: the name, the platform in rust, the build on the right. */
    text(l, L_MARGIN, 26.0f, "QUAKE II", 4, 2.0f, rgb(C_TEXT, 1.0f));
    round_rect(l, L_MARGIN, 66.0f, 40.0f, 3.0f, 1.5f, rgb(C_ACCENT, 1.0f));
    text(l, L_MARGIN + 52.0f, 64.0f, "PLAYSTATION", 1, 2.0f,
         rgb(C_ACCENT, 1.0f));
    snprintf(line, sizeof(line), "Q2PSX-PC %s", Q2PSX_VERSION_GIT);
    text(l, L_W - L_MARGIN - text_w(l, line, 1, 0.0f), 30.0f, line, 1, 0.0f,
         rgb(C_TEXT_3, 1.0f));

    text(l, L_MARGIN, 92.0f, "CHOOSE A DISC", 1, 2.0f, rgb(C_TEXT_2, 1.0f));
    if (l->count > 1) {
        snprintf(line, sizeof(line), "%d SOURCES", l->count);
        text(l, L_W - L_MARGIN - text_w(l, line, 1, 2.0f), 92.0f, line, 1,
             2.0f, rgb(C_TEXT_3, 1.0f));
    }

    if (l->count == 0) {
        static const char *a = "No Quake II disc found yet";
        static const char *b = "Insert the CD, drop a .cue, .iso or .bin here,";
        static const char *c = "or press Browse to find one.";

        frame_rect(l, L_MARGIN, L_LIST_Y, L_W - 2 * L_MARGIN, 120.0f, 10.0f,
                   1.0f, rgb(C_BORDER, 0.6f), rgb(C_CARD, 1.0f));
        text(l, (L_W - text_w(l, a, 2, 0.0f)) * 0.5f, L_LIST_Y + 30.0f, a, 2,
             0.0f, rgb(C_TEXT, 1.0f));
        text(l, (L_W - text_w(l, b, 1, 0.0f)) * 0.5f, L_LIST_Y + 62.0f, b, 1,
             0.0f, rgb(C_TEXT_2, 1.0f));
        text(l, (L_W - text_w(l, c, 1, 0.0f)) * 0.5f, L_LIST_Y + 78.0f, c, 1,
             0.0f, rgb(C_TEXT_2, 1.0f));
    } else {
        SDL_Rect clip;

        clip.x = (int)floorf(px_x(l, 0));
        clip.y = (int)floorf(px_y(l, L_LIST_Y - 2.0f));
        clip.w = (int)ceilf(L_W * l->ui);
        clip.h = (int)ceilf((L_LIST_H + 4.0f) * l->ui);
        SDL_SetRenderClipRect(l->r, &clip);
        for (i = 0; i < l->count; i++) {
            float y = list_top(l, i);
            if (y + L_CARD_H < L_LIST_Y - 2.0f || y > L_LIST_Y + L_LIST_H + 2.0f)
                continue;
            draw_card(l, i);
        }
        SDL_SetRenderClipRect(l->r, NULL);

        if (content_h(l) > L_LIST_H) {
            float track = L_LIST_H;
            float knob = track * L_LIST_H / content_h(l);
            float at = (track - knob) * l->scroll / (content_h(l) - L_LIST_H);

            round_rect(l, L_W - L_MARGIN + 10.0f, L_LIST_Y + at, 4.0f, knob,
                       2.0f, rgb(C_BORDER, 1.0f));
        }
    }

    /* The status line, then the two buttons. */
    if (l->status[0]) {
        fit_middle(l, l->status, 1, L_W - 2 * L_MARGIN - 2 * L_BUTTON_W - 30.0f,
                   line, sizeof(line));
        if (l->status_error)
            circle(l, L_MARGIN + 4.0f, L_BUTTON_Y + 20.0f, 4.0f,
                   rgb(C_ERROR, 1.0f));
        text(l, L_MARGIN + (l->status_error ? 14.0f : 0.0f), L_BUTTON_Y + 16.0f,
             line, 1, 0.0f, rgb(l->status_error ? C_ERROR : C_TEXT_2, 1.0f));
    } else if (l->count && l->sel >= 0 && l->sel < l->count &&
               can_play(&l->src[l->sel])) {
        char ready[96];

        snprintf(ready, sizeof(ready), "Ready: %s", l->src[l->sel].title);
        fit_middle(l, ready, 1, L_W - 2 * L_MARGIN - 2 * L_BUTTON_W - 30.0f,
                   line, sizeof(line));
        circle(l, L_MARGIN + 4.0f, L_BUTTON_Y + 20.0f, 4.0f, rgb(C_OK, 1.0f));
        text(l, L_MARGIN + 14.0f, L_BUTTON_Y + 16.0f, line, 1, 0.0f,
             rgb(C_TEXT_2, 1.0f));
    }

    draw_button(l, BTN_BROWSE, "Browse", false, !l->dialog_open);
    draw_button(l, BTN_PLAY, "Play", true,
                l->count > 0 && can_play(&l->src[l->sel]));

    /* As many of the keys as fit, most useful first: at 150% the small text
     * is two pixels a glyph pixel and the whole set does not. */
    {
        static const char *keys[] = {
            "ENTER play", "O browse", "ESC quit", "DEL forget", "F5 refresh",
            "UP/DOWN choose",
        };
        char hints[160] = "";
        size_t k;

        for (k = 0; k < Q2PSX_ARRAY_COUNT(keys); k++) {
            char next[160];

            snprintf(next, sizeof(next), "%s%s%s", hints, hints[0] ? "   " : "",
                     keys[k]);
            if (text_w(l, next, 1, 0.0f) <= L_W - 2 * L_MARGIN)
                copy_str(hints, sizeof(hints), next);
        }
        text(l, L_MARGIN, 458.0f, hints, 1, 0.0f, rgb(C_TEXT_3, 1.0f));
    }
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */
static void scroll_to_selection(launcher *l)
{
    float top, bottom, max_scroll = content_h(l) - L_LIST_H;

    if (l->sel < 0 || l->sel >= l->count)
        return;
    top = (float)l->sel * (L_CARD_H + L_CARD_GAP);
    bottom = top + L_CARD_H;
    if (top < l->scroll)
        l->scroll = top;
    if (bottom > l->scroll + L_LIST_H)
        l->scroll = bottom - L_LIST_H;
    if (l->scroll > max_scroll) l->scroll = max_scroll;
    if (l->scroll < 0) l->scroll = 0;
}

static void move_selection(launcher *l, int by)
{
    if (!l->count)
        return;
    l->sel += by;
    if (l->sel < 0) l->sel = 0;
    if (l->sel >= l->count) l->sel = l->count - 1;
    l->status[0] = '\0';
    scroll_to_selection(l);
}

static void hit_test(launcher *l)
{
    int i;
    SDL_FRect b;

    l->hover = -1;
    l->hover_button = BTN_NONE;

    for (i = BTN_BROWSE; i <= BTN_PLAY; i++) {
        button_rect(i, &b);
        if (l->mx >= b.x && l->mx < b.x + b.w &&
            l->my >= b.y && l->my < b.y + b.h)
            l->hover_button = i;
    }
    if (l->my < L_LIST_Y || l->my >= L_LIST_Y + L_LIST_H ||
        l->mx < L_MARGIN || l->mx >= L_W - L_MARGIN)
        return;
    for (i = 0; i < l->count; i++) {
        float y = list_top(l, i);
        if (l->my >= y && l->my < y + L_CARD_H)
            l->hover = i;
    }
}

static void SDLCALL dialog_done(void *user, const char *const *files,
                                int filter)
{
    launcher *l = (launcher *)user;
    SDL_Event ev;

    SDL_zero(ev);
    ev.type = l->dialog_event;
    ev.user.code = files ? 0 : -1;
    ev.user.data1 = (files && files[0]) ? SDL_strdup(files[0]) : NULL;
    SDL_PushEvent(&ev);
}

static void browse(launcher *l)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disc images (.cue, .iso, .bin, .img)", "cue;iso;bin;img" },
        { "All files", "*" },
    };
    char dir[Q2_LAUNCH_PATH_MAX];
    const char *at = NULL;

    if (l->dialog_open)
        return;
    if (l->count && l->src[l->sel].kind == SRC_IMAGE) {
        copy_str(dir, sizeof(dir), l->src[l->sel].path);
        dir[path_tail(dir) - dir] = '\0';
        at = dir;
    }
    l->dialog_open = true;
    SDL_ShowOpenFileDialog(dialog_done, l, l->window, filters, 2, at, false);
}

/* A file from the picker or dropped on the window. */
static void take_path(launcher *l, const char *path)
{
    src_entry *s;

    if (cd_drive_parse(path, NULL, 0, NULL, 0)) {
        build_sources(l, path);
        return;
    }
    s = add_image(l, path);
    if (!s) {
        set_status(l, true, "That file cannot be opened.");
        return;
    }
    l->sel = (int)(s - l->src);
    scroll_to_selection(l);
    if (s->state == SRC_READY)
        set_status(l, false, "%s is ready.", s->title);
    else
        set_status(l, true, "%s is %s.", path_tail(s->path),
                   s->state == SRC_NOT_Q2 ? "not a Quake II PlayStation disc"
                                          : "not a disc image this can read");
}

static void forget(launcher *l)
{
    char keep[Q2_LAUNCH_PATH_MAX];
    src_entry *s;

    if (!l->count)
        return;
    s = &l->src[l->sel];
    copy_str(keep, sizeof(keep), s->path);
    q2_launch_cfg_forget(&l->cfg, keep);
    q2_launch_cfg_save(&l->cfg, l->cfg_path);
    build_sources(l, NULL);
    /* Found again beside the executable or in a drive: it stays, just not
     * as the remembered one. */
    set_status(l, false, "Forgot %s.", path_tail(keep));
    scroll_to_selection(l);
}

/* Read the selection once more and, if it is the game, remember it. */
static bool activate(launcher *l)
{
    src_entry *s;

    if (!l->count || l->sel < 0)
        return false;
    s = &l->src[l->sel];

    if (s->kind == SRC_DRIVE) {
        if (s->state == SRC_NO_DISC) {
            set_status(l, true, "There is no disc in %s.", s->path);
            return false;
        }
        /* A drive can take seconds to spin up; say so before blocking. */
        set_status(l, false, "Reading the disc in %s...", s->path);
        draw(l);
        SDL_RenderPresent(l->r);
        identify(s);
        l->status[0] = '\0';
    }

    switch (s->state) {
    case SRC_READY:
        break;
    case SRC_NO_DISC:
        set_status(l, true, "%s has no readable disc in it.", s->path);
        return false;
    case SRC_NOT_Q2:
        set_status(l, true, "That is not a Quake II PlayStation disc.");
        return false;
    case SRC_UNREADABLE:
    case SRC_UNCHECKED:
        set_status(l, true, "That cannot be read as a disc image.");
        return false;
    }

    q2_launch_cfg_use(&l->cfg, s->path);
    if (!q2_launch_cfg_save(&l->cfg, l->cfg_path))
        Q2_WARN("launcher: could not save %s", l->cfg_path);
    return true;
}

static void open_pad(launcher *l, SDL_JoystickID id)
{
    int i;

    for (i = 0; i < L_MAX_PADS; i++) {
        if (!l->pads[i]) {
            l->pads[i] = SDL_OpenGamepad(id);
            return;
        }
    }
}

/* The drives change under the launcher — a disc goes in, a USB drive comes
 * out — and nobody should have to press anything to see it. */
static void poll_drives(launcher *l)
{
    cd_drive_info drives[CD_DRIVE_LIST_MAX];
    Uint64 now = SDL_GetTicks();
    int n;

    if (now < l->drive_poll_at)
        return;
    l->drive_poll_at = now + L_DRIVE_POLL_MS;
    n = cd_drive_list(drives, CD_DRIVE_LIST_MAX);
    if (drives_signature(drives, n) != l->drive_signature) {
        char keep[Q2_LAUNCH_PATH_MAX];

        copy_str(keep, sizeof(keep), l->count ? l->src[l->sel].path : "");
        build_sources(l, keep);
        scroll_to_selection(l);
    }
}

/* ------------------------------------------------------------------------- */
/* The loop                                                                   */
/* ------------------------------------------------------------------------- */
enum { RUN_ON, RUN_PLAY, RUN_QUIT };

static int handle(launcher *l, SDL_Event *e)
{
    if (e->type == l->dialog_event) {
        l->dialog_open = false;
        if (e->user.data1) {
            take_path(l, (const char *)e->user.data1);
            SDL_free(e->user.data1);
        } else if (e->user.code < 0) {
            set_status(l, true, "The file picker could not be opened: %s",
                       SDL_GetError());
        }
        return RUN_ON;
    }

    SDL_ConvertEventToRenderCoordinates(l->r, e);

    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return RUN_QUIT;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        l->drive_poll_at = 0;
        break;

    case SDL_EVENT_DROP_FILE:
        if (e->drop.data)
            take_path(l, e->drop.data);
        break;

    case SDL_EVENT_MOUSE_MOTION:
        l->mx = (e->motion.x - l->ox) / l->ui;
        l->my = (e->motion.y - l->oy) / l->ui;
        hit_test(l);
        break;

    case SDL_EVENT_MOUSE_WHEEL: {
        float max_scroll = content_h(l) - L_LIST_H;

        l->scroll -= e->wheel.y * (L_CARD_H + L_CARD_GAP) * 0.5f;
        if (l->scroll > max_scroll) l->scroll = max_scroll;
        if (l->scroll < 0) l->scroll = 0;
        hit_test(l);
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button != SDL_BUTTON_LEFT)
            break;
        l->mx = (e->button.x - l->ox) / l->ui;
        l->my = (e->button.y - l->oy) / l->ui;
        hit_test(l);
        l->press_button = l->hover_button;
        l->press_card = -1;
        if (l->hover >= 0) {
            if (l->sel != l->hover)
                l->status[0] = '\0';
            l->sel = l->hover;
            if (e->button.clicks >= 2)
                l->press_card = l->hover;
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int pressed = l->press_button, card = l->press_card;

        if (e->button.button != SDL_BUTTON_LEFT)
            break;
        l->press_button = BTN_NONE;
        l->press_card = -1;
        if (card >= 0 && card == l->hover && activate(l))
            return RUN_PLAY;
        if (pressed != BTN_NONE && pressed == l->hover_button) {
            if (pressed == BTN_BROWSE)
                browse(l);
            else if (pressed == BTN_PLAY && activate(l))
                return RUN_PLAY;
        }
        break;
    }

    case SDL_EVENT_KEY_DOWN:
        switch (e->key.key) {
        /* The keypad's keys too: with NumLock off they are the arrows. */
        case SDLK_ESCAPE:   return RUN_QUIT;
        case SDLK_UP:
        case SDLK_KP_8:     move_selection(l, -1); break;
        case SDLK_DOWN:
        case SDLK_KP_2:     move_selection(l, 1); break;
        case SDLK_PAGEUP:
        case SDLK_KP_9:     move_selection(l, -4); break;
        case SDLK_PAGEDOWN:
        case SDLK_KP_3:     move_selection(l, 4); break;
        case SDLK_HOME:
        case SDLK_KP_7:     move_selection(l, -l->count); break;
        case SDLK_END:
        case SDLK_KP_1:     move_selection(l, l->count); break;
        case SDLK_O:        browse(l); break;
        case SDLK_F5:
            build_sources(l, l->count ? l->src[l->sel].path : NULL);
            scroll_to_selection(l);
            set_status(l, false, "Looked again: %d source%s.", l->count,
                       l->count == 1 ? "" : "s");
            break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE:
            forget(l);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            /* On the release, so the key is up before the game starts
             * reading the keyboard — a held Enter would confirm its menu. */
            if (!e->key.repeat)
                l->armed_key = e->key.key;
            break;
        default:
            break;
        }
        break;

    case SDL_EVENT_KEY_UP:
        if (l->armed_key && e->key.key == l->armed_key) {
            l->armed_key = 0;
            if (activate(l))
                return RUN_PLAY;
        }
        break;

    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(l, e->gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:   move_selection(l, -1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move_selection(l, 1); break;
        case SDL_GAMEPAD_BUTTON_WEST:      browse(l); break;
        case SDL_GAMEPAD_BUTTON_NORTH:
            build_sources(l, l->count ? l->src[l->sel].path : NULL);
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_BACK:      return RUN_QUIT;
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_START:     l->armed_pad = true; break;
        default: break;
        }
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (l->armed_pad && (e->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                             e->gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
            l->armed_pad = false;
            if (activate(l))
                return RUN_PLAY;
        }
        break;

    default:
        break;
    }
    return RUN_ON;
}

static void shutdown_ui(launcher *l, bool keep_window)
{
    int i;

    for (i = 0; i < L_MAX_PADS; i++)
        if (l->pads[i])
            SDL_CloseGamepad(l->pads[i]);
    if (l->r)
        SDL_DestroyRenderer(l->r);
    l->r = NULL;
    if (!keep_window && l->window) {
        SDL_DestroyWindow(l->window);
        l->window = NULL;
    }
    /* The game polls the keyboard and enumerates its own pads; nothing the
     * launcher was handed should arrive there a second time. */
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_DROP_POSITION);
}

bool q2_launcher_run(char *out, size_t cap, SDL_Window **window)
{
    launcher *l;
    const char *shot = SDL_getenv("Q2PSX_LAUNCHER_SHOT");
    float cs;
    int result = RUN_ON;
    bool ok = false;

    if (window)
        *window = NULL;
    if (!out || cap == 0)
        return false;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    l = (launcher *)SDL_calloc(1, sizeof(*l));
    if (!l)
        return false;
    l->hover = -1;
    l->press_card = -1;
    l->cfg_path = SDL_getenv("Q2PSX_LAUNCHER_CFG");
    if (!l->cfg_path || !l->cfg_path[0])
        l->cfg_path = q2_launch_cfg_default_path();
    q2_launch_cfg_load(&l->cfg, l->cfg_path);
    l->dialog_event = SDL_RegisterEvents(1);

    cs = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (cs < 1.0f)
        cs = 1.0f;
    l->window = SDL_CreateWindow("Q2PSX-PC", (int)(L_W * cs), (int)(L_H * cs),
                                 SDL_WINDOW_HIDDEN);
    if (l->window)
        l->r = SDL_CreateRenderer(l->window, NULL);
    if (!l->window || !l->r) {
        fprintf(stderr, "launcher: %s\n", SDL_GetError());
        shutdown_ui(l, false);
        SDL_free(l);
        return false;
    }
    SDL_SetRenderVSync(l->r, 1);

    build_sources(l, NULL);
    scroll_to_selection(l);
    l->drive_poll_at = SDL_GetTicks() + L_DRIVE_POLL_MS;

    draw(l);
    if (shot && shot[0]) {
        /* A capture of the first frame, for documentation and for checking
         * the layout without a person at the keyboard. */
        SDL_Surface *s = SDL_RenderReadPixels(l->r, NULL);

        if (!s || !SDL_SaveBMP(s, shot))
            fprintf(stderr, "launcher: cannot write %s: %s\n", shot,
                    SDL_GetError());
        SDL_DestroySurface(s);
        shutdown_ui(l, false);
        SDL_free(l);
        return false;
    }
    SDL_RenderPresent(l->r);
    SDL_ShowWindow(l->window);
    SDL_RaiseWindow(l->window);

    while (result == RUN_ON) {
        SDL_Event e;

        if (SDL_WaitEventTimeout(&e, 250)) {
            do {
                result = handle(l, &e);
            } while (result == RUN_ON && SDL_PollEvent(&e));
        }
        if (result == RUN_ON) {
            poll_drives(l);
            draw(l);
            SDL_RenderPresent(l->r);
        }
    }

    if (result == RUN_PLAY) {
        copy_str(out, cap, l->src[l->sel].path);
        ok = true;
    }
    shutdown_ui(l, ok && window != NULL);
    if (ok && window)
        *window = l->window;
    SDL_free(l);
    return ok;
}
