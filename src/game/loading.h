/*
 * loading.h — the screen between two levels: black, LOADING, and the logo
 * turning in the top right.
 *
 * ---------------------------------------------------------------------------
 * There IS one, and it is a menu page — 0x80079178
 * ---------------------------------------------------------------------------
 * A level change on this disc does not go straight to the loader. The event
 * script's transition opcodes call `0x80079178` with the twelve-byte name of
 * the level they want, and that function is the loading screen:
 *
 *     8007917C  lw   v0, 0x800AEBCC     ; suppressed while this is set
 *     8007919C  bne  v0, zero, +0x54    ;   ...return 0, no screen
 *     800791E0  jal  0x8006DBC0         ; the name against 0x800E465C
 *     800791EC  bne  v0, zero, ...      ;   already there: return 0
 *     ...       the name -> 0x800E465C and 0x800E4674
 *     80079360  sw   0x8007901C, 0x800B2D90   ; the deferred LOAD
 *     80079364  jal  0x8001A384; li a0, 46    ; enter page 46
 *     80079374  jal  0x8001A474; li a1, 16    ; 0x800A3314, at size 16
 *     80079384  jal  0x8001A474; li a1, 16    ; 0x800A3344 — all zeros
 *     80079398  sh   1, 0x800C3638             ; drawable 0's +0x48: highlight
 *
 * `0x800A3314` is `{ "LOADING", 256, 124 }` and `0x800A3344` is a NULL record,
 * so the page is one line of text with nothing navigable under it — the same
 * shape as RESTARTING and QUITTING (pages.c). `0x800C35F0` is drawable 0 and
 * `0x800C3638` is its `+0x48`, which menufont.h records as the highlight flag:
 * at size 16 that selects palette 70, the bright one.
 *
 * The LOAD ITSELF IS DEFERRED. `0x8007901C` is installed as a one-shot hook and
 * clears its own slot at `0x8007916C` when it has run, so the frame that goes
 * out carries the screen and the load happens after it. That is the whole
 * mechanism: one frame with LOADING on it, then a synchronous read during which
 * nothing is drawn at all, so the console's loading screen is a still.
 *
 * `0x800AEBCC` is written by the level selector at `0x8007C7C8` / `0x8007C7E8`
 * from the level record's `+0x20`, which leveltable.h records as "always 1 on a
 * real level" — so the screen is armed for every level and disarmed only for a
 * record that is not one.
 *
 * ---------------------------------------------------------------------------
 * THE LOGO IS NOT A MODEL. It is a sprite strip in `frontend.lbm`
 * ---------------------------------------------------------------------------
 * The turning logo in the corner is a PRE-RENDERED SPRITE STRIP, and it is in
 * the atlas this screen already writes its word with. Rows 144..203 of
 * `frontend.lbm` — under the 32-pixel face at rows 0..109 and the 16-pixel face
 * at 111..142, above the panel and prompt art at 213..255 — hold twenty-four
 * cells of a **rotation of the Quake II logo** on a 32 x 20 grid, eight cells
 * across and three rows down.
 *
 * The cell WIDTHS are what prove both the grid and what the strip is. Read on
 * that grid they run
 *
 *     17 16 14 13 11 9 7 5 4 5 7 9 11 13 14 16 18 19 19 21 22 22 22 [24]
 *
 * — a smooth narrowing to a single minimum and out again, which is |cos| and
 * is a rotation about the vertical axis. Every cell is the full 20 rows tall,
 * because a thing turning that way does not change height.
 *
 * THE TWENTY-FOURTH SLOT IS NOT `RETRY`. This file used to say it was, and the
 * decoded sheet says otherwise: (224, 184) is a full-height 24-wide logo cell,
 * differing from the broadside cell at (224, 0) in 75 of its 640 texels — the
 * next rotation step, not a word. SpriteAnim never draws it anyway, because
 * the counter wraps one short of it (below).
 *
 * The grid was found by getting it wrong first. Read as two rows of 30 the same
 * sixty rows give widths 17 16 14 13 12 13 14 16 / 18 19 19 21 22 22 22, which
 * is lumpy, has its minimum in the wrong place, and makes a broadside cell 22
 * wide by 30 tall — an aspect of 0.47 once the frame buffer's 2:3 pixel is
 * accounted for, against the logo's own 1452:1997 = 0.73. On the 20-row grid a
 * broadside cell is 22 by 20, which is 0.71. The row-density profile agrees:
 * it dips to 18 lit texels at row 164 and to 8 at row 184, the two boundaries.
 *
 * That settles three things a model could not. It explains the COLOUR — the
 * strip is in the menu font's own palette, so the logo is the pale blue the
 * text is rather than the gold of the title screen's model or the mint green of
 * `q2logowire`'s line sheet. It explains how an in-level load can show a logo
 * AT ALL, since `frontend.lbm` is in every playable map's `SNDVRAM.DAT` and no
 * model of the logo is. And it explains the size: 32 x 20 drawn 1:1, like every
 * other thing cut from this sheet.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS RETRACTS, TWICE
 * ---------------------------------------------------------------------------
 * This file first argued that the screen draws `Q2LOGO` out of
 * `LEVELS/QDUMMY/`, on the strength of what that directory holds — level table
 * record 3, `Dummy`, with ONE model (`Q2LOGO`), ONE named image
 * (`FrontEnd.lbm`) and an 840-byte zone with no world in it. A retail capture
 * showed the logo HOLLOW, so the solid model was wrong; the next reading
 * reached for `q2logowire`, QFRONT's outlined twin of the same mesh, which is
 * the right shape, the wrong colour, and in the one directory an in-level load
 * cannot borrow from.
 *
 * Both were models because the title screen's logo is a model. The sheet was
 * open in front of this port the whole time — `frontend.lbm` is what the menu
 * font is cut from — and neither pass looked at it.
 *
 * QDUMMY is still where the asset comes from here, and now for a reason that
 * needs no inference: at 21,332 bytes its `SNDVRAM.DAT` is the smallest thing
 * on the disc carrying `FrontEnd.lbm`, which is the whole of what this screen
 * needs. Its lone `Q2LOGO` is not drawn. FORMATS §11.13 keeps the shape of that
 * directory as an observation rather than as an argument.
 *
 * ---------------------------------------------------------------------------
 * THE CODE THAT COUNTS THE CELL INDEX — SpriteAnim, 0x80038820 (MAINLOOP.C)
 * ---------------------------------------------------------------------------
 * This file used to say "a cell index is a number some code counts, and that
 * code has not been found", and then chose a rate and a direction of its own.
 * The code is `SpriteAnim`, and it disagrees on every point that was guessed.
 *
 * Called with a0 = 0 it draws ONE 32 x 20 SPRITE at (460, 40) — 0x800388F4 and
 * 0x800388FC for the corner, 0x8003890C and 0x80038914 for the size, which is
 * where `Q2_LOADING_X/Y/CELL_W/CELL_H` already agreed — out of VRAM slot 13
 * (`s1 = 13` at 0x800388F0, which indexes the page tables at 0x800A3274 and
 * 0x800A329C), and the cell it draws comes from a single free-running halfword
 * at `gp+612` (0x800AE864):
 *
 *     c  < 5   -> (u,v) = (224,   0)      0x80038850 / 0x80038854
 *     c == 5   ->        = (192,  80)     0x8003886C / 0x80038874
 *     c == 6   ->        = (224, 120)     0x80038888 / 0x80038890
 *     c == 7   ->        = (  0, 220)     0x800388A4 / 0x800388A0
 *     c >= 8   ->  u = (c & 7) * 32       0x800388B8 / 0x800388BC
 *                  v = 124 + 20 * ((c >> 3) & 3)
 *                                         0x800388C0…0x800388D8
 *
 * and the tail at 0x80038CDC…0x80038CFC is `c = c + 1; if (c == 31) c = 0`.
 *
 * So THIRTY-ONE steps, not twenty-three, and FORWARDS. The four cells outside
 * the 144..203 block are further rotation frames the author parked in spare
 * corners of the sheet: measured on the decoded atlas in counter order the
 * inked widths run
 *
 *     24 24 24 24 24  21 20 19  17 16 14 13 11 9 7 5 4 5 7 9 11 13 14 16
 *     18 19 19 21 22 22 22
 *
 * — one smooth |cos| sweep with a five-step HOLD at broadside, which no
 * reading of the 8 x 3 block alone can produce. `c >= 8` covers exactly the
 * twenty-three cells at v = 144 / 164 / 184 that this file already knew about;
 * the wrap at 31 is what keeps the twenty-fourth out of the animation.
 *
 * The counter is NEVER RESET. `xrefs 0x800AE864` finds the two reads, the
 * increment and the wrap, and nothing else — InitLoadingAnim (0x80038DFC)
 * zeroes `gp+17100` and not this — so a new loading screen picks the rotation
 * up wherever the last one left it. `q2_loading_raise` used to set `spin` back
 * to zero on the strength of the argument that every screen is a new page;
 * the page is, and the counter behind the logo is not.
 *
 * The RATE is one step per VIDEO FIELD. SpriteAnim(0) is reached only from
 * 0x80038D54 in VBlankMainLoop (0x80038D1C), which is the tail of VBlankLoading
 * (0x8006E288, the hook TestIt installs at 0x8006E144), which the vertical-blank
 * ISR calls at 0x8001908C. The same ISR advances the level clock by
 * [0x800B2DD8] per call (0x800190B4), and that global is the HALF frame delta
 * briefing.c already names — 6 of the 1/300 s units this port counts in, which
 * the ISR itself spells out as a literal on its other arm (0x800190F0,
 * `addiu v0, v0, 6`). One cell every 6 units: a whole turn-and-hold is
 * 31 * 6 = 186 units, 0.62 s, against the 1.687 s this file used to take.
 *
 * And the PRIMITIVE is a rectangle, not a polygon — 0x80038B24…0x80038B60 fill
 * a libgpu `SPRT` field for field and 0x80038B68 calls `SetSprt` (0x8008A338).
 * That matters here rather than being bookkeeping: a rectangle steps one texel
 * per pixel and wraps in eight bits, and six of the thirty-one steps sit at
 * u = 224, where a quad's far corner would be 256. See `loading.c`.
 *
 * ---------------------------------------------------------------------------
 * The corner
 * ---------------------------------------------------------------------------
 *
 * WHERE it stands is measured off the capture, in the console's own 512 x 248
 * pixels, against the two text rows as a ruler: those are `STARTING` at
 * (256, 111) and `GAME` at (256, 137) from QFRONT's own records, so a distance
 * in the picture is a known number of frame-buffer pixels.
 */
