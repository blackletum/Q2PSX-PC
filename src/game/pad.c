#include "pad.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The style table                                                            */
/* ------------------------------------------------------------------------- */
/*
 * Nine arms of one jump table, reduced to the four things that actually differ.
 *
 * The reduction is safe because every arm has the same shape: assign four axes,
 * leave four pad masks in t2..t5, fall into the shared tail. Nothing in an arm
 * reads state, and nothing in the tail reads the style. The one exception is
 * BOTH STICKS, whose two extra behaviours are called out where they happen.
 */

/* Where a look axis comes from and how it is scaled. */
typedef enum look_src {
    LOOK_MOUSE,   /* left pair,  scaled by (mouse_speed + 32) >> 4 */
    LOOK_LEFT,    /* left pair,  raw signed byte                   */
    LOOK_RIGHT,   /* right pair, raw signed byte                   */
    LOOK_BUTTONS  /* two pad bits, +-127                           */
} look_src;

typedef struct style_def {
    look_src look;

    /*
     * Which way SWAP Y AXIS points. The mouse schemes negate the pitch byte
     * when the setting is ON; the two stick schemes that read a stick pair
     * negate it when the setting is OFF (0x80019568 and 0x800194B4 are `bne`
     * where 0x8001924C is `beq`). So LEFT STICK and RIGHT STICK are inverted
     * out of the box relative to everything else, and that is the console's
     * behaviour rather than a transcription slip.
     */
    bool invert_when_swap_off;

    /* Digital axis sources. 0 means the axis is not driven by buttons. */
    u32 fwd_pos, fwd_neg;     /* forward: +127 / -127                        */
    u32 side_neg, side_pos;   /* side:    -127 / +127                        */
    u32 yaw_neg, yaw_pos;     /* yaw, when look == LOOK_BUTTONS              */
    u32 look_down, look_up;   /* pitch -127 (bit 29) / +127 (bit 30)         */

    /* The four masks the shared tail turns into derived bits. */
    u32 fire, jump, wnext, wprev;
} style_def;

#define NO 0u

static const style_def STYLES[Q2_PAD_STYLE_COUNT] = {
/* 0 RIGHT MOUSE    0x80019224 */
{LOOK_MOUSE, false,
 Q2_PAD_UP, Q2_PAD_DOWN, Q2_PAD_LEFT, Q2_PAD_RIGHT, NO, NO, NO, NO,
 Q2_PAD_MOUSE_L, Q2_PAD_MOUSE_R, Q2_PAD_L1, Q2_PAD_L2},

/* 1 RIGHT MOUSE 2  0x800192F4 — the same pad, the two mouse buttons swapped */
{LOOK_MOUSE, false,
 Q2_PAD_UP, Q2_PAD_DOWN, Q2_PAD_LEFT, Q2_PAD_RIGHT, NO, NO, NO, NO,
 Q2_PAD_MOUSE_R, Q2_PAD_MOUSE_L, Q2_PAD_L1, Q2_PAD_L2},

/* 2 HUNTER MOUSE   0x800193C4 — walk on the mouse's right button, jump on UP */
{LOOK_MOUSE, false,
 Q2_PAD_MOUSE_R, Q2_PAD_DOWN, Q2_PAD_LEFT, Q2_PAD_RIGHT, NO, NO, NO, NO,
 Q2_PAD_MOUSE_L, Q2_PAD_UP, Q2_PAD_L1, Q2_PAD_L2},

/* 3 RIGHT STICK    0x80019494 */
{LOOK_RIGHT, true,
 Q2_PAD_UP, Q2_PAD_DOWN, Q2_PAD_LEFT, Q2_PAD_RIGHT, NO, NO, NO, NO,
 Q2_PAD_L1, Q2_PAD_L2, Q2_PAD_R1, Q2_PAD_R2},

/* 4 LEFT STICK     0x80019548 — the face buttons move, the stick looks */
{LOOK_LEFT, true,
 Q2_PAD_TRIANGLE, Q2_PAD_CROSS, Q2_PAD_SQUARE, Q2_PAD_CIRCLE, NO, NO, NO, NO,
 Q2_PAD_R1, Q2_PAD_R2, Q2_PAD_L1, Q2_PAD_L2},

/* 5 BOTH STICKS    0x800195EC — every axis analogue; see q2_pad_read */
{LOOK_LEFT, false,
 NO, NO, NO, NO, NO, NO, NO, NO,
 Q2_PAD_R1, Q2_PAD_R2, Q2_PAD_L1, Q2_PAD_L2},

/* 6 STANDARD A     0x800196E8 */
{LOOK_BUTTONS, false,
 Q2_PAD_UP, Q2_PAD_DOWN, Q2_PAD_L2, Q2_PAD_R2, Q2_PAD_LEFT, Q2_PAD_RIGHT,
 Q2_PAD_R1, Q2_PAD_L1,
 Q2_PAD_CROSS, Q2_PAD_SQUARE, Q2_PAD_TRIANGLE, Q2_PAD_CIRCLE},

/* 7 STANDARD B     0x800197D0 — look on the face buttons instead of the
 *                               shoulders, so the shoulders cycle weapons */
{LOOK_BUTTONS, false,
 Q2_PAD_UP, Q2_PAD_DOWN, Q2_PAD_L2, Q2_PAD_R2, Q2_PAD_LEFT, Q2_PAD_RIGHT,
 Q2_PAD_TRIANGLE, Q2_PAD_CIRCLE,
 Q2_PAD_CROSS, Q2_PAD_SQUARE, Q2_PAD_R1, Q2_PAD_L1},

/* 8 STANDARD C     0x800198EC — turn on the shoulders, strafe on the pad */
{LOOK_BUTTONS, false,
 Q2_PAD_UP, Q2_PAD_DOWN, Q2_PAD_LEFT, Q2_PAD_RIGHT, Q2_PAD_L2, Q2_PAD_R2,
 Q2_PAD_R1, Q2_PAD_L1,
 Q2_PAD_CROSS, Q2_PAD_SQUARE, Q2_PAD_TRIANGLE, Q2_PAD_CIRCLE}
};

