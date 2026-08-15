/*
 * statusbar.h — the status bar. Health, ammo, armour, and their icons.
 *
 * ---------------------------------------------------------------------------
 * The bar this project spent a long time proving did not exist
 * ---------------------------------------------------------------------------
 * FORMATS.md §11.1 asserted, as a *finding*, that Quake II PSX shows no health,
 * ammo or armour readout. It was reached by enumerating every `printf` and
 * `sprintf` site and every reader of the font table, exhaustively — and it was
 * wrong, because the bar renders **sprites**, not characters, and neither
 * instrument can see a sprite. Retail capture settled it.
 *
 * The retraction is in §11.1. What follows is the thing itself.
 *
 * ---------------------------------------------------------------------------
 * Where it is drawn from — and it is not a screen
 * ---------------------------------------------------------------------------
 * `0x800337D0` is not "some composite reached through a pointer". It is the
 * **per-viewport draw hook** — the function the one-player layout stores in the
 * view record at `+308`, alongside `0x80033D30` and `0x80034288` for the two
 * splits (screen.h). So the bar is drawn once per viewport, by the same call
 * that draws that viewport's world, and everything about it follows from that:
 * it is anchored to the viewport rather than the screen, it is drawn per player
 * in split screen, and its icons shrink with the viewport.
 *
 * ---------------------------------------------------------------------------
 * The anchor — view+304 and view+306
 * ---------------------------------------------------------------------------
 * screen.h carried these two halfwords as `pad_a` / `pad_b`, unknown. They are
 * the bar's origin: every field's position is a literal offset from them
 * (`0x800337EC` onward), so a layout positions the whole bar by writing two
 * numbers and the fields follow.
 *
 * The layouts already wrote them, which is the confirmation: one player gets
 * **(93, 201)** (`0x80077E60` / `0x80077E6C`) and a split viewport (0, 95)
 * (`0x80077A98`). At (93, 201) the health digits land at x = 22, 46 and 70 with
 * the cross at 93, and the row sits at y = 201 of a 248-line screen — the
 * bottom-left corner retail capture shows, arrived at from the code alone.
 *
 * ---------------------------------------------------------------------------
 * The fields
 * ---------------------------------------------------------------------------
 * Thirteen 10-byte records are built on the stack at `sp+200`, each
 * `{s16 x, s16 y, u8 u, u8 v, u8 w, u8 h, u8 dst_w, u8 dst_h}` — a source rect
 * in the sheet plus a destination size. They initialise to the 1 x 1 blank at
 * (255, 255) and the sub-draws fill in the real rect.
 *
 * Sorted by x they fall into TWO ROWS. The main row, at the anchor, is **three
 * digits then an icon, three times**, digits 24 apart because a numeral cell is
 * 24 wide, plus a single field far right:
 *
 *     -71  -47  -23   digits, health           +0    icon   (38 wide)
 *     +64  +88  +112  digits, ammo             +135  icon
 *     +179 +203 +227  digits, armour           +250  icon
 *     +330            the FRAG count in deathmatch
 *
 * A second row sits **24 to 25 above** it, and capture shows what it carries: an
 * icon on the left beside a pickup caption, and a two-digit counter with its own
 * icon on the right.
 *
 *     -71             upper-left icon          (9 wide)
 *     +256 +280       two digits               +330  icon
 *
 * Health's icon is initialised 38 wide and the upper-left one 9, where the rest
 * are 8; every one of those is overwritten by the sub-draw with either the icon
 * record's own size or the split-screen clamp, so an initialiser only shows for
 * a field that is never filled.
 *
 * ---------------------------------------------------------------------------
 * The numerals — 0x8009C598
 * ---------------------------------------------------------------------------
 * Ten four-byte `{u, v, w, h}` records, all at **v = 168**, **24 x 24**, with
 * `u = 24 * digit`. They sit in the same sheet as the icons, on the row below
 * the icon grid — which is why the sheet decoded to "32 x 24 item icons
 * followed by a set of large digits" and why that was the tell nobody read.
 *
 * Two more records follow: one at (0, 192) 24 x 24, and the 1 x 1 blank.
 *
 * ---------------------------------------------------------------------------
 * What is still not read
 * ---------------------------------------------------------------------------
 * Which counter is which. The three groups are structurally identical and the
 * sub-draws that fill them (`0x800352C0`, `0x80035554`, `0x800359C0`) were not
 * followed far enough to say which reads health and which reads armour. Retail
 * capture shows the order left-to-right is health, ammo, armour, and this
 * module uses that — from the screenshot, not from the code, and it says so at
 * the enum.
 *
 * ---------------------------------------------------------------------------
 * THREE LAYOUTS, and this is the one-player one
 * ---------------------------------------------------------------------------
 * The three hooks the layouts install are three DIFFERENT field tables, not one
 * table drawn three times. The two-player hook at `0x80033D30` builds its own:
 * two counters rather than three, digits **20 apart** rather than 24, at
 * +46/+66/+86 with its icon at +106 and +170/+190/+210 with +230, and its
 * far-right field at +354. Capture agrees — a split viewport shows health and
 * ammo and a frag count, and no armour.
 *
 * The table below is the ONE-PLAYER layout. The two-player offsets are recorded
 * in `q2_sbar_fields_2p`; the quad hook at `0x80034288` has not been read.
 */
