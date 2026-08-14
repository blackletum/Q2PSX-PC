/*
 * hud.h — the console's heads-up display: the overlay, and everything it does.
 *
 * ---------------------------------------------------------------------------
 * What the HUD is, and what it is not
 * ---------------------------------------------------------------------------
 * There is no status bar. No health number, no ammo counter, no armour
 * readout, no weapon icon parked in a corner. That is not a gap in the
 * reverse engineering — it is the finding. Every string the executable ever
 * formats was enumerated (all 30 call sites of the text printf at 0x80043518,
 * all 3 of the queueing wrapper at 0x800434B8, and every `sprintf` in the
 * image) and not one of them formats a player statistic. Hammerhead shipped a
 * clean screen and told the player how they were doing three other ways:
 *
 *   - a **screen flash** whose colour distinguishes a hit absorbed by armour
 *     from one that reached flesh, and whose strength scales with the damage
 *   - **pain sounds** chosen by health bracket (25 / 50 / 75 / 100)
 *   - **messages**, which is what the overlay is for
 *
 * So a port that adds a status bar is adding a feature. This module implements
 * what is there.
 *
 * ---------------------------------------------------------------------------
 * The parts
 * ---------------------------------------------------------------------------
 *   text            a markup language interpreted at 0x80042328, drawn as 8x8
 *                   sprites out of `chars.lbm` — see hudtables.h for the atlas
 *   notifications   a four-slot ring at ctx+0x140, one line expiring every 60
 *                   ticks, stacked down the top-left corner (0x80042B30)
 *   centre message  one line at ctx+0x240 with a 1200-tick life and a shaded
 *                   backdrop that fades as it ages
 *   crosshair       a 16x16 sprite at the middle of the view, switchable off
 *                   from the menu (0x80043A58). There is only one style: the
 *                   uv pair it reads is never written anywhere in the image
 *   flash           the damage tint described above (0x8003AE10..0x8003AF34)
 *
 * ---------------------------------------------------------------------------
 * The markup language
 * ---------------------------------------------------------------------------
 * Strings are not plain text. The interpreter consumes escapes inline, so the
 * pickup message really is the literal `@03BB8^828282|0~8%s~4`:
 *
 *     @XXXYY     move the pen: three hex digits of x, two of y
 *     ^RRGGBB    set the colour, six hex digits, modulating the glyph sprite
 *     |N         palette: `|0` picks built-in CLUT 72, anything else picks 74
 *                (and `|1` additionally makes spaces draw as glyphs)
 *     ~N         space width, N in 1..15, default 4 — this is why `~8` reads as
 *                looser text: a drawn glyph always advances 8, only the space
 *                is variable
 *     #XXXYYY    left and right margins, three hex digits each; setting either
 *                non-zero turns word wrapping on
 *     &X         draw an icon — a pre-rendered word from the atlas. Five of the
 *                letters draw two, because "Machine" and "gun" are separate
 *                cut-outs of one row
 *     *N         arm the backdrop: after the string is laid out, drop a 16x16
 *                shaded tile behind every glyph. N selects the tile, and
 *                smaller N is a smaller blob, which is how the box fades
 *     \n \r      newline. Both do the same thing; `\r` falls through into the
 *                newline arm in the original and is not a carriage return
 *
 * Escape state is global in the original — the pen, the margins, the palette
 * and the space width all persist from one string to the next — so it lives in
 * q2_hud_pen here rather than being reset per call.
 *
 * ---------------------------------------------------------------------------
 * Fidelity notes worth keeping
 * ---------------------------------------------------------------------------
 * Two of the original's quirks are reproduced deliberately and are called out
 * at their implementation sites: the string measurer at 0x800702A0 returns
 * `strlen - 1`, and the centre-message layout mixes character counts with a
 * pixel width when a string starts with an icon. Both shift text by a few
 * pixels, both are visible in the original, and "fixing" either would make the
 * port's centred messages land somewhere the console's never did.
 */
#ifndef Q2PSX_HUD_H
#define Q2PSX_HUD_H

#include "gpu.h"
#include "hudtables.h"
#include "inventory.h"
#include "q2psx.h"
#include "vram.h"

/* ------------------------------------------------------------------------- */
/* The font, once it is resident in VRAM                                      */
/* ------------------------------------------------------------------------- */
typedef struct q2_hud_font {
    const q2_hud_tables *tab;

    u16  tpage;        /* chars.lbm's texture page, 4bpp                     */
    u16  clut_font;    /* built-in palette 72 — `|0`, and the crosshair      */
    u16  clut_alt;     /* built-in palette 74 — `|1`, and the default        */
    u16  clut_box;     /* built-in palette 76 — the `*N` backdrop ramp       */

    bool resident;     /* false until the atlas and palettes are uploaded    */
} q2_hud_font;

