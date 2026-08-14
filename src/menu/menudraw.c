/*
 * menudraw.c — the menu, drawn.
 *
 * Layout facts, all from the executable:
 *
 *   0x8001CF74  the title's y is (framebuffer_height - 188) / 2 + 10, and its
 *               x is a hard 256 — the centre of the 512-wide framebuffer
 *   0x8001CFA4  the title is drawn at font size 32, items at 16
 *               (set_items passes 16 at every call site)
 *   0x8001BBB4  a slider's bar is 133 wide and 8 tall, its top at y - 4, and
 *               it starts where the label ends — which is why every slider
 *               label in the tables is space-padded to exactly 11 characters
 *   0x8001BD28  the fill runs to bar_x + value + 3
 *   0x8001BAA8  white when the row is selected, (74,143,170) when not,
 *               (32,32,32) when the label is greyed
 *
 * An item's y is the vertical *centre* of its row, not its top: the title sits
 * at 40 with a 32-pixel face while the first item sits at 64 with a 16-pixel
 * one, which only avoids overlapping if both are centred.
 */
#include "menudraw.h"

#include "font.h"

#include <string.h>

void q2_menu_style_default(q2_menu_style *st)
{
    if (!st)
        return;

    /* psx_rgb555 takes 8-bit components, so these are the original's own
     * numbers where it has them: 0x8001B14C modulates a glyph by 128 normally
     * and 32 when greyed, and 0x8001BAA8 gives the slider its three colours. */
    st->bright         = psx_rgb555(255, 255, 255);
    st->dim            = psx_rgb555(128, 128, 144);
    st->grey           = psx_rgb555(64, 64, 64);
    st->title          = psx_rgb555(255, 210, 96);

    st->bar_frame      = psx_rgb555(74, 143, 170);
    st->bar_selected   = psx_rgb555(255, 255, 255);
    st->bar_normal     = psx_rgb555(74, 143, 170);
    st->bar_grey       = psx_rgb555(32, 32, 32);

    st->backdrop       = psx_rgb555(0, 0, 0);
    st->backdrop_alpha = 150;
}

/* ------------------------------------------------------------------------- */
/* The original's space mapped onto this surface. */

static int map_x(const psx_framebuffer *fb, int x)
{
    return x * fb->width / Q2_MENU_SCREEN_W;
}

static int map_y(const psx_framebuffer *fb, int y)
{
    return y * fb->height / Q2_MENU_SCREEN_H;
}

/*
 * The largest integer glyph scale that still fits the original's row, so text
 * is never filtered.
 *
 * Width is the binding constraint, not height: the longest label the menu ever
 * draws is the death screen's "RESUPPLY AND RESTART (2 LEFT)" at 29 characters,
 * and the original gives each of them a 16-pixel cell inside a 512-wide
 * framebuffer. Matching that ratio — 6 pixels of placeholder cell against 16 of
 * the original's, scaled by the surface — is `width / 192`. Height is checked
 * too, for surfaces that are wide and short.
 */
static int item_scale(const psx_framebuffer *fb)
{
    int by_width  = fb->width / ((Q2_FONT_W + 1) * Q2_MENU_SCREEN_W / 16);
    int by_height = (16 * fb->height / Q2_MENU_SCREEN_H) / Q2_FONT_H;
    int s = by_width < by_height ? by_width : by_height;

    if (s < 1) s = 1;
    if (s > 8) s = 8;
    return s;
}

/* ------------------------------------------------------------------------- */
/* Text with the original's colour codes. */

static u16 code_colour(char code, const q2_menu_style *st, u16 base)
{
    switch (code) {
    case Q2_MENU_CODE_BRIGHT: return st->bright;
    case Q2_MENU_CODE_DIM:    return st->dim;
    case Q2_MENU_CODE_GREY:   return st->grey;
    case Q2_MENU_CODE_UNDER:  return st->bright;
    default:                  return base;
    }
}

static int coded_width(const char *s, int scale)
{
    return q2_menu_text_length(s) * (Q2_FONT_W + 1) * scale;
}

/* Draw `s`, honouring the codes, with its top-left at (x, y). */
static void draw_coded(psx_framebuffer *fb, int x, int y, const char *s,
                       u16 base, int scale, const q2_menu_style *st)
{
    char run[64];
    u16  colour = base;
    int  n = 0;

    for (;;) {
        char c = *s;

        if (c == '\0' || q2_menu_is_code(c)) {
            if (n) {
                run[n] = '\0';
                x += psx_fb_text(fb, x, y, run, colour, scale);
                n = 0;
            }
            if (c == '\0')
                return;
            colour = code_colour(c, st, base);
            s++;
            continue;
        }

        if (n < (int)sizeof(run) - 1)
            run[n++] = c;
        s++;
    }
}

