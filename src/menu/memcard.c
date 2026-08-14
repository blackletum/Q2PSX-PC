#include "memcard.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The tables                                                                 */
/*                                                                            */
/* Transcribed record for record from the executable's data segment. Every     */
/* action pointer in the original is 0x8001FD80 — `jr ra; nop` — so every item */
/* here carries Q2_ACT_NONE, which is the same statement in this port's terms. */
/* The two-table screens put the question in the first and the answers in the  */
/* second, which is what makes only the answers navigable (§10.1).             */
/* ------------------------------------------------------------------------- */

#define TEXT(l, X, Y) { (l), (X), (Y), Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 }
/* The answers fire on the release, which the arms test inline rather than
 * through the record's flag — the record's own bit 14 is clear. Marking them
 * on-release here keeps this port's engine doing what the console does. */
#define ANSWER(l, X, Y) { (l), (X), (Y), Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 }

/* 0x8009B2C4 */
static const q2_menu_item k_select_slot[] = {
    TEXT("SELECT A",          256,  33),
    TEXT("MEMORY CARD SLOT",  256,  55)
};

/* 0x8009B1A4 + 0x8009B204 */
static const q2_menu_item k_not_formatted[] = {
    TEXT("MEMORY CARD",            256,  72),
    TEXT("NOT FORMATTED",          256,  98),
    TEXT("DO YOU WANT TO FORMAT?", 256, 124),
    ANSWER("NO",                   256, 176),
    ANSWER("YES",                  256, 150)
};

/* 0x8009B24C — the busy screen. Its first row is the literal "XXX" at
 * 0x800AE670, a placeholder the runtime overwrites with the card's name. */
static const q2_menu_item k_formatting[] = {
    TEXT("XXX",                  256,  72),
    TEXT("MEMORY CARD",          256,  98),
    TEXT("DO NOT POWER-OFF OR",  256, 150),
    TEXT("REMOVE MEMORY CARD",   256, 176)
};

/* 0x8009B114 — one heading and the four runtime-filled rows. */
static const q2_menu_item k_save_file[] = {
    TEXT("SAVE FILE", 256,  40),
    TEXT("",          256,  66),
    TEXT("",          256, 164),
    TEXT("",          256, 180),
    TEXT("",          256, 196)
};

/* 0x8009B324 + 0x8009B36C */
static const q2_menu_item k_overwrite[] = {
    TEXT("DO YOU WANT",    256,  85),
    TEXT("TO OVERWRITE?",  256, 111),
    ANSWER("NO",           256, 163),
    ANSWER("YES",          256, 137)
};

/* 0x8009B3B4 + 0x8009B3E4 — the one screen whose answers DO carry actions:
 * YES is 0x80020428, which enters page 39 and installs the state machine, and
 * NO is 0x80020140, the ordinary resume. */
static const q2_menu_item k_save_prompt[] = {
    TEXT("DO YOU WANT TO SAVE?", 256,  98),
    /* YES is 0x80020428 — page 39 and the state machine. The port has no menu
     * action for that because it is not a page transition in this engine's
     * sense; the client opens the front end instead. */
    { "YES", 256, 124, Q2_ACT_NONE,   Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "NO",  256, 150, Q2_ACT_RESUME, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 }
};

/* 0x8009B0B4 */
static const q2_menu_item k_saving[] = {
    TEXT("LONGER LOAD",  256,  98),
    TEXT("SAVE MESSAGE", 256, 124),
    TEXT("HERE",         256, 150)
};

/* 0x8009B06C */
static const q2_menu_item k_load_message[] = {
    TEXT("LOAD MESSAGE", 256, 111),
    TEXT("HERE",         256, 137)
};

/* 0x8009B42C — this one IS a page (38), and is also reachable here. */
static const q2_menu_item k_no_controller[] = {
    TEXT("PLEASE INSERT",       256,  98),
    TEXT("CONTROLLER INTO",     256, 124),
    TEXT("CONTROLLER PORT XX",  256, 150)
};

#undef TEXT
#undef ANSWER

#define N(a) ((u8)(sizeof(a) / sizeof((a)[0])))

