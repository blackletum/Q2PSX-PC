#include "mission.h"

#include <stdio.h>
#include <string.h>

/* The three colours the format strings carry, as bytes. */
const u8 q2_mission_rgb_label[3] = { 0xBE, 0xF0, 0xE6 };
const u8 q2_mission_rgb_value[3] = { 0xDC, 0xF0, 0x82 };
const u8 q2_mission_rgb_total[3] = { 0xC8, 0xF0, 0x00 };

/* 0x800AB9DC. The doubled spaces are the original's. */
const char q2_mission_title_format[] = "Mission  %d  -  Complete";

void q2_mission_box_default(q2_mission_box *b)
{
    if (!b)
        return;
    /*
     * READ OFF THE IMAGE, not authored here — and the four used to be a guess.
     *
     * `MissionRect` is eight bytes of initialised data at 0x800AE760:
     *
     *     800AE760  54 00 1E 00 58 01 7C 00     84, 30, 344, 124
     *
     * and `xrefs` settles which of them are constants. x (gp+352), y (gp+354)
     * and w (gp+356) have READS ONLY — eight, one and four of them, all inside
     * `DBInterMission` — so nothing in the executable ever writes them. Only h
     * (gp+358) has a writer, and exactly one: `SetInterMission`'s
     * `sh v0, 358(gp)` at 0x800224E8, from `q2_mission_box_height` below. 124
     * is its answer for a full six-row board, which is why that is the seed.
     *
     * The port had (96, 24, 320, 200) with a comment calling them "what the
     * screen is authored against". They are not: the 12-pixel difference in x
     * moved the whole Location/Secrets/Kills table right, because the column
     * indent is `box.x + 20`. The three centred lines hid it, since 84 + 344/2
     * and 96 + 320/2 are both 256.
     */
    b->x = 84;
    b->y = 30;
    b->w = 344;
    b->h = 124;
}

/*
 * 0x80022664, PreProcessInterMission — the board's height, and it hugs the
 * table.
 *
 * `SetInterMission` is the board's only setup and its last act is to store this
 * answer into gp+358 (0x800224E8), so the darkened rectangle behind the text is
 * as tall as the unit is long. The arithmetic, instruction for instruction:
 *
 *     80022678  addiu s0, zero, 24          start at 24
 *     80022688  jal   strlen(0x8009B634)    the wrapped SECOND objective line
 *     80022690  beq   v0, zero, ...         branch on empty...
 *     80022694  addiu s0, s0, 18            ...but the delay slot runs anyway
 *     80022698  addiu s0, zero, 32          non-empty: start again at 32
 *     8002269C  addiu s0, s0, 18            and add the same 18
 *     800226A4  jal   strlen(row[i])        six records, 25 bytes apart
 *     800226B4  addiu s0, s0, 10            +10 for each non-empty name
 *     800226CC  addiu v0, s0, 14            and 14 for the frame
 *
 * so it is 42 for a one-line objective and 50 for a wrapped one, plus ten a
 * row, plus fourteen. 50 + 14 + 6*10 = 124, which is exactly the .data seed
 * above — the clearest confirmation available that the formula is read right.
 *
 * The result is narrowed to s16 by the `sll`/`sra` pair at 0x800226D0.
 */
s16 q2_mission_box_height(const q2_mission *m)
{
    int h, i;

    if (!m)
        return 0;

    /* The empty test is on the SECOND wrapped line, not on the first: a short
     * objective leaves `footer` empty and the board is eight pixels shorter. */
    h = m->footer[0] ? (32 + 18) : (24 + 18);

    for (i = 0; i < Q2_MISSION_ROWS; i++)
        if (m->row[i].name[0])
            h += 10;

    return (s16)(h + 14);
}

bool q2_mission_pad_end(s32 *gate, s32 dt, bool fire_edge)
{
    s32 d;

    if (!gate)
        return false;

    /*
     * Both operands are loaded as UNSIGNED halfwords and the difference is
     * stored back as a halfword (0x800224F4/0x800224FC/0x8002250C), so the
     * widths are kept here rather than done in int: a counter below the delta
     * wraps high and is then rejected by the SIGNED 16-bit test, which is what
     * `sll v0,16` before the `bgtz` is for.
     */
    d     = (s32)(u16)((u32)*gate - (u32)dt);
    *gate = d;

    if ((s16)(u16)d > 0)
        return false;               /* 0x80022514: return 0, pad not read */

    *gate = 0;                      /* 0x8002251C */

    if (!fire_edge)
        return false;

    *gate = Q2_MISSION_GATE_TICKS;  /* 0x80022538, and only here */
    return true;                    /* 0x8002253C: answer 4 */
}

