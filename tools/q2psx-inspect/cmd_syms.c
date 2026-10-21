/*
 * cmd_syms.c — the linker's own symbol table.
 *
 * `q2psx-inspect funcs` finds function starts by sweeping the image for call
 * targets, which is what you do when there is no symbol table. On the North
 * American disc there is one: `/MAIN.SYM`, 3.3 MB of Psy-Q `MND` v1, left in
 * the build by whoever cut the master. It names every function in the
 * executable and says which C file it came from.
 *
 * That turns a question this project asks constantly — "has anyone looked at
 * this code?" — from a judgement into a lookup. Sweep the tree for the
 * addresses it cites, subtract, and what remains is the executable's
 * unexamined half, named and grouped by the module it belongs to.
 *
 * The format is a flat stream of records, each `u32 address`, `u8 type`, then
 * a payload whose length the type decides. There is no index and no record
 * length, so a type whose payload size is guessed wrong does not produce one
 * bad record — it desynchronises everything after it, and the damage looks
 * like data rather than like a parse error. The reader below therefore stops
 * dead on a type it does not know rather than resynchronising, and the summary
 * reports how much of the file it consumed: the check that this is being read
 * correctly at all is that the last record ends on the last byte.
 */
#include "cmd_syms.h"

#include "exe.h"
#include "q2psx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYM_PATH "/MAIN.SYM"

/* A function record's two strings are bounded by the format's length byte. */
#define SYM_NAME_MAX 256

typedef struct sym_func {
    u32  addr;                    /* in this build's own space          */
    u32  pal;                     /* translated into SLES-01534's       */
    u32  line;                    /* first line of the body             */
    u32  fsize;                   /* stack frame bytes                  */
    char file[SYM_NAME_MAX];      /* as the compiler saw it, with path  */
    char name[SYM_NAME_MAX];
} sym_func;

/*
 * The other kind of record: an address and a name, with no file and no line.
 * Most name a function again, but the ones past the last named function are
 * the other half of what the table knows: the Psy-Q library entry points the
 * game calls (`DrawOTag`, `VSync`, `rand`), and then the globals —
 * `ScreenYOff`, `CreColList`, the tables this project has been reading by
 * address since before it could name them. `data` prints that band, marking
 * the ones in the BSS, whose start the table gives as `_bss_org`.
 */
typedef struct sym_name {
    u32  addr;
    u32  pal;
    char name[SYM_NAME_MAX];
} sym_name;

typedef struct sym_table {
    sym_func *funcs;
    u32       func_count;
    u32       func_cap;
    sym_name *syms;
    u32       sym_count;
    u32       sym_cap;
    u32       records;
    u32       consumed;           /* bytes the walk accounted for       */
    u32       size;
    u32       text_end;           /* first address past the code        */
} sym_table;

/* ------------------------------------------------------------------------- */
/* Reading                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct sym_reader {
    const u8 *p;
    size_t    n;
    size_t    at;
    bool      bad;
} sym_reader;

static u8 rd_u8(sym_reader *r)
{
    if (r->at + 1 > r->n) { r->bad = true; return 0; }
    return r->p[r->at++];
}

static u16 rd_u16(sym_reader *r)
{
    u16 v;
    if (r->at + 2 > r->n) { r->bad = true; return 0; }
    v = (u16)(r->p[r->at] | ((u16)r->p[r->at + 1] << 8));
    r->at += 2;
    return v;
}

static u32 rd_u32(sym_reader *r)
{
    u32 v;
    if (r->at + 4 > r->n) { r->bad = true; return 0; }
    v = (u32)r->p[r->at] | ((u32)r->p[r->at + 1] << 8) |
        ((u32)r->p[r->at + 2] << 16) | ((u32)r->p[r->at + 3] << 24);
    r->at += 4;
    return v;
}

/* A counted string: one length byte, then that many characters, no NUL. */
static void rd_str(sym_reader *r, char *out, size_t cap)
{
    u8     len = rd_u8(r);
    size_t i;

    if (r->bad || r->at + len > r->n) { r->bad = true; if (cap) out[0] = '\0'; return; }
    for (i = 0; i < len && i + 1 < cap; i++)
        out[i] = (char)r->p[r->at + i];
    if (cap)
        out[(i < cap) ? i : cap - 1] = '\0';
    r->at += len;
}