const char *q2_pad_style_name(int style)
{
    static const char *const names[Q2_PAD_STYLE_COUNT] = {
        "RIGHT MOUSE", "RIGHT MOUSE 2", "HUNTER MOUSE", "RIGHT STICK",
        "LEFT STICK",  "BOTH STICKS",   "STANDARD A",   "STANDARD B",
        "STANDARD C"
    };

    if (style < 0 || style >= Q2_PAD_STYLE_COUNT)
        return names[Q2_PAD_STYLE_STANDARD_A];
    return names[style];
}

void q2_pad_config_default(q2_pad_config *cfg)
{
    if (!cfg)
        return;
    cfg->style       = Q2_PAD_STYLE_STANDARD_A;   /* 0x8001BDA8 writes 6 */
    cfg->swap_y      = 0;
    cfg->mouse_speed = 64;
}

/* ------------------------------------------------------------------------- */
/* The table, read backwards — see pad.h                                      */
/* ------------------------------------------------------------------------- */
bool q2_pad_style_bindings(int style, q2_pad_bindings *out)
{
    const style_def *s;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));

    if (style < 0 || style >= Q2_PAD_STYLE_COUNT)
        return false;

    s = &STYLES[style];

    /*
     * Which of `fwd_pos`/`fwd_neg` is forward, and which of `side_pos`/
     * `side_neg` is right, is decided by `axis()` and not by the field names:
     * it returns +127 for the `pos` mask, and the wish arithmetic takes a
     * positive side axis as a step to the RIGHT.
     */
    out->forward      = s->fwd_pos;
    out->back         = s->fwd_neg;
    out->strafe_left  = s->side_neg;
    out->strafe_right = s->side_pos;

    /* Zero on every style that does not look with buttons — the six below
     * Q2_PAD_STYLE_EASED_FROM. */
    out->turn_left    = s->yaw_neg;
    out->turn_right   = s->yaw_pos;
    out->look_up      = s->look_up;
    out->look_down    = s->look_down;

    out->fire         = s->fire;
    out->jump         = s->jump;
    out->weapon_next  = s->wnext;
    out->weapon_prev  = s->wprev;

    return true;
}

q2_pad_look_src q2_pad_style_look(int style)
{
    if (style < 0 || style >= Q2_PAD_STYLE_COUNT)
        return Q2_PAD_LOOK_BUTTONS;

    switch (STYLES[style].look) {
    case LOOK_MOUSE: return Q2_PAD_LOOK_MOUSE;
    case LOOK_LEFT:  return Q2_PAD_LOOK_LEFT_STICK;
    case LOOK_RIGHT: return Q2_PAD_LOOK_RIGHT_STICK;
    default:         return Q2_PAD_LOOK_BUTTONS;
    }
}

