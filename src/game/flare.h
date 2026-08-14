/*
 * flare.h — the lens flares, and what a light's `type` byte actually selects.
 *
 * ---------------------------------------------------------------------------
 * The type byte, which was open question #26
 * ---------------------------------------------------------------------------
 * Every one of the 7,814 lights on the disc carries a `type` of 7, 15, 23, 31
 * or 39 — `(n << 3) | 7` for n = 0..4 — and what those five values do was
 * recorded as unknown. They are not "styles" in the Quake sense (there is no
 * animation table anywhere in the image). The flare pass at 0x80075708 splits
 * the byte three ways:
 *
 *     bits 0-2   always 7 on this disc, never read by the flare path
 *     bits 3-5   the FLARE STYLE, 0..4. Style 0 draws nothing; 1..4 select one
 *                of four element lists in the executable
 *     bits 6-7   a SIZE SHIFT: the flare's world reach is 64 << k. Always 0
 *                here, so every flare on the disc is drawn at 64
 *
 * The style is tested before anything else is computed: the per-viewport pass
 * at 0x80075AA4 and 0x80075B58 masks the word at light+16 with 0x3800 and skips
 * the light entirely when it is zero. So a style-0 light lights the world and
 * is never seen; a style-1..4 light also puts a flare on the screen.
 *
 * Disc-wide: 6,183 lights at style 0, 660 at 1, 183 at 2, 2 at 3 and 786 at 4.
 *
 * ---------------------------------------------------------------------------
 * What a flare is made of
 * ---------------------------------------------------------------------------
 * A style is a NULL-terminated list of 8-byte elements at 0x800A1FDC,
 * 0x800A2014, 0x800A2024 and 0x800A203C:
 *
 *     u16 kind      1 = glow disc plus a starburst, 2 = plain disc
 *     s16 size      1.12 multiplier on the flare's screen radius
 *     s16 pos       1.12 position ALONG THE LINE from the screen centre to the
 *                   light: 4096 is at the light, 0 at the centre, negative is
 *                   mirrored through it
 *     s16 colour    1.12 multiplier on the flare's colour
 *
 * That `pos` field is what makes this a lens flare rather than a corona: style
 * 1's six elements sit at 1.0, -0.93, -0.25, 0.15, 1.5 and -1.25 of the way
 * along that line, at shrinking sizes and dimming colours. Style 4 is the same
 * list without its last element, style 3 is its first two, style 2 is its first
 * one alone — so the four styles are nested, and the only real authored choice
 * is how much of the flare a given light shows.
 *
 * ---------------------------------------------------------------------------
 * Where it is drawn, and how bright
 * ---------------------------------------------------------------------------
 * 0x80075708, per light per viewport:
 *
 *   - the light's position relative to the camera goes through MVMVA against
 *     the view rotation with NO translation (the delta is already relative),
 *     and the flare is dropped if the camera-space Z is under 256;
 *   - the same attenuation the entity lighting uses runs again, but with the
 *     size shift as its scale, so a flare's reach is the light's radius scaled
 *     by 64 << k rather than by 64;
 *   - inside the inner radius the colour is the light's own at full strength
 *     and the SIZE grows as `atten >> 7`; outside it the size pins to 32 and
 *     the COLOUR dims as `colour * atten >> 8`. One expression each side of
 *     4096, which is why a flare blooms as you walk into it and fades as you
 *     walk away.
 *
 * Both element kinds are untextured and additive: the disc is a fan of Gouraud
 * quads bright at the centre and black at the rim, and kind 1 adds eight
 * Gouraud lines radiating from the centre. There is no flare texture anywhere
 * on the disc, which is why looking for one never found anything.
 */
#ifndef Q2PSX_FLARE_H
#define Q2PSX_FLARE_H

/* The Lights chunk parser; see the note in lighting.h about the two entity.h. */
#include "../formats/entity.h"

#include "gpu.h"
#include "gte.h"
#include "lighting.h"
#include "q2psx.h"
#include "world.h"

/* Style 0 draws nothing; 1..4 have element lists. */
#define Q2_FLARE_STYLE_COUNT 5

/* The base screen reach before the type byte's size shift. */
#define Q2_FLARE_BASE_SIZE 64