/* ------------------------------------------------------------------------- */
/* TestCamera — 0x8009C668, four 24-byte records                              */
/* ------------------------------------------------------------------------- */
/*
 * Transcribed halfword for halfword out of the image:
 *
 *   8009C668  9E DB FF FF  FE F5 FF FF  62 29 00 00  F4 01  72 15  00 00 ...
 *   8009C680  10 00 00 00  F7 D3 FF FF  05 A2 FF FF  F4 01  FF 0C  00 00 ...
 *   8009C698  26 B5 FF FF  44 F5 FF FF  1A D3 FF FF  F4 01  C8 2C  00 00 ...
 *   8009C6B0  BF 0B 00 00  16 E8 FF FF  5A BC FF FF  F4 01  95 FD  00 00 ...
 *
 * The yaw is kept here as the UNSIGNED halfword the console loads
 * (0x800387F4 `lhu`), because what it stores is `0 - that` narrowed back to a
 * halfword (0x800387FC `subu`, 0x80038800 `sh`) — and for unit 4, whose raw
 * value is 0xFD95, the two readings differ: negating the u16 gives +619 and
 * negating the s16 would give -619. `q2_mission_camera_for_unit` does the
 * console's.
 *
 * Every record's pitch is 500 and every record's roll is 0. That is not a
 * simplification here: all four really are authored that way, which is what a
 * fixed establishing shot looks like.
 */
typedef struct mission_camera_raw {
    s32 pos[3];
    u16 pitch, yaw, roll, cell, area;
} mission_camera_raw;

static const mission_camera_raw g_mission_camera[Q2_MISSION_CAMERAS] = {
    { { -9314,  -2562,  10594 },  500,  5490, 0, 170, 31 },  /* 0x8009C668 */
    { {    16, -11273, -24059 },  500,  3327, 0,  81, 27 },  /* 0x8009C680 */
    { { -19162, -2748, -11494 },  500, 11464, 0,  10, 17 },  /* 0x8009C698 */
    { {   3007,  -6122, -17318 }, 500, 0xFD95, 0,  8, 15 }   /* 0x8009C6B0 */
};

bool q2_mission_camera_for_unit(int unit, q2_mission_camera *out)
{
    const mission_camera_raw *r;

    if (!out)
        return false;

    /* 0x8003874C's `addiu v0, v0, -24` is the whole of the index: record
     * `unit - 1`. The console does not range-check it; this does, and the
     * header says what it costs on unit 5. */
    if (unit < 1 || unit > Q2_MISSION_CAMERAS)
        return false;

    r = &g_mission_camera[unit - 1];

    out->pos[0] = r->pos[0];
    out->pos[1] = r->pos[1];
    out->pos[2] = r->pos[2];
    out->pitch  = (s16)r->pitch;
    /* 0x800387F4..0x80038800: `lhu`, `subu v0, zero, v0`, `sh`. */
    out->yaw    = (s16)(u16)(0u - (u32)r->yaw);
    out->roll   = (s16)r->roll;
    out->cell   = (s16)r->cell;
    out->area   = (s16)r->area;
    return true;
}

void q2_mission_init(q2_mission *m)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    q2_mission_box_default(&m->box);
    m->unit = 1;
}

void q2_mission_clear_rows(q2_mission *m)
{
    if (!m)
        return;

    /* One memset over the whole 150 bytes, which is six records of 25 — names
     * and counters together (0x80021AC8's `a2 = 150`). The box, the unit and
     * the two objective lines are outside it and survive. */
    memset(m->row, 0, sizeof(m->row));
}

static u8 clamp_count(int v)
{
    if (v < 0)                     return 0;
    if (v > Q2_MISSION_COUNT_MAX)  return Q2_MISSION_COUNT_MAX;
    return (u8)v;
}

