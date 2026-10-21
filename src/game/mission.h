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
 *
 * ---------------------------------------------------------------------------
 * WHEN IT IS SHOWN, and it is NOT at the end of a level
 * ---------------------------------------------------------------------------
 * This header used to say the screen is "the one the game shows when a level
 * ends". It is not, and the call graph settles it in three steps, each of
 * which has exactly one edge:
 *
 *     0x80021ADC  the draw below      <- called ONLY from 0x80018944
 *     0x80018868  one tally frame     <- called ONLY from 0x80018F08
 *     0x80018ED8  game state 7        <- state 7 written ONLY by 0x8002DCB4
 *     0x8002DC68  MISCOMPLETE's exec  (UserFuncs binding-table index 31)
 *
 * So the board can only be reached by a `MISCOMPLETE` — the end of a UNIT. A
 * `LOADMAP`, which is the end of a LEVEL, writes state 2 at `0x8002DCB4`'s
 * sibling `0x8002DD80` and never touches this screen: the outer state machine
 * simply loads the map the primitive named. That is why the board has six rows
 * and a "Mission %d - Complete" title — it lists a unit's levels, once, when
 * the unit is over.
 *
 * `0x80018ED8` spins `while (0x80018868() == 0);`, and the frame function
 * returns the draw's own answer, so the board holds until the pad edge latched
 * at `0x80018214` arrives. There is no timeout.
 *
 * ---------------------------------------------------------------------------
 * The rows are keyed by NAME, and every level registers its own
 * ---------------------------------------------------------------------------
 * Filling the six rows in visit order is the obvious model and it is not the
 * console's. Four functions share the array and all four match by name:
 *
 *     0x800222B8(name)  find-or-take-first-free; stamps the live counters in.
 *                       Exported to the level modules at engine+0x474.
 *     0x80022210(name)  the reverse: load the row's four counters back into
 *                       the live globals. Called from the save restore.
 *     0x800223A8(name)  stamp the live secrets_found into the row  (INSECRET)
 *     0x80022420(name)  stamp the live kills_found into the row    (a death)
 *
 * And **every map registers itself**: BASE1's LevelBin `init` looks up the key
 * `MapTitle` through engine+0x124 and hands the result straight to
 * engine+0x474 (`80100434`..`80100448` in the relocated module). So a row
 * exists from the moment a level is entered, and the two counters are written
 * into it as they move rather than totted up when the level ends.
 *
 * The whole 150-byte array (6 x 25) is cleared by `0x8003DDB8`'s `memset` on a
 * new game and restored from the save block by the six-record copy loop just
 * above it — which is what carries it ACROSS LEVELS rather than rebuilding it
 * at each one. It does not carry across UNITS: `ClearLevelIntermission`
 * (`0x80021A0C`) empties the block's copy when the tally board is dismissed,
 * and the next `InitialiseQuakePlayer` restores the emptiness. See
 * `q2_mission_register` below, which is where that used to be documented the
 * other way round.
 *
 * ---------------------------------------------------------------------------
 * The two centred body lines ARE the unit objective, wrapped
 * ---------------------------------------------------------------------------
 * This header used to call them "the remaining gap" because what fed them was
 * a `0x800701B4` lookup whose key had not been read. It has:
 * `0x80021FD8` builds `"Unit%dMiss1"` (`0x800AB9D0`) with the unit at
 * `0x800B2E20`, looks it up, and its caller `0x80022094` hands the result to
 * the wrapper at `0x800220C8`. That is the same key `leveltext.h` already
 * calls the objective — the briefing's "Mission Objective" field — so the
 * board repeats the unit's objective under its title.
 *
 * The wrap is `0x800220C8`'s own and is not a column count:
 *
 *     strlen < 36            -> the whole string is line one, line two empty
 *     otherwise              -> cut at the LAST space, repeatedly, until the
 *                               head is under 36; line one is that head and
 *                               line two is the remainder, leading space and
 *                               all (`a1 = src + len` at `0x800221E4`).
 *
 * Both buffers are 42 bytes and are zeroed first, so a cut that lands exactly
 * on the boundary cannot run off the end.
 */
#ifndef Q2PSX_MISSION_H
#define Q2PSX_MISSION_H

#include "hud.h"