/* ------------------------------------------------------------------------- */
/* Pieces of one arm                                                          */
/* ------------------------------------------------------------------------- */

/*
 * A digital axis. `pos` wins over `neg` because the original tests it first and
 * branches away, so pressing both directions at once walks forwards.
 */
static s16 axis(u32 buttons, u32 pos, u32 neg)
{
    if (pos && (buttons & pos))
        return  Q2_PAD_FULL;
    if (neg && (buttons & neg))
        return -Q2_PAD_FULL;
    return 0;
}

/*
 * The mouse scale, 0x80019230. `(byte * (speed + 32)) >> 4` — computed in 32
 * bits and stored as a halfword, so a fast mouse wraps rather than saturating.
 *
 * The pitch term uses `srl` where the yaw term uses `sra`. That looks like it
 * matters and does not: both feed a 16-bit store and the low sixteen bits of a
 * right shift by four are the same either way.
 */
static s16 scaled(s32 byte, int mouse_speed)
{
    return (s16)((byte * (mouse_speed + 32)) >> 4);
}

/* ------------------------------------------------------------------------- */
/* The shared tail — 0x80019A04                                               */
/* ------------------------------------------------------------------------- */
/*
 * Four masks, three derived bits each, and the bit numbers are not adjacent
 * because the tail assigns them in the order it happens to hold them:
 *
 *     fire   ->  23 pressed   25 held   24 held-and-was-held
 *     jump   ->  22 pressed   21 held
 *     wnext  ->  26 pressed
 *     wprev  ->  27 pressed
 *
 * `jump` producing both a press edge (22, the jump) and a held bit (21, swim
 * up) out of ONE mask is the load-bearing part: those two are the same button.
 */
static u32 derive(u32 out, u32 cur, u32 prev, u32 mask,
                  int bit_press, int bit_held, int bit_repeat)
{
    bool now  = mask != 0 && (cur  & mask) != 0;
    bool was  = mask != 0 && (prev & mask) != 0;

    if (bit_press >= 0) {
        out &= ~(1u << bit_press);
        if (now && !was)
            out |= 1u << bit_press;
    }
    if (bit_held >= 0) {
        out &= ~(1u << bit_held);
        if (now)
            out |= 1u << bit_held;
    }
    if (bit_repeat >= 0) {
        /* 0x80019A6C: the repeat bit is the HELD bit gated on the button also
         * having been down last frame, so it is clear on the frame of the
         * press and set from the second frame on. */
        out &= ~(1u << bit_repeat);
        if (was && now)
            out |= 1u << bit_repeat;
    }
    return out;
}

/* ------------------------------------------------------------------------- */
void q2_pad_roll(q2_pad_state *pad, u32 buttons)
{
    if (!pad)
        return;
    pad->prev    = pad->buttons;
    pad->buttons = buttons;
}

void q2_pad_roll_resume(q2_pad_state *pad, u32 buttons, u32 held)
{
    if (!pad)
        return;
    /*
     * The previous word is what is DOWN, not what was seen: `derive` raises a
     * press edge on `now && !was`, so seeding `was` with the held set is
     * exactly "nothing carried in counts as a press". Everything else about
     * the step is ordinary.
     */
    pad->prev    = held;
    pad->buttons = buttons;
}

