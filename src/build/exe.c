#include "exe.h"

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

    return Q2_OK;
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