void q2_mission_set_row(q2_mission *m, int index, const char *name,
                        int secrets, int secrets_total,
                        int kills, int kills_total)
{
    q2_mission_row *r;

    if (!m || index < 0 || index >= Q2_MISSION_ROWS)
        return;

    r = &m->row[index];
    memset(r, 0, sizeof(*r));

    if (name) {
        /* The record's name field is 21 bytes; anything longer is truncated
         * rather than overrunning into the counters, which is what the console
         * would do to it. */
        strncpy(r->name, name, Q2_MISSION_NAME_LEN);
        r->name[Q2_MISSION_NAME_LEN] = '\0';
    }

    r->secrets       = clamp_count(secrets);
    r->secrets_total = clamp_count(secrets_total);
    r->kills         = clamp_count(kills);
    r->kills_total   = clamp_count(kills_total);
}

/*
 * 0x800222B8. The search is two compares a row: the name we want, then the
 * empty string at 0x800AE768 — so "already registered" wins over "first free"
 * and a level that is re-entered keeps the row it had rather than taking a
 * second one.
 */
int q2_mission_register(q2_mission *m, const char *name)
{
    int i, found;

    if (!m || !name || !name[0])
        return -1;

    found = q2_mission_find(m, name);
    if (found >= 0)
        return found;

    for (i = 0; i < Q2_MISSION_ROWS; i++) {
        if (m->row[i].name[0] == '\0') {
            /* The counters go in with the name: the console stamps whatever is
             * live at that moment, which at a level start is the four zeroes
             * 0x80070874 has just written. */
            q2_mission_set_row(m, i, name, 0, 0, 0, 0);
            return i;
        }
    }

    /* All six taken by other levels. The console's search falls off the end the
     * same way and registers nothing; see the header. */
    return -1;
}

int q2_mission_find(const q2_mission *m, const char *name)
{
    int i;

    if (!m || !name || !name[0])
        return -1;

    for (i = 0; i < Q2_MISSION_ROWS; i++)
        if (strncmp(m->row[i].name, name, Q2_MISSION_NAME_LEN) == 0)
            return i;

    return -1;
}

void q2_mission_set_counts(q2_mission *m, int index,
                           int secrets, int secrets_total,
                           int kills, int kills_total)
{
    q2_mission_row *r;

    if (!m || index < 0 || index >= Q2_MISSION_ROWS)
        return;

    r = &m->row[index];
    r->secrets       = clamp_count(secrets);
    r->secrets_total = clamp_count(secrets_total);
    r->kills         = clamp_count(kills);
    r->kills_total   = clamp_count(kills_total);
}

/*
 * 0x80022210. Four `lbu`/`sw` pairs off the matched row — +21 into
 * 0x800B29FC, +22 into 0x800B29F8, +23 into 0x800B29E8, +24 into 0x800B29E4 —
 * and nothing else. Taking it by index rather than by name is the only
 * difference: the caller here has already resolved the name with
 * `q2_mission_find`, which is the same compare the console's loop makes.
 */
bool q2_mission_get_counts(const q2_mission *m, int index,
                           int *secrets, int *secrets_total,
                           int *kills, int *kills_total)
{
    const q2_mission_row *r;

    if (!m || index < 0 || index >= Q2_MISSION_ROWS)
        return false;

    r = &m->row[index];
    if (secrets)       *secrets       = r->secrets;
    if (secrets_total) *secrets_total = r->secrets_total;
    if (kills)         *kills         = r->kills;
    if (kills_total)   *kills_total   = r->kills_total;
    return true;
}

/* 0x800220C8's two constants: the length that forces a wrap, and the size of
 * each of the two buffers it writes. */
#define MISSION_WRAP_AT   36
#define MISSION_LINE_CAP  42

