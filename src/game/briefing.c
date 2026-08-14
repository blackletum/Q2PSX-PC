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
    if (menu_font)
        n += q2_panel_build_ot(menu_font, &b->box, ot,
                               body_bucket, frame_bucket, NULL);

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