/*
 * `first` is where the navigable group begins. On a two-table screen that is
 * the second table's first record; on a pure-text screen it equals `count`,
 * which is how the engine expresses "nothing here is selectable".
 *
 * SAVE FILE is the exception worth naming: its heading is in the same table as
 * its four rows, and the rows are navigable, so `first` is 1.
 */
static const q2_menu_page k_screens[] = {
    { 0, NULL, k_select_slot,   N(k_select_slot),   N(k_select_slot),
      Q2_ACT_NONE, 0x8009B2C4u, 0 },
    { 0, NULL, k_not_formatted, N(k_not_formatted), 3,
      Q2_ACT_NONE, 0x8009B1A4u, 0x8009B204u },
    { 0, NULL, k_formatting,    N(k_formatting),    N(k_formatting),
      Q2_ACT_NONE, 0x8009B24Cu, 0 },
    { 0, NULL, k_save_file,     N(k_save_file),     1,
      Q2_ACT_NONE, 0x8009B114u, 0 },
    { 0, NULL, k_overwrite,     N(k_overwrite),     2,
      Q2_ACT_NONE, 0x8009B324u, 0x8009B36Cu },
    { 0, NULL, k_save_prompt,   N(k_save_prompt),   1,
      Q2_ACT_NONE, 0x8009B3B4u, 0x8009B3E4u },
    { 0, NULL, k_saving,        N(k_saving),        N(k_saving),
      Q2_ACT_NONE, 0x8009B0B4u, 0 },
    { 0, NULL, k_load_message,  N(k_load_message),  N(k_load_message),
      Q2_ACT_NONE, 0x8009B06Cu, 0 },
    { 0, NULL, k_no_controller, N(k_no_controller), N(k_no_controller),
      Q2_ACT_NONE, 0x8009B42Cu, 0 }
};

#undef N

const q2_menu_page *q2_mcard_pages(u32 *count)
{
    if (count)
        *count = (u32)(sizeof(k_screens) / sizeof(k_screens[0]));
    return k_screens;
}

const q2_menu_page *q2_mcard_page(q2_mcard_screen s)
{
    if (s <= Q2_MCARD_NONE || s >= Q2_MCARD_SCREEN_COUNT)
        return NULL;
    return &k_screens[(int)s - 1];
}

const char *q2_mcard_screen_name(q2_mcard_screen s)
{
    switch (s) {
    case Q2_MCARD_SELECT_SLOT:   return "SELECT SLOT";
    case Q2_MCARD_NOT_FORMATTED: return "NOT FORMATTED";
    case Q2_MCARD_FORMATTING:    return "FORMATTING";
    case Q2_MCARD_SAVE_FILE:     return "SAVE FILE";
    case Q2_MCARD_OVERWRITE:     return "OVERWRITE?";
    case Q2_MCARD_SAVE_PROMPT:   return "SAVE?";
    case Q2_MCARD_SAVING:        return "SAVING";
    case Q2_MCARD_LOAD_MESSAGE:  return "LOAD MESSAGE";
    case Q2_MCARD_NO_CONTROLLER: return "NO CONTROLLER";
    default:                     return "none";
    }
}

/* ------------------------------------------------------------------------- */
/* The state machine                                                          */
/* ------------------------------------------------------------------------- */
bool q2_mcard_state_live(int state)
{
    switch (state) {
    case Q2_MCARD_STATE_LIST:
    case Q2_MCARD_STATE_CHOICE_5:
    case Q2_MCARD_STATE_REPORT:
    case Q2_MCARD_STATE_ACCEPT_A:
    case Q2_MCARD_STATE_ACCEPT_B:
    case Q2_MCARD_STATE_CHOICE_19:
        return true;
    default:
        return false;
    }
}

q2_mcard_screen q2_mcard_screen_for_state(int state)
{
    /*
     * Only what the arms themselves settle. State 3 hands a chosen ROW to the
     * card code, and SAVE FILE is the only screen with a row list; state 13
     * formats a cheat level into its text, and LOAD MESSAGE is the only screen
     * whose text is runtime-composed. The two-way choices at 5 and 19 share a
     * shape with three different screens (NOT FORMATTED, OVERWRITE, SAVE?), and
     * which is which was NOT established — the nine table installers are
     * separate functions and their callers were not followed. Returning NONE
     * there is the honest answer; inventing a mapping would look identical in
     * the port and be wrong on the console.
     */
    switch (state) {
    case Q2_MCARD_STATE_LIST:   return Q2_MCARD_SAVE_FILE;
    case Q2_MCARD_STATE_REPORT: return Q2_MCARD_LOAD_MESSAGE;
    default:                    return Q2_MCARD_NONE;
    }
}