#ifndef Q2PSX_LOADING_H
#define Q2PSX_LOADING_H

#include "disc.h"
#include "gpu.h"
#include "hudtables.h"
#include "menu.h"
#include "menufont.h"
#include "q2psx.h"
#include "vram.h"

/*
 * Where the screen's one asset comes from.
 *
 * Any map carrying `frontend.lbm` would do — which is every playable one — but
 * this screen holds its bank open for the whole run, so the smallest carrier is
 * the right one to hold: QDUMMY's `SNDVRAM.DAT` is 21 KB against BASE0's 684.
 */
#define Q2_LOADING_MAP    "QDUMMY"

/*
 * How long the screen is held, in the level clock's 1/300 s units — the same
 * units `Q2_START_BEAT_UNITS` is in, and for the same reason: a frame count
 * would be a different duration at every frame rate.
 *
 * THE CONSOLE HAS NO SUCH NUMBER. Its screen lasts exactly as long as the disc
 * takes, which is one deferred frame plus the read. This port's read is
 * effectively free, so without a floor the screen would flash for a single
 * frame and be gone. 150 is half a second.
 */
#define Q2_LOADING_HOLD_UNITS  150

/*
 * The logo strip in `frontend.lbm`, measured off the decoded sheet and mapped
 * by SpriteAnim's own switch.
 *
 * The quad is 32 x 20 (0x8003890C, 0x80038914). The block at v = 144 is eight
 * cells across on a 32-texel pitch and three rows down on a 20-texel one, and
 * it is where counter values 8..30 land — `Q2_LOADING_CELL_V` and
 * `Q2_LOADING_CELL_COLS` are the `(c & 7) * 32` and `124 + 20 * ((c >> 3) & 3)`
 * at 0x800388B8…0x800388D8, written as a grid. Values 0..7 are the four cells
 * elsewhere on the sheet; `q2_loading_cell_uv` holds the whole map.
 */