/*
 * Decode `chars.lbm` out of a map's VRAM section and upload it, plus the
 * executable's built-in palettes, into `vram` at the addresses the console
 * used. After this the ordinary 4bpp CLUT sampling path just works, because
 * VRAM holds what the console's VRAM held.
 *
 * Returns Q2_ERR_NOT_FOUND when the map has no `chars.lbm` — two of the 49 do
 * not carry it, and a caller should treat that as "no overlay on this map"
 * rather than as an error.
 */
q2_result q2_hud_font_upload(q2_hud_font *out, const q2_hud_tables *tab,
                             const q2_vram_section *section, psx_vram *vram);

/* ------------------------------------------------------------------------- */
/* The drawing context — the original's per-player text context, cut down      */
/* ------------------------------------------------------------------------- */
/*
 * Layout happens in the console's own space and nowhere else. The `@` escape
 * carries three hex digits of x and only TWO of y, so a y over 255 does not
 * clamp — it lengthens the field and shifts everything after it. 512 x 248 is
 * the framebuffer the display init actually programs (FORMATS.md §9.9), and
 * every position the game hardcodes is inside it.
 *
 * `origin` places that 512 x 248 block inside a larger target surface. It is
 * deliberately a translation and not a scale: an 8 x 8 sprite is 8 x 8 texels
 * on this hardware, and stretching one would need filtering the rest of the
 * pipeline does not have. Presenting the overlay at a higher resolution is a
 * client decision, not a HUD one.
 */
#define Q2_HUD_SPACE_W 512
#define Q2_HUD_SPACE_H 248

typedef struct q2_hud_ctx {
    s16 width, height;    /* ctx+0x112 / +0x114: the view this draws into    */
    s16 home_x, home_y;   /* ctx+0x290 / +0x292: where a string starts       */
    u8  rgb[3];           /* ctx+0x294..296: the colour `^` writes           */

    s16 origin_x, origin_y;   /* where the block lands on the target surface */
} q2_hud_ctx;

/* The console's own 512 x 248, at the origin. */
void q2_hud_ctx_default(q2_hud_ctx *ctx, int width, int height);

/* The same, centred inside a target of `out_w` x `out_h`. */
void q2_hud_ctx_centre_in(q2_hud_ctx *ctx, int out_w, int out_h);

/* ------------------------------------------------------------------------- */
/* The pen — the original's $gp text globals, which persist across strings     */
/* ------------------------------------------------------------------------- */
typedef struct q2_hud_pen {
    s16  x, y;            /* 0x800B2AC8 / 0x800B2ACA                         */
    s32  left, right;     /* 0x800AE8E0 / 0x800AE8E4, set by `#`             */
    bool wrap;            /* 0x800AE8DC — (left | right) != 0                */
    s32  space_advance;   /* 0x800AE8E8, set by `~`; ships as 4              */
    s32  glyph_advance;   /* 0x800AE8EC — a constant 8, never written        */
    s32  palette;         /* 0x800AE8F0 — 72 or 74; ships as 74              */
    s32  palette_sub;     /* 0x800AE8F4 — 2 makes spaces draw                */
    bool overflow;        /* 0x800B2ACC — the primitive pool ran out         */
} q2_hud_pen;

void q2_hud_pen_default(q2_hud_pen *pen);

/*
 * Lay out and emit one markup string. Glyphs, icons and the backdrop all go
 * into bucket `otz` of `ot`; the backdrop is added last so that it draws first,
 * which is how the original gets it behind the text (see the ordering-table
 * note in gpu.h — within a bucket, last in draws first).
 *
 * Returns the number of glyph cells laid out, which is what the backdrop pass
 * walks. `pen` carries state out as well as in.
 */
u32 q2_hud_print(const q2_hud_font *font, q2_hud_ctx *ctx, q2_hud_pen *pen,
                 psx_ot *ot, u16 otz, const char *text);

/* The original's string measurer at 0x800702A0, off-by-one and all. */
int q2_hud_measure(const char *s);

/* ------------------------------------------------------------------------- */
/* Overlay state                                                              */
/* ------------------------------------------------------------------------- */
#define Q2_HUD_MSG_SLOTS      4
#define Q2_HUD_MSG_LEN       64
#define Q2_HUD_MSG_TICKS     60     /* one line retires every 60 ticks       */
#define Q2_HUD_MSG_X         24     /* the notification column               */
#define Q2_HUD_MSG_TOP       16
#define Q2_HUD_MSG_SPACING    9
#define Q2_HUD_CENTRE_LEN    64
#define Q2_HUD_CENTRE_TICKS 1200
#define Q2_HUD_CENTRE_DROP   20     /* below the middle of the view          */
#define Q2_HUD_BOX_START      7     /* `*7`, the darkest backdrop            */