void q2_mission_set_objective(q2_mission *m, const char *text)
{
    char scratch[Q2_MISSION_LINE_MAX * 4];
    size_t n, cut;

    if (!m)
        return;

    memset(m->subtitle, 0, sizeof(m->subtitle));
    memset(m->footer,   0, sizeof(m->footer));

    if (!text || !text[0])
        return;

    n = strlen(text);
    if (n < MISSION_WRAP_AT) {
        /* One line, and the second stays empty — 0x8002215C. */
        strncpy(m->subtitle, text, MISSION_LINE_CAP - 1);
        m->subtitle[MISSION_LINE_CAP - 1] = '\0';
        return;
    }

    /*
     * The cut loop at 0x80022180: copy, then keep truncating at the LAST space
     * until what is left is under 36. `cut` is the index that space sat at,
     * which is why line two begins with it (0x800221E4 passes `src + cut`).
     */
    if (n >= sizeof(scratch))
        n = sizeof(scratch) - 1;
    memcpy(scratch, text, n);
    scratch[n] = '\0';

    cut = n;
    for (;;) {
        char *space = strrchr(scratch, ' ');

        if (!space)
            break;              /* no space to cut at: line one takes it all */
        cut = (size_t)(space - scratch);
        *space = '\0';
        if (cut < MISSION_WRAP_AT)
            break;
    }

    if (cut >= MISSION_LINE_CAP)
        cut = MISSION_LINE_CAP - 1;

    memcpy(m->subtitle, text, cut);
    m->subtitle[cut] = '\0';

    {
        size_t rest = strlen(text) - cut;

        if (rest >= MISSION_LINE_CAP)
            rest = MISSION_LINE_CAP - 1;
        memcpy(m->footer, text + cut, rest);
        m->footer[rest] = '\0';
    }
}

void q2_mission_totals(const q2_mission *m, int *secrets, int *secrets_total,
                       int *kills, int *kills_total)
{
    int s = 0, st = 0, k = 0, kt = 0, i;

    if (m) {
        /*
         * Summed over EVERY record, including the ones whose empty name means
         * their row is not drawn — the accumulators at 0x80021E58 are updated
         * after the `blez` that skips the drawing, not before it. On this disc
         * an unused record is all zeroes so the distinction is invisible, but
         * it is the code's, not a simplification.
         */
        for (i = 0; i < Q2_MISSION_ROWS; i++) {
            s  += m->row[i].secrets;
            st += m->row[i].secrets_total;
            k  += m->row[i].kills;
            kt += m->row[i].kills_total;
        }
    }

    if (secrets)       *secrets       = s;
    if (secrets_total) *secrets_total = st;
    if (kills)         *kills         = k;
    if (kills_total)   *kills_total   = kt;
}

/* ------------------------------------------------------------------------- */
/* Which creatures the tally is about — 0x80057860 and 0x8007CD5C             */
/* ------------------------------------------------------------------------- */
bool q2_mission_class_counts(u32 pop_class_id)
{
    /*
     * CreatureCount's denominator filter, written as the console writes it:
     * `addiu v0, v0, -34` then `sltiu v0, v0, 3` (0x80057860/0x80057864), an
     * UNSIGNED window test, so a class below 34 wraps high and passes. The
     * three it excludes are the scenery marines and the two riders.
     */
    return (u32)(pop_class_id - Q2_MISSION_CLASS_INSANE) >= 3u;
}

bool q2_mission_class_kills(u32 pop_class_id)
{
    /*
     * ProcessQuakeDeath's numerator filter, 0x8007CD5C/0x8007CD60 — a single
     * `beq` against 34 and nothing else. Deliberately NOT the same test as
     * above: the console excludes three classes from the denominator and one
     * from the numerator, and making them agree would be inventing a rule.
     */
    return pop_class_id != Q2_MISSION_CLASS_INSANE;
}

const char *q2_mission_title(const q2_mission *m, char *out, u32 out_size)
{
    if (!out || out_size == 0)
        return out;
    if (!m) {
        out[0] = '\0';
        return out;
    }
    snprintf(out, out_size, q2_mission_title_format, m->unit);
    return out;
}

int q2_mission_centre_x(const char *s, int left, int right)
{
    /*
     * 0x80022550. The width is the HUD's own measure — `q2_hud_measure`, which
     * returns `strlen - 1` (hud.h §"quirks") — times the fixed 8-pixel advance.
     * Using a corrected length here would centre every row one half-glyph off
     * from where the console puts it.
     */
    int w = q2_hud_measure(s) * Q2_HUD_GLYPH_W;
    int x = left + ((right - left) - w) / 2;

    if (x < left)
        x = left;
    return x;
}