#define Q2_LOADING_CELL_W     32
#define Q2_LOADING_CELL_H     20
#define Q2_LOADING_CELL_V    144
#define Q2_LOADING_CELL_COLS   8

/*
 * How many steps the counter takes before it starts again — 0x80038CE0's
 * `addiu v1, zero, 31` against 0x80038CF4's `bne v0, v1`, with the reset at
 * 0x80038CFC. The increment comes first, so the values the draw ever sees are
 * 0..30 and the thirty-first is never one of them.
 *
 * It was 23 here, which is the size of the grid block and not the length of
 * the animation: the four cells at (224,0), (192,80), (224,120) and (0,220)
 * were simply not being shown.
 */
#define Q2_LOADING_CELLS      31

/*
 * One cell every 6 units of the level clock — one per VIDEO FIELD.
 *
 * SpriteAnim runs off the vertical-blank hook, and the same ISR advances the
 * clock by [0x800B2DD8] per call (0x800190B4). briefing.c already records that
 * global as HALF the frame delta, and the ISR spells the value out as a literal
 * on the arm where it must not read it (0x800190F0, `addiu v0, v0, 6`); six of
 * the 1/300 s units this port counts in is 50 steps a second, the PAL field
 * rate, which is what Q2_DT_HZ is measured against.
 *
 * It was 22 — a rate derived from the front end's model spin on the assumption
 * that nothing on the disc stated one, which made the turn 3.7 times too slow.
 */
