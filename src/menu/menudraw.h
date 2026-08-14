/*
 * menudraw.h — draw a menu page into a framebuffer.
 *
 * The layout is the original's, in its own 512x248 space; this maps that space
 * onto whatever surface it is handed rather than re-authoring it, so a page
 * keeps its proportions at 320x240 and at 1600x1200 alike.
 *
 * The glyphs are not the original's. The console's menu font is texture data in
 * VRAM that has not been located yet, so this uses the placeholder 5x7 font in
 * src/render — the *positions*, the widths, the centring, the bar geometry and
 * the colour codes are read out of the executable, the letterforms are not.
 */
#ifndef Q2PSX_MENUDRAW_H
#define Q2PSX_MENUDRAW_H

#include "menu.h"
#include "raster.h"

typedef struct q2_menu_style {
    u16 bright;        /* the normal text colour, and 'b'                  */
    u16 dim;           /* 'd' — the second font palette at 0x800E3FB4      */
    u16 grey;          /* 'g' — the (32,32,32) modulation at 0x8001B14C    */
    u16 title;

    u16 bar_frame;     /* the four edge lines of a slider                  */
    u16 bar_selected;  /* (255,255,255) when the row is under the cursor   */
    u16 bar_normal;    /* (74,143,170) otherwise — 0x8001BAA8              */
    u16 bar_grey;      /* (32,32,32) when the label is greyed              */

    u16 backdrop;      /* what the page is drawn over                      */
    int backdrop_alpha;/* 0 leaves the world visible, 255 hides it         */
} q2_menu_style;

void q2_menu_style_default(q2_menu_style *st);

/* Draw the current page. Does nothing when the menu is closed. */
void q2_menu_draw(const q2_menu *m, psx_framebuffer *fb,
                  const q2_menu_style *st);

#endif /* Q2PSX_MENUDRAW_H */
