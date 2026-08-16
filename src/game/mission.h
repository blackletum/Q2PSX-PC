/*
 * mission.h — the MISSION / level-completion screen.
 *
 * ---------------------------------------------------------------------------
 * What it is
 * ---------------------------------------------------------------------------
 * The screen the pause menu's MISSION item leaves for, and the one the game
 * shows when a level ends. It is not a menu page: it belongs to the HUD, and it
 * is drawn at `0x80021ADC` through the same markup layer the overlay uses
 * (hud.h), into the **overlay camera's** context at `0x800D6870` rather than
 * into a viewport.
 *
 * That makes it cheap to reconstruct and expensive to get subtly wrong, so
 * everything below carries the address it came from.
 *
 * ---------------------------------------------------------------------------
 * The strings ARE markup, and that is the confirmation
 * ---------------------------------------------------------------------------
 * Every row is a `sprintf` into a scratch buffer at `0x800C6EE8` and then one
 * call to the text printf at `0x80043518`. The formats are:
 *
 *     0x800AB8DC  "@%03X%2X^BEF0E6|0~4%s"     the title line
 *     0x800AB8F4  "@%03X%2X^DCF082|0~4%s"     the second body line
 *     0x800AB90C  "@%03X%2X^BEF0E6|0Location"
 *     0x800AB928  "@%03X%2X^BEF0E6|0Secrets"
 *     0x800AB944  "@%03X%2X^BEF0E6|0Kills"
 *     0x800AB95C  "@%3X%2X^DCF082|0~8%s"      a value, loose spacing
 *     0x800AB978  "@%3X%2X^DCF082|0%s"        a value
 *     0x800AB988  "@%03X%2X^BEF0E6|0Totals:"
 *     0x800AB9A4  "@%3X%2X^C8F000|0%s"        a total
 *     0x800AB9B8  "@%3X%2X^C8F000|0%s~4"      a total, trailing space width
 *
 * `@%03X%2X` is exactly the `@XXXYY` pen escape hud.h documents — three hex
 * digits of x and two of y, formatted in at run time. Finding the game
 * assembling its own escapes with `sprintf` is independent confirmation that
 * the markup reading is right, because the format strings only make sense if
 * the interpreter reads them that way.
 *
 * Three colours, and they are the screen's whole visual grammar:
 *
 *     BEF0E6  pale cyan    labels and the title
 *     DCF082  pale yellow  values
 *     C8F000  yellow-green totals
 *
 * ---------------------------------------------------------------------------
 * Layout — and it is a TABLE, not a list of rows
 * ---------------------------------------------------------------------------
 * The text box is four `gp`-relative halfwords — `gp+352` x, `gp+354` y,
 * `gp+356` w, `gp+358` h (`0x800AE760`…`0x800AE766`).
 *
 * **Three centred body lines**, from runtime buffers at `0x8009B5E8`,
 * `0x8009B608` and `0x8009B634`, each centred by the helper at `0x80022550`
 * against the box's left edge and `left + width`. They start at y = 36 and step
 * **+10 then +8** — the one irregular gap on the screen.
 *
 * **One header row.** `Location`, `Secrets` and `Kills` are NOT stacked: they
 * are three columns of a single row at y + 6, at `box_x + 20`, `+ 176` and
 * `+ 256` (the three `addiu a2` in the delay slots at `0x80021D14` and
 * `0x80021D3C`). Reading them as a stacked column is the obvious mistake and it
 * produces a screen that looks plausible and is nothing like the console's.
 *
 * **Six data rows**, one per level in the unit. The loop at
 * `0x80021D68`…`0x80021E9C` walks **six 25-byte records** from `0x8009B550`,
 * and a record is:
 *
 *     +0   char name[21]     the level's own name
 *     +21  u8   secrets_found
 *     +22  u8   secrets_total
 *     +23  u8   kills_found
 *     +24  u8   kills_total
 *
 * A record whose name is empty is **skipped without consuming a row** — the
 * `blez` on `strlen` at `0x80021D70` jumps past the y bump — so a unit with
 * three levels draws three rows and no gaps. The first data row is 12 below the
 * header and each drawn row advances 10.
 *
 * The name is drawn at the Location column with `~8`; the two counters are
 * `"%d/%d"` (`0x800AE76C`) placed by the helper at `0x8002260C`, which
 * right-aligns against `box_x + 20 + 176` and `box_x + 20 + 248`. Note that
 * Kills' *label* is at +256 while its *value* right-aligns to +248: the label
 * is placed and the value is aligned, and they are not the same number.
 *
 * **A totals row** at `box_x + 20 + 112`, summing all four columns across the
 * six records (the accumulators at `fp`, `sp+56`, `sp+64` and `sp+72`).
 *
 * The title buffer is composed from `"Mission  %d  -  Complete"`
 * (`0x800AB9DC`); `"Unit%dMiss1"` at `0x800AB9D0` is the key that selects the
 * level's own name.
 *
 * ---------------------------------------------------------------------------
 * What the port has to supply, and why it could not before
 * ---------------------------------------------------------------------------
 * The counters. `Kills` and `Secrets` are simulation state, and the sim did not
 * tally either — which is the real reason this screen stayed unimplemented long
 * after the machinery to draw it existed. They are inputs here rather than
 * something this module invents, so a caller that has not counted anything gets
 * a screen that says zero rather than a screen that lies.
 *
 * Both counters are **u8 in the record**, so each is capped at 255 per level on
 * the console; the port clamps rather than wrapping, and says so at the site.
 */