#define Q2_LOADING_CELL_UNITS  6

/*
 * The top-left corner of the quad, in the console's 512 x 248 pixels.
 *
 * From the capture: the drawn logo spans x 465..488, centred on 476, and a
 * broadside cell's own content sits at texels 5..26 of its 32 — so the cell
 * starts at 460. Vertically it is centred on 50, and the cell is 20 tall.
 *
 * The capture measures 23 x 19 for that logo, against the 22 x 20 a broadside
 * cell drawn 1:1 gives. Both halves agreeing to a pixel is what says the quad
 * is 1:1, which is what every other thing cut from this sheet is.
 */
#define Q2_LOADING_X   460
#define Q2_LOADING_Y    40

/*
 * The palette the logo is drawn through — the executable's built-in bank, id 4.
 *
 * A monotonic sixteen-step grey ramp, `000000 080808 181818 ... D8D8D8 E8E8E8`.
 * The strip is authored against palette 68, whose sixteen entries are a blue
 * ramp in the same index order, so 4 is the same picture with the colour taken
 * out. See loading.c for why the other greys in the bank are not it.
 */
#define Q2_LOADING_PALETTE  4

/*
 * The ordering-table bucket the logo is linked into.
 *
 * Inside the overlay slice at OT[206..216] (screen.h) and BELOW the menu's own
 * OT[213], because compose walks the table in ascending order. The two do not
 * overlap on screen, so this says which is furniture and which is the page
 * rather than deciding who wins.
 */
#define Q2_LOADING_OT_BUCKET  (210 * PSX_OT_SUBDIV)

/*
 * WHICH SCREEN, because there are two and they are the same furniture.
 *
 * LOADING is the executable's, `0x800A3314`, put up by every transition. The
 * other is the front end's own and is the one the retail capture shows:
 * QFRONT's module+0x0EBF4 installs `STARTING` at (256, 111) and `GAME` at
 * (256, 137), the same two-row shape as RESTARTING / LEVEL, and it is what the
 * half second between a difficulty being confirmed and the opening reel
 * actually looks like. This port drew a blank front end for those fifteen
 * frames.
 *
 * The module carries a third, one row reading `DEMO OF GAME` at (256, 111)
 * (module+0x0EBC4), which belongs to the attract loop this port does not run.
 * It is not offered here because nothing would raise it.
 */
typedef enum q2_loading_page {
    Q2_LOADING_PAGE_LOADING = 0,   /* 0x800A3314, the executable's   */
    Q2_LOADING_PAGE_STARTING       /* QFRONT module+0x0EBF4          */
} q2_loading_page;

