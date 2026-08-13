/*
 * cmd_exe.c — the executable-side half of the harness.
 *
 * `q2psx-inspect` could already answer questions about bytes on the disc. These
 * commands let it answer questions about the code that read them, which is
 * where every remaining unknown now lives. Nothing here needs a disassembler
 * session or a database: point it at the disc and it reads the boot executable
 * straight out of the ISO.
 */
#include "cmd_exe.h"

#include "exe.h"
#include "mips.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                             */
/* ------------------------------------------------------------------------- */

static bool open_exe(const disc *d, q2_exe *exe)
{
    q2_result r = q2_exe_load(exe, d, NULL);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the boot executable: %s\n",
                q2_result_str(r));
        return false;
    }
    return true;
}

static u32 parse_addr(const char *s)
{
    return (u32)strtoul(s, NULL, 0);
}

/*
 * A printable string at `addr`, or NULL. Used to annotate resolved pointers:
 * half the value of a constant-tracking dump is seeing that the address a
 * function is building is a filename.
 */
static const char *string_at(const q2_exe *e, u32 addr)
{
    static char buf[64];
    const u8 *p = q2_exe_ptr(e, addr, 2);
    size_t i;

    if (!p)
        return NULL;

    for (i = 0; i + 1 < sizeof(buf); i++) {
        u8 c;
        if (!q2_exe_u8(e, addr + (u32)i, &c))
            return NULL;
        if (c == 0)
            break;
        if (c < 0x20 || c > 0x7E)
            return NULL;
        buf[i] = (char)c;
    }
    if (i < 3 || i + 1 >= sizeof(buf))
        return NULL;
    buf[i] = '\0';
    return buf;
}

/*
 * Constant tracking, MIPS-flavoured.
 *
 * This compiler materialises every global address as `lui` plus an `addiu` or
 * `ori`, often dozens of instructions apart, and reaches most globals through
 * $gp. Following that by eye is where disassembly reading time actually goes,
 * so the dump does it: registers hold a known value or nothing, `lui` seeds
 * one, and the arithmetic that consumes it resolves.
 */
typedef struct reg_state {
    u32  value[32];
    bool known[32];
} reg_state;

static void reg_reset(reg_state *st, u32 gp)
{
    memset(st, 0, sizeof(*st));
    st->known[0]  = true;   /* $zero */
    st->value[0]  = 0;
    st->known[28] = true;   /* $gp is set once at entry and never moves */
    st->value[28] = gp;
}

/* Apply one instruction, returning the address it resolved (0 if none) in
 * `*resolved` so the caller can annotate the line. */
static void reg_step(reg_state *st, const q2_mips_insn *in, u32 *resolved)
{
    u32 v = 0;
    bool have = false;

    *resolved = 0;

    switch (in->op) {
    case 0x0F: /* lui */
        st->known[in->rt] = true;
        st->value[in->rt] = in->uimm << 16;
        return;
    case 0x09: /* addiu */
    case 0x08: /* addi  */
        if (st->known[in->rs]) {
            v = st->value[in->rs] + (u32)in->imm;
            have = true;
        }
        if (in->rt != 0) {
            st->known[in->rt] = have;
            st->value[in->rt] = v;
        }
        if (have && in->rs != 0)
            *resolved = v;
        return;
    case 0x0D: /* ori */
        if (st->known[in->rs]) {
            v = st->value[in->rs] | in->uimm;
            have = true;
        }
        if (in->rt != 0) {
            st->known[in->rt] = have;
            st->value[in->rt] = v;
        }
        if (have && in->rs != 0)
            *resolved = v;
        return;
    default:
        break;
    }

    if (in->kind == Q2_MIPS_LOAD || in->kind == Q2_MIPS_STORE) {
        if (st->known[in->rs])
            *resolved = st->value[in->rs] + (u32)in->imm;
        /* A load clobbers its destination. */
        if (in->kind == Q2_MIPS_LOAD && in->rt != 0)
            st->known[in->rt] = false;
        return;
    }

    /* Anything else: invalidate what it writes. `move rd, rs` (addu with
     * $zero) is common enough to be worth propagating. */
    if (in->op == 0x00) {
        u8 rd = in->rd;
        if (rd == 0)
            return;
        if (in->funct == 0x21 && in->rt == 0 && st->known[in->rs]) {
            st->known[rd] = true;
            st->value[rd] = st->value[in->rs];
        } else if (in->funct == 0x21 && in->rs == 0 && st->known[in->rt]) {
            st->known[rd] = true;
            st->value[rd] = st->value[in->rt];
        } else if (in->funct != 0x08 && in->funct != 0x09) {
            st->known[rd] = false;
        }
        return;
    }

    if (in->kind == Q2_MIPS_CALL) {
        int i;
        /* Caller-saved registers do not survive a call; $s0..$s7, $gp, $sp and
         * $fp do. Keeping that distinction is what makes tracking survive the
         * long functions. */
        for (i = 1; i <= 15; i++)
            st->known[i] = false;
        for (i = 24; i <= 25; i++)
            st->known[i] = false;
        st->known[31] = false;
        return;
    }

    if (in->op >= 0x08 && in->op <= 0x0E && in->rt != 0)
        st->known[in->rt] = false;
}

