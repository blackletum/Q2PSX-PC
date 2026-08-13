#include "mips.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const g_reg[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

const char *q2_mips_reg(int r)
{
    return (r >= 0 && r < 32) ? g_reg[r] : "?";
}

/* COP2 (GTE) data and control registers, named as the SDK named them. */
static const char *const g_cop2d[32] = {
    "VXY0","VZ0","VXY1","VZ1","VXY2","VZ2","RGB","OTZ",
    "IR0","IR1","IR2","IR3","SXY0","SXY1","SXY2","SXYP",
    "SZ0","SZ1","SZ2","SZ3","RGB0","RGB1","RGB2","RES1",
    "MAC0","MAC1","MAC2","MAC3","IRGB","ORGB","LZCS","LZCR"
};

static const char *const g_cop2c[32] = {
    "R11R12","R13R21","R22R23","R31R32","R33","TRX","TRY","TRZ",
    "L11L12","L13L21","L22L23","L31L32","L33","RBK","GBK","BBK",
    "LR1LR2","LR3LG1","LG2LG3","LB1LB2","LB3","RFC","GFC","BFC",
    "OFX","OFY","H","DQA","DQB","ZSF3","ZSF4","FLAG"
};

/* GTE opcode, from the low 6 bits of the COP2 command word. Only the commands
 * this generation of tools actually emitted are named; anything else prints as
 * a raw cop2 command so it is still visible. */
static const char *gte_command(u32 fn)
{
    switch (fn) {
    case 0x01: return "rtps";
    case 0x06: return "nclip";
    case 0x0C: return "op";
    case 0x10: return "dpcs";
    case 0x11: return "intpl";
    case 0x12: return "mvmva";
    case 0x13: return "ncds";
    case 0x14: return "cdp";
    case 0x16: return "ncdt";
    case 0x1B: return "nccs";
    case 0x1C: return "cc";
    case 0x1E: return "ncs";
    case 0x20: return "nct";
    case 0x28: return "sqr";
    case 0x29: return "dcpl";
    case 0x2A: return "dpct";
    case 0x2D: return "avsz3";
    case 0x2E: return "avsz4";
    case 0x30: return "rtpt";
    case 0x3D: return "gpf";
    case 0x3E: return "gpl";
    case 0x3F: return "ncct";
    default:   return NULL;
    }
}

static void fmt(q2_mips_insn *in, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsnprintf(in->text, sizeof(in->text), f, ap);
    va_end(ap);
}

static void mem_op(q2_mips_insn *in, const char *mn, u8 width, bool sx,
                   bool store)
{
    in->mnemonic    = mn;
    in->kind        = store ? Q2_MIPS_STORE : Q2_MIPS_LOAD;
    in->width       = width;
    in->sign_extend = sx;
    fmt(in, "%-7s %s, %d(%s)", mn, q2_mips_reg(in->rt), in->imm,
        q2_mips_reg(in->rs));
}

static void branch2(q2_mips_insn *in, const char *mn)
{
    in->mnemonic       = mn;
    in->kind           = Q2_MIPS_BRANCH;
    in->has_delay_slot = true;
    in->target         = in->addr + 4 + (u32)(in->imm * 4);
    fmt(in, "%-7s %s, %s, 0x%08X", mn, q2_mips_reg(in->rs),
        q2_mips_reg(in->rt), in->target);
}

static void branch1(q2_mips_insn *in, const char *mn)
{
    in->mnemonic       = mn;
    in->kind           = Q2_MIPS_BRANCH;
    in->has_delay_slot = true;
    in->target         = in->addr + 4 + (u32)(in->imm * 4);
    fmt(in, "%-7s %s, 0x%08X", mn, q2_mips_reg(in->rs), in->target);
}