/* Below this camera-space Z the flare is dropped (0x80075814). */
#define Q2_FLARE_MIN_Z 256

/* The floor the screen radius pins to once the light is past its inner radius
 * (0x800759BC), and the shift that turns a larger attenuation into a larger
 * flare (0x8007594C). */
#define Q2_FLARE_MIN_SCALE 32
#define Q2_FLARE_SCALE_SHIFT 7

/* The reference screen the ring generator's divisors are expressed against
 * (0x80074F94's /983040 and 0x80074EE4's /1310720, both over 4096). */
#define Q2_FLARE_REF_W 320
#define Q2_FLARE_REF_H 240

typedef enum q2_flare_kind {
    Q2_FLARE_KIND_NONE  = 0,
    Q2_FLARE_KIND_BURST = 1,   /* 12-gon glow + 8 radiating lines */
    Q2_FLARE_KIND_DISC  = 2    /* 6-gon flat disc                 */
} q2_flare_kind;

typedef struct q2_flare_element {
    u16 kind;      /* q2_flare_kind; 0 terminates the list */
    s16 size;      /* 1.12 */
    s16 pos;       /* 1.12 along centre -> light */
    s16 colour;    /* 1.12 */
} q2_flare_element;

/* The four lists, read out of the executable's data segment. `count` excludes
 * the terminator. Style 0 has none. */
typedef struct q2_flare_style {
    const q2_flare_element *element;
    u32                     count;
} q2_flare_style;

/* The style a light's type byte selects, and its size shift. */
u32 q2_flare_style_of(u8 type);
u32 q2_flare_size_of(u8 type);

/* The built-in table. Returns a style with count == 0 for 0 and for anything
 * out of range. */
const q2_flare_style *q2_flare_style_table(u32 style);

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Where the flare pass draws into. `centre` and `extent` are the viewport's
 * own, and they are two different things in the original — the screen centre
 * at playerctx+266/+268 that elements are positioned against, and the width and
 * height at +278/+280 that the ring generator scales by.
 */
typedef struct q2_flare_view {
    s16 centre[2];
    s16 extent[2];
    u16 bucket;      /* the OT bucket flares are linked into */
} q2_flare_view;

typedef struct q2_flare_stats {
    u32 lights_considered;
    u32 lights_styled;      /* style != 0                          */
    u32 rejected_near;      /* camera-space Z under Q2_FLARE_MIN_Z  */
    u32 rejected_dark;      /* attenuation zero                     */
    u32 flares_drawn;
    u32 prims_emitted;
    u32 ot_overflow;
} q2_flare_stats;

/*
 * Draw one light's flare, if it has one. Returns the number of primitives
 * emitted, which is zero for style 0, for a light behind the camera and for one
 * out of range.
 *
 * The GTE's rotation matrix must already hold the view rotation — this uses it
 * through MVMVA exactly as the original does, so the flare lands on the same
 * pixel the geometry does.
 *
 * The GTE's TRANSLATION must be zero. The original's per-viewport pass loads
 * TRX/TRY/TRZ from a scratch matrix at 0x800DDD7C whose contents were not
 * traced — neither of its two readers writes it. Zero is what makes the
 * transform self-consistent, because the vector fed to the GTE is already the
 * light's offset FROM the camera, and a non-zero translation there would move
 * every flare off its light by the same amount. It is a substitution, and this
 * is where it is recorded.
 */
u32 q2_flare_draw(const q2_light *l, const q2_camera *cam,
                  const q2_flare_view *view, psx_ot *ot, gte_state *gte,
                  q2_flare_stats *stats);

/*
 * The per-viewport pass at 0x800759F0: every dynamic light, then every static
 * light of the collision node the camera stands in. `coll_node` may be negative,
 * in which case only the dynamic lights are considered — that is the original's
 * own behaviour, and it means a flare is only visible from inside a node the
 * level's build tool linked it to.
 */
u32 q2_flare_draw_all(const q2_light_world *w, const q2_camera *cam,
                      s32 coll_node, const q2_flare_view *view,
                      psx_ot *ot, gte_state *gte, q2_flare_stats *stats);

#endif /* Q2PSX_FLARE_H */
