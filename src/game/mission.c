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
     * The console fills these at run time; what is fixed is the space they are
     * expressed in. A box inset from the 512 x 248 framebuffer is what the
     * layout constants assume — the first body line at y = 36 and the labels at
     * box_x + 20 only make sense against one.
     */
    b->x = 96;
    b->y = 24;
    b->w = 320;
    b->h = 200;
}

void q2_mission_init(q2_mission *m)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    q2_mission_box_default(&m->box);
    m->unit = 1;
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

    y += Q2_MISSION_BODY_STEP_2;
    n += emit_row(font, ctx, pen, ot, otz,
                  q2_mission_centre_x(m->footer, left, right), y,
                  q2_mission_rgb_value, 4, m->footer, true);

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