#ifndef Q2PSX_STATUSBAR_H
#define Q2PSX_STATUSBAR_H

#include "gpu.h"
#include "icontable.h"

/* Forward-declared so a caller that never colours the bar need not pull the
 * whole executable-table module in. */
struct q2_hud_tables;

/* ------------------------------------------------------------------------- */
/* The numeral cells — 0x8009C598                                             */
/* ------------------------------------------------------------------------- */
#define Q2_SBAR_DIGIT_ADDR  0x8009C598u
#define Q2_SBAR_DIGITS      10
#define Q2_SBAR_DIGIT_W     24
#define Q2_SBAR_DIGIT_H     24
#define Q2_SBAR_DIGIT_V    168
#define Q2_SBAR_DIGIT_PITCH 24   /* and the field table's own x stride */

/* Unity for a modulated primitive — the same 0x80 every other UI sprite uses. */
#define Q2_SBAR_MOD 128

/* ------------------------------------------------------------------------- */
/* The icons — RECT INDICES, hard-coded, NOT item effect ids                  */
/* ------------------------------------------------------------------------- */
/*
 * CORRECTION — the bar indexes the rect table. It does not scan it.
 *
 * This block used to name item `effect` ids and resolve them through
 * `q2_icon_rect_for_id()`, on icontable.h's reading that a rect record's fifth
 * byte is the item's touch-dispatch index. The three sub-draws say otherwise,
 * in the plainest way a disassembly can:
 *
 *     80035190  lbu  v0, 170(t0)     t0 = 0x8009C478   health, ALWAYS 170
 *     8003565C  lbu  v0, 150(a0)     a0 = 0x8009C478   armour, ALWAYS 150
 *     80035374  sll  v0, a0, 2       a0 = ammoIcon[weapon]
 *     80035378  addu v0, v0, a0                        ammo,   a0 * 5
 *     80035380  addu v1, a0, t0                        &rect[a0]
 *
 * 170 and 150 are byte offsets into a five-byte record: rect 34 and rect 30.
 * The ammo path multiplies its table entry by five and adds the same base, so
 * `ammoIcon[]` is a rect index too. There is no strcmp, no scan, no compare of
 * the fifth byte anywhere in any of the three.
 *
 * WHAT THE OLD READING DREW, and why it looked plausible enough to ship:
 * scanning for effect 34 finds rect **30**, because rect 30's fifth byte
 * happens to be 34. Rect 30 is the ARMOUR icon. So the health field drew the
 * armour box — a chunky machine-looking sprite where retail shows a blue cross
 * — and because it drew *something* recognisable, it read as a mis-picked icon
 * rather than as a whole mis-read table. Rect 34 is the cross.
 *
 * The fifth byte is a PALETTE INDEX (see below), which is why joining it to
 * the item table produced a near-monotonic and therefore convincing-looking
 * correspondence. Two independent tells confirm the index reading against it:
 * the cell the effect reading assigns to the power shield is EMPTY, and the
 * cells the index reading assigns to the six ammo types are six ammo boxes.
 */
#define Q2_SBAR_ICON_HEALTH   34   /* 0x80035178, offset 170 — the cross */
#define Q2_SBAR_ICON_ARMOUR   30   /* 0x80035554, offset 150             */
#define Q2_SBAR_ICON_NONE      0   /* rect 0 is the 1x1 blank            */