/*
 * THE CONSOLE HAS TWO LOADING SCREENS AND THIS PORT RAISES ONE OF THEM.
 *
 * The level screen is the one `ProcessGame` puts up around `LoadLevel`
 * (0x80018C88 `jal 0x8006E150`, TestIt in TITLE.C). It clears both ordering
 * tables (0x8006E188, 0x8006E194), calls InitLoadingAnim (0x80038DFC) and
 * InitialiseVBlankLoading (0x8006DFB8), and the last thing that does is
 * `sw 0x8006E288, 0x800B2D94` at 0x8006E144 — installing VBlankLoading as the
 * VERTICAL-BLANK HOOK. From then until FadeOutLoading (0x8007C8F8) spin-waits
 * for it at 0x8007C914 and unhooks at 0x8007C92C, every vblank draws a whole
 * animated frame while the CD read blocks. It owns the screen, for seconds.
 * This is the one below.
 *
 * The other is MaybeLoadZoneName's (0x80079178, LOADLEV.C), a still over one
 * frame while the zone streams in. This port does not raise it: its own zone
 * load is milliseconds off a file on disk, there is no read to cover, and a
 * zone seam is meant to pass without the player seeing anything at all.
 */
typedef struct q2_loading {
    /*
     * Its own VRAM image rather than the live one.
     *
     * The screen is up while a map's pages are being replaced, and a zone gate
     * inside one map does not re-upload them at all (client_load_zone) — so a
     * screen drawing out of the session's image would be drawing out of a bank
     * that is being taken away. A separate 1 MB image is what makes it
     * independent of whatever is loading behind it.
     */
    psx_vram       *vram;
    q2_menu_font    font;
    bool            font_ready;

    /* The page, through the same draw path every other page uses. A second
     * q2_menu rather than the session's, for the reason the memory-card front
     * end keeps one: it must not disturb where the player was. */
    q2_menu         menu;

    bool            ready;    /* the atlas is open                           */

    /* Live state. */
    bool            open;
    bool            timed;    /* it runs its own hold, rather than a caller's */
    double          hold;     /* 1/300 s units still to run, when `timed`    */
    double          spin;     /* 1/300 s units spent on the strip            */
} q2_loading;

/*
 * Open the screen's atlas. Q2_ERR_NOT_FOUND when the disc has no QDUMMY or no
 * `frontend.lbm` in it, which is a disc without a loading screen and not a
 * fault: the caller carries on with `ready` false and the screen is never
 * raised.
 *
 * `tab` supplies the palette bank the font upload needs; `settings` is the one
 * the session already owns, since a q2_menu holds a pointer to it.
 */
q2_result q2_loading_open(q2_loading *l, const disc *d,
                          const q2_hud_tables *tab,
                          q2_menu_settings *settings);
void      q2_loading_close(q2_loading *l);

/*
 * Raise the LEVEL screen and run its hold, or restart that hold if it is
 * already up. Does nothing when the assets are not there.
 *
 * The hold is a port invention and stays: the console's level screen lasts a
 * blocking CD read of a whole level directory, and this port's load is a
 * handful of milliseconds off a file on disk. Without it the screen would
 * flash for one frame where the console shows it for seconds.
 */
void q2_loading_raise(q2_loading *l);


/*
 * Put a page up with no clock of its own, for a caller that already has one —
 * the STARTING screen rides the opening reel's beat, and two countdowns for one
 * half second is exactly the pair that drifts apart.
 */
void q2_loading_show(q2_loading *l, q2_loading_page page);
void q2_loading_hide(q2_loading *l);

/*
 * Turn the logo, and spend the hold if this screen is running one.
 *
 * Returns true only while the screen OWNS the frame, which a raised one does
 * and a shown one does not: the caller that showed it is the one deciding what
 * else runs. Call it once per frame whatever is on screen.
 */
bool q2_loading_step(q2_loading *l, double dt);

/* Which step of the strip is up — SpriteAnim's `gp+612`, 0..30 — so the turn
 * can be checked without a framebuffer. */
u32 q2_loading_cell(const q2_loading *l);

/*
 * Where that step's cell sits on `frontend.lbm`, as the texel coordinates a
 * POLY_FT4's `u0`/`v0` take. The whole of SpriteAnim's switch,
 * 0x80038840…0x800388DC, including the four cells that are not on the grid.
 * A step past the counter's range is clamped to the last one rather than
 * wrapping off the sheet.
 */
void q2_loading_cell_uv(u32 cell, u8 *u_out, u8 *v_out);

/* Build the black screen's contents into `ot`: the logo, then the page over it.
 * `width` and `height` are the framebuffer's, so the 512 x 248 layout can be
 * centred in it. Returns the number of primitives emitted. */
u32 q2_loading_build_ot(const q2_loading *l, psx_ot *ot,
                        int width, int height);

#endif /* Q2PSX_LOADING_H */
