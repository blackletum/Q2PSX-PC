/*
 * memcard.h — the memory-card front end.
 *
 * ---------------------------------------------------------------------------
 * What it is, and why it did not fall out with the rest of the menu
 * ---------------------------------------------------------------------------
 * The menu reconstruction (menu.h) covers seventeen pages reached through the
 * page-enter routine at `0x8001A384`. The memory-card screens are not among
 * them, and the reason is structural rather than incidental: **they do not use
 * the page mechanism at all.** Their item tables are installed directly, by
 * nine small functions in `0x8001D2xx`…`0x8002040x`, and what decides which one
 * runs is a separate state machine.
 *
 * Two facts make that machine legible.
 *
 * **The screens are ordinary menu tables.** Every one is the same 24-byte item
 * record the rest of the menu uses, laid out at x = 256 with the same loader
 * and the same font, so they navigate, draw and grey exactly like any other
 * page. They are transcribed below and `q2psx-inspect menu` checks them against
 * the disc alongside the seventeen real pages.
 *
 * **Every action pointer in them is `0x8001FD80`, which is `jr ra; nop`.**
 * That is not a gap in the reading — it is the design. The front end does not
 * hang behaviour off the item callbacks the way the pause menu does; it reads
 * the *cursor* when the button is released and decides for itself. Slot 0 of
 * the weapon array being a do-nothing shot was the same tell (FORMATS.md §13.1),
 * and it means a port that wires these labels to actions has misread them.
 *
 * ---------------------------------------------------------------------------
 * The state machine — `0x8001EFDC`, installed by page 39
 * ---------------------------------------------------------------------------
 * `0x80020428` — the YES on the save prompt — enters page id **39**, stores
 * `0x8001EFDC` into the menu state block at `gp`-relative `+656`, and sets the
 * mode flag at `0x800B32A2`. From then on that function runs per frame.
 *
 * It reads a state from a function pointer at `0x800B3234`, subtracts one, and
 * dispatches through a **19-entry jump table at `0x800AB734`**. Thirteen of the
 * nineteen entries land on the common exit at `0x8001F7F8`, so six states have
 * behaviour of their own:
 *
 *     3   `0x8001F140`  the CARD list: the chosen row -> SetActiveCard, then 4
 *     5   `0x8001F790`  NOT FORMATTED: YES (row 1) -> 6, NO (row 0) -> 1
 *    13   `0x8001F19C`  the FILE window: composes the highlighted file's
 *                       RESUPPLIES / DIFFICULTY / CHEATS lines every frame;
 *                       the chosen row -> SetActiveFile, then 21 on a load,
 *                       22 for NEW FILE on a save, 19 for any other save row
 *    14   `0x8001F0A4`  accept: applies the game variables and leaves
 *    16   `0x8001F0A4`  the same arm as 14
 *    19   `0x8001F718`  OVERWRITE?: YES (row 1) -> 20, NO (row 0) -> 13
 *
 * ---------------------------------------------------------------------------
 * The state IS the screen — MemoryCardSetStatus, 0x80082720
 * ---------------------------------------------------------------------------
 * `0x800B3234` is engine+0x250, MemoryCardGetStatus (0x80082710, MEMCARD.C),
 * which returns the word at 0x800E82BC. Its only writer outside init is
 * MemoryCardSetStatus (0x80082720) and the copies of it MemoryCardUpdate2
 * inlines, and every one of them is the same three instructions: store the
 * status, then call engine+0x278 — InGame_PrintMemCardMessage (0x8001E3D4) —
 * WITH THAT SAME NUMBER (0x80082774..0x80082788; the inlined copies are the
 * `addu v0, a0, zero` / `jalr v1` / `sw v0, 12(...)` triples at 0x80081350,
 * 0x80081384 ... 0x800826B8). The one place a message is printed without a
 * status store is 0x80081F44, which re-prints message 3 while the status is
 * already 3 — the card list redrawn after a card is pulled or inserted.
 *
 * So the nine table installers are all arms of ONE function, and which screen
 * a state shows is that function's 22-entry jump table at 0x800AB6DC, indexed
 * by `status - 1` (0x8001E4A4..0x8001E4D0). `q2_mcard_screen_for_state` is
 * that table; `q2_mcard_compose` is the text each arm writes over it.
 *
 * ---------------------------------------------------------------------------
 * How an item fires — and it is the release, on both pads
 * ---------------------------------------------------------------------------
 * Every live arm opens with the same four instructions:
 *
 *     if (held & CROSS)         return;    -- 0x800B3290, the current mask
 *     if (!(released & CROSS))  return;    -- 0x800B3298, prev & ~cur
 *
 * which is the on-release rule the confirmations elsewhere use (§10.3), spelled
 * out inline rather than through the item's flag bit 13. The selection is then
 * `cursor - first` — `0x800B32AC` minus `0x800B32AE`, the same two globals the
 * item loader maintains — so the screens are read positionally.
 *
 * ---------------------------------------------------------------------------
 * What a PC port can and cannot reconstruct
 * ---------------------------------------------------------------------------
 * The card operations themselves are three function pointers in the state block
 * — `0x800B3234` (poll the state), `0x800B3238` (request a transition) and
 * `0x800B324C` (act on a chosen row) — and behind them is the console's
 * `libmcrd` talking to hardware this port does not have. Reconstructing *those*
 * would be inventing them.
 *
 * So this module reconstructs what is actually there: the screens, their text,
 * their coordinates, the navigation and the release rule, and a host interface
 * where a PC's save-file handling plugs into the same shape. The state numbers
 * are the original's, so a host that later wants to match them can.
 */
