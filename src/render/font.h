/*
 * font.h — a small bitmap font for drawing text into a framebuffer.
 *
 * This is NOT the original's HUD font. The game's own digits are texture data
 * that has not been located yet, and drawing them will mean sampling VRAM like
 * any other primitive. Until then a HUD needs *something* legible, and an
 * honest placeholder beats either inventing a font file or having no readout at
 * all — the numbers it shows are the simulation's real ones.
 *
 * Glyphs are 5x7, drawn as solid rectangles at an integer scale so they stay
 * crisp at any output size and never introduce filtering the rest of the
 * pipeline does not have.
 */
#ifndef Q2PSX_FONT_H
#define Q2PSX_FONT_H

#include "raster.h"
#include "q2psx.h"

#define Q2_FONT_W 5
#define Q2_FONT_H 7

/*
 * Draw `text` with its top-left at (x, y). Supports 0-9, A-Z, a-z (folded to
 * upper case), space, and : - . / % + . Unknown characters advance without
 * drawing. Returns the width drawn, so a caller can right-align.
 */
int psx_fb_text(psx_framebuffer *fb, int x, int y, const char *text,
                u16 colour, int scale);

/* Width `text` would occupy, without drawing it. */
int psx_fb_text_width(const char *text, int scale);

/* A filled rectangle, clipped to the framebuffer. Useful for HUD panels. */
void psx_fb_rect(psx_framebuffer *fb, int x, int y, int w, int h, u16 colour);

/*
 * Blend a filled rectangle towards `colour` by `alpha` in 0..255. The HUD
 * background wants to darken what is behind it without hiding it.
 */
void psx_fb_rect_blend(psx_framebuffer *fb, int x, int y, int w, int h,
                       u16 colour, int alpha);

#endif /* Q2PSX_FONT_H */
