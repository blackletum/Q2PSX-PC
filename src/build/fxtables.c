#include "fxtables.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* The six laser arms, transcribed                                            */
/*                                                                            */
/* 0x80048DC8 bounds its fourth argument with `sltiu v0, v1, 6` and jumps      */
/* through 0x800ACCD4. Three of the six arms fall through into another arm's   */
/* body rather than jumping, which is why the table has six entries but only   */
/* three distinct bodies:                                                      */
/*                                                                            */
/*   kind 0  0x80048E34  radius 16  -> body A (0x80048E80)                     */
/*   kind 1  0x80048E48  radius 16  -> body B (0x80048ED0)                     */
/*   kind 2  0x80048E5C  radius 16  -> body C (0x80048F20)                     */
/*   kind 3  0x80048EC0  radius 64  -> body B, by falling through              */
/*   kind 4  0x80048E70  radius 64  -> body A, by falling through              */
/*   kind 5  0x80048F10  radius 64  -> body C, by falling through              */
/*                                                                            */
/* The radius arrives as a2 and is loaded with `addiu a2, zero, 16` /          */
/* `addiu a2, zero, 64`. In the radius-16 arms the same register is then used  */
/* as a shift amount to sign-extend the area index — one constant doing two    */
/* jobs, which is why the radius-64 arms need an explicit `sll/sra 16` pair    */
/* instead. Reading 64 as a shift there would be a decode error that produced  */
/* plausible-looking geometry, so it is called out.                            */
/* ------------------------------------------------------------------------- */
static const q2_fx_laser_kind k_laser[Q2_FX_LASER_KIND_COUNT] = {
    /* radius style ramp damage mod  arm         */
    {  16,     0,    1,   512,  11, 0x80048E34u },  /* body A: red,    laser */
    {  16,     1,    0,     1,  16, 0x80048E48u },  /* body B: blue,   mod 16 */
    {  16,     2,    9,   512,  12, 0x80048E5Cu },  /* body C: yellow, mod 12 */
    {  64,     1,    0,     1,  16, 0x80048EC0u },
    {  64,     0,    1,   512,  11, 0x80048E70u },
    {  64,     2,    9,   512,  12, 0x80048F10u }
};

/* The three bodies, in the order the arms above name them. */
static const u32 k_laser_body[3] = { 0x80048E80u, 0x80048ED0u, 0x80048F20u };

/* Which arm addresses the jump table must hold, in kind order. */
static const u32 k_laser_arm[Q2_FX_LASER_KIND_COUNT] = {
    0x80048E34u, 0x80048E48u, 0x80048E5Cu,
    0x80048EC0u, 0x80048E70u, 0x80048F10u
};

/* ------------------------------------------------------------------------- */
/* The beam hull's index sets                                                 */
/*                                                                            */
/* Vertices 0..5 are the near hexagon and 6..11 the far one, both generated at */
/* draw time. The tube's six quads walk the ring; the caps close each end with */
/* two quads apiece. Written out because it is what makes a stride error       */
/* visible: read at 196 or 204 bytes the sets stop closing the hull.           */
/* ------------------------------------------------------------------------- */
static const u8 k_tube_idx[Q2_FX_BEAM_TUBE_FACES][4] = {
    { 0, 1,  6,  7 }, { 1, 2,  7,  8 }, { 2, 3,  8,  9 },
    { 3, 4,  9, 10 }, { 4, 5, 10, 11 }, { 5, 0, 11,  6 }
};
static const u8 k_cap_near_idx[Q2_FX_BEAM_CAP_FACES][4] = {
    { 1, 0, 2, 3 }, { 4, 3, 5, 0 }
};
static const u8 k_cap_far_idx[Q2_FX_BEAM_CAP_FACES][4] = {
    { 0, 1, 3, 2 }, { 3, 4, 0, 5 }
};

/* ------------------------------------------------------------------------- */
static bool read_face(const q2_exe *e, u32 addr, q2_fx_face *out)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (!q2_exe_u8(e, addr + (u32)i, &out->v[i]))
            return false;
    }
    for (i = 0; i < 4; i++) {
        if (!q2_exe_u32(e, addr + 4u + 4u * (u32)i, &out->colour[i]))
            return false;
    }
    return true;
}

