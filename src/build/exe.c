#include "exe.h"

#include "ident.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

/*
 * PS-X EXE header, all little-endian:
 *
 *   0x00  char   magic[8]      "PS-X EXE"
 *   0x10  u32    pc0           entry point
 *   0x14  u32    gp0           initial $gp
 *   0x18  u32    t_addr        load address of the segment
 *   0x1C  u32    t_size        its size (a multiple of 2048)
 *   0x28  u32    b_addr        zero-filled region
 *   0x2C  u32    b_size
 *   0x30  u32    s_addr        initial stack top
 *   0x34  u32    s_size
 *   0x800                      the segment itself
 */
/*
 * The header's gp0 is zero in this build — as it is in most PS-X EXEs, because
 * the C runtime sets $gp itself in the startup stub rather than asking the
 * loader to. Almost every global in this image is reached as `gp + imm16`, so
 * without the real value a disassembly of any function is a wall of unresolved
 * offsets. Recover it the same way a human would: read the prologue and find
 * where $gp is materialised.
 */
static u32 derive_gp(const q2_exe *e)
{
    u32 addr, hi = 0;
    bool have_hi = false;
    int i;

    for (i = 0, addr = e->pc0; i < 256; i++, addr += 4) {
        const u8 *p = q2_exe_ptr(e, addr, 4);
        u32 w, op, rt, rs;

        if (!p)
            break;
        w  = q2_rd_u32(p);
        op = (w >> 26) & 0x3F;
        rs = (w >> 21) & 0x1F;
        rt = (w >> 16) & 0x1F;

        if (op == 0x0F && rt == 28) {            /* lui gp, hi     */
            hi = (w & 0xFFFF) << 16;
            have_hi = true;
        } else if (op == 0x09 && rt == 28 && rs == 28 && have_hi) {
            return hi + (u32)(s32)(s16)(w & 0xFFFF);   /* addiu gp, gp, lo */
        }
    }
    return 0;
}

q2_result q2_exe_load(q2_exe *out, const disc *d, const char *exe_name)
{
    disc_boot_info boot;
    q2_result r;
    const u8 *h;
    char name[32];

    if (!out || !d)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (exe_name && exe_name[0]) {
        snprintf(name, sizeof(name), "%s", exe_name);
    } else {
        r = disc_read_boot_info(d, &boot);
        if (r != Q2_OK)
            return r;
        snprintf(name, sizeof(name), "%s", boot.exe_name);
    }

    if (!name[0])
        return Q2_ERR_NOT_FOUND;

    r = disc_read_file(d, name, &out->file);
    if (r != Q2_OK)
        return r;

    if (out->file.size < Q2_EXE_HEADER_SIZE ||
        memcmp(out->file.data, "PS-X EXE", 8) != 0) {
        q2_buf_free(&out->file);
        return Q2_ERR_BAD_FORMAT;
    }

    h = out->file.data;
    out->pc0       = q2_rd_u32(h + 0x10);
    out->gp0       = q2_rd_u32(h + 0x14);
    out->text_addr = q2_rd_u32(h + 0x18);
    out->text_size = q2_rd_u32(h + 0x1C);
    out->bss_addr  = q2_rd_u32(h + 0x28);
    out->bss_size  = q2_rd_u32(h + 0x2C);
    out->sp_base   = q2_rd_u32(h + 0x30);
    out->sp_size   = q2_rd_u32(h + 0x34);
    snprintf(out->name, sizeof(out->name), "%s", name);

    /*
     * t_size is what the loader copies, and it is rounded up to a sector. The
     * file may be shorter than the header claims if the dump is truncated, so
     * clamp rather than trusting the header — a reader that walks off the end
     * of a short file would report plausible-looking garbage.
     */
    if ((size_t)out->text_size > out->file.size - Q2_EXE_HEADER_SIZE)
        out->text_size = (u32)(out->file.size - Q2_EXE_HEADER_SIZE);

    if (out->text_size == 0) {
        q2_buf_free(&out->file);
        return Q2_ERR_BAD_FORMAT;
    }

    if (out->gp0 == 0)
        out->gp0 = derive_gp(out);

    {
        u8   digest[SHA256_DIGEST_SIZE];
        char hex[SHA256_HEX_SIZE];

        sha256_buffer(out->file.data, out->file.size, digest);
        sha256_hex(digest, hex);
        out->build       = q2_build_by_hash(hex);
        out->build_exact = out->build != NULL;

        /*
         * An executable the catalogue does not know by hash but DOES know by
         * name — a revision, a patched copy — is read with that release's
         * layout, which is what the table loaders did when they keyed on the
         * serial alone. q2_identify still reports it as uncatalogued.
         */
        if (!out->build)
            out->build = q2_build_by_exe_name(out->name);
    }

    return Q2_OK;
}

bool q2_exe_has_layout(const q2_exe *e)
{
    return e && q2_build_has_layout(e->build);
}