/* ------------------------------------------------------------------------- */
/* exe — header, map and the landmarks FORMATS.md already claims              */
/* ------------------------------------------------------------------------- */

typedef struct landmark {
    u32         addr;
    const char *expect;    /* mnemonic the documentation implies, or NULL */
    const char *what;
} landmark;

int cmd_exe(const disc *d, const char *save_path)
{
    q2_exe exe;
    int i, ok = 0, bad = 0;

    /*
     * Each entry is a claim already made in docs/FORMATS.md, restated as
     * something the build can check. If one of these ever stops matching, the
     * documentation is describing a different executable than the disc holds.
     */
    static const landmark marks[] = {
        { 0x80068A58, NULL,   "img_open — VRAM image loader"              },
        { 0x80068BD0, "lbu",  "PackBits control byte fetch"               },
        { 0x80068C7C, "bgtz", "PackBits loop, bounded on output remaining" },
        { 0x80069214, "srl",  "upload rect width: bytes >> 1"             },
        { 0x80076628, NULL,   "texture-page loader"                       },
        { 0x80076378, NULL,   "4bpp CLUT id builder"                      },
        { 0x800279F8, NULL,   "Events item loop bound"                    },
        { 0x80026DC0, NULL,   "Events load-time pre-pass"                 },
        { 0x8002DC04, NULL,   "per-frame handler call site (obj+0x2C)"    },
    };

    if (!open_exe(d, &exe))
        return 1;

    printf("executable   : %s\n", exe.name);
    printf("size         : %zu bytes\n", exe.file.size);
    printf("entry point  : 0x%08X\n", exe.pc0);
    printf("initial $gp  : 0x%08X\n", exe.gp0);
    printf("text segment : 0x%08X .. 0x%08X (%u bytes)\n",
           exe.text_addr, q2_exe_end(&exe), exe.text_size);
    if (exe.bss_size)
        printf("bss          : 0x%08X .. 0x%08X (%u bytes)\n",
               exe.bss_addr, exe.bss_addr + exe.bss_size, exe.bss_size);
    printf("stack        : 0x%08X (%u bytes)\n", exe.sp_base, exe.sp_size);

    printf("\nlandmarks (documented in FORMATS.md):\n");
    for (i = 0; i < (int)(sizeof(marks) / sizeof(marks[0])); i++) {
        const landmark *m = &marks[i];
        q2_mips_insn in;
        u32 word;
        bool mapped = q2_exe_u32(&exe, m->addr, &word);
        bool match;

        if (!mapped) {
            printf("  %08X  UNMAPPED  %s\n", m->addr, m->what);
            bad++;
            continue;
        }

        q2_mips_decode(word, m->addr, &in);
        match = !m->expect || strcmp(in.mnemonic, m->expect) == 0;
        printf("  %08X  %-28s %s%s\n", m->addr, in.text, m->what,
               match ? "" : "   << MISMATCH");
        if (match)
            ok++;
        else
            bad++;
    }

    printf("\n%d landmark%s confirmed, %d failed\n", ok, ok == 1 ? "" : "s", bad);

    /*
     * Write the loaded segment out, header stripped, so that a raw import at
     * text_addr in any external disassembler maps one to one with the
     * addresses used throughout the documentation. Writing the whole file
     * instead would shift every address by 0x800, which is exactly the sort of
     * silent off-by-header that makes two passes disagree.
     */
    if (save_path) {
        const u8 *seg = q2_exe_ptr(&exe, exe.text_addr, exe.text_size);
        FILE *fp = seg ? fopen(save_path, "wb") : NULL;

        if (!fp) {
            fprintf(stderr, "cannot write '%s'\n", save_path);
            bad++;
        } else {
            size_t written = fwrite(seg, 1, exe.text_size, fp);
            fclose(fp);
            printf("\nwrote %s: %zu bytes, load address 0x%08X\n",
                   save_path, written, exe.text_addr);
            if (written != exe.text_size)
                bad++;
        }
    }

    q2_exe_free(&exe);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* disasm                                                                     */
/* ------------------------------------------------------------------------- */

int cmd_disasm(const disc *d, const char *addr_s, int count)
{
    q2_exe exe;
    reg_state st;
    u32 addr;
    int i;
    bool to_end = (count <= 0);

    if (!open_exe(d, &exe))
        return 1;

    addr = parse_addr(addr_s);
    if (!q2_exe_contains(&exe, addr, 4)) {
        fprintf(stderr, "0x%08X is outside the loaded segment 0x%08X..0x%08X\n",
                addr, q2_exe_begin(&exe), q2_exe_end(&exe));
        q2_exe_free(&exe);
        return 1;
    }

    if (to_end)
        count = 4096;   /* a hard cap: "to the end of the function" must end */

    reg_reset(&st, exe.gp0);

    for (i = 0; i < count; i++) {
        u32 a = addr + (u32)i * 4, word, resolved = 0;
        q2_mips_insn in;
        const char *str;

        if (!q2_exe_u32(&exe, a, &word))
            break;

        q2_mips_decode(word, a, &in);
        reg_step(&st, &in, &resolved);

        printf("%08X  %08X  %s", a, word, in.text);

        if (resolved) {
            printf("   ; 0x%08X", resolved);
            /* Only label a $gp offset when the instruction actually addressed
             * through $gp — otherwise every constant that happens to land in
             * the same 64 KiB window acquires a spurious gp-relative reading. */
            if (in.rs == 28 && (in.kind == Q2_MIPS_LOAD || in.kind == Q2_MIPS_STORE))
                printf(" (gp%+d)", (int)(resolved - exe.gp0));
            str = string_at(&exe, resolved);
            if (str)
                printf(" \"%s\"", str);
        }
        printf("\n");

        /* Stop at the delay slot of a return, which is where a function ends
         * unless it has tail-merged epilogues — and this compiler does not. */
        if (to_end && in.kind == Q2_MIPS_RETURN) {
            u32 slot;
            if (q2_exe_u32(&exe, a + 4, &slot)) {
                q2_mips_insn ds;
                q2_mips_decode(slot, a + 4, &ds);
                printf("%08X  %08X  %s\n", a + 4, slot, ds.text);
            }
            break;
        }
    }

    q2_exe_free(&exe);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* xrefs                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Four ways this build refers to an address, all of which have to be swept for
 * or the answer is misleadingly empty:
 *
 *   1. jal / j            — a direct call or tail jump.
 *   2. lui + addiu/ori    — a materialised constant, the halves usually apart.
 *   3. $gp + imm16        — the common case for globals in this build.
 *   4. a plain word       — a function pointer in a dispatch table.
 */
int cmd_xrefs(const disc *d, const char *addr_s)
{
    q2_exe exe;
    u32 want, a, begin, end;
    int n_call = 0, n_const = 0, n_gp = 0, n_word = 0;
    /* The high half a `lui` would carry for this address, accounting for the
     * sign extension of the following addiu. */
    u16 hi, hi_alt;

    if (!open_exe(d, &exe))
        return 1;

    want  = parse_addr(addr_s);
    begin = q2_exe_begin(&exe);
    end   = q2_exe_end(&exe);

    hi     = (u16)(want >> 16);
    hi_alt = (u16)((want >> 16) + 1);   /* when the low half is negative */

    printf("references to 0x%08X\n", want);

    /* 1 + 2 + 3: a linear code sweep with per-register high halves. */
    {
        u32 pending_val[32];
        bool pending[32];
        memset(pending, 0, sizeof(pending));
        memset(pending_val, 0, sizeof(pending_val));

        for (a = begin; a + 4 <= end; a += 4) {
            q2_mips_insn in;
            u32 word;

            if (!q2_exe_u32(&exe, a, &word))
                break;
            q2_mips_decode(word, a, &in);

            if (in.kind == Q2_MIPS_CALL && in.op == 0x03 && in.target == want) {
                printf("  call  %08X  jal\n", a);
                n_call++;
            } else if (in.op == 0x02 && in.target == want) {
                printf("  jump  %08X  j\n", a);
                n_call++;
            }

            if (in.op == 0x0F) {           /* lui */
                pending[in.rt]     = (in.uimm == hi || in.uimm == hi_alt);
                pending_val[in.rt] = in.uimm << 16;
                continue;
            }

            if ((in.op == 0x09 || in.op == 0x08 || in.op == 0x0D) &&
                pending[in.rs]) {
                u32 v = (in.op == 0x0D) ? (pending_val[in.rs] | in.uimm)
                                        : (pending_val[in.rs] + (u32)in.imm);
                if (v == want) {
                    printf("  const %08X  %s\n", a, in.text);
                    n_const++;
                }
            }

            if ((in.kind == Q2_MIPS_LOAD || in.kind == Q2_MIPS_STORE)) {
                if (pending[in.rs] && pending_val[in.rs] + (u32)in.imm == want) {
                    printf("  mem   %08X  %s\n", a, in.text);
                    n_const++;
                } else if (in.rs == 28 && exe.gp0 + (u32)in.imm == want) {
                    printf("  gp    %08X  %s\n", a, in.text);
                    n_gp++;
                }
            }

            /* A register written by anything else no longer holds a high
             * half; not tracking that produces false positives that cost more
             * time than the missed reference would. */
            if (in.op == 0x0F)
                continue;
            if (in.kind == Q2_MIPS_LOAD && in.rt < 32)
                pending[in.rt] = false;
            else if (in.op >= 0x08 && in.op <= 0x0E && in.rt < 32 && in.rt != in.rs)
                pending[in.rt] = false;
            else if (in.op == 0x00 && in.rd < 32)
                pending[in.rd] = false;
        }
    }

    /* 4: the whole image again, as data. */
    for (a = begin; a + 4 <= end; a += 4) {
        u32 word;
        if (!q2_exe_u32(&exe, a, &word))
            break;
        if (word == want) {
            q2_mips_insn in;
            q2_mips_decode(word, a, &in);
            printf("  word  %08X  0x%08X\n", a, word);
            n_word++;
        }
    }

    printf("\n%d call%s, %d materialised constant%s, %d gp-relative, "
           "%d raw word%s\n",
           n_call, n_call == 1 ? "" : "s",
           n_const, n_const == 1 ? "" : "s",
           n_gp,
           n_word, n_word == 1 ? "" : "s");

    q2_exe_free(&exe);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* bytes / find                                                               */
/* ------------------------------------------------------------------------- */

int cmd_bytes(const disc *d, const char *addr_s, int count)
{
    q2_exe exe;
    u32 addr;
    int row;

    if (!open_exe(d, &exe))
        return 1;

    addr = parse_addr(addr_s);
    if (count <= 0)
        count = 128;

    for (row = 0; row < count; row += 16) {
        int i, n = (count - row < 16) ? count - row : 16;
        printf("%08X ", addr + (u32)row);
        for (i = 0; i < 16; i++) {
            u8 b;
            if (i < n && q2_exe_u8(&exe, addr + (u32)(row + i), &b))
                printf(" %02X", b);
            else
                printf("   ");
        }
        printf("  ");
        for (i = 0; i < n; i++) {
            u8 b;
            if (!q2_exe_u8(&exe, addr + (u32)(row + i), &b))
                break;
            putchar((b >= 0x20 && b < 0x7F) ? (char)b : '.');
        }
        printf("\n");
    }

    q2_exe_free(&exe);
    return 0;
}

/* `pattern` is ASCII unless it starts with "0x", in which case it is a hex
 * byte string. Both forms matter: one finds a chunk name, the other finds a
 * constant a function is comparing against. */
int cmd_find(const disc *d, const char *pattern)
{
    q2_exe exe;
    u8 needle[64];
    size_t len = 0;
    u32 a, begin, end;
    int hits = 0;

    if (!open_exe(d, &exe))
        return 1;

    if (pattern[0] == '0' && (pattern[1] == 'x' || pattern[1] == 'X')) {
        const char *p = pattern + 2;
        while (p[0] && p[1] && len < sizeof(needle)) {
            char tmp[3] = { p[0], p[1], 0 };
            needle[len++] = (u8)strtoul(tmp, NULL, 16);
            p += 2;
        }
    } else {
        while (pattern[len] && len < sizeof(needle)) {
            needle[len] = (u8)pattern[len];
            len++;
        }
    }

    if (len == 0) {
        fprintf(stderr, "empty pattern\n");
        q2_exe_free(&exe);
        return 1;
    }

    begin = q2_exe_begin(&exe);
    end   = q2_exe_end(&exe);

    for (a = begin; a + len <= end; a++) {
        const u8 *p = q2_exe_ptr(&exe, a, (u32)len);
        if (p && memcmp(p, needle, len) == 0) {
            printf("  0x%08X\n", a);
            hits++;
        }
    }

    printf("%d occurrence%s\n", hits, hits == 1 ? "" : "s");
    q2_exe_free(&exe);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Function attribution                                                       */
/* ------------------------------------------------------------------------- */

/*
 * There is no symbol table, so a function start is defined as "an address
 * something calls". That is exact for everything reachable by jal, which is
 * how this engine calls anything worth finding, and it is enough to say which
 * function an interesting instruction lives in.
 */
typedef struct func_map {
    u32 *start;
    u32  count;
} func_map;

static void func_map_build(const q2_exe *e, func_map *m)
{
    u32 a, begin = q2_exe_begin(e), end = q2_exe_end(e);
    u32 cap = 0;

    m->start = NULL;
    m->count = 0;

    for (a = begin; a + 4 <= end; a += 4) {
        q2_mips_insn in;
        u32 word;

        if (!q2_exe_u32(e, a, &word))
            break;
        q2_mips_decode(word, a, &in);
        if (in.kind != Q2_MIPS_CALL || in.op != 0x03)
            continue;
        if (!q2_exe_contains(e, in.target, 4))
            continue;

        if (m->count == cap) {
            u32 ncap = cap ? cap * 2 : 256;
            u32 *n = (u32 *)realloc(m->start, ncap * sizeof(u32));
            if (!n)
                return;
            m->start = n;
            cap = ncap;
        }
        m->start[m->count++] = in.target;
    }

    /* Sort ascending, then unique — an insertion-order list would make the
     * lookup below wrong rather than slow. */
    {
        u32 i, j, w = 0;
        for (i = 1; i < m->count; i++) {
            u32 v = m->start[i];
            for (j = i; j > 0 && m->start[j - 1] > v; j--)
                m->start[j] = m->start[j - 1];
            m->start[j] = v;
        }
        for (i = 0; i < m->count; i++)
            if (i == 0 || m->start[i] != m->start[i - 1])
                m->start[w++] = m->start[i];
        m->count = w;
    }
}

static u32 func_map_owner(const func_map *m, u32 addr)
{
    u32 lo = 0, hi = m->count, best = 0;

    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (m->start[mid] <= addr) {
            best = m->start[mid];
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best;
}

/* ------------------------------------------------------------------------- */
/* access — every instruction that touches a given structure offset           */
/* ------------------------------------------------------------------------- */

/*
 * The workhorse for structure archaeology. "Which code reads +8 of a record"
 * is the question that identifies a consumer, and grouping the answer by
 * function turns a list of addresses into a short list of candidates.
 */
int cmd_access(const disc *d, const char *imm_s, const char *mnemonic)
{
    q2_exe exe;
    func_map fm;
    s32 want;
    u32 a, begin, end, last_func = 0;
    int hits = 0, funcs = 0;

    if (!open_exe(d, &exe))
        return 1;

    want  = (s32)strtol(imm_s, NULL, 0);
    begin = q2_exe_begin(&exe);
    end   = q2_exe_end(&exe);
    func_map_build(&exe, &fm);

    for (a = begin; a + 4 <= end; a += 4) {
        q2_mips_insn in;
        u32 word, owner;

        if (!q2_exe_u32(&exe, a, &word))
            break;
        q2_mips_decode(word, a, &in);

        if (in.kind != Q2_MIPS_LOAD && in.kind != Q2_MIPS_STORE)
            continue;
        if (in.imm != want)
            continue;
        if (in.rs == 28 || in.rs == 29)
            continue;   /* $gp and $sp are frames, not records */
        if (mnemonic && strcmp(in.mnemonic, mnemonic) != 0)
            continue;

        owner = func_map_owner(&fm, a);
        if (owner != last_func) {
            printf("\nfunction 0x%08X:\n", owner);
            last_func = owner;
            funcs++;
        }
        printf("  %08X  %s\n", a, in.text);
        hits++;
    }

    printf("\n%d access%s in %d function%s\n", hits, hits == 1 ? "" : "es",
           funcs, funcs == 1 ? "" : "s");

    free(fm.start);
    q2_exe_free(&exe);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* funcs — where the functions are                                            */
/* ------------------------------------------------------------------------- */

/*
 * There is no symbol table, so function starts are inferred: an address is a
 * function start if something calls it. That is exact for everything reachable
 * by `jal` and by function-pointer tables, which between them is how this
 * engine calls anything worth finding.
 */
int cmd_funcs(const disc *d, const char *filter)
{
    q2_exe exe;
    u32 a, begin, end;
    u8 *is_target;
    u32 n_words, i, count = 0;
    u32 want = filter ? parse_addr(filter) : 0;

    if (!open_exe(d, &exe))
        return 1;

    begin   = q2_exe_begin(&exe);
    end     = q2_exe_end(&exe);
    n_words = (end - begin) / 4;

    is_target = (u8 *)calloc(n_words, 1);
    if (!is_target) {
        q2_exe_free(&exe);
        return 1;
    }

    for (a = begin; a + 4 <= end; a += 4) {
        q2_mips_insn in;
        u32 word;

        if (!q2_exe_u32(&exe, a, &word))
            break;
        q2_mips_decode(word, a, &in);

        if (in.kind == Q2_MIPS_CALL && in.op == 0x03) {
            u32 t = q2_exe_norm(in.target);
            if (t >= q2_exe_norm(begin) && t < q2_exe_norm(end))
                is_target[(t - q2_exe_norm(begin)) / 4] |= 1;
        }
        /* Words that look like code addresses are dispatch-table entries. */
        if (word >= begin && word < end && (word & 3) == 0)
            is_target[(q2_exe_norm(word) - q2_exe_norm(begin)) / 4] |= 2;
    }

    printf("%-10s %-6s %s\n", "address", "callers", "first instruction");
    for (i = 0; i < n_words; i++) {
        u32 addr = begin + i * 4, word;
        q2_mips_insn in;

        if (!(is_target[i] & 1))
            continue;
        if (want && addr != want)
            continue;
        if (!q2_exe_u32(&exe, addr, &word))
            continue;
        q2_mips_decode(word, addr, &in);
        printf("0x%08X %-6s %s\n", addr, (is_target[i] & 2) ? "+table" : "",
               in.text);
        count++;
    }

    printf("\n%u call target%s\n", count, count == 1 ? "" : "s");

    free(is_target);
    q2_exe_free(&exe);
    return 0;
}