q2_result q2_fx_tables_load(q2_fx_tables *out, const q2_exe *exe)
{
    u32 i, k;

    if (!out || !exe)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /* --- ramps ---------------------------------------------------------- */
    for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
        u32 base = Q2_FXT_ADDR_RAMPS + i * Q2_FX_RAMP_STRIDE;
        u32 c;

        out->ramp_addr[i] = base;

        if (!q2_exe_u16(exe, base + 0u, &out->ramp[i].abr) ||
            !q2_exe_u16(exe, base + 2u, &out->ramp[i].reserved))
            return Q2_ERR_RANGE;

        for (c = 0; c < Q2_FX_RAMP_COLOURS; c++) {
            if (!q2_exe_u32(exe, base + 4u + 4u * c, &out->ramp[i].colour[c]))
                return Q2_ERR_RANGE;
        }
    }

    /* --- the ramp id table, resolved to array indices -------------------- */
    out->ramp_index_is_permutation = true;
    {
        u8 seen[Q2_FX_RAMP_COUNT];

        memset(seen, 0, sizeof(seen));

        for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
            u32 ptr, off;

            if (!q2_exe_u32(exe, Q2_FXT_ADDR_RAMP_INDEX + 4u * i, &ptr))
                return Q2_ERR_RANGE;

            /* A pointer that is not on a record boundary is the signature of a
             * wrong stride or base, so it is recorded rather than rounded. */
            if (ptr < Q2_FXT_ADDR_RAMPS) {
                out->ramp_index_is_permutation = false;
                out->ramp_id_to_index[i] = 0xFFu;
                continue;
            }

            off = ptr - Q2_FXT_ADDR_RAMPS;
            if (off % Q2_FX_RAMP_STRIDE != 0 ||
                off / Q2_FX_RAMP_STRIDE >= Q2_FX_RAMP_COUNT) {
                out->ramp_index_is_permutation = false;
                out->ramp_id_to_index[i] = 0xFFu;
                continue;
            }

            out->ramp_id_to_index[i] = (u8)(off / Q2_FX_RAMP_STRIDE);
            if (seen[out->ramp_id_to_index[i]]++)
                out->ramp_index_is_permutation = false;
        }

        for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
            if (!seen[i])
                out->ramp_index_is_permutation = false;
        }
    }

    /* --- beam styles ---------------------------------------------------- */
    for (i = 0; i < Q2_FX_BEAM_STYLE_COUNT; i++) {
        u32 base = Q2_FXT_ADDR_BEAM_STYLES + i * Q2_FX_BEAM_STYLE_STRIDE;

        for (k = 0; k < Q2_FX_BEAM_TUBE_FACES; k++) {
            if (!read_face(exe, base + 20u * k, &out->beam[i].tube[k]))
                return Q2_ERR_RANGE;
        }
        base += 20u * Q2_FX_BEAM_TUBE_FACES;

        for (k = 0; k < Q2_FX_BEAM_CAP_FACES; k++) {
            if (!read_face(exe, base + 20u * k, &out->beam[i].cap_near[k]))
                return Q2_ERR_RANGE;
        }
        base += 20u * Q2_FX_BEAM_CAP_FACES;

        for (k = 0; k < Q2_FX_BEAM_CAP_FACES; k++) {
            if (!read_face(exe, base + 20u * k, &out->beam[i].cap_far[k]))
                return Q2_ERR_RANGE;
        }
    }

    /* --- the laser dispatch --------------------------------------------- */
    for (i = 0; i < Q2_FX_LASER_KIND_COUNT; i++) {
        if (!q2_exe_u32(exe, Q2_FXT_ADDR_LASER_JUMP + 4u * i,
                        &out->laser_jump[i]))
            return Q2_ERR_RANGE;
        out->laser[i] = k_laser[i];
    }

    for (i = 0; i < Q2_FX_LASER_KIND_COUNT; i++) {
        u32 s = out->laser[i].style;
        if (s < Q2_FX_BEAM_STYLE_COUNT)
            out->beam_style_reachable[s] = true;
    }

    out->loaded = true;
    return Q2_OK;
}