static void rd_skip_str(sym_reader *r)
{
    char scratch[SYM_NAME_MAX];
    rd_str(r, scratch, sizeof scratch);
}

static bool push_sym(sym_table *t, const sym_name *n)
{
    if (t->sym_count == t->sym_cap) {
        u32       cap = t->sym_cap ? t->sym_cap * 2 : 256;
        sym_name *grown = (sym_name *)realloc(t->syms, (size_t)cap * sizeof *grown);
        if (!grown)
            return false;
        t->syms = grown;
        t->sym_cap = cap;
    }
    t->syms[t->sym_count++] = *n;
    return true;
}

static bool push_func(sym_table *t, const sym_func *f)
{
    if (t->func_count == t->func_cap) {
        u32       cap = t->func_cap ? t->func_cap * 2 : 256;
        sym_func *grown = (sym_func *)realloc(t->funcs, (size_t)cap * sizeof *grown);
        if (!grown)
            return false;
        t->funcs = grown;
        t->func_cap = cap;
    }
    t->funcs[t->func_count++] = *f;
    return true;
}

/*
 * The record types this build's table actually uses. Everything above 0x80 is
 * the line-number stream the debugger walked to map an address to a source
 * line; only 0x8C, the function header, carries anything this command wants,
 * but all of them have to be sized correctly to stay in step.
 */
static bool sym_parse(sym_table *t, const u8 *data, size_t size, const q2_exe *exe)
{
    sym_reader r;

    memset(&r, 0, sizeof r);
    r.p = data;
    r.n = size;
    r.at = 8;                       /* "MND", version, and four reserved */
    t->size = (u32)size;

    while (r.at + 5 <= r.n && !r.bad) {
        size_t start = r.at;
        u8     type;

        (void)rd_u32(&r);           /* the record's address              */
        type = rd_u8(&r);

        switch (type) {
        case 0x01:                  /* a global, a local, or a static    */
        case 0x02:
        case 0x05:
        case 0x06: {
            sym_name n;
            memset(&n, 0, sizeof n);
            rd_str(&r, n.name, sizeof n.name);
            n.addr = (u32)(data[start] | ((u32)data[start + 1] << 8) |
                           ((u32)data[start + 2] << 16) | ((u32)data[start + 3] << 24));
            n.pal = q2_exe_pal(exe, n.addr);
            if (r.bad || !push_sym(t, &n))
                return false;
            break;
        }

        case 0x80:                  /* line-number stream: +1            */
            break;
        case 0x82: (void)rd_u8(&r);  break;   /* +n, byte                */
        case 0x84: (void)rd_u16(&r); break;   /* +n, halfword            */
        case 0x86: (void)rd_u32(&r); break;   /* set line                */
        case 0x88:                            /* set file and line       */
            (void)rd_u32(&r);
            rd_skip_str(&r);
            break;
        case 0x8A:                  /* end of the stream for this file   */
            break;

        case 0x8C: {                /* function start                    */
            sym_func f;
            memset(&f, 0, sizeof f);
            (void)rd_u16(&r);       /* frame pointer register            */
            f.fsize = rd_u32(&r);
            (void)rd_u16(&r);       /* return address register           */
            (void)rd_u32(&r);       /* saved-register mask               */
            (void)rd_u32(&r);       /* ...and where it is saved          */
            f.line = rd_u32(&r);
            rd_str(&r, f.file, sizeof f.file);
            rd_str(&r, f.name, sizeof f.name);
            f.addr = (u32)(data[start] | ((u32)data[start + 1] << 8) |
                           ((u32)data[start + 2] << 16) | ((u32)data[start + 3] << 24));
            f.pal = q2_exe_pal(exe, f.addr);
            if (r.bad || !push_func(t, &f))
                return false;
            break;
        }

        case 0x8E:                  /* function end                      */
        case 0x90:                  /* block start                       */
        case 0x92:                  /* block end                         */
            (void)rd_u32(&r);
            break;

        case 0x94:                  /* a definition: class, type, size   */
            (void)rd_u32(&r);
            (void)rd_u32(&r);
            rd_skip_str(&r);
            break;

        case 0x96: {                /* ...one with array dimensions      */
            u16 dims;
            (void)rd_u16(&r);       /* storage class                     */
            (void)rd_u16(&r);       /* type                              */
            (void)rd_u32(&r);       /* size in bytes                     */
            dims = rd_u16(&r);
            while (dims-- && !r.bad)
                (void)rd_u32(&r);
            rd_skip_str(&r);        /* the struct/union/enum tag         */
            rd_skip_str(&r);        /* the name                          */
            break;
        }

        default:
            fprintf(stderr,
                    "%s: unknown record type 0x%02X at offset %lu — stopping "
                    "rather than guessing its length\n",
                    SYM_PATH, type, (unsigned long)start);
            return false;
        }

        t->records++;
    }

    t->consumed = (u32)r.at;
    return !r.bad;
}