/* ------------------------------------------------------------------------- */

static void draw_slider(psx_framebuffer *fb, int label_right, int cy,
                        int value, bool selected, bool greyed,
                        const q2_menu_style *st)
{
    /* 0x8001BBB4: 133 x 8 in the original's pixels, top at y - 4. */
    const int bar_w = 133, bar_h = 8;
    int x0 = label_right + 4;
    int x1 = x0 + bar_w;
    int y0 = cy - bar_h / 2;
    int y1 = y0 + bar_h;

    int px0 = map_x(fb, x0), px1 = map_x(fb, x1);
    int py0 = map_y(fb, y0), py1 = map_y(fb, y1);
    int fill;
    u16 colour = greyed ? st->bar_grey
                        : (selected ? st->bar_selected : st->bar_normal);

    if (py1 <= py0) py1 = py0 + 1;

    /* The frame is four one-pixel lines, drawn as four quads in the original. */
    psx_fb_rect(fb, px0, py0, px1 - px0, 1, st->bar_frame);
    psx_fb_rect(fb, px0, py1 - 1, px1 - px0, 1, st->bar_frame);
    psx_fb_rect(fb, px0, py0, 1, py1 - py0, st->bar_frame);
    psx_fb_rect(fb, px1 - 1, py0, 1, py1 - py0, st->bar_frame);

    /* 0x8001BD28: the fill's right edge is bar_x + value + 3. */
    if (value < 0) value = 0;
    if (value > Q2_MENU_SLIDER_MAX) value = Q2_MENU_SLIDER_MAX;
    fill = map_x(fb, x0 + value + 3) - px0;
    if (fill < 0) fill = 0;

    psx_fb_rect(fb, px0 + 1, py0 + 1, fill, (py1 - py0) - 2, colour);
}

void q2_menu_draw(const q2_menu *m, psx_framebuffer *fb,
                  const q2_menu_style *st)
{
    q2_menu_style fallback;
    int scale, title_scale, glyph_h, i;

    if (!m || !m->open || !m->page || !fb || !fb->px)
        return;

    if (!st) {
        q2_menu_style_default(&fallback);
        st = &fallback;
    }

    if (st->backdrop_alpha > 0)
        psx_fb_rect_blend(fb, 0, 0, fb->width, fb->height,
                          st->backdrop, st->backdrop_alpha);

    scale       = item_scale(fb);
    title_scale = scale * 2;
    glyph_h     = Q2_FONT_H * scale;

    if (m->page->title) {
        int ty = q2_menu_title_y(m->screen_h);
        int w  = coded_width(m->page->title, title_scale);
        draw_coded(fb, map_x(fb, 256) - w / 2,
                   map_y(fb, ty) - (Q2_FONT_H * title_scale) / 2,
                   m->page->title, st->title, title_scale, st);
    }

    for (i = 0; i < (int)m->page->count; i++) {
        const q2_menu_item *it = &m->page->items[i];
        char line[80];
        const char *label;
        bool selected, greyed;
        int w, x, y;

        label = q2_menu_item_text(m, i);
        if (!label[0] && it->widget != Q2_WIDGET_CHOICE)
            continue;                     /* the empty-string records */

        selected = (i == m->cursor) && q2_menu_item_selectable(m, i);
        greyed   = !q2_menu_item_selectable(m, i) &&
                   i >= (int)m->page->first;

        q2_menu_item_display(m, i, line, (u32)sizeof(line));

        w = coded_width(line, scale);
        x = map_x(fb, it->x) - w / 2;
        y = map_y(fb, it->y) - glyph_h / 2;

        draw_coded(fb, x, y, line,
                   selected ? st->bright : (greyed ? st->grey : st->dim),
                   scale, st);

        if (it->widget == Q2_WIDGET_SLIDER) {
            int value = 0;
            /* The bar begins where the label ends. The label is centred on
             * it->x, so its right edge is half its width further on — taken
             * back into the original's units, since that is where the bar's
             * 133x8 geometry is defined. */
            int label_right = it->x + (w / 2) * Q2_MENU_SCREEN_W / fb->width;

            if (m->set && it->setting > Q2_SET_NONE && it->setting < Q2_SET_COUNT)
                value = m->set->v[it->setting];

            draw_slider(fb, label_right, it->y, value, selected, greyed, st);
        }
    }

    /* The single-player pause menu's status line, at (256, 204). */
    if (m->status[0]) {
        int w = coded_width(m->status, scale);
        draw_coded(fb, map_x(fb, 256) - w / 2,
                   map_y(fb, 204) - glyph_h / 2,
                   m->status, st->dim, scale, st);
    }
}