/* ------------------------------------------------------------------------- */
/* The palettes — one per sprite, and the port used to ignore all of them     */
/* ------------------------------------------------------------------------- */
/*
 * A field record is TEN bytes and the last two are not padding:
 *
 *     +0 s16 x   +2 s16 y   +4 u  +5 v  +6 w  +7 h  +8 PALETTE  +9 SLOT
 *
 * `0x800337EC` onward initialises all thirteen with +9 = 14 — the VRAM slot
 * qk_menu.lbm registers under — and +8 = 8 for every field except the health
 * icon, which gets 38 (`addiu v0, zero, 38` at 0x800337FC, stored at
 * 0x80033810). The sub-draws then overwrite +8 per sprite: an icon takes the
 * fifth byte of its own rect record (0x80035210 / 0x8003540C), and a counter
 * running low takes 7.
 *
 * Those three indices are the built-in palette bank's (hudtables.h), and
 * reading them out settles what they are beyond argument:
 *
 *     8   pale cyan ramp to (160,200,224)   the numerals
 *     38  blue ramp to near-white           the health cross
 *     7   red/orange ramp to (248,64,0)     the low-value flash
 *
 * The port passed ONE clut — the menu font's — for every sprite in the bar,
 * so the numerals, the icons and the flash all came out in the font's colours.
 * That is what made the HUD look wrong while the world looked right, and it is
 * a separate defect from the framebuffer-format swap in client/main.c: this one
 * is visible in a `--shot` capture, and that one is not.
 */
#define Q2_SBAR_PAL_DIGITS   8   /* field init, 0x800337FC..0x80033814      */
#define Q2_SBAR_PAL_LOW      7   /* 0x8003524C / 0x80035460                 */

/*
 * When a counter flashes. Health is `slti v1, 26` at 0x8003523C and ammo is
 * `sltiu v0, 6` at 0x80035440 — different numbers, so they are kept apart
 * rather than folded into one "low" constant.
 *
 * Below the threshold the digits alternate between their own palette and 7 on
 * bit 7 of the frame counter at `0x800AEBAC`, except that health at or below
 * zero holds 7 solid (the `blez` at 0x80035248 skips the blink test).
 */
#define Q2_SBAR_LOW_HEALTH  26
#define Q2_SBAR_LOW_AMMO     6
#define Q2_SBAR_BLINK_BIT 0x80u

/*
 * The one weapon whose ammo digits are blanked — the blaster, slot 1.
 * `0x8003549C` tests for exactly this id and overwrites the three digit fields
 * with a zero-height rect; see the note at the ammo counter in statusbar.c.
 */
#define Q2_SBAR_WEAPON_NO_AMMO 1

/* ------------------------------------------------------------------------- */
/* The field table — 0x800337EC onward                                        */
/* ------------------------------------------------------------------------- */
#define Q2_SBAR_FIELDS      17
#define Q2_SBAR_COUNTERS     3
#define Q2_SBAR_COUNTER_DIGITS 3

/* The main row's far-right field, and the upper row's four. */
#define Q2_SBAR_FIELD_FRAGS      12
#define Q2_SBAR_FIELD_UP_ICON    13
#define Q2_SBAR_FIELD_UP_DIGIT0  14
#define Q2_SBAR_FIELD_UP_DIGIT1  15
#define Q2_SBAR_FIELD_UP_LEFT    16

typedef struct q2_sbar_field {
    s16 dx, dy;      /* from the viewport anchor at view+304 / view+306 */
    u8  init_w, init_h;
} q2_sbar_field;

extern const q2_sbar_field q2_sbar_fields[Q2_SBAR_FIELDS];

/*
 * The two-player layout's x offsets, from `0x80033D30`. Two counters, digits 20
 * apart. Recorded as read; the port draws the one-player table and a split
 * screen should select this one, which is why they are separate rather than
 * scaled from each other — the console does not scale, it has another table.
 */
#define Q2_SBAR_FIELDS_2P 9
extern const q2_sbar_field q2_sbar_fields_2p[Q2_SBAR_FIELDS_2P];

/*
 * And the QUAD layout's, from `0x80034288` — openquestions #20f, the last of
 * the three.
 *
 * None of the three is a table in the data segment: all three are built inline
 * on the stack and copied into the view record, which is why a search for
 * arrays of ten-byte records never found them. Extracting this one the same way
 * as the other two also re-derives the one-player table byte for byte, so the
 * transcription above is now confirmed from two directions rather than one.
 *
 * SIXTEEN fields, and the shape is the giveaway: four counters of three digits
 * and an icon, in TWO ROWS. In four-player split the bar is not per viewport —
 * it carries every player's counters at once, two along the top and two along
 * the bottom. The second row's `dy` is `40 - screen_height`, read from the live
 * framebuffer height at `0x800B2DA2` rather than from a constant, so it tracks
 * the display mode.
 *
 * The digit pitch is **20**, the two-player table's, not the one-player 24.
 */
#define Q2_SBAR_FIELDS_4P 16