/* ------------------------------------------------------------------------- */
/* The text box — gp+352…gp+358                                               */
/* ------------------------------------------------------------------------- */
/*
 * `MissionRect`, 0x800AE760 — and three quarters of it is INITIALISED DATA
 * rather than runtime state, which this header used to have the wrong way
 * round.
 *
 *     800AE760  54 00 1E 00 58 01 7C 00     x 84, y 30, w 344, h 124
 *
 * `xrefs` on each halfword: x, y and w are read thirteen times between them and
 * written never; h has one writer, `SetInterMission` at 0x800224E8. So the
 * rectangle is fixed except for its height, which is recomputed per unit by
 * `q2_mission_box_height`.
 *
 * AND THE RECTANGLE IS DRAWN. `DBInterMission`'s first act, before a single
 * glyph, is to pack the four halfwords into two registers —
 *
 *     80021B40  sll a0, a0, 16        a0 = x | (y << 16)
 *     80021B44  or  a0, v0, a0
 *     80021B48  sll a1, a1, 16        a1 = w | (h << 16)
 *     80021B50  or  a1, v1, a1
 *     80021B74  jal 0x8003F0E4        DrawDarkenedBox
 *
 * — and `DrawDarkenedBox` spills them straight back to the stack and reads
 * (x, y, w, h) out as four s16 (0x8003F168..0x8003F174). That is the port's
 * `q2_panel_build_ot`: two half-strength black TILEs and the eight-quad rounded
 * frame, the same furniture the briefing and the end-of-mission placard sit in.
 * The tally board had none of it and drew as bare text over the live scene.
 */
typedef struct q2_mission_box {
    s16 x, y, w, h;
} q2_mission_box;

void q2_mission_box_default(q2_mission_box *b);

/* ------------------------------------------------------------------------- */
/* The board's own camera — QCAMERA.C `TestCamera`, 0x8009C668                 */
/* ------------------------------------------------------------------------- */
/*
 * THE TALLY IS NOT DRAWN OVER WHERE THE PLAYER WAS STANDING. It is drawn over
 * a fixed, hand-authored view of the level, one per unit, and the port had no
 * such thing: the world behind the board was whatever the exit happened to be
 * facing, which on most maps is a doorframe.
 *
 * `SetInterMission`'s VERY FIRST store is the hook:
 *
 *     800224C4  addiu v1, v1, -31648        0x80038460
 *     800224C8  sw    v1, 23900(v0)         view[0] + 300
 *
 * `ProcessAllCameras` (0x80037FFC) calls that hook per view at 0x80038038 and
 * `QuakeInterMissionLoop` calls `ProcessAllCameras` at 0x800392D0, so
 * `MoveQuakeCameraInterMission` runs on EVERY frame the board is up — which is
 * why this belongs in the intermission frame rather than at the raise.
 *
 * `MoveQuakeCameraInterMission` ORs 7 into view+144, and then, only when the
 * owning entity exists and `lh 264(ent)` is positive (0x800384A0), calls
 * `QuakeCameraInterMission` (0x80038720). That indexes
 *
 *     80038740  sll   v0, unit, 1           v0 = unit * 24
 *     80038744  addu  v0, v0, unit
 *     80038748  sll   v0, v0, 3
 *     8003874C  addiu v0, v0, -24           ...minus one record
 *     80038750  addu  a0, v0, 0x8009C668
 *
 * so the record for unit N is `TestCamera[N - 1]`, and writes it straight into
 * the view — the same view+0/+4/+8 that `QuakeCamera` fills from the player at
 * 0x8003862C/0x80038640/0x8003864C, plus the three angles at +12/+14/+16.
 *
 * TWO THINGS THE PORT DOES NOT TAKE, stated rather than hidden:
 *
 *   - view+146 and view+158 (record +18 and +20) are the PrimaryColl camera
 *     cell and its area — FORMATS.md §PrimaryColl: "the renderer indexes the
 *     PrimaryColl node through view+146". This port derives both from the
 *     camera position every frame, so seeding them would be overwritten.
 *     They are carried in the record anyway, because they are what identifies
 *     these numbers as an authored camera and not a coincidence.
 *
 *   - THE FIFTH UNIT HAS NO RECORD. The table is four records long: 0x8009C6C8
 *     is the string "QFront". `Mission` reaches 5 (0x80018F24 compares it
 *     against 5 to choose `Extro FMV`), the index is unbounded, and the console
 *     therefore reads `51 46 72 6F / 6E 74 00 00 / 00 00 00 00` as a camera at
 *     (1868088913, 29798, 0). A port cannot reproduce what a GTE does with
 *     that, so unit 5 keeps the player's camera here. That is a DEVIATION and
 *     it is the only one on this screen.
 */
