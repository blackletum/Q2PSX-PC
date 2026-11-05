#include "memcard.h"

#include <stdio.h>
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
    case Q2_MCARD_STATE_CARDS:
    case Q2_MCARD_STATE_FORMAT_ASK:
    case Q2_MCARD_STATE_FILES:
    case Q2_MCARD_STATE_ACCEPT_A:
    case Q2_MCARD_STATE_DONE:
    case Q2_MCARD_STATE_OVERWRITE:
        return true;
    default:
        return false;
    }
}

/*
 * InGame_PrintMemCardMessage's jump table, 0x800AB6DC, `status - 1` in
 * 0..21. Each entry is the arm's first instruction; the table each arm hands
 * to FrontEndCreateMenu (engine+0x200) is the screen.
 */
static const u8 k_status_screen[Q2_MCARD_STATUS_MAX] = {
    Q2_MCARD_LOAD_MESSAGE,   /*  1  0x8001E52C  0x8009B06C            */
    Q2_MCARD_LOAD_MESSAGE,   /*  2  0x8001E844                        */
    Q2_MCARD_SELECT_SLOT,    /*  3  0x8001E5E0  0x8009B2C4 + cards    */
    Q2_MCARD_LOAD_MESSAGE,   /*  4  0x8001E7B4                        */
    Q2_MCARD_NOT_FORMATTED,  /*  5  0x8001EBDC  0x8009B1A4 + B204     */
    Q2_MCARD_FORMATTING,     /*  6  0x8001EC1C  0x8009B24C            */
    Q2_MCARD_NONE,           /*  7  0x8001EFB0  installs nothing      */
    Q2_MCARD_LOAD_MESSAGE,   /*  8  0x8001EC7C                        */
    Q2_MCARD_LOAD_MESSAGE,   /*  9  0x8001E8D4                        */
    Q2_MCARD_LOAD_MESSAGE,   /* 10  0x8001E7B4, as 4                  */
    Q2_MCARD_LOAD_MESSAGE,   /* 11  0x8001E7B4, as 4                  */
    Q2_MCARD_SAVING,         /* 12  0x8001ED2C  0x8009B0B4            */
    Q2_MCARD_SAVE_FILE,      /* 13  0x8001EEC0  -> 0x8001D84C, B114   */
    Q2_MCARD_FORMATTING,     /* 14  0x8001EED0  0x8009B24C            */
    Q2_MCARD_FORMATTING,     /* 15  0x8001EED0                        */
    Q2_MCARD_LOAD_MESSAGE,   /* 16  0x8001EA28                        */
    Q2_MCARD_LOAD_MESSAGE,   /* 17  0x8001E964                        */
    Q2_MCARD_SAVING,         /* 18  0x8001EDF4  0x8009B0B4            */
    Q2_MCARD_OVERWRITE,      /* 19  0x8001E4D8  0x8009B324 + B36C     */
    Q2_MCARD_FORMATTING,     /* 20  0x8001EED0                        */
    Q2_MCARD_FORMATTING,     /* 21  0x8001EED0                        */
    Q2_MCARD_FORMATTING      /* 22  0x8001EED0                        */
};

q2_mcard_screen q2_mcard_screen_for_state(int state)
{
    if (state < 1 || state > Q2_MCARD_STATUS_MAX)
        return Q2_MCARD_NONE;
    return (q2_mcard_screen)k_status_screen[state - 1];
}

q2_mcard_screen q2_mcard_screen_for_state_port(int state)
{
    /* The console's map is complete, so the port's flow walks it as it is. */
    return q2_mcard_screen_for_state(state);
}

static int put(char (*text)[Q2_MENU_TEXT_MAX], int rows, int row,
               const char *s)
{
    if (row < rows)
        snprintf(text[row], Q2_MENU_TEXT_MAX, "%s", s);
    return row + 1;
}