q2_result q2_fx_tables_load_disc(q2_fx_tables *out, const disc *d,
                                 const q2_build_id *id)
{
    q2_exe exe;
    q2_result r;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("effect table locations are unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }
    if (!id->exe_name[0])
        return Q2_ERR_NOT_FOUND;

    r = q2_exe_load(&exe, d, id->exe_name);
    if (r != Q2_OK)
        return r;

    /* The tables are copied out whole, so the image can go straight back. */
    r = q2_fx_tables_load(out, &exe);
    q2_exe_free(&exe);
    return r;
}

/* ------------------------------------------------------------------------- */
const q2_fx_ramp *q2_fx_ramp_at(const q2_fx_tables *t, u32 index)
{
    if (!t || !t->loaded || index >= Q2_FX_RAMP_COUNT)
        return NULL;
    return &t->ramp[index];
}

const q2_fx_ramp *q2_fx_ramp_by_id(const q2_fx_tables *t, u32 id)
{
    u8 index;

    if (!t || !t->loaded || id >= Q2_FX_RAMP_COUNT)
        return NULL;

    index = t->ramp_id_to_index[id];
    if (index >= Q2_FX_RAMP_COUNT)
        return NULL;

    return &t->ramp[index];
}

const q2_fx_beam_style *q2_fx_beam_style_at(const q2_fx_tables *t, u32 index)
{
    if (!t || !t->loaded || index >= Q2_FX_BEAM_STYLE_COUNT)
        return NULL;
    return &t->beam[index];
}

const q2_fx_laser_kind *q2_fx_laser_kind_at(const q2_fx_tables *t, u32 kind)
{
    if (!t || !t->loaded || kind >= Q2_FX_LASER_KIND_COUNT)
        return NULL;
    return &t->laser[kind];
}

/* ------------------------------------------------------------------------- */
/* Checking                                                                   */
/* ------------------------------------------------------------------------- */
static void note(q2_fx_report_fn report, void *user, u32 *bad,
                 const char *what, s64 got, s64 want)
{
    (*bad)++;
    if (report)
        report(user, what, got, want);
}

/*
 * A GPU polygon command byte. Bit 5 selects polygon, bit 4 quad, bit 3
 * gouraud, bit 2 textured, bit 1 ABE, bit 0 raw texture. Anything outside
 * 0x20..0x3F is not a polygon at all, and a table read at the wrong stride
 * lands on vertex indices or on the next record's header, neither of which
 * passes.
 */
static bool code_is_polygon(u8 code)
{
    return code >= 0x20u && code <= 0x3Fu;
}

u32 q2_fx_tables_check(const q2_fx_tables *t, const q2_exe *exe,
                       q2_fx_report_fn report, void *user)
{
    char what[96];
    u32 bad = 0, i, c, f;

    if (!t || !t->loaded) {
        note(report, user, &bad, "tables loaded", 0, 1);
        return bad;
    }

    /* --- ramps ---------------------------------------------------------- */
    for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
        const q2_fx_ramp *r = &t->ramp[i];
        u16 abr = (u16)(r->abr & Q2_FX_ABR_MASK);

        if ((r->abr & ~(u16)Q2_FX_ABR_MASK) != 0) {
            snprintf(what, sizeof(what), "ramp[%u].abr outside the mode field", i);
            note(report, user, &bad, what, (s64)r->abr, (s64)abr);
        }

        if (r->reserved != 0) {
            snprintf(what, sizeof(what), "ramp[%u].reserved", i);
            note(report, user, &bad, what, (s64)r->reserved, 0);
        }

        for (c = 0; c < Q2_FX_RAMP_COLOURS; c++) {
            u8 code = q2_fx_colour_code(r->colour[c]);
            if (!code_is_polygon(code)) {
                snprintf(what, sizeof(what), "ramp[%u].colour[%u] code", i, c);
                note(report, user, &bad, what, (s64)code, -1);
                break;   /* one complaint per ramp is enough to localise it */
            }
        }
    }

    if (!t->ramp_index_is_permutation)
        note(report, user, &bad, "0x8009C42C is a permutation of 0..18", 0, 1);

    /* --- beam styles ---------------------------------------------------- */
    for (i = 0; i < Q2_FX_BEAM_STYLE_COUNT; i++) {
        const q2_fx_beam_style *s = &t->beam[i];

        for (f = 0; f < Q2_FX_BEAM_TUBE_FACES; f++) {
            for (c = 0; c < 4; c++) {
                if (s->tube[f].v[c] != k_tube_idx[f][c]) {
                    snprintf(what, sizeof(what),
                             "beam[%u].tube[%u].v[%u]", i, f, c);
                    note(report, user, &bad, what,
                         (s64)s->tube[f].v[c], (s64)k_tube_idx[f][c]);
                }
            }
        }
        for (f = 0; f < Q2_FX_BEAM_CAP_FACES; f++) {
            for (c = 0; c < 4; c++) {
                if (s->cap_near[f].v[c] != k_cap_near_idx[f][c]) {
                    snprintf(what, sizeof(what),
                             "beam[%u].cap_near[%u].v[%u]", i, f, c);
                    note(report, user, &bad, what,
                         (s64)s->cap_near[f].v[c], (s64)k_cap_near_idx[f][c]);
                }
                if (s->cap_far[f].v[c] != k_cap_far_idx[f][c]) {
                    snprintf(what, sizeof(what),
                             "beam[%u].cap_far[%u].v[%u]", i, f, c);
                    note(report, user, &bad, what,
                         (s64)s->cap_far[f].v[c], (s64)k_cap_far_idx[f][c]);
                }
            }
        }

        /* Every face is authored flat even though the primitive is gouraud. */
        for (f = 0; f < Q2_FX_BEAM_TUBE_FACES; f++) {
            for (c = 1; c < 4; c++) {
                if (s->tube[f].colour[c] != s->tube[f].colour[0]) {
                    snprintf(what, sizeof(what),
                             "beam[%u].tube[%u] corner %u colour", i, f, c);
                    note(report, user, &bad, what,
                         (s64)s->tube[f].colour[c], (s64)s->tube[f].colour[0]);
                }
            }
        }

        if (!code_is_polygon(q2_fx_colour_code(s->tube[0].colour[0]))) {
            snprintf(what, sizeof(what), "beam[%u] primitive code", i);
            note(report, user, &bad, what,
                 (s64)q2_fx_colour_code(s->tube[0].colour[0]), -1);
        }
    }

    /* --- the laser dispatch --------------------------------------------- */
    for (i = 0; i < Q2_FX_LASER_KIND_COUNT; i++) {
        if (t->laser_jump[i] != k_laser_arm[i]) {
            snprintf(what, sizeof(what), "laser jump[%u]", i);
            note(report, user, &bad, what,
                 (s64)t->laser_jump[i], (s64)k_laser_arm[i]);
        }
        if (exe && !q2_exe_contains(exe, t->laser_jump[i], 4)) {
            snprintf(what, sizeof(what), "laser jump[%u] inside the segment", i);
            note(report, user, &bad, what, 0, 1);
        }
        if (t->laser[i].style >= Q2_FX_BEAM_STYLE_COUNT) {
            snprintf(what, sizeof(what), "laser[%u].style", i);
            note(report, user, &bad, what, (s64)t->laser[i].style,
                 Q2_FX_BEAM_STYLE_COUNT - 1);
        }
        if (t->laser[i].ramp >= Q2_FX_RAMP_COUNT) {
            snprintf(what, sizeof(what), "laser[%u].ramp", i);
            note(report, user, &bad, what, (s64)t->laser[i].ramp,
                 Q2_FX_RAMP_COUNT - 1);
        }
    }

    /* The three distinct bodies must still be inside the segment, because the
     * transcription above claims their contents. */
    if (exe) {
        for (i = 0; i < 3; i++) {
            if (!q2_exe_contains(exe, k_laser_body[i], 4)) {
                snprintf(what, sizeof(what), "laser body %u mapped", i);
                note(report, user, &bad, what, 0, 1);
            }
        }
    }

    return bad;
}

