/*
 * pad.h — the pad, and the nine control schemes it is read through.
 *
 * `0x80019154`. This is the function the player's own frame calls at
 * `0x8003A4A4` to fill the 12-byte input record `0x8003A1C8` then consumes, and
 * until now the port had no equivalent: the client invented a mapping and the
 * simulation's `look_scheme` field had nothing to set it.
 *
 * ---------------------------------------------------------------------------
 * Why this is not "just input handling"
 * ---------------------------------------------------------------------------
 * Three things decided in here change how the player MOVES, so they belong with
 * the movement code rather than with the host's key handling:
 *
 *   - Full digital deflection is 127, not 128. The wish velocity is
 *     `(maxspeed * axis) >> 7`, so 128 would reach exactly maxspeed and 127
 *     reaches 2778 of 2800. A host that feeds 128 makes the player 0.8% faster
 *     than the console on every scheme.
 *   - The scheme decides whether the look rate is EASED or SET. `0x8003A670`
 *     branches on `style < 6`, and the six mouse and stick schemes take the
 *     direct path while the three STANDARD ones ease. (This port had the
 *     comparison the other way round, which made the analogue schemes glide and
 *     the digital ones snap — exactly backwards.)
 *   - Jump and swim-up are THE SAME BUTTON. `0x80019A9C` writes bit 22 from its
 *     press edge and bit 21 from it being held, so tapping jumps and holding
 *     swims up. Binding them to two keys is a mechanic the console does not
 *     have.
 *
 * ---------------------------------------------------------------------------
 * The shape of it
 * ---------------------------------------------------------------------------
 * The function is a jump table on the style (`0x800AB53C`, nine entries) whose
 * arms differ only in which pad bits and which analogue bytes feed the four
 * output axes. Every arm then leaves four PAD MASKS in t2..t5 and falls into one
 * shared tail at `0x80019A04` that turns each mask into up to three bits of the
 * output word: a press edge, a held bit, and a held-and-was-held bit.
 *
 * So there are really only two things to transcribe — a per-style axis table and
 * a per-style button table — and that is what this module is.
 *
 * ---------------------------------------------------------------------------
 * Where the settings live
 * ---------------------------------------------------------------------------
 * Per player, in a 34-byte record at `0x800B32B4`:
 *
 *     +0x02   style        the nine below, read at 0x8003A668 and 0x80019200
 *     +0x0A   swap Y       SWAP Y AXIS on the controller page
 *     +0x0E   mouse speed  the look scale, default 64
 *
 * `menu.h` records the style as `0x800B32D2`, which is this record's +30 rather
 * than its +2. Both fields exist — the menu page edits one and the pad reads the
 * other — and this module deliberately reads what `0x80019154` reads.
 *
 * The look scale is `(mouse_speed + 32) >> 4` and it applies to the MOUSE and
 * RIGHT STICK schemes only. LEFT STICK and BOTH STICKS take the raw signed stick
 * byte, so their look axes are bounded by +-127 and the others' are not.
 */
#ifndef Q2PSX_PAD_H
#define Q2PSX_PAD_H

#include "q2psx.h"
#include "sim.h"

/* ------------------------------------------------------------------------- */
/* The pad word                                                               */
/* ------------------------------------------------------------------------- */
/*
 * ACTIVE HIGH. The hardware reports these inverted; whatever fills the pad
 * record at 0x800D5EDC+0x0C has already flipped them, because every arm of
 * 0x80019154 tests a set bit for "pressed".
 */
#define Q2_PAD_SELECT   0x0001u
#define Q2_PAD_L3       0x0002u
#define Q2_PAD_R3       0x0004u
#define Q2_PAD_START    0x0008u
#define Q2_PAD_UP       0x0010u
#define Q2_PAD_RIGHT    0x0020u
#define Q2_PAD_DOWN     0x0040u
#define Q2_PAD_LEFT     0x0080u
#define Q2_PAD_L2       0x0100u
#define Q2_PAD_R2       0x0200u
#define Q2_PAD_L1       0x0400u
#define Q2_PAD_R1       0x0800u
#define Q2_PAD_TRIANGLE 0x1000u
#define Q2_PAD_CIRCLE   0x2000u
#define Q2_PAD_CROSS    0x4000u
#define Q2_PAD_SQUARE   0x8000u

/*
 * The two mouse buttons arrive in the same word. Schemes 0-2 read L3 and R3,
 * which is where the mouse's buttons are merged: RIGHT MOUSE fires on one and
 * jumps on the other, RIGHT MOUSE 2 is the same pair swapped.
 */
#define Q2_PAD_MOUSE_L  Q2_PAD_L3
#define Q2_PAD_MOUSE_R  Q2_PAD_R3

/* ------------------------------------------------------------------------- */
/* The nine styles                                                            */
/* ------------------------------------------------------------------------- */
/*
 * The order the CONTROLLER row cycles them, which is the order their names sit
 * in the string pool at 0x800AB1D8. `Q2_PAD_STYLE_STANDARD_A` is the default
 * (0x8001BDA8 writes 6).
 */
typedef enum q2_pad_style {
    Q2_PAD_STYLE_RIGHT_MOUSE = 0,
    Q2_PAD_STYLE_RIGHT_MOUSE2,
    Q2_PAD_STYLE_HUNTER_MOUSE,
    Q2_PAD_STYLE_RIGHT_STICK,
    Q2_PAD_STYLE_LEFT_STICK,
    Q2_PAD_STYLE_BOTH_STICKS,
    Q2_PAD_STYLE_STANDARD_A,
    Q2_PAD_STYLE_STANDARD_B,
    Q2_PAD_STYLE_STANDARD_C,
    Q2_PAD_STYLE_COUNT
} q2_pad_style;