#ifndef Q2PSX_MEMCARD_H
#define Q2PSX_MEMCARD_H

#include "menu.h"

/*
 * `jr ra; nop`. Every action pointer in the memory-card tables is this address,
 * and a record pointing at it has an action and does nothing — the same idiom
 * as slot 0 of the weapon array (FORMATS.md §13.1). Anything comparing the
 * port's transcription against the disc has to know the stub, or it reports a
 * mismatch on every one of them.
 */
#define Q2_MENU_ACTION_NOP 0x8001FD80u

/* The one memory-card record with a real action: SAVE?'s YES, which enters
 * page 39 and installs the state machine (0x80020428). */
#define Q2_MCARD_ACTION_ENTER 0x80020428u

/* ------------------------------------------------------------------------- */
/* The screens                                                                */
/* ------------------------------------------------------------------------- */
/*
 * Named for what they say. The address of each table is carried so the
 * transcription is checkable rather than asserted; a screen with a second
 * table is the question-and-answers idiom the confirmations use (§10.1), where
 * the LAST table installed is the navigable one.
 */
typedef enum q2_mcard_screen {
    Q2_MCARD_NONE = 0,
    Q2_MCARD_SELECT_SLOT,     /* SELECT A / MEMORY CARD SLOT                 */
    Q2_MCARD_NOT_FORMATTED,   /* MEMORY CARD / NOT FORMATTED / FORMAT? + N/Y */
    Q2_MCARD_FORMATTING,      /* DO NOT POWER-OFF OR / REMOVE MEMORY CARD    */
    Q2_MCARD_SAVE_FILE,       /* SAVE FILE, then four runtime-filled rows    */
    Q2_MCARD_OVERWRITE,       /* DO YOU WANT / TO OVERWRITE? + NO/YES        */
    Q2_MCARD_SAVE_PROMPT,     /* DO YOU WANT TO SAVE? + YES/NO               */
    Q2_MCARD_SAVING,          /* LONGER LOAD / SAVE MESSAGE / HERE           */
    Q2_MCARD_LOAD_MESSAGE,    /* LOAD MESSAGE / HERE                         */
    Q2_MCARD_NO_CONTROLLER,   /* PLEASE INSERT / CONTROLLER INTO / PORT XX   */
    Q2_MCARD_SCREEN_COUNT
} q2_mcard_screen;

/* The screen as a page, so it draws and navigates through the ordinary menu
 * path. `id` is 0 on all of them: these have no page id, which is the finding.
 * Returns NULL for Q2_MCARD_NONE or an out-of-range screen. */
const q2_menu_page *q2_mcard_page(q2_mcard_screen s);

/* Every screen, for the transcription check. */
const q2_menu_page *q2_mcard_pages(u32 *count);

const char *q2_mcard_screen_name(q2_mcard_screen s);

/* ------------------------------------------------------------------------- */
/* The four rows the runtime fills in                                         */
/* ------------------------------------------------------------------------- */
/*
 * `SAVE FILE`'s table is one label followed by **four records pointing at the
 * empty string** at (256, 66), (256, 164), (256, 180) and (256, 196). They are
 * not padding: the empty label is what the selection bar tests for
 * (`strcmp` against `0x800AE634` at `0x8001A7FC`), so an unfilled row draws
 * nothing and gets no bar, and a filled one behaves like any other item.
 *
 * A host writes the file names here, in slot order.
 */