/*
 * The screen flash (raised at 0x8003AE10, drawn at 0x80076764).
 *
 * Two colours, four bytes each in the executable's data: armour damage tints
 * the screen grey, health damage tints it red. `strength` counts down one per
 * frame and the tile's colour is scaled by strength/initial, so the flash is a
 * linear fade over as many frames as its initial strength.
 *
 * The strength arithmetic is asymmetric, and deliberately reproduced. Health
 * damage gives `damage/2 + 2`, saturating at 5 — more damage, brighter flash.
 * Armour damage gives `2 - loss/2`, which is *smaller* for a bigger hit until
 * it goes negative, at which point the original's `(u8)x < 6` test sees 0xFF,
 * fails, and clamps to the maximum. So an armour graze flashes faintly, a
 * five-point hit or worse flashes at full strength, and there is a dead band in
 * between where it does not flash at all. That is what the console does.
 */
#define Q2_HUD_FLASH_MAX 5
#define Q2_HUD_FLASH_MODE_SCALE 2   /* ctx+0x2A8: the mode the damage path uses */

typedef struct q2_hud_flash {
    u8  rgb[3];       /* ctx+0x2A0..0x2A2                                    */
    s16 strength;     /* ctx+0x2A4, decremented once per drawn frame         */
    s16 initial;      /* ctx+0x2A6, what strength is scaled against          */
    s16 mode;         /* ctx+0x2A8                                           */
} q2_hud_flash;

/* 0x800AE884 and 0x800AE888. */
extern const u8 q2_hud_flash_armour_rgb[3];
extern const u8 q2_hud_flash_health_rgb[3];

typedef struct q2_hud {
    /* Notifications: a ring, newest at write_index - 1. */
    char msg[Q2_HUD_MSG_SLOTS][Q2_HUD_MSG_LEN];
    /* ctx+0x284: zeroed when a line is pushed and aged here, but no reader was
     * located in the original. Kept because the field exists, not because it is
     * known to do anything. */
    s16  msg_age[Q2_HUD_MSG_SLOTS];
    s16  msg_write;
    s16  msg_count;
    s16  msg_expire;      /* ctx+0x28E, counts to 60 then retires one        */
    u8   msg_max;         /* from the by-player-count table; 4 single-player */

    /* The centre line. */
    char centre[Q2_HUD_CENTRE_LEN];
    s32  centre_age;      /* ctx+0x28C; drawn while < 1200                   */

    /* Crosshair. */
    bool crosshair;
    u8   crosshair_u, crosshair_v;    /* gp+796 / gp+797 — the style         */
    u8   crosshair_rgb[3];            /* gp+800..802                         */
    s16  crosshair_dx, crosshair_dy;  /* view+124 / +126 — recoil offset     */

    q2_hud_flash flash;

    /*
     * The pen is per-HUD rather than per-call because it is $gp state in the
     * original: a `|0` in a pickup message leaves palette 72 selected for every
     * string drawn after it, and a `~8` widens the spaces in the next line too.
     * Resetting it per string would be tidier and would not match.
     */
    q2_hud_pen pen;

    /* What the flash compares against, kept here so a caller only has to hand
     * over the current values. */
    s16 last_health;
    s16 last_armour;
    bool have_last;
} q2_hud;

void q2_hud_init(q2_hud *hud, const q2_hud_tables *tab, int players);

/* Push a notification. Long strings are truncated to 63 characters, which is
 * what the original's `strncpy(dst, src, 63)` at 0x80042DB0 does. */
void q2_hud_message(q2_hud *hud, const char *text);

/*
 * Set the centre line from an already-marked-up body. The position and the
 * backdrop are added here, reproducing the layout at 0x80042E14: centred on the
 * view, twenty pixels below the middle, `*7` unless the body already begins
 * with `|0`.
 */
void q2_hud_centre(q2_hud *hud, const q2_hud_tables *tab,
                   const q2_hud_ctx *ctx, const char *body);

/* The messages the game itself raises. */
void q2_hud_pickup(q2_hud *hud, const char *item_name);
void q2_hud_weapon_selected(q2_hud *hud, const q2_hud_tables *tab, int weapon_id);
void q2_hud_need_key(q2_hud *hud, const char *key_name);

/*
 * Feed the player's condition in. The first call only records a baseline; every
 * later one raises a flash if either value fell, with the armour case taking
 * precedence exactly as the original's branch order does.
 */
void q2_hud_track(q2_hud *hud, s16 health, s16 armour);

/* Advance one game tick: age the messages, retire the oldest, fade the box. */
void q2_hud_tick(q2_hud *hud, int ticks);

/*
 * Emit the whole overlay into bucket `otz`: the flash tile first so it lands
 * behind everything, then the notifications, the centre line and the
 * crosshair. Decrements the flash the way the draw at 0x80076980 does, so this
 * is once per drawn frame, not once per logic tick.
 */
void q2_hud_build_ot(q2_hud *hud, const q2_hud_font *font, q2_hud_ctx *ctx,
                     psx_ot *ot, u16 otz);

#endif /* Q2PSX_HUD_H */