static void alu_i(q2_mips_insn *in, const char *mn, bool logical)
{
    in->mnemonic = mn;
    if (logical)
        fmt(in, "%-7s %s, %s, 0x%X", mn, q2_mips_reg(in->rt),
            q2_mips_reg(in->rs), in->uimm);
    else
        fmt(in, "%-7s %s, %s, %d", mn, q2_mips_reg(in->rt),
            q2_mips_reg(in->rs), in->imm);
}

static void alu_r(q2_mips_insn *in, const char *mn)
{
    in->mnemonic = mn;
    fmt(in, "%-7s %s, %s, %s", mn, q2_mips_reg(in->rd), q2_mips_reg(in->rs),
        q2_mips_reg(in->rt));
}

static void shift_i(q2_mips_insn *in, const char *mn)
{
    in->mnemonic = mn;
    fmt(in, "%-7s %s, %s, %u", mn, q2_mips_reg(in->rd), q2_mips_reg(in->rt),
        in->shamt);
}

static void shift_v(q2_mips_insn *in, const char *mn)
{
    in->mnemonic = mn;
    fmt(in, "%-7s %s, %s, %s", mn, q2_mips_reg(in->rd), q2_mips_reg(in->rt),
        q2_mips_reg(in->rs));
}

static bool decode_special(q2_mips_insn *in)
{
    switch (in->funct) {
    case 0x00:
        /* The canonical nop is sll zero, zero, 0 — worth naming, because a
         * dump full of "sll zero, zero, 0" reads as corruption. */
        if (in->word == 0) {
            in->mnemonic = "nop";
            fmt(in, "nop");
        } else {
            shift_i(in, "sll");
        }
        return true;
    case 0x02: shift_i(in, "srl");  return true;
    case 0x03: shift_i(in, "sra");  return true;
    case 0x04: shift_v(in, "sllv"); return true;
    case 0x06: shift_v(in, "srlv"); return true;
    case 0x07: shift_v(in, "srav"); return true;
    case 0x08:
        in->mnemonic       = "jr";
        in->kind           = (in->rs == 31) ? Q2_MIPS_RETURN : Q2_MIPS_JUMP;
        in->has_delay_slot = true;
        fmt(in, "%-7s %s", "jr", q2_mips_reg(in->rs));
        return true;
    case 0x09:
        in->mnemonic       = "jalr";
        in->kind           = Q2_MIPS_CALL;
        in->has_delay_slot = true;
        if (in->rd == 31)
            fmt(in, "%-7s %s", "jalr", q2_mips_reg(in->rs));
        else
            fmt(in, "%-7s %s, %s", "jalr", q2_mips_reg(in->rd),
                q2_mips_reg(in->rs));
        return true;
    case 0x0C:
        in->mnemonic = "syscall";
        fmt(in, "syscall 0x%X", (in->word >> 6) & 0xFFFFF);
        return true;
    case 0x0D:
        in->mnemonic = "break";
        fmt(in, "break   0x%X", (in->word >> 6) & 0xFFFFF);
        return true;
    case 0x10:
        in->mnemonic = "mfhi";
        fmt(in, "%-7s %s", "mfhi", q2_mips_reg(in->rd));
        return true;
    case 0x11:
        in->mnemonic = "mthi";
        fmt(in, "%-7s %s", "mthi", q2_mips_reg(in->rs));
        return true;
    case 0x12:
        in->mnemonic = "mflo";
        fmt(in, "%-7s %s", "mflo", q2_mips_reg(in->rd));
        return true;
    case 0x13:
        in->mnemonic = "mtlo";
        fmt(in, "%-7s %s", "mtlo", q2_mips_reg(in->rs));
        return true;
    case 0x18: case 0x19: case 0x1A: case 0x1B: {
        static const char *const mn[4] = { "mult", "multu", "div", "divu" };
        in->mnemonic = mn[in->funct - 0x18];
        fmt(in, "%-7s %s, %s", in->mnemonic, q2_mips_reg(in->rs),
            q2_mips_reg(in->rt));
        return true;
    }
    case 0x20: alu_r(in, "add");  return true;
    case 0x21: alu_r(in, "addu"); return true;
    case 0x22: alu_r(in, "sub");  return true;
    case 0x23: alu_r(in, "subu"); return true;
    case 0x24: alu_r(in, "and");  return true;
    case 0x25: alu_r(in, "or");   return true;
    case 0x26: alu_r(in, "xor");  return true;
    case 0x27: alu_r(in, "nor");  return true;
    case 0x2A: alu_r(in, "slt");  return true;
    case 0x2B: alu_r(in, "sltu"); return true;
    default:
        return false;
    }
}