u32 q2_exe_addr(const q2_exe *e, u32 pal)
{
    const q2_build_desc *b;
    u32 i, a;

    if (!q2_exe_has_layout(e))
        return 0;
    b = e->build;
    if (b->layout_runs == 0)
        return pal;

    /* Runs are in SLES-01534 address order and do not overlap; the segment
     * selector is kept, so KSEG0 in means KSEG0 out. */
    a = q2_exe_norm(pal) | 0x80000000u;
    for (i = 0; i < b->layout_runs; i++) {
        const q2_addr_run *r = &b->layout[i];
        if (a >= r->begin && a < r->end)
            return (u32)((s64)pal + r->delta);
    }
    return 0;
}

bool q2_exe_lo_relocates(const q2_exe *e, s32 pal_lo, s32 got)
{
    u32 hi;

    /* The image and its data and BSS span these segments; a %lo pairs with
     * one of them. */
    for (hi = 0x8001u; hi <= 0x800Fu; hi++) {
        u32 pal = (hi << 16) + (u32)pal_lo;     /* pal_lo is sign-extended */
        u32 at  = q2_exe_addr(e, pal);

        if (at && at != pal && (s32)(s16)(u16)(at & 0xFFFFu) == got)
            return true;
    }
    return false;
}

bool q2_exe_word_relocates(const q2_exe *e, u32 pal_word, u32 got)
{
    u32 op = pal_word >> 26;

    if (pal_word == got)
        return true;
    if (!q2_exe_has_layout(e))
        return false;

    /* A pointer stored as data. */
    if (q2_exe_addr(e, pal_word) == got)
        return true;

    if ((got >> 26) != op)
        return false;

    if (op == 0x02 || op == 0x03) {                     /* j, jal */
        u32 tp = 0x80000000u | ((pal_word & 0x03FFFFFFu) << 2);
        u32 tg = 0x80000000u | ((got & 0x03FFFFFFu) << 2);
        return q2_exe_addr(e, tp) == tg;
    }

    /* Same opcode, rs and rt, and an immediate that is a %lo that moved.
     * A `lui` whose high half changed is not accepted: a move of a few
     * hundred bytes changes one only across a 64 KB boundary, and a check
     * that meets one should say so rather than be waved through. */
    if (op == 0x0F || (pal_word & 0xFFFF0000u) != (got & 0xFFFF0000u))
        return false;
    return q2_exe_lo_relocates(e, (s32)(s16)(u16)(pal_word & 0xFFFFu),
                               (s32)(s16)(u16)(got & 0xFFFFu));
}

u32 q2_exe_pal(const q2_exe *e, u32 addr)
{
    const q2_build_desc *b;
    u32 i, a;

    if (!q2_exe_has_layout(e))
        return 0;
    b = e->build;
    if (b->layout_runs == 0)
        return addr;

    a = q2_exe_norm(addr) | 0x80000000u;
    for (i = 0; i < b->layout_runs; i++) {
        const q2_addr_run *r = &b->layout[i];
        s64 lo = (s64)r->begin + r->delta, hi = (s64)r->end + r->delta;
        if ((s64)a >= lo && (s64)a < hi)
            return (u32)((s64)addr - r->delta);
    }
    return 0;
}

void q2_exe_free(q2_exe *e)
{
    if (!e)
        return;
    q2_buf_free(&e->file);
    memset(e, 0, sizeof(*e));
}

bool q2_exe_contains(const q2_exe *e, u32 addr, u32 len)
{
    u32 base, off;

    if (!e || !e->file.data)
        return false;

    base = q2_exe_norm(e->text_addr);
    addr = q2_exe_norm(addr);
    if (addr < base)
        return false;

    off = addr - base;
    if (off > e->text_size)
        return false;
    return len <= e->text_size - off;
}

const u8 *q2_exe_ptr(const q2_exe *e, u32 addr, u32 len)
{
    if (!q2_exe_contains(e, addr, len))
        return NULL;
    return e->file.data + Q2_EXE_HEADER_SIZE +
           (q2_exe_norm(addr) - q2_exe_norm(e->text_addr));
}

bool q2_exe_u8(const q2_exe *e, u32 addr, u8 *out)
{
    const u8 *p = q2_exe_ptr(e, addr, 1);
    if (!p)
        return false;
    if (out)
        *out = *p;
    return true;
}

bool q2_exe_u16(const q2_exe *e, u32 addr, u16 *out)
{
    const u8 *p = q2_exe_ptr(e, addr, 2);
    if (!p)
        return false;
    if (out)
        *out = q2_rd_u16(p);
    return true;
}

bool q2_exe_u32(const q2_exe *e, u32 addr, u32 *out)
{
    const u8 *p = q2_exe_ptr(e, addr, 4);
    if (!p)
        return false;
    if (out)
        *out = q2_rd_u32(p);
    return true;
}

bool q2_exe_s16(const q2_exe *e, u32 addr, s16 *out)
{
    u16 v;
    if (!q2_exe_u16(e, addr, &v))
        return false;
    if (out)
        *out = (s16)v;
    return true;
}