#define Q2_MCARD_SLOTS 4
#define Q2_MCARD_NAME_MAX Q2_MENU_TEXT_MAX

/* ------------------------------------------------------------------------- */
/* The states                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * The original's own numbering — MEMCARD.C's status word, which is also the
 * message number InGame_PrintMemCardMessage switches on. What each one SHOWS
 * is read off that function's arms (0x800AB6DC):
 *
 *    1  LOAD MESSAGE  "CHECKING FOR" / "MEMORY CARDS"          0x8001E52C
 *    2  LOAD MESSAGE  "NO MEMORY" / "CARDS FOUND"              0x8001E844
 *    3  SELECT SLOT   + one row per card at y = 100 + 16i      0x8001E5E0
 *    4  LOAD MESSAGE  "CHECKING FOR" / "QUAKE 2 FILES"         0x8001E7B4
 *    5  NOT FORMATTED + NO / YES                               0x8001EBDC
 *    6  FORMATTING    row 0 "XXX" -> "FORMATTING"              0x8001EC1C
 *    7  (nothing installed: the screen stays, cursor = first)  0x8001EFB0
 *    8  LOAD MESSAGE  "MEMORY CARD" / "FORMAT COMPLETE"        0x8001EC7C
 *    9  LOAD MESSAGE  "MEMORY CARD" / "FORMAT FAILED"          0x8001E8D4
 *   10  as 4                                                   0x8001E7B4
 *   11  as 4                                                   0x8001E7B4
 *   12  SAVING        "NO QUAKE 2" / "SINGLE PLAYER" or
 *                     "MULTIPLAYER" / "FILES FOUND"            0x8001ED2C
 *   13  SAVE FILE     FrontEnd_DoFileWindow (0x8001D84C)       0x8001EEC0
 *   14  FORMATTING    row 0 -> "LOADING FROM" / "SAVING TO"    0x8001EED0
 *   15  as 14
 *   16  LOAD MESSAGE  "MULTIPLAYER" / "CONFIG LOADED",
 *                     "STARTING" / "GAME", "MULTIPLAYER" /
 *                     "DATA SAVED" or "GAME" / "SAVED"         0x8001EA28
 *   17  LOAD MESSAGE  "MEMORY CARD" / "LOAD FAILED" or
 *                     "SAVE FAILED"                            0x8001E964
 *   18  SAVING        "INSUFFICIENT FREE BLOCKS" /
 *                     "PLEASE INSERT ANOTHER" / "MEMORY CARD"  0x8001EDF4
 *   19  OVERWRITE?    + NO / YES                               0x8001E4D8
 *   20  as 14  (21 and 22 too)                                 0x8001EED0
 *
 * SAVE? and NO CONTROLLER are not statuses: SAVE? is page 39, put up by
 * InGame_GOTO_WANT_TO_SAVE (0x800203C4), and NO CONTROLLER is page 38, put up
 * by InGame_GOTO_REINSERT_CONTROLLER (0x8001D200).
 */
#define Q2_MCARD_STATE_CARDS       3    /* 0x8001F140: card row -> 4          */
#define Q2_MCARD_STATE_FORMAT_ASK  5    /* 0x8001F790: -> 6 or 1              */
#define Q2_MCARD_STATE_FORMATTING  6
#define Q2_MCARD_STATE_FILES      13    /* 0x8001F19C: file row -> 19/21/22   */
#define Q2_MCARD_STATE_ACCEPT_A   14    /* 0x8001F0A4                         */
#define Q2_MCARD_STATE_DONE       16    /* 0x8001F0A4, the success message    */
#define Q2_MCARD_STATE_ACCEPT_B   Q2_MCARD_STATE_DONE
#define Q2_MCARD_STATE_FAILED     17    /* no arm: BACK is the way out        */
#define Q2_MCARD_STATE_OVERWRITE  19    /* 0x8001F718: -> 20 or 13            */
#define Q2_MCARD_STATE_SAVE_OVER  20    /* busy: writing over a file          */
#define Q2_MCARD_STATE_LOADING    21    /* busy: reading                      */
#define Q2_MCARD_STATE_SAVE_NEW   22    /* busy: writing a NEW FILE           */

/* The two-way choices under their older names. */
#define Q2_MCARD_STATE_CHOICE_5   Q2_MCARD_STATE_FORMAT_ASK
#define Q2_MCARD_STATE_CHOICE_19  Q2_MCARD_STATE_OVERWRITE

