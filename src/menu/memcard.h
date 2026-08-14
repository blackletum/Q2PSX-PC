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
 * dispatches through a **19-entry jump table at `0x800AB734`**. Fourteen of the
 * nineteen entries land on the common exit at `0x8001F7F8`, so five states have
 * behaviour of their own:
 *
 *     3   `0x8001F140`  a LIST: hands the chosen row to the card code
 *     5   `0x8001F790`  a two-way choice: item 1 -> state 6, item 0 -> state 1
 *    13   `0x8001F19C`  formats the cheat-level name into the screen's text
 *    14   `0x8001F0A4`  accept: applies the game variables and leaves
 *    16   `0x8001F0A4`  the same arm as 14
 *    19   `0x8001F718`  a two-way choice: item 1 -> state 20
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
 * The original's own numbering, so a host that wants to match it can. Only the
 * six with an arm of their own are named; the rest fall through to the common
 * exit and are left as numbers rather than given invented meanings.
 */
#define Q2_MCARD_STATE_LIST        3    /* 0x8001F140 */
#define Q2_MCARD_STATE_CHOICE_5    5    /* 0x8001F790 -> 6 or 1 */
#define Q2_MCARD_STATE_REPORT     13    /* 0x8001F19C */
#define Q2_MCARD_STATE_ACCEPT_A   14    /* 0x8001F0A4 */
#define Q2_MCARD_STATE_ACCEPT_B   16    /* the same arm */
#define Q2_MCARD_STATE_CHOICE_19  19    /* 0x8001F718 -> 20 */
#define Q2_MCARD_STATE_MAX        19    /* the jump table's length */

/* Does this state have an arm of its own, or does it fall through? */
bool q2_mcard_state_live(int state);

/* ------------------------------------------------------------------------- */
/* The host's side of the three function pointers                             */
/* ------------------------------------------------------------------------- */
/*
 * `poll` stands in for `0x800B3234` — what the card is doing now, as one of the
 * state numbers above. `request` stands in for `0x800B3238`, which the arms
 * call with the state they want next. `choose` stands in for `0x800B324C`,
 * which state 3 calls with the selected row.
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

    /* The cheat level state 13 formats into its message (NONE/BRONZE/SILVER/
     * GOLD, from the array the arm builds on its stack at 0x8001F19C). */
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
 *   state 3    calls `choose(cursor)`, then asks for the next state
 *   state 5    cursor 1 -> request(6),  cursor 0 -> request(1)
 *   state 19   cursor 1 -> request(20)
 *   14 / 16    accept: `accepted` comes back true so the caller can apply the
 *              loaded settings and leave, which is what 0x8001C698 and
 *              0x8001E1A4 do on the console
 *
 * Returns true when the front end wants to close.
 */
bool q2_mcard_advance(q2_mcard *m, u16 buttons);

/* The screen the current state shows, or Q2_MCARD_NONE when the state has no
 * arm. This is the one place the reconstruction is INCOMPLETE and says so: the
 * nine table installers are separate functions and which state calls which was
 * not followed, so this maps only the states whose screen is unambiguous from
 * the arm's own behaviour. */
q2_mcard_screen q2_mcard_screen_for_state(int state);

/*
 * The same question, answered for the PORT's own file-backed flow.
 *
 * A host that implements the three function pointers has to put something on
 * screen for every state it uses, including the ones whose console mapping was
 * never established. This is that mapping, and it is the port's: it agrees with
 * `q2_mcard_screen_for_state` wherever that one commits, and fills in the rest
 * with the screen the port's own flow needs there (saveui.h explains which of
 * the two each state is). Keeping them as separate functions is what stops a
 * reader mistaking the port's arrangement for a reading of the disc.
 */
q2_mcard_screen q2_mcard_screen_for_state_port(int state);

#endif /* Q2PSX_MEMCARD_H */