const char *q2_pad_style_name(int style);

/*
 * 0x8003A670: styles below this SET the look rate, styles from it up EASE it.
 * The same constant is `Q2_LOOK_SCHEME_ANALOGUE` in worldscale.h, and the sense
 * of the comparison there is the one that matters.
 */
#define Q2_PAD_STYLE_EASED_FROM  6

/* ------------------------------------------------------------------------- */
/* Full deflection                                                            */
/* ------------------------------------------------------------------------- */
/*
 * 127, at every one of the eighteen `addiu v0, zero, 127 / -127` sites. Not 128:
 * see the header comment. Q2_INPUT_FULL in sim.h is the value the WISH
 * arithmetic saturates at, which is a different number and deliberately so.
 */
#define Q2_PAD_FULL  127

/* ------------------------------------------------------------------------- */
/* What the pad delivers                                                      */
/* ------------------------------------------------------------------------- */
/*
 * The per-player record at 0x800D5EDC + player*0x110, cut down to the fields
 * 0x80019154 actually reads.
 *
 * The four analogue bytes are read as SIGNED by every scheme (`lb`, or `lbu`
 * followed by `sll 24 / sra 24`), so a centred stick is 0 here and not 128.
 * Whatever fills the record has already re-centred them.
 *
 * Which physical stick is which follows from the schemes that read them: LEFT
 * STICK takes its look axes from the first pair and RIGHT STICK from the
 * second, so the first pair is the left stick and the second the right. The
 * mouse's delta arrives in the first pair as well, which is why the three mouse
 * schemes read the same offsets LEFT STICK does.
 */
typedef struct q2_pad_state {
    u32 buttons;   /* +0x0C, this frame                                      */
    u32 prev;      /* +0x10, last frame — the press edges come from the pair */

    s8  lx, ly;    /* +0x14, +0x15: left stick, or the mouse's delta         */
    s8  rx, ry;    /* +0x16, +0x17: right stick                              */
} q2_pad_state;

/* The three fields of the settings record this reads. */
typedef struct q2_pad_config {
    int style;        /* q2_pad_style, +0x02                                 */
    int swap_y;       /* SWAP Y AXIS, +0x0A                                  */
    int mouse_speed;  /* +0x0E, default 64                                   */
} q2_pad_config;

/* ------------------------------------------------------------------------- */
/* Reading it                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * 0x80019154 — fill `out` from `pad` under `cfg`.
 *
 * `out` is overwritten completely, including the button word: the original
 * clears bits 28..31 at 0x800191D8 before any arm runs and every derived bit is
 * assigned rather than OR'd, so nothing carries over from the caller's previous
 * frame. The one thing it does NOT set is `q2_input.attack`, which is left to
 * the caller because it is bit 25 of the word — see Q2_BTN_ATTACK.
 */
void q2_pad_read(const q2_pad_state *pad, const q2_pad_config *cfg,
                 q2_input *out);

/*
 * The default configuration, from 0x8001BDA8: STANDARD A, no swap, speed 64.
 */
void q2_pad_config_default(q2_pad_config *cfg);

/* ------------------------------------------------------------------------- */
/* The same table, read the other way round                                   */
/* ------------------------------------------------------------------------- */
/*
 * Which pad bits a style reads for each thing the player can ASK FOR.
 *
 * A host that maps its keys straight to pad BUTTONS has silently committed to
 * one style. The client's keyboard was STANDARD A's arm written out by hand, so
 * selecting any other style turned the strafe keys into weapon switches and the
 * look keys into nothing — the keys had not moved, but what they meant had.
 * This is the per-style table read backwards, so ONE key mapping serves all
 * nine and a style change moves the meaning rather than breaking it.
 *
 * A field is zero where the style has no button for that meaning, and those
 * blanks are not gaps in the transcription — they are what the style IS. The
 * three mouse and three stick styles have no turn or look buttons at all
 * because their look comes from an analogue pair, and BOTH STICKS has no
 * movement buttons for the same reason. A host that wants those keys to keep
 * working feeds the AXIS instead; q2_pad_style_look says which one.
 */
typedef struct q2_pad_bindings {
    u32 forward, back;
    u32 strafe_left, strafe_right;
    u32 turn_left, turn_right;
    u32 look_up, look_down;
    u32 fire, jump;
    u32 weapon_next, weapon_prev;
} q2_pad_bindings;

/* False, leaving `out` zeroed, for a style outside the table. */
bool q2_pad_style_bindings(int style, q2_pad_bindings *out);

/*
 * Where a style's look comes from, which is the one thing a host has to know
 * before it can decide what to do with a pointing device.
 */
typedef enum q2_pad_look_src {
    Q2_PAD_LOOK_BUTTONS = 0,  /* the two look bits above                     */
    Q2_PAD_LOOK_MOUSE,        /* lx/ly, scaled by MOUSE SPEED                */
    Q2_PAD_LOOK_LEFT_STICK,   /* lx/ly, raw signed bytes                     */
    Q2_PAD_LOOK_RIGHT_STICK   /* rx/ry, raw signed bytes                     */
} q2_pad_look_src;

/*
 * BOTH STICKS reports LEFT because that is where its yaw comes from; its pitch
 * is the right stick's, which is the one place this summary is coarser than
 * the arm it summarises. Nothing that only wants to know "is the look an axis,
 * and is it scaled" is affected.
 */
q2_pad_look_src q2_pad_style_look(int style);

#endif /* Q2PSX_PAD_H */