#define Q2_MCARD_STATE_MAX        19    /* the input jump table's length      */
#define Q2_MCARD_STATUS_MAX       22    /* the message jump table's length    */

/* Does this state have an arm of its own, or does it fall through? */
bool q2_mcard_state_live(int state);

/* ------------------------------------------------------------------------- */
/* The host's side of the three function pointers                             */
/* ------------------------------------------------------------------------- */
/*
 * `poll` stands in for `0x800B3234` — what the card is doing now, as one of the
 * state numbers above. `request` stands in for `0x800B3238`, which the arms
 * call with the state they want next. `choose` stands in for `0x800B324C`
 * (SetActiveCard), which state 3 calls with the selected card, and for
 * `0x800B3250` (SetActiveFile), which state 13 calls with the selected file.
 *
 * A port with no memory card supplies its own save handling behind these; the
 * shape is the original's, the implementation is not, and nothing here pretends
 * otherwise.
 */
typedef struct q2_mcard_host {
    int  (*poll)(void *user);
    void (*request)(void *user, int state);
    void (*choose)(void *user, int row);
    void *user;
} q2_mcard_host;

typedef struct q2_mcard {
    const q2_mcard_host *host;      /* not owned                             */

    q2_mcard_screen screen;
    int             state;          /* the last polled state                 */

    /* The four rows of SAVE FILE, in slot order. An empty name is an empty
     * slot and draws nothing. */
    char            name[Q2_MCARD_SLOTS][Q2_MCARD_NAME_MAX];

    /* The highlighted file's cheat level, which state 13 formats into its
     * `CHEATS %s` line (NONE/BRONZE/SILVER/GOLD, from the array the arm
     * builds on its stack at 0x8001F19C). */
    int             cheat_level;

    int             cursor;         /* 0x800B32AC minus 0x800B32AE           */
    u16             pad_prev;
    bool            fired;          /* the release was seen this frame       */
} q2_mcard;

void q2_mcard_init(q2_mcard *m, const q2_mcard_host *host);

/* Put a name in one of SAVE FILE's four rows. NULL or "" empties it. */
void q2_mcard_set_name(q2_mcard *m, int slot, const char *name);

/*
 * One frame. Polls the state, maps it to a screen, and — on the RELEASE of
 * CROSS, which is what every live arm tests — acts on the cursor:
 *
 *   state 3    `choose(cursor)` (SetActiveCard), then request(4)
 *   state 5    cursor 1 -> request(6),  cursor 0 -> request(1)
 *   state 13   `choose(cursor)` (SetActiveFile), then request(13). The
 *              console's arm picks 21 / 22 / 19 itself from the save flag,
 *              the row and GetNumFreeBlocks (0x8001F6A4..0x8001F714); here
 *              the host owns the slots, so asking for 13 FROM 13 means
 *              "this row was chosen" and the host makes that same choice
 *   state 19   cursor 1 -> request(20), cursor 0 -> request(13)
 *   14 / 16    accept: `accepted` comes back true so the caller can apply the
 *              loaded settings and leave, which is what 0x8001C698 and
 *              0x8001E1A4 do on the console
 *
 * Returns true when the front end wants to close.
 */
bool q2_mcard_advance(q2_mcard *m, u16 buttons);

/* ------------------------------------------------------------------------- */
/* What raises the SAVE? screen outside the front end                         */
/* ------------------------------------------------------------------------- */
/*
 * GameCheckPaused (0x8003E538, QUAKE.C) runs every frame, and the only thing
 * in the image that reaches InGame_GOTO_WANT_TO_SAVE is its arm at
 * 0x8003E6FC..0x8003E75C:
 *
 *   8003E700  lw   v1, 0x800AEBD0   LevelChange — incremented at 0x80018C4C,
 *                                   once per level load in ProcessGame
 *   8003E704  lw   v0, gp+700       the latch (0x800AE8BC), written at
 *                                   0x8003E728 and read here, and those are
 *                                   its only two references in the image
 *   8003E70C  beq  v1, v0 -> out    this load has already been asked about
 *   8003E718  lw   v0, 0x800AEBCC   LevelIsMultiPlayer
 *   8003E720  bne  v0, zero -> out  never in deathmatch
 *   8003E728  sw   v1, gp+700       LATCH FIRST...
 *   8003E730  lw   v1, 0x800B271C   ...THEN read the exit code the transition
 *                                   carried (stored at 0x80018C84)
 *   8003E738  beq  v1, 2 -> raise   LOADMAP (screen.h)
 *   8003E740  bne  v1, 4 -> out     4 has no dispatch arm of its own
 *
 * THE ORDER OF THOSE LAST FOUR IS THE BEHAVIOUR, which is why this is a
 * function rather than an `if` at the call site. The latch is advanced for a
 * load that arrived by ANY exit code, so a level reached by a RESTART (21) or
 * a RESUPPLY (20) is not asked about — and is not asked about a frame later
 * either, because the counter and the latch now agree. The deathmatch test
 * comes BEFORE the latch write, so an arena leaves the latch where it was.
 *
 * `latch` is the caller's copy of gp+700 and is advanced in place.
 */