int q2_mcard_compose(int state, const q2_mcard_context *ctx,
                     char (*text)[Q2_MENU_TEXT_MAX], int rows)
{
    bool saving = ctx && ctx->saving;
    bool single = ctx && ctx->single_player;
    int  i, n = 0;

    if (!text || rows <= 0)
        return 0;
    for (i = 0; i < rows; i++)
        text[i][0] = '\0';

    switch (state) {
    case 1:                                    /* 0x8001E59C / 0x8001E5A4 */
        n = put(text, rows, 0, "CHECKING FOR");
        n = put(text, rows, 1, "MEMORY CARDS");
        break;
    case 2:                                    /* 0x8001E890 / 0x8001E898 */
        n = put(text, rows, 0, "NO MEMORY");
        n = put(text, rows, 1, "CARDS FOUND");
        break;
    case 4: case 10: case 11:                  /* 0x8001E800 / 0x8001E808 */
        n = put(text, rows, 0, "CHECKING FOR");
        n = put(text, rows, 1, "QUAKE 2 FILES");
        break;
    case 6:                                    /* 0x8001EC68 */
        n = put(text, rows, 0, "FORMATTING");
        break;
    case 8:                                    /* 0x8001ECC8 / 0x8001ECD0 */
        n = put(text, rows, 0, "MEMORY CARD");
        n = put(text, rows, 1, "FORMAT COMPLETE");
        break;
    case 9:                                    /* 0x8001E920 / 0x8001E928 */
        n = put(text, rows, 0, "MEMORY CARD");
        n = put(text, rows, 1, "FORMAT FAILED");
        break;
    case 12:                                   /* 0x8001ED64..0x8001EDE4 */
        n = put(text, rows, 0, "NO QUAKE 2");
        n = put(text, rows, 1, single ? "SINGLE PLAYER" : "MULTIPLAYER");
        n = put(text, rows, 2, "FILES FOUND");
        break;
    case 13:                                   /* 0x8001D88C..0x8001D90C */
        n = put(text, rows, 0, saving ? "SAVE FILE"
                             : single ? "LOAD SINGLE" : "LOAD MP SETUP");
        break;
    case 14: case 15: case 20: case 21: case 22:   /* 0x8001EF08..0x8001EF28 */
        n = put(text, rows, 0, saving ? "SAVING TO" : "LOADING FROM");
        break;
    case 16:                                   /* 0x8001EA48..0x8001EB74 */
        if (saving) {
            n = put(text, rows, 0, single ? "GAME" : "MULTIPLAYER");
            n = put(text, rows, 1, single ? "SAVED" : "DATA SAVED");
        } else {
            n = put(text, rows, 0, single ? "STARTING" : "MULTIPLAYER");
            n = put(text, rows, 1, single ? "GAME" : "CONFIG LOADED");
        }
        break;
    case 17:                                   /* 0x8001E9B0..0x8001E9DC */
        n = put(text, rows, 0, "MEMORY CARD");
        n = put(text, rows, 1, saving ? "SAVE FAILED" : "LOAD FAILED");
        break;
    case 18:                                   /* 0x8001EE40..0x8001EE64 */
        n = put(text, rows, 0, "INSUFFICIENT FREE BLOCKS");
        n = put(text, rows, 1, "PLEASE INSERT ANOTHER");
        n = put(text, rows, 2, "MEMORY CARD");
        break;
    default:
        /* 3, 5, 7 and 19 write nothing over their tables. */
        break;
    }
    return n < rows ? n : rows;
}

void q2_mcard_prompt_rows(int state, bool front_end, int y[2])
{
    if (!y)
        return;
    y[0] = y[1] = 260;                         /* 0x8001E400..0x8001E424 */

    switch (state) {
    case Q2_MCARD_STATE_CARDS:
        y[0] = y[1] = front_end ? 216 : 220;   /* 0x8001E428..0x8001E488 */
        break;
    case Q2_MCARD_STATE_FILES:
        y[1] = front_end ? 216 : 220;
        y[0] = 220;                            /* 0x8001DD0C, last word */
        break;
    case 8:
    case Q2_MCARD_STATE_DONE:
        y[0] = 220;                            /* 0x8001ED10, 0x8001EB8C */
        break;
    case Q2_MCARD_STATE_OVERWRITE:
        y[0] = y[1] = 220;                     /* 0x8001E510, 0x8001ED14 */
        break;
    default:
        break;
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
    case Q2_MCARD_STATE_CARDS:
        /* 0x8001F178..0x8001F198: `cursor - first` to SetActiveCard
         * (engine+0x268), then SetStatus(4) — CHECKING FOR QUAKE 2 FILES. */
        if (m->host && m->host->choose)
            m->host->choose(m->host->user, m->cursor);
        if (m->host && m->host->request)
            m->host->request(m->host->user, 4);
        break;

    case Q2_MCARD_STATE_FILES:
        /* 0x8001F67C..0x8001F714: `cursor - first` to SetActiveFile
         * (engine+0x26C), then 21 / 22 / 19 by the save flag, the row and the
         * free blocks. The host owns all three facts here, so it is told the
         * row was chosen and decides (memcard.h). */
        if (m->host && m->host->choose)
            m->host->choose(m->host->user, m->cursor);
        if (m->host && m->host->request)
            m->host->request(m->host->user, Q2_MCARD_STATE_FILES);
        break;

    case Q2_MCARD_STATE_FORMAT_ASK:
        /* 0x8001F790: YES (row 1) formats, NO (row 0) starts again. Any other
         * row is a return with no transition (0x8001F7E0). */
        if (m->host && m->host->request) {
            if (m->cursor == 1)
                m->host->request(m->host->user, Q2_MCARD_STATE_FORMATTING);
            else if (m->cursor == 0)
                m->host->request(m->host->user, 1);
        }
        break;

    case Q2_MCARD_STATE_OVERWRITE:
        /* 0x8001F718: YES (row 1) writes over the file (20); NO (row 0) goes
         * back to the file window (13, 0x8001F770..0x8001F780). */
        if (m->host && m->host->request) {
            if (m->cursor == 1)
                m->host->request(m->host->user, Q2_MCARD_STATE_SAVE_OVER);
            else if (m->cursor == 0)
                m->host->request(m->host->user, Q2_MCARD_STATE_FILES);
        }
        break;

    case Q2_MCARD_STATE_ACCEPT_A:
    case Q2_MCARD_STATE_DONE:
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

bool q2_mcard_save_prompt_due(u32 level_change, u32 *latch,
                              bool deathmatch, int exit_code)
{
    if (!latch)
        return false;

    if (level_change == *latch)          /* 0x8003E70C */
        return false;
    if (deathmatch)                      /* 0x8003E720, before the latch */
        return false;

    *latch = level_change;               /* 0x8003E728 */

    /* 0x8003E738 / 0x8003E740. Both arms fall into 0x8003E748 together. */
    return exit_code == 2 || exit_code == 4;
}
