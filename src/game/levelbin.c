/*
 * levelbin.c — see levelbin.h for the derivation.
 */
#include "levelbin.h"

#include <string.h>

bool q2_levelbin_names_group(const u8 *module, u32 size, const q2_pop_group *g)
{
    u32 i;
    size_t n;

    if (!module || !g || !g->name[0])
        return false;

    n = strlen(g->name);
    if (n == 0 || n > 12 || size < 12)
        return false;

    /*
     * A whole twelve-byte field, NUL-padded. Anchoring on the padding is what
     * stops `Zone1` matching the front of `Zone1Key` — which matters, because a
     * map that ships both would otherwise have the wrong one selected and the
     * error would look like a spawn bug rather than a string bug.
     *
     * A name that fills the field has no padding to check, which is the same
     * rule the Strings chunk's own reader carries: `FoundASecret` is exactly
     * twelve characters and is not terminated there either.
     */
    for (i = 0; i + 12 <= size; i++) {
        if (memcmp(module + i, g->name, n) != 0)
            continue;
        if (n < 12 && module[i + n] != 0)
            continue;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* The call decode                                                            */
/* ------------------------------------------------------------------------- */
#define OP(w)      ((w) >> 26)
#define RS(w)      (((w) >> 21) & 0x1F)
#define RT(w)      (((w) >> 16) & 0x1F)
#define IMM(w)     ((s16)((w) & 0xFFFF))
#define FUNCT(w)   ((w) & 0x3F)

#define OP_LW      0x23u
#define OP_ADDIU   0x09u
#define FN_JALR    0x09u

/* How far back to look for the instruction that set a register. The scheduler
 * moves loads a long way from their use, but not arbitrarily: BASE1's widest
 * gap between the `addiu` that forms the name pointer and the `jalr` that
 * consumes it is three instructions, and glint_scan's worst case was five. */
#define LOOKBACK   200

u32 q2_levelbin_selected_slot(const u8 *module, u32 size, s32 slot_off,
                              u32 *out, u32 max)
{
    u32 i, n = 0;

    if (!module || !out || size < 12)
        return 0;

    for (i = 0; i + 4 <= size; i += 4) {
        u32 w = q2_rd_u32(module + i);
        u32 target, j;
        bool found = false;
        u32 name_at = 0;

        /* `jalr rD` — the call. */
        if (OP(w) != 0 || FUNCT(w) != FN_JALR)
            continue;
        target = RS(w);

        /* Walk back for `lw rD, 40(rB)`, the slot fetch that produced it. A
         * different offset is a different service and not ours. */
        for (j = 4; j <= LOOKBACK && j <= i; j += 4) {
            u32 p = q2_rd_u32(module + i - j);

            if (OP(p) == OP_LW && RT(p) == target) {
                found = (IMM(p) == slot_off);
                break;              /* the nearest load of rD decides */
            }
        }
        if (!found)
            continue;

        /*
         * And back for the `addiu rX, rY, imm` that formed the NAME POINTER.
         *
         * Not `a0`, and that was the first attempt's mistake. BASE1's zone
         * handler at module+0x94 does
         *
         *     801000A0  addiu a3, v0, 12       ; a3 = module + 0xC = "Zone0"
         *     801000A4  lbu   v1, 1(a3)        ; ...and twenty more
         *     801000C4  or    a0, a0, v1       ; assembled into a0/a1/a2
         *
         * so the twelve bytes arrive BY VALUE in three registers and the
         * pointer that produced them is whatever the compiler picked. Taking
         * the nearest `addiu` whose immediate lands on a printable twelve-byte
         * field is what survives that, and it is checkable: the name must be a
         * group the map ships.
         */
        found = false;
        for (j = 4; j <= LOOKBACK && j <= i; j += 4) {
            u32 p = q2_rd_u32(module + i - j);
            s16 off;
            u32 c;
            bool printable = true;

            if (OP(p) != OP_ADDIU)
                continue;

            off = IMM(p);
            if (off <= 0 || (u32)off + 12 > size)
                continue;

            /* A name field: at least one character, then printable or NUL. */
            if (module[off] < 32 || module[off] > 126)
                continue;
            for (c = 0; c < 12; c++) {
                u8 ch = module[off + c];
                if (ch != 0 && (ch < 32 || ch > 126)) {
                    printable = false;
                    break;
                }
            }
            if (!printable)
                continue;

            name_at = (u32)off;
            found   = true;
            break;
        }
        if (!found)
            continue;

        if (n < max)
            out[n] = name_at;
        n++;
    }

    return n;
}

u32 q2_levelbin_selected(const u8 *module, u32 size, u32 *out, u32 max)
{
    return q2_levelbin_selected_slot(module, size,
                                     Q2_LEVELBIN_SLOT_SELECT, out, max);
}