/* ------------------------------------------------------------------------- */
/* Reporting                                                                  */
/* ------------------------------------------------------------------------- */

/* "C:\Castle\QPLAYER.C" is how the compiler saw it; the leaf is what matters. */
static const char *leaf(const char *path)
{
    const char *s = path;
    const char *p;

    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            s = p + 1;
    return s;
}

static int by_addr(const void *a, const void *b)
{
    const sym_func *x = (const sym_func *)a, *y = (const sym_func *)b;
    return (x->pal < y->pal) ? -1 : (x->pal > y->pal) ? 1 : 0;
}

typedef struct file_row {
    const char *name;
    u32         count;
} file_row;

static int by_count(const void *a, const void *b)
{
    const file_row *x = (const file_row *)a, *y = (const file_row *)b;
    if (x->count != y->count)
        return (x->count < y->count) ? 1 : -1;
    return strcmp(x->name, y->name);
}

static u32 count_past_code(const sym_table *t)
{
    u32 n = 0, i;
    for (i = 0; i < t->sym_count; i++)
        if (t->syms[i].pal > t->text_end)
            n++;
    return n;
}

/* The BSS start, as the linker's own `_bss_org` gives it. 0 if it is absent. */
static u32 find_bss(const sym_table *t)
{
    u32 i;
    for (i = 0; i < t->sym_count; i++)
        if (strcmp(t->syms[i].name, "_bss_org") == 0)
            return t->syms[i].pal;
    return 0;
}

static int by_sym_addr(const void *a, const void *b)
{
    const sym_name *x = (const sym_name *)a, *y = (const sym_name *)b;
    if (x->pal != y->pal)
        return (x->pal < y->pal) ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void print_census(const sym_table *t)
{
    file_row *rows = (file_row *)calloc(t->func_count, sizeof *rows);
    u32       n = 0, i, j;

    if (!rows)
        return;

    for (i = 0; i < t->func_count; i++) {
        const char *f = leaf(t->funcs[i].file);
        for (j = 0; j < n; j++)
            if (strcmp(rows[j].name, f) == 0)
                break;
        if (j == n)
            rows[n++].name = f;
        rows[j].count++;
    }
    qsort(rows, n, sizeof *rows, by_count);

    printf("\n%u source files:\n\n", n);
    for (i = 0; i < n; i++)
        printf("  %-16s %4u\n", rows[i].name, rows[i].count);
    free(rows);
}

static void print_sym(const sym_name *n, u32 bss)
{
    const char *band = (bss && n->pal >= bss) ? " bss" : "";

    if (n->pal != n->addr)
        printf("  0x%08X  (0x%08X)  %-40s%s\n", n->pal, n->addr, n->name, band);
    else
        printf("  0x%08X  %-40s%s\n", n->pal, n->name, band);
}

static void print_one(const sym_func *f, bool both)
{
    if (both && f->pal != f->addr)
        printf("  0x%08X  (0x%08X)  %-16s %-36s line %u\n",
               f->pal, f->addr, leaf(f->file), f->name, f->line);
    else
        printf("  0x%08X  %-16s %-36s line %u\n",
               f->pal, leaf(f->file), f->name, f->line);
}

/* Case-insensitive substring, so `syms <disc> damage` finds `T_Damage`. */
static bool name_matches(const char *hay, const char *needle)
{
    size_t hn = strlen(hay), nn = strlen(needle), i, j;

    if (nn > hn)
        return false;
    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) {
            int a = (int)(unsigned char)hay[i + j], b = (int)(unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b)
                break;
        }
        if (j == nn)
            return true;
    }
    return false;
}