typedef struct q2_mission_camera {
    s32 pos[3];    /* +0, +4, +8   -> view+0/+4/+8, the eye outright     */
    s16 pitch;     /* +12          -> view+12                            */
    s16 yaw;       /* +14          -> view+14, NEGATED on the way in     */
    s16 roll;      /* +16          -> view+16                            */
    s16 cell;      /* +18          -> view+146, the PrimaryColl cell     */
    s16 area;      /* +20          -> view+158                           */
} q2_mission_camera;

/* How many records the table really has (0x8009C668..0x8009C6C7). */
#define Q2_MISSION_CAMERAS 4

/*
 * `TestCamera[unit - 1]`, with the yaw already negated as 0x800387FC does.
 * Returns false — writing nothing — for a unit outside 1..4, where the console
 * reads past the end of the table (see above).
 */
bool q2_mission_camera_for_unit(int unit, q2_mission_camera *out);

/* ------------------------------------------------------------------------- */
/* Layout constants                                                           */
/* ------------------------------------------------------------------------- */
#define Q2_MISSION_BODY_Y       36  /* the first centred line (0x80021B18)   */
#define Q2_MISSION_BODY_STEP_1  10  /* to the second (0x80021C00)            */
#define Q2_MISSION_BODY_STEP_2   8  /* to the third  (0x80021C4C)            */
/*
 * AND A FOURTH STEP, WHICH ONLY HAPPENS WHEN THE THIRD LINE IS DRAWN — the
 * port used to leave it out, and the header row landed on top of the wrapped
 * objective on every level whose objective wraps.
 *
 *     80021C4C  addiu v0, v0, 8          y += 8, unconditional
 *     80021C50  jal   strlen(0x8009B634) ...on the SECOND wrapped line
 *     80021C58  beq   v0, zero, 0x80021CB8   empty: straight to the header
 *     80021CB0  addiu v0, v0, 8          drawn: y += 8 again
 *     80021CDC  addiu v0, v0, 6          and then the header row
 *
 * So the header sits at 60 when the objective fitted on one line and at 68
 * when it wrapped. This is the same eight pixels `PreProcessInterMission` adds
 * to the panel's height for a wrapped objective (24 -> 32), which is the
 * cross-check: the rectangle grows by exactly what the text grows by.
 */
#define Q2_MISSION_BODY_STEP_3   8  /* past the third, if drawn (0x80021CB0) */
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
 * 0x80022664 — how tall the rectangle is for THIS unit.
 *
 * 42 when the objective fits on one line and 50 when it wrapped, plus 10 for
 * every row with a name, plus 14. `SetInterMission` calls it once when the
 * board goes up and stores the answer into gp+358 (0x800224E8), so the panel
 * grows with the table rather than being a fixed slab. See mission.c for the
 * instructions it is transcribed from, and why 124 is the .data seed.
 */
s16 q2_mission_box_height(const q2_mission *m);

/* ------------------------------------------------------------------------- */
/* The dismissal lock — 0x800224F4, PadEndInterMission                        */
/* ------------------------------------------------------------------------- */
/*
 * THE BOARD CANNOT BE DISMISSED FOR ITS FIRST 1200 TICKS, and the port used to
 * let the first press through.
 *
 * `PadEndInterMission` runs once per tally frame, before the draw
 * (0x8001893C, immediately ahead of `DBInterMission` at 0x80018944):
 *
 *     800224F4  lhu  v0, 362(gp)              the counter
 *     800224FC  lhu  v1, 0x800B2DB4           the frame delta
 *     80022508  subu v0, v0, v1
 *     8002250C  sh   v0, 362(gp)
 *     80022510  sll  v0, v0, 16
 *     80022514  bgtz v0, 0x80022540           still positive -> return 0
 *     8002251C  sh   zero, 362(gp)            otherwise clamp to 0
 *     80022520  jal  0x8001FC50               MapPadInputToFire(0)
 *     8002252C  addiu v0, zero, 1200          on an edge, re-arm...
 *     80022538  sh   v0, 362(gp)
 *     8002253C  addiu v0, zero, 4             ...and end the spin
 *
 * While the counter is positive THE PAD IS NOT EVEN READ. 1200 in the
 * console's own 1/300 s units is four seconds, and the delta is the one
 * `MainInterMissionLoop` clamps to 30 at 0x80018914 — the same
 * `Q2_SCREEN_DT_MAX` the rest of this port already uses.
 *
 * THE COUNTER IS SEEDED IN .DATA, NOT AT THE RAISE. 0x800AE76A holds 0x04B0
 * and `xrefs` finds exactly four references, all four inside this function —
 * `SetInterMission` does not touch it. So the first board is gated too, and
 * every board after it is gated because the DISMISSAL re-arms the counter. A
 * port that reset it when the board went up would behave identically and would
 * be describing a different mechanism; this does what the executable does.
 *
 * And the press is the FIRE binding, `MapPadInputToFire(0)`, not a menu
 * confirm: 0x8001FCD8's default mask is 0x4000 (CROSS) with 0x0002/0x0004/
 * 0x0400/0x0800 on the mouse and stick styles, and the answer is a RISING EDGE
 * — `pad & mask` against `prev & mask` at 0x8001FCDC..0x8001FCFC. START never
 * dismisses this board on the console, and it used to here.
 */