bool q2_mcard_save_prompt_due(u32 level_change, u32 *latch,
                              bool deathmatch, int exit_code);

/*
 * The screen a status shows: InGame_PrintMemCardMessage's jump table at
 * 0x800AB6DC, whole. Q2_MCARD_NONE for 7, whose arm installs nothing and
 * leaves whatever was up, and for anything outside 1..22.
 */
q2_mcard_screen q2_mcard_screen_for_state(int state);

/*
 * The same map, for the port's own file-backed flow (saveui.h). Since the
 * console's map is complete this IS it; it is kept as a separate entry point
 * because the client and the inspector call it, and so that a port-only
 * arrangement, if one is ever needed again, has somewhere to live that is not
 * the reading.
 */
q2_mcard_screen q2_mcard_screen_for_state_port(int state);

/* ------------------------------------------------------------------------- */
/* What each arm writes over its table                                        */
/* ------------------------------------------------------------------------- */
/*
 * Two of the nine tables are placeholders the arms overwrite through
 * FrontEndModifyText: LOAD MESSAGE's `LOAD MESSAGE` / `HERE` and SAVING's
 * `LONGER LOAD` / `SAVE MESSAGE` / `HERE` are never seen as such, and
 * FORMATTING's row 0 is the literal `XXX`. What goes there depends on two
 * flags the console keeps in the menu block:
 *
 *   saving         0x800B32A2 — 0 on a load, 1 on a save (the same byte
 *                  0x80020428 sets when SAVE? is answered YES)
 *   single_player  the byte at 0x800B32A3 — `(0x800B32A0 & 0x7F000000) ==
 *                  0x01000000` (0x8001ED64..0x8001ED74); zero is the
 *                  front end's multiplayer settings file
 */
typedef struct q2_mcard_context {
    bool saving;
    bool single_player;
} q2_mcard_context;

/*
 * Fill `text[0..rows-1]` — a q2_menu's text overrides — with what status
 * `state`'s arm writes, and empty the rest. Returns how many leading rows were
 * written. A row the arm does not touch is left "", so the table's own label
 * shows, which is exactly what the console draws there.
 *
 * Status 13 writes row 0 only — `SAVE FILE`, `LOAD SINGLE` or `LOAD MP SETUP`
 * (0x8001D88C..0x8001D8D8) — and row 1 is the empty string. Its rows 2..4 are,
 * on the console, the highlighted file's `RESUPPLIES %d` / `DIFFICULTY %s` /
 * `CHEATS %s` (0x8001F19C..0x8001F644), and the files themselves are appended
 * BELOW the table at y = 100 + 16i with `NEW FILE` after them on a save
 * (0x8001DAB0..0x8001DCA4). The port's slot rows sit in rows 1..4 instead,
 * which is the one layout here that is the port's.
 */
int q2_mcard_compose(int state, const q2_mcard_context *ctx,
                     char (*text)[Q2_MENU_TEXT_MAX], int rows);

/*
 * The prompt rows each status asks for, as the y it passes to MoveBottomPoly
 * (0x8001F938) or 260 for parked: [0] SELECT, [1] BACK.
 *
 *   3, 13       SELECT and BACK at 216 in the front end, 220 in game
 *               (0x8001E3E0..0x8001E488, keyed on 0x800AE8B4)
 *   8, 16       SELECT at 220 (0x8001ED10, 0x8001EB8C)
 *   19          SELECT and BACK at 220 (0x8001E510, then 0x8001ED14)
 *   all others  both parked (0x8001E400..0x8001E424)
 *
 * Status 13 then asks for SELECT at 220 again from FrontEnd_DoFileWindow
 * (0x8001DD0C), so in the front end its SELECT ends at 220 and its BACK at 216.
 */
void q2_mcard_prompt_rows(int state, bool front_end, int y[2]);

#endif /* Q2PSX_MEMCARD_H */