int q2_mission_field_x(const char *s, int x)
{
    /*
     * 0x8002260C, and it is not a right-align — it CENTRES the string inside a
     * 56-pixel field whose left edge is `x`:
     *
     *     n = strlen(s)
     *     if ((n & 1) == 0) n -= 1          <- 0x80022624, and it is deliberate
     *     return x + (56 - n * 8) / 2
     *
     * The even-length adjustment is the quirk. `"4/4"` is odd and measures 24,
     * so it starts 16 in; `"99/100"` is even, measures as five characters
     * rather than six, and starts 8 in — half a glyph right of true centre.
     * Reproducing it is what makes a six-character total line up with the
     * console's, and "fixing" it moves every even-length figure four pixels.
     */
    int n = (int)strlen(s ? s : "");

    if ((n & 1) == 0)
        n -= 1;
    if (n < 0)
        n = 0;

    return x + (Q2_MISSION_FIELD_W - n * Q2_HUD_GLYPH_W) / 2;
}

/* ------------------------------------------------------------------------- */
/* Emission                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * One row. The console builds each of these with `sprintf` into 0x800C6EE8 and
 * hands the result to the text printf, so the escapes really are composed as
 * text at run time — which is why this does the same rather than calling the
 * markup layer's internals.
 *
 * `%03X` and `%3X` both appear in the original's formats, and the port emits the
 * PADDED form for all of them. That is a deliberate normalisation, and the
 * reasoning is worth keeping because it corrects an earlier guess of mine.
 *
 * The escape's digit converter at `0x80041F94` is `c>='a' ? c-87 : c>=58 ?
 * c-55 : c-48`, so a space yields **-16**, and the three digits combine as
 * `(d0<<8)|(d1<<4)|d2` into a 16-bit store — a leading space would put the pen
 * at -3992 and the row would simply not appear. Retail screenshots show the
 * Location column drawing correctly at an x well below 0x100, so a leading
 * space is never produced: whatever Sony's `sprintf` does with `%3X`, it is not
 * emitting one here.
 *
 * I first read that the other way round — that `%3X` implied the value columns
 * had to sit at x >= 256 — and it is wrong: the column offsets are +0, +176 and
 * +256 from a box whose left edge is around 84, all under 0x100, and the
 * screenshots agree with the disassembly. What matters to a port is the
 * coordinate, not the byte sequence, so emitting three padded digits reaches
 * the same pen position without depending on a libc detail.
 */
static u32 emit_row(const q2_hud_font *font, q2_hud_ctx *ctx, q2_hud_pen *pen,
                    psx_ot *ot, u16 otz, int x, int y, const u8 rgb[3],
                    int space_width, const char *text, bool pad_x)
{
    char buf[Q2_MISSION_LINE_MAX * 2];
    char sp[8];

    if (!text || !text[0])
        return 0;

    if (x < 0)     x = 0;
    if (x > 0xFFF) x = 0xFFF;
    if (y < 0)     y = 0;
    if (y > 0xFF)  y = 0xFF;

    /* `~N` is only present on the rows that carry it; the rest inherit the
     * pen's current space width, which is the point of the escape being $gp
     * state rather than a per-string argument. */
    if (space_width > 0)
        snprintf(sp, sizeof(sp), "~%X", space_width & 0xF);
    else
        sp[0] = '\0';

    snprintf(buf, sizeof(buf), pad_x ? "@%03X%02X^%02X%02X%02X|0%s%s"
                                     : "@%3X%02X^%02X%02X%02X|0%s%s",
             x, y, rgb[0], rgb[1], rgb[2], sp, text);

    return q2_hud_print(font, ctx, pen, ot, otz, buf);
}