#ifndef Q2PSX_MISSION_H
#define Q2PSX_MISSION_H

#include "hud.h"

/* ------------------------------------------------------------------------- */
/* The text box — gp+352…gp+358                                               */
/* ------------------------------------------------------------------------- */
/*
 * The console's values are runtime state, not constants; these are what the
 * screen is authored against in the 512 x 248 space and what the port uses
 * until a caller overrides them.
 */
typedef struct q2_mission_box {
    s16 x, y, w, h;
} q2_mission_box;

void q2_mission_box_default(q2_mission_box *b);

/* ------------------------------------------------------------------------- */
/* Layout constants                                                           */
/* ------------------------------------------------------------------------- */
#define Q2_MISSION_BODY_Y       36  /* the first centred line (0x80021B18)   */
#define Q2_MISSION_BODY_STEP_1  10  /* to the second (0x80021C00)            */
#define Q2_MISSION_BODY_STEP_2   8  /* to the third  (0x80021C4C)            */
#define Q2_MISSION_LABEL_INDENT 20  /* box_x + 20    (0x80021B54)            */
#define Q2_MISSION_HEADER_DROP   6  /* to the header row (0x80021CDC)        */
#define Q2_MISSION_FIRST_ROW    12  /* header to the first data row (0x80021D60) */
#define Q2_MISSION_ROW_STEP     10  /* between drawn data rows (0x80021E7C)  */

/* The four columns, as offsets from the label indent. Labels are PLACED at
 * their x; values are RIGHT-ALIGNED to theirs, which is why Kills differs. */
#define Q2_MISSION_COL_LOCATION   0    /* 0x80021CD8                         */
#define Q2_MISSION_COL_SECRETS  176    /* 0x80021D14                         */
#define Q2_MISSION_COL_KILLS    256    /* 0x80021D3C                         */
#define Q2_MISSION_COL_TOTALS   112    /* 0x80021EC0                         */
#define Q2_MISSION_VAL_SECRETS  176    /* 0x80021DB8, right-aligned          */
#define Q2_MISSION_VAL_KILLS    248    /* 0x80021E0C, right-aligned          */

/* Six 25-byte records at 0x8009B550 (0x80021E88 / 0x80021E94). */
#define Q2_MISSION_ROWS          6
#define Q2_MISSION_RECORD_STRIDE 25
#define Q2_MISSION_NAME_LEN      21
#define Q2_MISSION_COUNT_MAX    255  /* the counters are u8 in the record    */

/* The three colours, as the format strings spell them. */
extern const u8 q2_mission_rgb_label[3];   /* BEF0E6 */
extern const u8 q2_mission_rgb_value[3];   /* DCF082 */
extern const u8 q2_mission_rgb_total[3];   /* C8F000 */

/* "Mission  %d  -  Complete" — 0x800AB9DC. */
extern const char q2_mission_title_format[];

/* ------------------------------------------------------------------------- */
/* The screen                                                                 */
/* ------------------------------------------------------------------------- */
#define Q2_MISSION_LINE_MAX 64

