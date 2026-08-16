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

/* The shipped contents of the two runtime fields.
 *
 * CORRECTION to the note above — this pairing is NOT an inference. The block
 * at the top of this header says the two defaults "are addressed as
 * 0x8009B4D8 + 68 and + 92 off the base the page code already holds", so the
 * pairing is read off position and wording. They are also each reached by a
 * MATERIALISED CONSTANT, one apiece, and both inside the writer:
 *
 *     0x80021064   addiu v0, v0, -19168   ; 0x8009B520, the orders fallback
 *     0x8002110C   addiu v0, v0, -19148   ; 0x8009B534, the objective fallback
 *
 * `xrefs 0x8009B520` and `xrefs 0x8009B534` each return exactly one. They are
 * the `else` arms of the two Strings lookups in q2_briefing_popup_set, so the
 * pairing is two instructions rather than a reading of the data segment.
 */
extern const char q2_briefing_default_orders[];    /* 0x8009B520 */
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

/* ------------------------------------------------------------------------- */
/* The pop-up — the state machine around the composer                         */
/* ------------------------------------------------------------------------- */
/*
 * The briefing is not only a between-levels screen. It is the OBJECTIVES
 * POP-UP, and the composer above is the body of a per-frame driver at
 * `0x8002150C` whose surrounding machinery was never transcribed. Three pieces
 * were missing, and without them the port had the picture and none of the
 * behaviour: HELPCOMPUTER wrote its two strings to the HUD's notification
 * overlay and the pause menu's MISSION row opened the level-completion tally.
 *
 * WHAT RAISES IT. Two things, both through `0x800213B0(delay, seconds)`:
 *
 *   - `Q2_UF_HELPCOMPUTER` from a trigger volume, via `0x80021250`
 *     (`0x8002BBF4 jal 0x80021250`). That sets the two fields and clamps the
 *     item's delay UP to a minimum of 5 (`slti v0,s3,5 / addiu s3,zero,5` at
 *     0x8002136C) before calling the raise with seconds = 15.
 *   - The pause menu's MISSION row, `0x8002033C`, which ends
 *     `jal 0x800213B0` with a0 = 1 and a1 = 15 — a raise, not an exit code.
 *     FORMATS.md reads that call as "it leaves the menu with exit code 15";
 *     it does not. The MISSION tally screen at 0x80021ADC has exactly one
 *     caller, the level-end state, and the pause menu never reaches it.
 *
 * WHAT THE RAISE DOES. A non-zero delay does not open anything: it arms a
 * countdown of `delay * frame_dt * 2` and remembers the seconds. The tick
 * (0x80021830..0x80021894) counts that down by the frame's dt and re-enters
 * the raise with delay 0 when it reaches zero. Delay 0 is the open: the screen
 * becomes visible, the engine's "a menu owns the frame" flag goes up — which
 * is what stops the world ticking, at 0x800190C8 — the per-view button latches
 * are cleared, and the deadline is set to `level_clock + seconds * 300`. So a
 * HELPCOMPUTER is a DELAYED pop-up, and the sound (`msc_comp_up`) plays at the
 * raise rather than at the open.
 *
 * WHAT CLOSES IT. Either the deadline passing — 0x80021530's
 * `sltu v0, [level_clock], [deadline]` falls through to the close — or CROSS.
 * The CROSS test is an EDGE across two of the three per-view button latches
 * and is gated by an arm flag that is only set on a frame where CROSS is not
 * held (0x80021818), so the press that opened a menu cannot also dismiss the
 * screen it opened.
 *
 * THE STATE ITSELF is two global `const char *` at 0x800B27A4 and 0x800B27A8,
 * with two writers each and one reader each, all four inside this screen. They
 * are never reset per level: the console shows "Awaiting Orders." until the
 * game's first HELPCOMPUTER, and each one after that advances the pair. That
 * is why they live on the pop-up rather than on `q2_briefing`, which a zone
 * load rebuilds.
 */