#define Q2_MISSION_GATE_TICKS 1200

/*
 * One frame of it. `gate` is the caller's copy of gp+362, `dt` the frame delta
 * already clamped, `fire_edge` the rising edge `MapPadInputToFire` returns.
 *
 * Returns true when the board is dismissed — the console's answer 4, which is
 * what ends `0x80018ED8`'s spin — and re-arms `gate` to 1200 on the way out.
 */
bool q2_mission_pad_end(s32 *gate, s32 dt, bool fire_edge);

/*
 * Fill row `index`. Counts above 255 clamp rather than wrap — the console
 * stores each as a u8 and a port that let them roll over would show a level
 * with 300 kills as one with 44.
 */
void q2_mission_set_row(q2_mission *m, int index, const char *name,
                        int secrets, int secrets_total,
                        int kills, int kills_total);

/*
 * 0x800222B8 — register a level in the table and return its row.
 *
 * Finds the row whose name matches, or takes the first whose name is empty and
 * stamps the four counters into it. Returns the row index, or -1 when the name
 * is empty or all six rows are taken by other levels.
 *
 * -1 is a real answer and the console has it too — a seventh DISTINCT level
 * inside one unit registers nothing. What it is NOT is a campaign-long
 * condition, and this header used to say it was.
 *
 * **CORRECTION: THE TABLE IS EMPTIED AT EVERY UNIT BOUNDARY.** The note here
 * used to read "Nothing empties it at a unit boundary", on the strength of
 * `0x80022498` being a six-iteration loop with no body. That loop really is
 * empty, and it is also not the clear. The clear is `ClearLevelIntermission`
 * (IMISSION.C `0x80021A0C`), and it is `GameInterMission`'s LAST act —
 * 0x8001902C, after the spin, after `EndMission N` or `Extro FMV` has been
 * written:
 *
 *     80021A18  bne  v0, zero, ...            bail if LevelIsMultiPlayer
 *     80021AB0  jal  0x8007FBEC               OpenSaveMemory("PlayerSave0")
 *     80021AB8  beq  v0, zero, ...            no block, nothing to clear
 *     80021ABC  addiu a0, v0, 228
 *     80021AC8  addiu a2, zero, 150           memset(block + 228, 0, 150)
 *
 * and block+228 is exactly where the six records live. `GamePlayerLoadSave`'s
 * save arm writes row i to `block + 16 + 212 + i*25` (0x8003D764 `s0 = s1+16`,
 * 0x8003DAB4 `swr t1, 212(v1)`) and its load arm reads it back from the same
 * place into 0x8009B550 (0x8003DCC8). 228 + 6*25 = 378, so the memset covers
 * the six and nothing else.
 *
 * The live array is not touched — it does not have to be. `InitialiseQuakePlayer`
 * calls `GamePlayerLoadSave(player, 0)` at 0x8003B4B0 on every level entry, and
 * that arm either copies the block's six records into 0x8009B550 or, with no
 * block at all, runs `0x8003DDB8`'s `memset(0x8009B550, 0, 150)`. Either way
 * the array the next unit starts with is empty.
 *
 * So the board really does list a unit's own levels, the six rows are a UNIT's
 * and not a campaign's, and the port's consequence — unit 2 showing unit 1's
 * levels and `q2_mission_register` refusing every level after the sixth — was
 * a defect here rather than a transcribed one. `q2_mission_clear_rows` is the
 * boundary clear; `q2_mission_init` is still the new-game one.
 */
int q2_mission_register(q2_mission *m, const char *name);

/*
 * `ClearLevelIntermission`'s memset, 0x80021ABC..0x80021AC8 — the six records
 * and nothing else. The unit number is left alone: it is `0x800B2E20`, which
 * lives outside the 150 bytes and is what the next board's title reads.
 */
void q2_mission_clear_rows(q2_mission *m);

/* The row whose name matches, or -1. Case-sensitive, as `0x80089D48` is. */
int q2_mission_find(const q2_mission *m, const char *name);

/*
 * The live half of `0x800223A8` and `0x80022420`: put today's counters into a
 * row that already exists. Does nothing for an index that names no row, which
 * is what the console's name search does when it falls off the sixth record.
 */