int cmd_syms(const disc *d, const char *what)
{
    q2_buf     buf;
    q2_exe     exe;
    sym_table  t;
    q2_result  r;
    bool       ok;
    u32        i, bss;

    memset(&t, 0, sizeof t);

    r = disc_read_file(d, SYM_PATH, &buf);
    if (r != Q2_OK) {
        fprintf(stderr,
                "no %s on this disc. The linker's symbol table ships on the "
                "North American release (SLUS-00757) and on no other catalogued "
                "build; point this command at that disc.\n", SYM_PATH);
        return 1;
    }
    if (buf.size < 8 || memcmp(buf.data, "MND", 3) != 0) {
        fprintf(stderr, "%s is not a Psy-Q MND symbol table\n", SYM_PATH);
        q2_buf_free(&buf);
        return 1;
    }

    r = q2_exe_load(&exe, d, NULL);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the boot executable: %s\n", q2_result_str(r));
        q2_buf_free(&buf);
        return 1;
    }

    ok = sym_parse(&t, buf.data, buf.size, &exe);
    qsort(t.funcs, t.func_count, sizeof *t.funcs, by_addr);
    qsort(t.syms, t.sym_count, sizeof *t.syms, by_sym_addr);

    /*
     * Where the code stops. The executable header gives one segment covering
     * code, rodata and BSS alike, so the boundary comes from the table itself:
     * past the last named function there is nothing left but data. A symbol
     * below it is a label or a static inside some function's body; one above
     * it is a global, and those are what this project has spent its time
     * reading by address.
     */
    for (i = 0; i < t.func_count; i++)
        if (t.funcs[i].pal > t.text_end)
            t.text_end = t.funcs[i].pal;
    bss = find_bss(&t);

    if (!what || strcmp(what, "list") == 0 || strcmp(what, "data") == 0) {
        printf("%s — MND v%u, %lu bytes\n", SYM_PATH, buf.data[3],
               (unsigned long)buf.size);
        printf("  %u records, %u functions with a source file, %u symbols\n",
               t.records, t.func_count, t.sym_count);
        printf("  %u symbols sit past the last named function: the library "
               "entry points and the globals\n", count_past_code(&t));
        printf("  %s: the walk %s at %u of %u bytes\n",
               ok && t.consumed == t.size ? "clean" : "INCOMPLETE",
               ok && t.consumed == t.size ? "ended" : "stopped",
               t.consumed, t.size);
        if (!q2_exe_has_layout(&exe))
            printf("  addresses are this build's own\n");
        else
            printf("  addresses translated into SLES-01534's space, this "
                   "build's in brackets\n");
    }

    if (!what) {
        print_census(&t);
    } else if (strcmp(what, "list") == 0) {
        putchar('\n');
        for (i = 0; i < t.func_count; i++)
            print_one(&t.funcs[i], true);
    } else if (strcmp(what, "data") == 0) {
        putchar('\n');
        for (i = 0; i < t.sym_count; i++)
            if (t.syms[i].pal > t.text_end)
                print_sym(&t.syms[i], bss);
    } else {
        u32 addr = 0, hits = 0;
        if (what[0] == '0' && (what[1] == 'x' || what[1] == 'X'))
            addr = (u32)strtoul(what, NULL, 0);
        for (i = 0; i < t.func_count; i++) {
            const sym_func *f = &t.funcs[i];
            bool hit = addr ? (f->pal == addr || f->addr == addr)
                            : (name_matches(f->name, what) ||
                               name_matches(leaf(f->file), what));
            if (hit) {
                if (!hits++)
                    printf("%s:\n", what);
                print_one(f, true);
            }
        }
        /* Then the globals, which is where most address questions end up. */
        for (i = 0; i < t.sym_count; i++) {
            const sym_name *n = &t.syms[i];
            if (n->pal <= t.text_end)
                continue;
            if (addr ? (n->pal == addr || n->addr == addr)
                     : name_matches(n->name, what)) {
                if (!hits++)
                    printf("%s:\n", what);
                print_sym(n, bss);
            }
        }
        if (!hits) {
            printf("%s: no function, source file, global or address of that "
                   "name\n", what);
            free(t.funcs);
            free(t.syms);
            q2_exe_free(&exe);
            q2_buf_free(&buf);
            return 1;
        }
    }

    free(t.funcs);
    free(t.syms);
    q2_exe_free(&exe);
    q2_buf_free(&buf);
    return ok ? 0 : 1;
}
