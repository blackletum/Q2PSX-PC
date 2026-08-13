/*
 * raster.h — a software rasteriser that obeys the PlayStation GPU's rules.
 *
 * This is the reference backend. It is not the fast one, and it is not meant to
 * be: it exists so that "what should this frame look like?" has an answer that
 * does not depend on a GPU driver's interpretation of anything. A hardware
 * backend is judged against this.
 *
 * The rules it implements, and why each one matters, are set out in
 * docs/FIDELITY.md. In brief:
 *
 *   - Vertices arrive as integers from the GTE. Nothing is re-interpolated at
 *     higher precision here, so the characteristic wobble survives into pixels.
 *   - UVs interpolate LINEARLY IN SCREEN SPACE. There is no W and no
 *     perspective correction. This is the single easiest rule to break by
 *     accident and the most visible when broken.
 *   - There is no depth buffer. Primitives are drawn in ordering-table order and
 *     later primitives overwrite earlier ones, full stop.
 *   - The framebuffer is RGB555. Colour is dithered with the GPU's 4x4 ordered
 *     matrix before truncation.
 *   - Four blend modes, no arbitrary alpha.
 *
 * Quads are drawn as two triangles in PSX corner order: (0,1,2) and (1,3,2).
 * The corners are a "Z", not a winding, so treating them as a triangle fan
 * produces an hourglass.
 */
#ifndef Q2PSX_RASTER_H
#define Q2PSX_RASTER_H

#include "gpu.h"
#include "q2psx.h"

typedef struct psx_framebuffer {
    u16 *px;        /* RGB555, width*height                       */
    int  width;
    int  height;
} psx_framebuffer;

q2_result psx_fb_init(psx_framebuffer *fb, int width, int height);
void      psx_fb_free(psx_framebuffer *fb);
void      psx_fb_clear(psx_framebuffer *fb, u16 colour);

/* Rendering options. Every one of these defaults to the faithful behaviour;
 * turning any of them off is a deliberate departure from the original. */
typedef struct psx_raster_opts {
    bool dither;             /* 4x4 ordered dither before 15-bit truncation  */
    bool affine_uv;          /* affine UVs — off means perspective-correct   */
    bool semi_transparency;  /* honour the four blend modes                  */
    bool textures;           /* sample VRAM — off draws flat/Gouraud only    */
} psx_raster_opts;

void psx_raster_opts_default(psx_raster_opts *opts);

/* Draw one primitive. `vram` may be NULL when textures are disabled. */
void psx_raster_prim(psx_framebuffer *fb,
                     const psx_prim *prim,
                     const psx_vram *vram,
                     const psx_raster_opts *opts);

/* Walk the ordering table far-to-near and draw everything in it. */
void psx_raster_ot(psx_framebuffer *fb,
                   const psx_ot *ot,
                   const psx_vram *vram,
                   const psx_raster_opts *opts);

/* Write the framebuffer as a binary PPM. The port does not need this, but the
 * offline tools do: being able to render a zone to a file without a window is
 * what makes the geometry pipeline testable before there is a client. */
q2_result psx_fb_write_ppm(const psx_framebuffer *fb, const char *path);

#endif /* Q2PSX_RASTER_H */