void q2_pad_read(const q2_pad_state *pad, const q2_pad_config *cfg,
                 q2_input *out)
{
    const style_def *s;
    q2_pad_config    def;
    int              style;
    bool             invert;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    if (!pad)
        return;

    if (!cfg) {
        q2_pad_config_default(&def);
        cfg = &def;
    }

    /* 0x80019190: START's press edge, bit 28. Set BEFORE the style is even
     * looked at, which is why pausing works on a style the table does not
     * have — and why the out-of-range return below comes after it. */
    if ((pad->buttons & Q2_PAD_START) && !(pad->prev & Q2_PAD_START))
        out->buttons |= Q2_BTN_PAUSE;

    /*
     * 0x800191FC: `sltiu v0, v1, 9`. A style outside the table skips every arm
     * and reaches the tail with t2..t5 never assigned. The original leaves them
     * holding whatever the caller's registers held, which is not a behaviour
     * worth reproducing; this treats them as zero, so an out-of-range style is
     * a dead pad rather than a fallback to STANDARD A.
     */
    style = cfg->style;
    if (style < 0 || style >= Q2_PAD_STYLE_COUNT)
        return;

    s = &STYLES[style];

    /*
     * SWAP Y AXIS. `invert` is what the arm's own branch resolves to, not the
     * setting: see style_def.invert_when_swap_off.
     */
    invert = s->invert_when_swap_off ? (cfg->swap_y == 0) : (cfg->swap_y != 0);

    /* ---- the look axes ---------------------------------------------- */
    switch (s->look) {
    case LOOK_MOUSE:
        out->yaw   = scaled(pad->lx, cfg->mouse_speed);
        out->pitch = scaled(invert ? -(s32)pad->ly : pad->ly, cfg->mouse_speed);
        break;

    case LOOK_LEFT:
        out->yaw   = pad->lx;
        out->pitch = (s16)(invert ? -(s32)pad->ly : pad->ly);
        break;

    case LOOK_RIGHT:
        out->yaw   = pad->rx;
        out->pitch = (s16)(invert ? -(s32)pad->ry : pad->ry);
        break;

    case LOOK_BUTTONS:
        out->yaw = axis(pad->buttons, s->yaw_pos, s->yaw_neg);

        /*
         * 0x8001978C. The two look buttons are tested independently and both
         * write the pitch, so holding them together leaves the SECOND one's
         * value — and, far more importantly, leaves both bit 29 and bit 30
         * set. That pair is the chord the view recentre watches for.
         */
        out->pitch = 0;
        if (pad->buttons & s->look_down) {
            out->pitch    = -Q2_PAD_FULL;
            out->buttons |= Q2_BTN_LOOK_DOWN;
        }
        if (pad->buttons & s->look_up) {
            out->pitch    =  Q2_PAD_FULL;
            out->buttons |= Q2_BTN_LOOK_UP;
        }
        break;
    }

    /* ---- the movement axes ------------------------------------------ */
    if (style == Q2_PAD_STYLE_BOTH_STICKS) {
        /*
         * 0x800195EC. The only fully analogue arm: the left stick turns and
         * walks, the right stick strafes and looks.
         */
        out->yaw     = pad->lx;
        out->pitch   = (s16)(invert ? -(s32)pad->ry : pad->ry);
        out->side    = pad->rx;
        out->forward = (s16)(-(s32)pad->ly);

        /* 0x8001964C: the "moving" bit comes from a DEADBAND on the stick,
         * not from a button — 101 of 127, so it takes four fifths of full
         * travel before the frame counts as walking. */
        {
            s32 f = out->forward < 0 ? -(s32)out->forward : out->forward;
            if (f >= 101)
                out->buttons |= Q2_BTN_MOVING;
        }

        /*
         * 0x80019678: this arm has no look buttons, so R3's press edge stands
         * in for the chord — one tap sets both bits at once and anything else
         * clears them. Which is why BOTH STICKS recentres on a tap where the
         * digital styles need two shoulders held.
         */
        if ((pad->buttons & Q2_PAD_R3) && !(pad->prev & Q2_PAD_R3))
            out->buttons |= Q2_BTN_LOOK_DOWN | Q2_BTN_LOOK_UP;
    } else {
        out->forward = axis(pad->buttons, s->fwd_pos, s->fwd_neg);
        out->side    = axis(pad->buttons, s->side_pos, s->side_neg);

        /* 0x8001929C: the FORWARD axis alone raises it. Strafing is not
         * "moving" as far as this bit is concerned, and the view recentre
         * downstream depends on that. */
        if (out->forward != 0)
            out->buttons |= Q2_BTN_MOVING;
    }

    /* ---- the derived button bits ------------------------------------ */
    out->buttons = derive(out->buttons, pad->buttons, pad->prev, s->fire,
                          23, 25, 24);
    out->buttons = derive(out->buttons, pad->buttons, pad->prev, s->jump,
                          22, 21, -1);
    out->buttons = derive(out->buttons, pad->buttons, pad->prev, s->wnext,
                          26, -1, -1);
    out->buttons = derive(out->buttons, pad->buttons, pad->prev, s->wprev,
                          27, -1, -1);

    /* The port keeps `attack` as its own field because the simulation's fire
     * path takes a bool; it is bit 25 and nothing else. */
    out->attack = (out->buttons & Q2_BTN_ATTACK) != 0;
}