static bool decode_regimm(q2_mips_insn *in)
{
    switch (in->rt) {
    case 0x00: branch1(in, "bltz");   return true;
    case 0x01: branch1(in, "bgez");   return true;
    case 0x10: branch1(in, "bltzal"); in->kind = Q2_MIPS_CALL; return true;
    case 0x11: branch1(in, "bgezal"); in->kind = Q2_MIPS_CALL; return true;
    default:
        return false;
    }
}

static bool decode_cop(q2_mips_insn *in, int cop)
{
    u32 rs = in->rs;
    const char *cmd;

    /* Moves between a coprocessor and the CPU share an encoding across COP0
     * and COP2; only the register names differ. */
    if (rs == 0x00 || rs == 0x02 || rs == 0x04 || rs == 0x06) {
        static const char *const mn[8] = {
            "mfc", NULL, "cfc", NULL, "mtc", NULL, "ctc", NULL
        };
        const char *base = mn[rs];
        const char *creg;
        char name[16];

        snprintf(name, sizeof(name), "%s%d", base, cop);
        in->mnemonic = "cop";

        if (cop == 2)
            creg = (rs == 0x02 || rs == 0x06) ? g_cop2c[in->rd] : g_cop2d[in->rd];
        else
            creg = NULL;

        if (creg)
            fmt(in, "%-7s %s, %s", name, q2_mips_reg(in->rt), creg);
        else
            fmt(in, "%-7s %s, $%u", name, q2_mips_reg(in->rt), in->rd);
        return true;
    }

    if (cop == 0 && (in->word & 0x3F) == 0x10) {
        in->mnemonic = "rfe";
        fmt(in, "rfe");
        return true;
    }

    if (cop == 2 && (in->word & (1u << 25))) {
        cmd = gte_command(in->word & 0x3F);
        in->mnemonic = cmd ? cmd : "cop2";
        if (cmd)
            fmt(in, "%-7s (0x%07X)", cmd, in->word & 0x1FFFFFF);
        else
            fmt(in, "%-7s 0x%07X", "cop2", in->word & 0x1FFFFFF);
        return true;
    }

    return false;
}

