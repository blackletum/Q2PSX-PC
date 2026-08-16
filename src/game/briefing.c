#include "briefing.h"

#include <stdio.h>
#include <string.h>

const u8 q2_briefing_rgb_label[3] = { 0x50, 0x8C, 0x78 };
const u8 q2_briefing_rgb_value[3] = { 0x78, 0x8C, 0x50 };

/* The `~4` is the original's: it resets the space width, because the escape
 * state is global and the screen before this one may have left it at 8. */
const char q2_briefing_label_location[]  = "~4Location:\n";
const char q2_briefing_label_orders[]    = "Current Orders:\n";
const char q2_briefing_label_objective[] = "\nMission Objective:\n";

const char q2_briefing_default_orders[]    = "Awaiting Orders.";
const char q2_briefing_default_objective[] = "Awaiting Mission Objective.";

static void copy_field(char *dst, const char *s)
{
    if (!s) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, s, Q2_BRIEFING_FIELD_MAX - 1);
    dst[Q2_BRIEFING_FIELD_MAX - 1] = '\0';
}

void q2_briefing_init(q2_briefing *b)
{
    if (!b)
        return;

    memset(b, 0, sizeof(*b));
    b->box.x = Q2_BRIEFING_BOX_X;
    b->box.y = Q2_BRIEFING_BOX_Y;
    b->box.w = Q2_BRIEFING_BOX_W;
    b->box.h = Q2_BRIEFING_BOX_H;

    copy_field(b->orders,    q2_briefing_default_orders);
    copy_field(b->objective, q2_briefing_default_objective);
}

void q2_briefing_set_location(q2_briefing *b, const char *s)
{
    if (b) copy_field(b->location, s);
}

void q2_briefing_set_orders(q2_briefing *b, const char *s)
{
    if (b) copy_field(b->orders, s);
}

void q2_briefing_set_objective(q2_briefing *b, const char *s)
{
    if (b) copy_field(b->objective, s);
}

/* ------------------------------------------------------------------------- */
/* The pop-up's state machine — 0x800213B0, 0x8002150C, 0x80021920            */
/* ------------------------------------------------------------------------- */
void q2_briefing_popup_init(q2_briefing_popup *p)
{
    if (!p)
        return;

    memset(p, 0, sizeof(*p));
    copy_field(p->orders,    q2_briefing_default_orders);
    copy_field(p->objective, q2_briefing_default_objective);
}

void q2_briefing_popup_set(q2_briefing_popup *p, const char *orders,
                           const char *objective)
{
    if (!p)
        return;

    /*
     * 0x80020F34, twice over: a Strings lookup, and the shipped default when
     * it misses. The caller does the lookup — this module has no text database
     * — and hands NULL or "" for a key that did not resolve.
     */
    copy_field(p->orders,
               (orders && orders[0]) ? orders : q2_briefing_default_orders);
    copy_field(p->objective,
               (objective && objective[0]) ? objective
                                           : q2_briefing_default_objective);
}

/* The open arm of 0x800213B0 — everything under `beq s1, zero`. */
static void popup_open(q2_briefing_popup *p, s32 seconds, s32 level_time)
{
    if (seconds <= 0) {
        /*
         * 0x8002147C: a non-positive second count becomes 0x127500, which is
         * 1,209,600 dt units — 67 minutes. Not a timeout so much as "until
         * something else closes it", and reproduced rather than clamped to a
         * sensible number, because a script that passes zero means that.
         */
        p->deadline = level_time + 0x127500;
    } else {
        p->deadline = level_time + seconds * Q2_BRIEFING_TICKS_PER_SEC;
    }

    p->visible       = true;
    p->delay_ticks   = 0;
    /* The three per-view button latches are cleared at the open, which is what
     * this flag stands in for: the dismiss has to see a clear frame first. */
    p->dismiss_armed = false;
}

bool q2_briefing_popup_raise(q2_briefing_popup *p, s32 delay, s32 seconds,
                             s32 level_time, s32 frame_dt)
{
    if (!p)
        return false;

    if (delay == 0) {
        popup_open(p, seconds, level_time);
        return true;
    }

    /*
     * 0x800213E8: `gp+284 = delay * frame_dt * 2` and `gp+288 = seconds`. The
     * doubling is the instruction's, not a unit conversion — the multiply is
     * against the frame delta and the result is shifted left one.
     */
    if (frame_dt <= 0)
        frame_dt = 1;
    p->delay_ticks   = delay * frame_dt * 2;
    p->delay_seconds = seconds;
    return false;
}

void q2_briefing_popup_close(q2_briefing_popup *p, s32 level_time)
{
    if (!p)
        return;

    /* 0x80021920: clear the visible flag, drop the menu-owns-the-frame flag,
     * and park the deadline at now. */
    p->visible       = false;
    p->deadline      = level_time;
    p->dismiss_armed = false;
}