q2_mcard_screen q2_mcard_screen_for_state_port(int state)
{
    /*
     * The two the function above commits to are taken from it rather than
     * repeated, so the port cannot drift from the reading. The rest are the
     * port's own use of states the console's mapping does not settle:
     *
     *   5  the two-way choice, used for OVERWRITE? — the only one of that
     *      screen's three candidates a filesystem has any use for
     *   6  the busy state the choice's YES arm targets, which is what SAVING
     *      is for
     *
     * State 19 stays unclaimed: its remaining candidate is NOT FORMATTED, and
     * there is no card to format.
     */
    q2_mcard_screen s = q2_mcard_screen_for_state(state);

    if (s != Q2_MCARD_NONE)
        return s;

    switch (state) {
    case Q2_MCARD_STATE_CHOICE_5: return Q2_MCARD_OVERWRITE;
    case 6:                       return Q2_MCARD_SAVING;
    default:                      return Q2_MCARD_NONE;
    }
}

void q2_mcard_init(q2_mcard *m, const q2_mcard_host *host)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    m->host   = host;
    m->screen = Q2_MCARD_NONE;
    m->state  = 0;
}

void q2_mcard_set_name(q2_mcard *m, int slot, const char *name)
{
    if (!m || slot < 0 || slot >= Q2_MCARD_SLOTS)
        return;
    if (!name) {
        m->name[slot][0] = '\0';
        return;
    }
    strncpy(m->name[slot], name, Q2_MCARD_NAME_MAX - 1);
    m->name[slot][Q2_MCARD_NAME_MAX - 1] = '\0';
}

bool q2_mcard_advance(q2_mcard *m, u16 buttons)
{
    u16 released;
    bool accepted = false;

    if (!m)
        return false;

    released  = (u16)(m->pad_prev & ~buttons);
    m->pad_prev = buttons;
    m->fired  = false;

    if (m->host && m->host->poll)
        m->state = m->host->poll(m->host->user);

    m->screen = q2_mcard_screen_for_state(m->state);

    if (!q2_mcard_state_live(m->state))
        return false;

    /*
     * The gate every live arm opens with: the button must be UP now and must
     * have been released this frame. Spelled out inline in the original rather
     * than taken from the item's flag bit.
     */
    if ((buttons & Q2_PAD_CROSS) != 0)
        return false;
    if ((released & Q2_PAD_CROSS) == 0)
        return false;

    m->fired = true;

    switch (m->state) {
    case Q2_MCARD_STATE_LIST:
        /* 0x8001F140: the chosen row goes to the card code, then a transition
         * is requested. The row is `cursor - first`, already relative here. */
        if (m->host && m->host->choose)
            m->host->choose(m->host->user, m->cursor);
        if (m->host && m->host->request)
            m->host->request(m->host->user, m->state);
        break;

    case Q2_MCARD_STATE_CHOICE_5:
        /* 0x8001F790: row 1 goes on, row 0 goes back to the start. */
        if (m->host && m->host->request)
            m->host->request(m->host->user, m->cursor == 1 ? 6 : 1);
        break;

    case Q2_MCARD_STATE_CHOICE_19:
        /* 0x8001F718: row 1 goes on; row 0 falls through to the exit, which
         * is a return with no transition rather than a transition to zero. */
        if (m->cursor == 1 && m->host && m->host->request)
            m->host->request(m->host->user, 20);
        break;

    case Q2_MCARD_STATE_ACCEPT_A:
    case Q2_MCARD_STATE_ACCEPT_B:
        /* 0x8001F0A4: apply and leave. On the console that is 0x8001C698 —
         * the same game-variable application the GAME VARIABLES page uses —
         * followed by 0x8001E1A4. The caller owns both here. */
        accepted = true;
        break;

    default:
        break;
    }

    return accepted;
}