typedef struct q2_briefing_popup {
    bool visible;           /* 0x800B27AC */

    /*
     * The level clock the screen closes at — gp+16800 (0x800B27A0), set to
     * `level_clock + seconds * 300` at the open.
     */
    s32  deadline;

    /* The armed countdown: gp+284 in dt units, gp+288 the seconds to pass on
     * when it fires. Zero ticks means nothing is pending. */
    s32  delay_ticks;
    s32  delay_seconds;

    /*
     * gp+292. The dismiss is armed only after a frame on which CROSS is NOT
     * held, so the press that raised the screen cannot dismiss it on the same
     * press.
     */
    bool dismiss_armed;

    /* 0x800B27A4 / 0x800B27A8 — global, and deliberately not per level. */
    char orders[Q2_BRIEFING_FIELD_MAX];
    char objective[Q2_BRIEFING_FIELD_MAX];
} q2_briefing_popup;

/* The clamp at 0x8002136C and the constants both raisers pass. */
#define Q2_BRIEFING_DELAY_MIN     5
#define Q2_BRIEFING_SECONDS       15
#define Q2_BRIEFING_MENU_DELAY    1
/* `level_clock + seconds * 300` — the level clock's own rate (combat.h). */
#define Q2_BRIEFING_TICKS_PER_SEC 300

/* The two defaults, and nothing pending. Call once per session, NOT per
 * level: the console never resets the pair. */
void q2_briefing_popup_init(q2_briefing_popup *p);

/*
 * 0x80020F34 — set the two fields, each from a Strings lookup with its own
 * hard-coded fallback. Pass NULL or an empty string for a key the level's text
 * does not carry and the fallback is used, which is what the console does when
 * the lookup misses.
 */
void q2_briefing_popup_set(q2_briefing_popup *p, const char *orders,
                           const char *objective);

/*
 * 0x800213B0. `delay` in the item's own units; zero opens immediately and
 * anything else arms the countdown. Returns true when the screen opened on
 * this call, which is the caller's cue to play `msc_comp_up`... except that
 * the console plays it at the RAISE, so the caller plays it either way — the
 * return is for the frame gate, not for the sound.
 */
bool q2_briefing_popup_raise(q2_briefing_popup *p, s32 delay, s32 seconds,
                             s32 level_time, s32 frame_dt);

/*
 * One frame of 0x8002150C's gates and 0x80021830's countdown.
 *
 * `cross_held` and `cross_prev` are this frame's and last frame's CROSS, which
 * is the edge the console builds out of its three per-view latches. Returns
 * true while the screen should be drawn — and while it is true the caller must
 * not tick the world, which is what the engine's own menu flag does.
 */
bool q2_briefing_popup_tick(q2_briefing_popup *p, s32 dt, s32 level_time,
                            bool cross_held, bool cross_prev);

/* 0x80021920 — close now, whoever asked. */
void q2_briefing_popup_close(q2_briefing_popup *p, s32 level_time);

/* ------------------------------------------------------------------------- */
/* The end-of-mission placard                                                 */
/* ------------------------------------------------------------------------- */
/*
 * A QENDMIS map is the movie player's container and carries no geometry
 * (levelbin.h): the campaign's last map draws two quads on a black field, which
 * reads as a crash rather than as an ending. What belongs there is a 19.5 MB
 * MDEC video this port cannot decode.
 *
 * So it says so, on the console's own panel — the same furniture the briefing
 * uses, because a second framed-text screen would be a second thing to keep in
 * step with `panel.h`'s geometry. The LABELS are not the briefing's: `Location`
 * and `Current Orders` over an ending would be worse than the black screen,
 * because they would look deliberate.
 *
 * `title` is the headline, `body` the lines under it. Both are the caller's —
 * this module knows how to draw a placard, not what a given ending should say.
 */
typedef struct q2_endmission {
    char title[Q2_BRIEFING_FIELD_MAX];
    char body[Q2_BRIEFING_FIELD_MAX * 2];
    q2_panel_rect box;
} q2_endmission;

void q2_endmission_init(q2_endmission *e);
void q2_endmission_set(q2_endmission *e, const char *title, const char *body);

/* Compose the markup, the way q2_briefing_compose does and with the same
 * margin escape — that escape is what turns wrapping on at all (hud.h). */
u32 q2_endmission_compose(const q2_endmission *e, char *out, u32 out_size);

/* Draw it. The bucket arguments are DEPTHS, as the briefing's are. */
u32 q2_endmission_build_ot(const q2_endmission *e, const q2_hud_font *font,
                           const q2_menu_font *menu_font,
                           q2_hud_ctx *ctx, q2_hud_pen *pen, psx_ot *ot,
                           u32 body_bucket, u32 frame_bucket, u16 text_bucket);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_GAME_BRIEFING_H */
