/*
 * mips.h — an R3000A disassembler, kept deliberately small and structural.
 *
 * This is not here to pretty-print assembly. It is here so that questions about
 * the original engine can be answered by the build system instead of by a
 * screenshot of a disassembler window: "which code reads offset 8 of this
 * record", "what does the loop at this address bound itself on", "who
 * references this table". The decoder therefore exposes fields, not just text —
 * a caller wants `insn.rt` and `insn.imm`, and only sometimes the string.
 *
 * Coverage is the R3000A user-mode set as GCC 2.x emitted it for this console,
 * plus COP0 and the COP2 (GTE) instructions, which appear all over the geometry
 * path and would otherwise disassemble as holes exactly where the interesting
 * code is.
 */
#ifndef Q2PSX_MIPS_H
#define Q2PSX_MIPS_H

#include "q2psx.h"

typedef enum q2_mips_kind {
    Q2_MIPS_OTHER = 0,
    Q2_MIPS_BRANCH,      /* PC-relative, taken target in `target`          */
    Q2_MIPS_JUMP,        /* j / jr                                          */
    Q2_MIPS_CALL,        /* jal / jalr                                      */
    Q2_MIPS_RETURN,      /* jr $ra                                          */
    Q2_MIPS_LOAD,        /* base+imm load,  width in `width`, `sign_extend` */
    Q2_MIPS_STORE,       /* base+imm store                                  */
    Q2_MIPS_INVALID
} q2_mips_kind;

typedef struct q2_mips_insn {
    u32  addr;
    u32  word;

    u8   op;             /* bits 31..26                                     */
    u8   rs, rt, rd;
    u8   shamt;
    u8   funct;
    s32  imm;            /* sign-extended 16-bit immediate                  */
    u32  uimm;           /* zero-extended, for the logical ops              */

    q2_mips_kind kind;
    u32  target;         /* branch/jump destination when known              */
    u8   width;          /* 1, 2 or 4 for loads and stores                  */
    bool sign_extend;    /* lb/lh versus lbu/lhu                            */
    bool has_delay_slot;

    const char *mnemonic;
    char text[64];       /* formatted operands, mnemonic included           */
} q2_mips_insn;

/* Decode one word. Always fills `out`; returns false for an unknown encoding,
 * which is still rendered (as `.word`) so a dump never loses alignment. */
bool q2_mips_decode(u32 word, u32 addr, q2_mips_insn *out);

const char *q2_mips_reg(int r);

/* $gp-relative accesses dominate this build's globals, so callers resolving an
 * effective address need $gp. Returns true and writes the address when `in` is
 * a load or store whose base register holds `base_value`. */
bool q2_mips_effective_addr(const q2_mips_insn *in, u32 base_value, u32 *out);

#endif /* Q2PSX_MIPS_H */