bool q2_briefing_popup_tick(q2_briefing_popup *p, s32 dt, s32 level_time,
                            bool cross_held, bool cross_prev)
{
    if (!p)
        return false;

    if (p->visible) {
        /*
         * 0x80021530: `sltu v0, level_clock, deadline` — falling through means
         * the deadline has been reached, and the screen closes.
         */
        if (level_time >= p->deadline) {
            q2_briefing_popup_close(p, level_time);
            return false;
        }

        /*
         * 0x800217C0. The dismiss is an EDGE and it is armed late: the arm
         * flag is only raised on a frame where CROSS is not held, so the press
         * that opened the screen cannot also close it.
         */
        if (p->dismiss_armed && cross_held && !cross_prev) {
            q2_briefing_popup_close(p, level_time);
            return false;
        }
        if (!cross_held)
            p->dismiss_armed = true;

        return true;
    }

    /*
     * 0x80021830: the countdown, by the frame's own dt, clamped at zero, and
     * on reaching it the raise is re-entered with delay 0.
     */
    if (p->delay_ticks > 0) {
        p->delay_ticks -= dt;
        if (p->delay_ticks <= 0) {
            p->delay_ticks = 0;
            popup_open(p, p->delay_seconds, level_time);
            return true;
        }
    }

    return false;
}

/* Append, refusing to truncate: a half-written escape would be interpreted. */
static bool append(char *out, u32 out_size, u32 *len, const char *s)
{
    u32 n = (u32)strlen(s);

    if (*len + n + 1 > out_size)
        return false;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
    return true;
}