/* Where the second row's dy is measured from — see above. `dy` in the records
 * below is the constant part, and a caller in quad split subtracts the live
 * screen height from the ones flagged by q2_sbar_field_4p_is_lower(). */
#define Q2_SBAR_4P_LOWER_DY 40

extern const q2_sbar_field q2_sbar_fields_4p[Q2_SBAR_FIELDS_4P];

/* True for the eight fields whose dy is relative to the bottom of the screen. */
bool q2_sbar_field_4p_is_lower(int field);

/* The record's four trailing bytes, which are the same in all three layouts:
 * a source rect of (255, 255) size 1 x 1 — the blank — and a draw size the
 * caller overwrites. They are initial values, not layout. */
#define Q2_SBAR_FIELD_INIT_U    255
#define Q2_SBAR_FIELD_INIT_V    255
#define Q2_SBAR_FIELD_INIT_RECT 1

/*
 * Which of the three counters is which.
 *
 * NOT read out of the executable — the three groups are structurally identical
 * and the sub-draws were not followed far enough. This is the left-to-right
 * order in retail capture, which is evidence of a different kind and is
 * labelled as such rather than being passed off as a transcription.
 */
typedef enum q2_sbar_counter {
    Q2_SBAR_HEALTH = 0,   /* leftmost, with the cross  — FROM CAPTURE */
    Q2_SBAR_AMMO,         /* middle                    — FROM CAPTURE */
    Q2_SBAR_ARMOUR        /* rightmost                 — FROM CAPTURE */
} q2_sbar_counter;

/* The field index of counter `c`'s first digit, and of its icon. */
int q2_sbar_digit_field(q2_sbar_counter c, int digit);
int q2_sbar_icon_field(q2_sbar_counter c);

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */
typedef struct q2_statusbar {
    const q2_icon_tables *icons;      /* not owned */

    /* Where the viewport put it — view+304 / view+306. */
    s16 anchor_x, anchor_y;

    /* What it shows. These are the port's inputs; the console reads them out
     * of the player record the same way. */
    s16 health;
    s16 armour;
    s16 ammo;
    int weapon;                       /* 1-based; selects the ammo icon */

    /*
     * The health and armour icons, as RECT INDICES into the table at
     * 0x8009C478 — see the correction above. The console hard-codes both (34
     * and 30); they are inputs here only so that armour can show the blank
     * when none is worn, which is what the armour sub-draw's other arm does.
     *
     * Q2_SBAR_ICON_NONE is rect 0, the 1x1 blank, and draws nothing.
     */
    u8 health_icon;
    u8 armour_icon;

    /*
     * The palette bank, for resolving a sprite's palette index to a CLUT word.
     * Optional: with no tables the bar falls back to the single clut passed to
     * q2_statusbar_build_ot(), which is what it always used to do.
     */
    const struct q2_hud_tables *hud;

    /*
     * The frame counter the flash is phased on — `0x800AEBAC`, tested against
     * Q2_SBAR_BLINK_BIT. Supplied by the caller because it is the engine's
     * global tick, not something the bar owns.
     */
    u32 ticks;

    int players;                      /* drives the icon size reduction */
    bool visible;
} q2_statusbar;

void q2_statusbar_init(q2_statusbar *b, const q2_icon_tables *icons,
                       int players);

/*
 * Give the bar the built-in palette bank, so each sprite can be drawn in its
 * own colours rather than all of them in one. Optional — see `hud` above.
 */
void q2_statusbar_set_palettes(q2_statusbar *b, const struct q2_hud_tables *t);

/* Place it. A viewport's own anchor — `sbar_x`/`sbar_y` in screen.h. */
void q2_statusbar_anchor(q2_statusbar *b, s16 x, s16 y);

/*
 * Emit the bar into bucket `bucket`.
 *
 * The sheet is VRAM slot 14 and something else does the uploading — the menu's
 * font loader happens to, because the console registers all its UI images in
 * one function — so this takes the resulting `tpage` and `clut` rather than a
 * loader's struct. The bar does not care who put the sheet in VRAM.
 *
 * Returns the number of primitives emitted.
 */
u32 q2_statusbar_build_ot(const q2_statusbar *b, u16 tpage, u16 clut,
                          psx_ot *ot, u32 bucket,
                          int origin_x, int origin_y);

/*
 * A counter's digits, most significant first, with leading zeroes suppressed
 * the way the capture shows them — "2" is one digit, not "002". Returns how
 * many were written; `out` takes Q2_SBAR_COUNTER_DIGITS.
 */
int q2_sbar_digits_of(int value, u8 out[Q2_SBAR_COUNTER_DIGITS]);

#endif /* Q2PSX_STATUSBAR_H */