/* ------------------------------------------------------------------------- */
u32 q2_fx_tables_compare(const q2_fx_tables *a, const q2_fx_tables *b,
                         q2_fx_report_fn report, void *user)
{
    char what[96];
    u32 bad = 0, i, c, f;

    if (!a || !b)
        return 1;

    for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
        if (a->ramp[i].abr != b->ramp[i].abr) {
            snprintf(what, sizeof(what), "ramp[%u].abr", i);
            note(report, user, &bad, what,
                 (s64)a->ramp[i].abr, (s64)b->ramp[i].abr);
        }
        for (c = 0; c < Q2_FX_RAMP_COLOURS; c++) {
            if (a->ramp[i].colour[c] != b->ramp[i].colour[c]) {
                snprintf(what, sizeof(what), "ramp[%u].colour[%u]", i, c);
                note(report, user, &bad, what,
                     (s64)a->ramp[i].colour[c], (s64)b->ramp[i].colour[c]);
            }
        }
        if (a->ramp_id_to_index[i] != b->ramp_id_to_index[i]) {
            snprintf(what, sizeof(what), "ramp id %u -> record", i);
            note(report, user, &bad, what,
                 (s64)a->ramp_id_to_index[i], (s64)b->ramp_id_to_index[i]);
        }
    }

    for (i = 0; i < Q2_FX_BEAM_STYLE_COUNT; i++) {
        for (f = 0; f < Q2_FX_BEAM_TUBE_FACES; f++) {
            if (a->beam[i].tube[f].colour[0] != b->beam[i].tube[f].colour[0]) {
                snprintf(what, sizeof(what), "beam[%u].tube[%u].colour", i, f);
                note(report, user, &bad, what,
                     (s64)a->beam[i].tube[f].colour[0],
                     (s64)b->beam[i].tube[f].colour[0]);
            }
        }
    }

    for (i = 0; i < Q2_FX_LASER_KIND_COUNT; i++) {
        if (a->laser_jump[i] != b->laser_jump[i]) {
            snprintf(what, sizeof(what), "laser jump[%u]", i);
            note(report, user, &bad, what,
                 (s64)a->laser_jump[i], (s64)b->laser_jump[i]);
        }
    }

    return bad;
}