static bool append_colour(char *out, u32 out_size, u32 *len, const u8 rgb[3])
{
    char esc[8];

    sprintf(esc, "^%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
    return append(out, out_size, len, esc);
}

u32 q2_briefing_compose(const q2_briefing *b, char *out, u32 out_size)
{
    char margins[16];
    u32 len = 0;

    if (!b || !out || out_size == 0)
        return 0;
    out[0] = '\0';

    /*
     * `#06A196` then `|0` and a newline — 0x800AE740 and 0x800AE748. Setting
     * either margin non-zero is what turns wrapping on (hud.h), so this one
     * escape is the whole reason the objective flows onto a second line.
     */
    sprintf(margins, "#%03X%03X", Q2_BRIEFING_MARGIN_L, Q2_BRIEFING_MARGIN_R);

    if (!append_colour(out, out_size, &len, q2_briefing_rgb_label)) return 0;
    if (!append(out, out_size, &len, margins))                      return 0;
    if (!append(out, out_size, &len, "|0\n"))                       return 0;

    if (!append(out, out_size, &len, q2_briefing_label_location))   return 0;
    if (!append_colour(out, out_size, &len, q2_briefing_rgb_value)) return 0;
    if (!append(out, out_size, &len, b->location))                  return 0;
    if (!append(out, out_size, &len, "\n\n"))                       return 0;

    if (!append_colour(out, out_size, &len, q2_briefing_rgb_label)) return 0;
    if (!append(out, out_size, &len, q2_briefing_label_orders))     return 0;
    if (!append_colour(out, out_size, &len, q2_briefing_rgb_value)) return 0;
    if (!append(out, out_size, &len, b->orders))                    return 0;
    if (!append(out, out_size, &len, "\n"))                         return 0;

    if (!append_colour(out, out_size, &len, q2_briefing_rgb_label)) return 0;
    if (!append(out, out_size, &len, q2_briefing_label_objective))  return 0;
    if (!append_colour(out, out_size, &len, q2_briefing_rgb_value)) return 0;
    if (!append(out, out_size, &len, b->objective))                 return 0;
    if (!append(out, out_size, &len, "\n"))                         return 0;

    /* 0x800AE758. Margins off, so the next string on any screen is not still
     * wrapping at 406. */
    if (!append(out, out_size, &len, "#000000"))                    return 0;

    return len;
}

u32 q2_briefing_build_ot(const q2_briefing *b, const q2_hud_font *font,
                         const q2_menu_font *menu_font,
                         q2_hud_ctx *ctx, q2_hud_pen *pen, psx_ot *ot,
                         u32 body_bucket, u32 frame_bucket, u16 text_bucket)
{
    char text[Q2_BRIEFING_FIELD_MAX * 4];
    u32 n = 0;

    if (!b || !font || !ctx || !pen || !ot)
        return 0;

    /*
     * The panel goes down first. On the console the order is the other way
     * round — the text is composed, then `q2_panel_draw` runs — but that is a
     * consequence of the panel owning fixed primitive slots, not of depth: the
     * two land in ordering-table buckets that put the frame behind the glyphs
     * either way. Here the buckets say it, so the call order is free.
     */
    /*
     * THE PANEL'S TWO NUMBERS ARE DEPTHS HERE AND BUCKETS THERE, and that is
     * why the briefing drew as unreadable text over bare geometry for as long
     * as it existed.
     *
     * `q2_hud_print` takes a DEPTH: `psx_ot_add` inverts it, so depth 0 is the
     * front. `q2_panel_build_ot` takes an ABSOLUTE BUCKET through
     * `psx_ot_add_bucket` — gpu.h's own "escape", for packets whose place is
     * structural rather than depth-derived — and `psx_ot_walk` draws bucket 0
     * FIRST. So the caller's `2, 1, 0`, which reads as three depths going from
     * back to front, put the panel at the two furthest-back buckets in the
     * whole table and the text at the front: the world was drawn on top of the
     * panel and under the glyphs.
     *
     * The two numbers are depths at this interface, because the third one is
     * and a caller cannot reasonably be asked to mix the two spaces in one
     * call. Mapping them here is what makes `2, 1, 0` mean what it looks like.
     *
     * It went unnoticed because this is the panel's ONLY caller in the port.
     */
    if (menu_font)
        n += q2_panel_build_ot(menu_font, &b->box, ot,
                               psx_ot_depth_bucket(ot, body_bucket),
                               psx_ot_depth_bucket(ot, frame_bucket), NULL);

    if (q2_briefing_compose(b, text, sizeof(text)) == 0)
        return n;

    /*
     * The start is the CONTEXT's, not the pen's. `q2_hud_print` loads
     * `pen->x`/`pen->y` from `ctx->home_x`/`home_y` on entry (ctx+0x290) and
     * writes them back when it returns, so setting the pen here instead looks
     * right and does nothing: the text lands wherever the previous string left
     * the context, which for this screen is twenty-odd pixels above the panel
     * it is supposed to sit inside.
     *
     * x matters only until the string's first newline — `#06A196` has turned
     * wrapping on by then, so every line after it starts at the left margin.
     */
    ctx->home_x = (s16)b->box.x;
    ctx->home_y = (s16)b->box.y;

    n += q2_hud_print(font, ctx, pen, ot, text_bucket, text);
    return n;
}

/* ------------------------------------------------------------------------- */
/* The end-of-mission placard                                                 */
/* ------------------------------------------------------------------------- */
void q2_endmission_init(q2_endmission *e)
{
    if (!e)
        return;

    memset(e, 0, sizeof(*e));
    e->box.x = Q2_BRIEFING_BOX_X;
    e->box.y = Q2_BRIEFING_BOX_Y;
    e->box.w = Q2_BRIEFING_BOX_W;
    e->box.h = Q2_BRIEFING_BOX_H;
}

void q2_endmission_set(q2_endmission *e, const char *title, const char *body)
{
    if (!e)
        return;

    if (title)
        snprintf(e->title, sizeof(e->title), "%s", title);
    if (body)
        snprintf(e->body, sizeof(e->body), "%s", body);
}

u32 q2_endmission_compose(const q2_endmission *e, char *out, u32 out_size)
{
    char margins[16];
    u32 len = 0;

    if (!e || !out || out_size == 0)
        return 0;
    out[0] = '\0';

    sprintf(margins, "#%03X%03X", Q2_BRIEFING_MARGIN_L, Q2_BRIEFING_MARGIN_R);

    if (!append(out, out_size, &len, margins))                      return 0;
    if (!append(out, out_size, &len, "|0\n\n"))                     return 0;

    if (!append_colour(out, out_size, &len, q2_briefing_rgb_label)) return 0;
    if (!append(out, out_size, &len, e->title))                     return 0;
    if (!append(out, out_size, &len, "\n\n"))                       return 0;

    if (!append_colour(out, out_size, &len, q2_briefing_rgb_value)) return 0;
    if (!append(out, out_size, &len, e->body))                      return 0;
    if (!append(out, out_size, &len, "\n"))                         return 0;

    if (!append(out, out_size, &len, "#000000"))                    return 0;

    return len;
}

u32 q2_endmission_build_ot(const q2_endmission *e, const q2_hud_font *font,
                           const q2_menu_font *menu_font,
                           q2_hud_ctx *ctx, q2_hud_pen *pen, psx_ot *ot,
                           u32 body_bucket, u32 frame_bucket, u16 text_bucket)
{
    char text[Q2_BRIEFING_FIELD_MAX * 4];
    u32 n = 0;

    if (!e || !font || !ctx || !pen || !ot)
        return 0;

    if (menu_font)
        n += q2_panel_build_ot(menu_font, &e->box, ot,
                               psx_ot_depth_bucket(ot, body_bucket),
                               psx_ot_depth_bucket(ot, frame_bucket), NULL);

    if (q2_endmission_compose(e, text, sizeof(text)) == 0)
        return n;

    ctx->home_x = (s16)e->box.x;
    ctx->home_y = (s16)e->box.y;

    n += q2_hud_print(font, ctx, pen, ot, text_bucket, text);
    return n;
}
