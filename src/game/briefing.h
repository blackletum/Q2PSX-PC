/*
 * briefing.h — the mission briefing screen.
 *
 * ---------------------------------------------------------------------------
 * What it is
 * ---------------------------------------------------------------------------
 * The screen shown between levels: three labelled fields over the same darkened
 * panel the MISSION screen uses. openquestions.md #43 had it as "a sibling of
 * the MISSION screen and drawn the same way; not located". It is located: the
 * composer runs from `0x800215A0` to `0x800217A8`, and it is a sibling in the
 * sense that matters — it builds one markup string and hands it to the same
 * text printf at `0x80043518` — but it is laid out completely differently.
 *
 * The MISSION screen positions every run of text with an explicit `@XXXYY` pen
 * escape, because it is a table. The briefing sets **margins** once and then
 * writes flowing text with newlines, because it is prose. That makes it the one
 * screen in the game that exercises the `#` escape, which is the answer to the
 * other half of #45: the word wrap the capture shows is the briefing's, not the
 * mission screen's.
 *
 * ---------------------------------------------------------------------------
 * The string it builds
 * ---------------------------------------------------------------------------
 * In order, with the address of each piece:
 *
 *     colour <- (0x50, 0x8C, 0x78)         gp+312, 0x800AE738 — the label green
 *     "#06A196"                            0x800AE740 — margins 106 and 406
 *     "|0\n"                               0x800AE748 — palette 72, then break
 *     "~4Location:\n"                      0x800AB888
 *     colour <- (0x78, 0x8C, 0x50)         gp+316, 0x800AE73C — the value olive
 *     "%s\n\n"                             0x800AE74C, filled with MapTitle
 *     colour <- label
 *     "Current Orders:\n"                  0x800AB8A4
 *     colour <- value
 *     "%s\n"                               0x800AE754, filled with the orders
 *     colour <- label
 *     "\nMission Objective:\n"             0x800AB8B8
 *     colour <- value
 *     "%s\n"                               filled with the objective
 *     "#000000"                            0x800AE758 — margins off again
 *     panel(96, 32, 336, 100)              gp+300..306, 0x800AE730
 *
 * Two things are worth pinning because they are easy to get subtly wrong:
 *
 *   - **The colour is not in the string.** Unlike the MISSION screen, whose
 *     `^BEF0E6` escapes carry the colour inline, the briefing writes a 4-byte
 *     RGB straight into the text context at `+660` before each run (the
 *     `swl`/`swr` pairs). Same visual result, different mechanism, and a port
 *     that assumed `^` escapes would produce a string the console never emits.
 *
 *   - **The margins are turned off at the end.** `#000000` clears both, which
 *     clears the wrap flag, because escape state is global and persists into
 *     whatever string is drawn next (hud.h). Leaving it set makes the next
 *     screen's text wrap at 406 for no visible reason.
 *
 * ---------------------------------------------------------------------------
 * The three fields
 * ---------------------------------------------------------------------------
 * `Location` is not a stored string: it is looked up by name. The composer
 * packs the literal `MapTitle` into three registers as four-character chunks
 * and calls the text database at `0x800701B4` — the same call shape the level
 * tables use. The orders and the objective are pointers the game sets as it
 * goes (`0x800B27A4` and `0x800B27A8`).
 *
 * Their initial values sit immediately after the layout table, at `0x8009B51C`
 * and `0x8009B534`:
 *
 *     "Awaiting Orders."
 *     "Awaiting Mission Objective."
 *
 * Neither is reached by a materialised constant — they are addressed as
 * `0x8009B4D8 + 68` and `+ 92` off the base the page code already holds — so
 * the pairing with these two fields is read off their position and their
 * wording, not off an instruction. It is the one inference on this screen.
 */
#ifndef Q2PSX_GAME_BRIEFING_H
#define Q2PSX_GAME_BRIEFING_H

#include "hud.h"
#include "panel.h"
#include "q2psx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The box, from gp+300..306 (0x800AE730). */
#define Q2_BRIEFING_BOX_X       96
#define Q2_BRIEFING_BOX_Y       32
#define Q2_BRIEFING_BOX_W       336
#define Q2_BRIEFING_BOX_H       100

/* The margins the `#06A196` escape sets (0x800AE740). */
#define Q2_BRIEFING_MARGIN_L    0x06A
#define Q2_BRIEFING_MARGIN_R    0x196

/* The two colours, written into the context rather than escaped into the text. */
extern const u8 q2_briefing_rgb_label[3];   /* 0x800AE738 */
extern const u8 q2_briefing_rgb_value[3];   /* 0x800AE73C */

/* The labels, verbatim, newline included. */
extern const char q2_briefing_label_location[];    /* 0x800AB888 */
extern const char q2_briefing_label_orders[];      /* 0x800AB8A4 */
extern const char q2_briefing_label_objective[];   /* 0x800AB8B8 */

/* The shipped contents of the two runtime fields. */
extern const char q2_briefing_default_orders[];    /* 0x8009B51C */
extern const char q2_briefing_default_objective[]; /* 0x8009B534 */

#define Q2_BRIEFING_FIELD_MAX   128

typedef struct q2_briefing {
    char location[Q2_BRIEFING_FIELD_MAX];    /* text("MapTitle")   */
    char orders[Q2_BRIEFING_FIELD_MAX];      /* 0x800B27A4         */
    char objective[Q2_BRIEFING_FIELD_MAX];   /* 0x800B27A8         */
    q2_panel_rect box;
} q2_briefing;

/* Sets the box and both defaults; leaves the location empty. */
void q2_briefing_init(q2_briefing *b);

void q2_briefing_set_location(q2_briefing *b, const char *s);
void q2_briefing_set_orders(q2_briefing *b, const char *s);
void q2_briefing_set_objective(q2_briefing *b, const char *s);

/*
 * Compose the screen's markup exactly as `0x800215A0` does, into `out`.
 *
 * The colour changes cannot be expressed as text in the console's string — it
 * writes them into the context directly — so they come out here as the `^`
 * escape that has the same effect through this port's interpreter. That is the
 * one deliberate difference, and it is a re-encoding of the same six bytes.
 *
 * Returns the length written, or 0 if it would not fit.
 */
u32 q2_briefing_compose(const q2_briefing *b, char *out, u32 out_size);

/*
 * Draw it: the panel, then the text over it.
 *
 * `body_bucket`/`frame_bucket` are the panel's (panel.h); `text_bucket` takes
 * the glyphs and must be nearer than both. The pen is left with its margins
 * cleared, as the console leaves them.
 */
u32 q2_briefing_build_ot(const q2_briefing *b, const q2_hud_font *font,
                         const q2_menu_font *menu_font,
                         q2_hud_ctx *ctx, q2_hud_pen *pen, psx_ot *ot,
                         u32 body_bucket, u32 frame_bucket, u16 text_bucket);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_GAME_BRIEFING_H */