void q2_mission_set_counts(q2_mission *m, int index,
                           int secrets, int secrets_total,
                           int kills, int kills_total);

/*
 * 0x80022210 — the other direction: a row's four counters back OUT.
 *
 * The console keeps the live counters in four words — secrets_found at
 * `0x800B29FC`, secrets_total at `0x800B29F8`, kills_found at `0x800B29E8`,
 * kills_total at `0x800B29E4` — and a row is the u8 copy of them at +21..+24.
 * `0x800222B8` and the two stamps push the globals into the row; this is the
 * one function that pulls them back, and it has exactly one caller: the save
 * restore, at `0x8003DD9C`, on the name `0x800701B4` just resolved for the
 * level being resumed.
 *
 * Without it a restored game would carry the table on screen and nothing in
 * the simulation, which is precisely what the port did before this existed:
 * the row said 2 secrets and the live counter said 0, and the live counter
 * won on the next frame.
 *
 * Returns false — writing nothing — for an index that names no row, which is
 * what the console's name search does when it falls off the sixth record.
 * Any of the four out-pointers may be NULL.
 */
bool q2_mission_get_counts(const q2_mission *m, int index,
                           int *secrets, int *secrets_total,
                           int *kills, int *kills_total);

/*
 * 0x800220C8 — the two centred body lines, from one string.
 *
 * Word-wrapped into `subtitle` and `footer` exactly as the console wraps it:
 * under 36 characters the whole string is line one; at or over, the last space
 * is cut repeatedly until the head fits, and the remainder — leading space
 * included — becomes line two. A NULL or empty string clears both.
 */
void q2_mission_set_objective(q2_mission *m, const char *text);

/* The column totals, summed across every row exactly as the four accumulators
 * at 0x80021E58…0x80021E80 do. */
void q2_mission_totals(const q2_mission *m, int *secrets, int *secrets_total,
                       int *kills, int *kills_total);

/*
 * ---------------------------------------------------------------------------
 * WHAT COUNTS AS A KILL, and it is not every creature
 * ---------------------------------------------------------------------------
 * The two counters this screen divides — kills_total at `0x800B29E4` and
 * kills_found at `0x800B29E8` — are written by two functions, and both of them
 * refuse certain CLASSES outright. The refusals are not the same refusal, and
 * the asymmetry is transcribed rather than tidied.
 *
 * THE DENOMINATOR. CreatureCount (CREATURE.C `0x8005771C`) runs once over the
 * whole of COMMON.DAT's Population (`0x8007AB2C`, stored to `0x800B29E4` at
 * `0x8007AB3C`) and, per record, does
 *
 *     80057860  addiu v0, v0, -34
 *     80057864  sltiu v0, v0, 3
 *     80057868  bne   v0, zero, 0x80057888     skip the increment
 *
 * so classes 34, 35 and 36 — Insane, Rider2 and Rider3 — never enter it. (The
 * same loop also applies the difficulty mask at `0x80057870`..`0x8005787C`;
 * that half is `q2_pop_spawn_allows_skill`, population.h.)
 *
 * THE NUMERATOR. QENTS.C ProcessQuakeDeath (`0x8007CD24`) is the only writer
 * that ever raises `0x800B29E8`. It takes the class from ent+0xD2, or from
 * ent+0xDA when ent+0xD2 is already 47 — a body that has been detached keeps
 * the row it died as (`0x8007CD40`..`0x8007CD54`) — and then
 *
 *     8007CD5C  addiu v1, zero, 34
 *     8007CD60  beq   v0, v1, 0x8007CD94        jump past the increment
 *
 * ONE class, not three. Shooting a wounded marine moves neither half; shooting
 * a Rider2 moves the numerator and was never in the denominator, which is the
 * console's own inconsistency and is left alone here.
 *
 * `0x8007CD7C` `slt v0, kills, total` also clamps: the count only rises while
 * it is strictly below the denominator, which matters only if the two halves
 * ever disagree about a record.
 *
 * These are predicates on the POPULATION class id, 0..37 — `pop_class_id` on a
 * live creature, not `class_id`, which the module's spawn overwrites with its
 * own class byte.
 */
bool q2_mission_class_counts(u32 pop_class_id);
bool q2_mission_class_kills(u32 pop_class_id);

/* The Population class ids the two rules above name. */
#define Q2_MISSION_CLASS_INSANE   34u
#define Q2_MISSION_CLASS_RIDER2   35u
#define Q2_MISSION_CLASS_RIDER3   36u

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