bool q2_mips_decode(u32 word, u32 addr, q2_mips_insn *out)
{
    q2_mips_insn in;
    bool ok = true;

    memset(&in, 0, sizeof(in));
    in.addr  = addr;
    in.word  = word;
    in.op    = (u8)((word >> 26) & 0x3F);
    in.rs    = (u8)((word >> 21) & 0x1F);
    in.rt    = (u8)((word >> 16) & 0x1F);
    in.rd    = (u8)((word >> 11) & 0x1F);
    in.shamt = (u8)((word >> 6)  & 0x1F);
    in.funct = (u8)(word & 0x3F);
    in.uimm  = word & 0xFFFF;
    in.imm   = (s32)(s16)(word & 0xFFFF);
    in.mnemonic = ".word";

    switch (in.op) {
    case 0x00: ok = decode_special(&in); break;
    case 0x01: ok = decode_regimm(&in);  break;
    case 0x02:
    case 0x03: {
        bool call = (in.op == 0x03);
        in.mnemonic       = call ? "jal" : "j";
        in.kind           = call ? Q2_MIPS_CALL : Q2_MIPS_JUMP;
        in.has_delay_slot = true;
        /* The jump target keeps the top 4 bits of the *delay slot's* address,
         * which for this image is always 0x8. */
        in.target = ((addr + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
        fmt(&in, "%-7s 0x%08X", in.mnemonic, in.target);
        break;
    }
    case 0x04:
        if (in.rs == 0 && in.rt == 0) {
            in.mnemonic       = "b";
            in.kind           = Q2_MIPS_BRANCH;
            in.has_delay_slot = true;
            in.target         = addr + 4 + (u32)(in.imm * 4);
            fmt(&in, "%-7s 0x%08X", "b", in.target);
        } else {
            branch2(&in, "beq");
        }
        break;
    case 0x05: branch2(&in, "bne"); break;
    case 0x06: branch1(&in, "blez"); break;
    case 0x07: branch1(&in, "bgtz"); break;
    case 0x08: alu_i(&in, "addi",  false); break;
    case 0x09: alu_i(&in, "addiu", false); break;
    case 0x0A: alu_i(&in, "slti",  false); break;
    case 0x0B: alu_i(&in, "sltiu", false); break;
    case 0x0C: alu_i(&in, "andi",  true);  break;
    case 0x0D: alu_i(&in, "ori",   true);  break;
    case 0x0E: alu_i(&in, "xori",  true);  break;
    case 0x0F:
        in.mnemonic = "lui";
        fmt(&in, "%-7s %s, 0x%X", "lui", q2_mips_reg(in.rt), in.uimm);
        break;
    case 0x10: ok = decode_cop(&in, 0); break;
    case 0x11: ok = decode_cop(&in, 1); break;
    case 0x12: ok = decode_cop(&in, 2); break;
    case 0x13: ok = decode_cop(&in, 3); break;
    case 0x20: mem_op(&in, "lb",  1, true,  false); break;
    case 0x21: mem_op(&in, "lh",  2, true,  false); break;
    case 0x22: mem_op(&in, "lwl", 4, false, false); break;
    case 0x23: mem_op(&in, "lw",  4, false, false); break;
    case 0x24: mem_op(&in, "lbu", 1, false, false); break;
    case 0x25: mem_op(&in, "lhu", 2, false, false); break;
    case 0x26: mem_op(&in, "lwr", 4, false, false); break;
    case 0x28: mem_op(&in, "sb",  1, false, true);  break;
    case 0x29: mem_op(&in, "sh",  2, false, true);  break;
    case 0x2A: mem_op(&in, "swl", 4, false, true);  break;
    case 0x2B: mem_op(&in, "sw",  4, false, true);  break;
    case 0x2E: mem_op(&in, "swr", 4, false, true);  break;
    /* The GTE transfer forms address memory the same way, but `rt` names a
     * COP2 data register — printing it as a CPU register reads as nonsense in
     * the middle of the geometry loops, which is exactly where they appear. */
    case 0x32:
        mem_op(&in, "lwc2", 4, false, false);
        fmt(&in, "%-7s %s, %d(%s)", "lwc2", g_cop2d[in.rt], in.imm,
            q2_mips_reg(in.rs));
        break;
    case 0x3A:
        mem_op(&in, "swc2", 4, false, true);
        fmt(&in, "%-7s %s, %d(%s)", "swc2", g_cop2d[in.rt], in.imm,
            q2_mips_reg(in.rs));
        break;
    default:
        ok = false;
        break;
    }

    if (!ok) {
        in.kind     = Q2_MIPS_INVALID;
        in.mnemonic = ".word";
        in.width    = 0;
        fmt(&in, "%-7s 0x%08X", ".word", word);
    }

    *out = in;
    return ok;
}

bool q2_mips_effective_addr(const q2_mips_insn *in, u32 base_value, u32 *out)
{
    if (!in || (in->kind != Q2_MIPS_LOAD && in->kind != Q2_MIPS_STORE))
        return false;
    if (out)
        *out = base_value + (u32)in->imm;
    return true;
}