/*
 * One level's row — the 25-byte record, unpacked. `name` empty means the slot
 * is unused and the row is skipped entirely rather than drawn blank.
 */
typedef struct q2_mission_row {
    char name[Q2_MISSION_NAME_LEN + 1];
    u8   secrets, secrets_total;
    u8   kills,   kills_total;
} q2_mission_row;

typedef struct q2_mission {
    q2_mission_box box;

    int  unit;                              /* the mission number            */

    /*
     * THE TWO CENTRED LINES UNDER THE TITLE, and they are ONE MESSAGE WRAPPED.
     *
     * Nothing in this port writes either, so both draw blank and the board has
     * an empty band where the console has text. What they are is now read
     * rather than guessed:
     *
     *   0x800220C8  takes a string in a0 and fills BOTH buffers. It memsets
     *               0x8009B608 and 0x8009B634 to 42 bytes of zero (0x80089E18),
     *               measures the source with 0x8008A0E8, and at 0x80022150
     *               compares the length against 36 — `slti v0, v0, 36`. Under
     *               36 the whole string goes to the first buffer and the second
     *               stays empty; at 36 or over it breaks at the last space
     *               (0x8008A0C8 is called with 32) and the remainder goes to the
     *               second. So `subtitle` and `footer` are line one and line two
     *               of a word-wrap, not two independent fields, and 42 is the
     *               buffer size rather than the column count.
     *
     *   0x80022094  is its only caller, and the string it hands over is the
     *               return of 0x800701B4 at 0x8002208C.
     *
     * WHAT 0x800701B4 RETURNS IS THE REMAINING GAP. Until that is read, filling
     * these with anything would be inventing the level-end text, so they are
     * left empty and the band with them.
     */
    char subtitle[Q2_MISSION_LINE_MAX];     /* 0x8009B608, wrapped line 1    */
    char footer[Q2_MISSION_LINE_MAX];       /* 0x8009B634, wrapped line 2    */

    /* The six level rows, which the caller tallies. */
    q2_mission_row row[Q2_MISSION_ROWS];
} q2_mission;

void q2_mission_init(q2_mission *m);

/*
 * Fill row `index`. Counts above 255 clamp rather than wrap — the console
 * stores each as a u8 and a port that let them roll over would show a level
 * with 300 kills as one with 44.
 */
void q2_mission_set_row(q2_mission *m, int index, const char *name,
                        int secrets, int secrets_total,
                        int kills, int kills_total);

/* The column totals, summed across every row exactly as the four accumulators
 * at 0x80021E58…0x80021E80 do. */
void q2_mission_totals(const q2_mission *m, int *secrets, int *secrets_total,
                       int *kills, int *kills_total);

/* The title line the console composes into 0x8009B5E8. Returns `out`. */
const char *q2_mission_title(const q2_mission *m, char *out, u32 out_size);

/*
 * The centring helper at 0x80022550: the x at which `s` is centred between
 * `left` and `right`, measured with the HUD's own string measurer — off-by-one
 * and all, because that is what the console centres against (hud.h).
 */
int q2_mission_centre_x(const char *s, int left, int right);

/*
 * 0x8002260C — and it is NOT a right-align, which is the easy misreading. It
 * centres `s` inside a **56-pixel field** whose left edge is `x`, using a
 * length that is decremented when it is even:
 *
 *     n = strlen(s);  if ((n & 1) == 0) n -= 1;  return x + (56 - n*8)/2;
 *
 * That is what puts a value column under its header without either being
 * aligned to the other, and it is why Kills' label sits at +256 while its
 * field starts at +248: the label is placed, the value is centred in a box.
 */
#define Q2_MISSION_FIELD_W 56
int q2_mission_field_x(const char *s, int x);

/*
 * Emit the whole screen into bucket `otz`, through the HUD's markup layer. The
 * pen state is the caller's, exactly as it is on the console, where these
 * strings run through the same globals every other string does.
 *
 * Returns the number of glyph cells laid out.
 */
u32 q2_mission_build_ot(const q2_mission *m, const q2_hud_font *font,
                        q2_hud_ctx *ctx, q2_hud_pen *pen,
                        psx_ot *ot, u16 otz);

#endif /* Q2PSX_MISSION_H */