u32 q2_mission_build_ot(const q2_mission *m, const q2_hud_font *font,
                        q2_hud_ctx *ctx, q2_hud_pen *pen,
                        psx_ot *ot, u16 otz)
{
    char line[Q2_MISSION_LINE_MAX];
    char value[Q2_MISSION_LINE_MAX];
    int left, right, col, y, i;
    int ts, tst, tk, tkt;
    u32 n = 0;

    if (!m || !font || !ctx || !pen || !ot)
        return 0;

    left  = m->box.x;
    right = m->box.x + m->box.w;
    col   = m->box.x + Q2_MISSION_LABEL_INDENT;

    /* --- the three centred body lines ---------------------------------- */
    y = Q2_MISSION_BODY_Y;

    q2_mission_title(m, line, (u32)sizeof(line));
    n += emit_row(font, ctx, pen, ot, otz,
                  q2_mission_centre_x(line, left, right), y,
                  q2_mission_rgb_label, 4, line, true);

    y += Q2_MISSION_BODY_STEP_1;
    n += emit_row(font, ctx, pen, ot, otz,
                  q2_mission_centre_x(m->subtitle, left, right), y,
                  q2_mission_rgb_value, 4, m->subtitle, true);

    /*
     * The third line's step is TAKEN WHETHER OR NOT THE LINE IS DRAWN
     * (0x80021C4C is ahead of the strlen), but the step PAST it is not: the
     * `beq` at 0x80021C58 jumps straight to the header, skipping both the draw
     * and the second `addiu v0, v0, 8` at 0x80021CB0.
     *
     * The port used to emit the line unconditionally — harmless, since an empty
     * string lays out nothing — and then apply neither step, so the header row
     * was eight pixels high on every level whose objective wraps. With the
     * panel behind the board that showed as the Location/Secrets/Kills row
     * printed over the objective's second line.
     */
    y += Q2_MISSION_BODY_STEP_2;
    if (m->footer[0]) {
        n += emit_row(font, ctx, pen, ot, otz,
                      q2_mission_centre_x(m->footer, left, right), y,
                      q2_mission_rgb_value, 4, m->footer, true);
        y += Q2_MISSION_BODY_STEP_3;
    }

    /* --- the header row: three columns, one row ------------------------- */
    y += Q2_MISSION_HEADER_DROP;

    n += emit_row(font, ctx, pen, ot, otz,
                  col + Q2_MISSION_COL_LOCATION, y,
                  q2_mission_rgb_label, 0, "Location", true);
    n += emit_row(font, ctx, pen, ot, otz,
                  col + Q2_MISSION_COL_SECRETS, y,
                  q2_mission_rgb_label, 0, "Secrets", true);
    n += emit_row(font, ctx, pen, ot, otz,
                  col + Q2_MISSION_COL_KILLS, y,
                  q2_mission_rgb_label, 0, "Kills", true);

    /* --- one row per level --------------------------------------------- */
    y += Q2_MISSION_FIRST_ROW;

    for (i = 0; i < Q2_MISSION_ROWS; i++) {
        const q2_mission_row *r = &m->row[i];

        /* An empty name skips the row WITHOUT advancing y (the `blez` at
         * 0x80021D70 jumps past the bump), so unused slots leave no gap. */
        if (!r->name[0])
            continue;

        n += emit_row(font, ctx, pen, ot, otz,
                      col + Q2_MISSION_COL_LOCATION, y,
                      q2_mission_rgb_value, 8, r->name, true);

        snprintf(value, sizeof(value), "%d/%d", r->secrets, r->secrets_total);
        n += emit_row(font, ctx, pen, ot, otz,
                      q2_mission_field_x(value, col + Q2_MISSION_VAL_SECRETS),
                      y, q2_mission_rgb_value, 0, value, true);

        snprintf(value, sizeof(value), "%d/%d", r->kills, r->kills_total);
        n += emit_row(font, ctx, pen, ot, otz,
                      q2_mission_field_x(value, col + Q2_MISSION_VAL_KILLS),
                      y, q2_mission_rgb_value, 0, value, true);

        y += Q2_MISSION_ROW_STEP;
    }

    /* --- the totals row -------------------------------------------------- */
    q2_mission_totals(m, &ts, &tst, &tk, &tkt);

    n += emit_row(font, ctx, pen, ot, otz,
                  col + Q2_MISSION_COL_TOTALS, y,
                  q2_mission_rgb_label, 0, "Totals:", true);

    snprintf(value, sizeof(value), "%d/%d", ts, tst);
    n += emit_row(font, ctx, pen, ot, otz,
                  q2_mission_field_x(value, col + Q2_MISSION_VAL_SECRETS), y,
                  q2_mission_rgb_total, 0, value, true);

    snprintf(value, sizeof(value), "%d/%d", tk, tkt);
    n += emit_row(font, ctx, pen, ot, otz,
                  q2_mission_field_x(value, col + Q2_MISSION_VAL_KILLS), y,
                  q2_mission_rgb_total, 0, value, true);

    return n;
}
