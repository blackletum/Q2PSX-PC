/*
 * levelbin.c — see levelbin.h for the derivation.
 */
#include "levelbin.h"

#include <stdio.h>
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

/* ------------------------------------------------------------------------- */
/* The map's own MISEVENT table                                               */
/* ------------------------------------------------------------------------- */
static bool misevent_name(const u8 *p, char *out, bool *out_padded)
{
    u32 i;
    bool ended = false;
    u32 n = 0;

    /*
     * Printable, then NUL padding, and the padding is checked: a field that
     * goes back to printable after a NUL is not a name, it is two things that
     * happen to sit next to each other.
     */
    for (i = 0; i < 12; i++) {
        u8 ch = p[i];

        if (ch == 0) {
            ended = true;
            continue;
        }
        if (ended)
            return false;
        if (ch < 32 || ch > 126)
            return false;
        n++;
    }

    if (n < 2)
        return false;

    if (out) {
        memcpy(out, p, 12);
        out[12] = '\0';
    }
    if (out_padded)
        *out_padded = ended;

    return true;
}

/*
 * A record cannot BEGIN in the middle of a printable RUN.
 *
 * This is what separates a field from a substring, and it took two goes.
 * BIGGUN needed the check at all: the backward walk from its real table ran on
 * into `TeleportDeat` and `royGlassZone`, twelve-character windows cut out of
 * longer strings, each followed by something that passed for a handler. But
 * "the preceding byte is not printable" was too blunt — three real tables sit
 * immediately after a function epilogue, and `addiu sp, sp, 32` ends in 0x27,
 * which is an apostrophe. BASE0's `DOCRATES`, COMMAND's `Comp1` and JAIL3's
 * `Bridge` all vanished on that.
 *
 * So it is a RUN, not a byte: text that reaches the field is text, and one or
 * two stray printable bytes out of an instruction word are not.
 */
#define Q2_MISEVENT_TEXT_RUN 3

static bool misevent_starts_field(const u8 *module, u32 at)
{
    u32 run = 0;

    while (run < Q2_MISEVENT_TEXT_RUN && run < at) {
        u8 ch = module[at - 1 - run];

        if (ch < 32 || ch > 126)
            break;
        run++;
    }

    return run < Q2_MISEVENT_TEXT_RUN;
}

static bool misevent_handler(const u8 *p, u32 size, u32 load_base, u32 *out)
{
    u32 h = q2_rd_u32(p + 12);

    /* A handler is code: non-zero, word-aligned, and inside this module. */
    if (h == 0 || (h & 3u) != 0)
        return false;
    if (h < load_base || h >= load_base + size)
        return false;

    if (out)
        *out = h;
    return true;
}

u32 q2_levelbin_misevents(const u8 *module, u32 size, u32 load_base,
                          q2_levelbin_misevent *out, u32 max)
{
    u32 i, n = 0;

    if (!module || size < 20)
        return 0;

    /*
     * Anchored on the TERMINATOR, not on a name.
     *
     * Two earlier anchors both failed, and both failures are worth keeping.
     * Anchoring on "any name-shaped record" walks straight into a module's
     * text: QENDMIS5's is thirty kilobytes of strings, and a twelve-byte window
     * inside `Initialise %` or `MDEC_out_sync` is printable and is followed by
     * something small, so twenty screens were reported that do not exist.
     * Anchoring on "a NUL-PADDED record" fixed that and then missed any table
     * whose names fill the field — BOSS1's begins `LaserButton0`,
     * `LaserButton1` and BIGGUN's is `STOPPLATFORM`, `Destroy Grav`, all four
     * exactly twelve characters, and all four came back as script naming
     * events that do not exist.
     *
     * The zero word is the engine's own marker: 0x8006DB10 stops its search
     * there. So find one, walk BACKWARD while the record shape holds, and take
     * the maximal run. It anchors on structure rather than on how long the
     * level designer's names happen to be.
     */
    for (i = 16; i + 4 <= size; i += 4) {
        u32 head, run = 0, j;

        if (q2_rd_u32(module + i) != 0)
            continue;
        if (!misevent_name(module + i - 16, NULL, NULL))
            continue;
        if (!misevent_handler(module + i - 16, size, load_base, NULL))
            continue;
        if (!misevent_starts_field(module, i - 16))
            continue;

        head = i;
        while (head >= 16 &&
               misevent_name(module + head - 16, NULL, NULL) &&
               misevent_handler(module + head - 16, size, load_base, NULL) &&
               misevent_starts_field(module, head - 16)) {
            head -= 16;
            run++;
        }

        for (j = 0; j < run; j++) {
            const u8 *rec = module + head + j * 16;

            if (n < max && out) {
                misevent_name(rec, out[n].name, NULL);
                misevent_handler(rec, size, load_base, &out[n].handler);
                out[n].offset = head + j * 16;
            }
            n++;
        }
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/* The front end's own menu pages                                             */
/* ------------------------------------------------------------------------- */
#define MENU_REC_SIZE 24u
#define MENU_ROW_MAX   8u

/* A row's text: printable, NUL-terminated, and inside the module. */
static bool menu_text_at(const u8 *module, u32 size, u32 off, char *out)
{
    u32 i;

    if (off >= size)
        return false;

    for (i = 0; i < 31 && off + i < size; i++) {
        u8 ch = module[off + i];

        if (ch == 0)
            break;
        if (ch < 0x20 || ch > 0x7E)
            return false;
    }

    if (i < 3 || off + i >= size || module[off + i] != 0)
        return false;

    memcpy(out, module + off, i);
    out[i] = '\0';
    return true;
}

/* Does a 24-byte record look like a menu row? */
static bool menu_row_at(const u8 *module, u32 size, u32 load_base, u32 off,
                        q2_lb_menu_row *out)
{
    u32 text = q2_rd_u32(module + off);
    s16 x    = q2_rd_s16(module + off + 4);
    s16 y    = q2_rd_s16(module + off + 6);
    u32 act  = q2_rd_u32(module + off + 8);
    char name[32];

    if (text < load_base || text - load_base >= size)
        return false;
    if (!menu_text_at(module, size, text - load_base, name))
        return false;

    /* Every front-end row is centred at 256 and sits on a 480-line screen. */
    if (x != 256 || y < 0 || y > 480)
        return false;

    /* An action, when there is one, is code in this module. A NULL action is
     * legal — the deathmatch setup page's rows have none. */
    if (act != 0 && (act < load_base || act - load_base >= size))
        return false;

    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", name);
        out->x      = x;
        out->y      = y;
        out->action = act;
        out->offset = off;
    }
    return true;
}

u32 q2_levelbin_menu_pages(const u8 *module, u32 size, u32 load_base,
                           q2_lb_menu_page *out, u32 max)
{
    u32 off, n = 0;

    if (!module || size < MENU_REC_SIZE)
        return 0;

    for (off = 0; off + MENU_REC_SIZE <= size; off += 4) {
        u32 run = 0, at;

        if (!menu_row_at(module, size, load_base, off, NULL))
            continue;

        for (at = off;
             at + MENU_REC_SIZE <= size && run < MENU_ROW_MAX;
             at += MENU_REC_SIZE) {
            if (!menu_row_at(module, size, load_base, at, NULL))
                break;
            run++;
        }

        /*
         * A page has at least two rows. One row on its own is far more likely
         * to be a pointer that happens to sit next to a plausible pair of
         * halfwords than a menu, and the front end has no one-row page.
         */
        if (run < 2) {
            continue;
        }

        if (n < max && out) {
            u32 k;

            memset(&out[n], 0, sizeof(out[n]));
            out[n].offset = off;
            out[n].count  = run;
            for (k = 0; k < run; k++)
                menu_row_at(module, size, load_base, off + k * MENU_REC_SIZE,
                            &out[n].row[k]);
        }
        n++;

        off += (run - 1) * MENU_REC_SIZE;   /* the loop's += 4 finishes it */
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/* QENDMIS — the movie table                                                  */
/* ------------------------------------------------------------------------- */
static bool movie_field(const u8 *p, char *out)
{
    u32 i;
    bool ended = false;
    u32 n = 0;

    for (i = 0; i < 12; i++) {
        u8 ch = p[i];

        if (ch == 0) {
            ended = true;
            continue;
        }
        if (ended)
            return false;
        if (ch < 32 || ch > 126) {
            /* The trace labels end in a newline before their padding —
             * `Do Intro\n` — so that one control character is a field
             * character here and nothing else is. */
            if (ch != '\n')
                return false;
        }
        n++;
    }

    if (n < 2)
        return false;

    memcpy(out, p, 12);
    out[12] = '\0';
    return true;
}

/* Does this field end in `.STX`? That is what makes a record a movie rather
 * than three names that happen to sit in a row. */
static bool movie_is_stx(const char *s)
{
    size_t n = strlen(s);

    return n > 4 && memcmp(s + n - 4, ".STX", 4) == 0;
}

u32 q2_levelbin_movies(const u8 *module, u32 size,
                       q2_levelbin_movie *out, u32 max)
{
    u32 i, n = 0;

    if (!module || size < 36)
        return 0;

    for (i = 0; i + 36 <= size; i += 4) {
        char a[13], b[13], f[13];

        if (!movie_field(module + i, a))
            continue;
        if (!movie_field(module + i + 12, b))
            continue;
        if (!movie_field(module + i + 24, f))
            continue;

        /*
         * Anchored on the FILENAME, which is the one field with a shape a
         * coincidence does not have. Three printable twelve-byte fields in a
         * row are common in a module; three of them where the third ends in
         * `.STX` are not.
         */
        if (!movie_is_stx(f))
            continue;

        if (n < max && out) {
            memcpy(out[n].screen, a, sizeof(out[n].screen));
            memcpy(out[n].label,  b, sizeof(out[n].label));
            memcpy(out[n].file,   f, sizeof(out[n].file));
            out[n].offset = i;
        }
        n++;
        i += 32;        /* the loop's own += 4 completes the record */
    }

    return n;
}

/* ------------------------------------------------------------------------- */
u32 q2_laserbeams_build(q2_laserbeam_set *out, const q2_events *events,
                        const q2_userfuncs *uf, const q2_uf_operands *ops,
                        const q2_collision *coll)
{
    q2_event_record rec;

    if (!out || !events || !uf)
        return 0;

    memset(out, 0, sizeof(*out));

    if (events->record_count == 0 || !q2_events_first_record(events, &rec))
        return 0;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call    call;
            const u8     *p;
            q2_laserbeam *b;
            s32           node;
            int           k;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;
            if ((item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                continue;
            if (q2_uf_decode_call(&call, uf, &item) != Q2_OK)
                continue;

            /* `lbu v1, 1(s0)` against 36, in both halves. */
            if (call.prim != Q2_UF_LASERBEAM || item.len != 36)
                continue;

            /* The constructor's own restore: both endpoints come from the
             * pristine buffer, not the working one (0x8002E744). */
            p = q2_uf_operand_at(ops, item.payload - 2, 36);

            b = &out->beam[out->count];
            memset(b, 0, sizeof(*b));

            for (k = 0; k < 3; k++) {
                b->from[k] = q2_rd_s32(p + 4  + 4 * k);
                b->to[k]   = q2_rd_s32(p + 20 + 4 * k);
            }
            b->kind   = q2_rd_s16(p + 34);
            b->area   = q2_rd_s16(p + 18);
            b->raiser = rec.offset;

            /* The load-time clamp, so an out-of-range kind is kind 0 here too
             * and not a beam the dispatcher silently drops. No disc beam needs
             * it — all 72 are already in range, which is itself the evidence
             * that +34 is a kind and not the counter this table once called
             * it. */
            if (b->kind >= 6 || b->kind < 0)
                b->kind = 0;

            /*
             * And the area, which the constructor resolves rather than reads:
             * hint -1 forces the brute-force sweep, so this is the node holding
             * the near end wherever in the hull it happens to be.
             */
            if (coll) {
                node = q2_coll_find_node(coll, b->from, -1, true);
                if (node >= 0)
                    b->area = (s16)node;
            }

            /*
             * `andi v0, v0, 1` at 0x8002E6C0, against the word the constructor
             * has just copied out of the ZONE's chunk. Dark in this room.
             */
            if ((b->from[0] & 1) == 0) {
                out->declined++;
                continue;
            }

            /* `slti v0, v1, 32` — the list is a fixed 32 and the overflow is
             * dropped, not wrapped. */
            if (out->count >= Q2_LASERBEAM_MAX)
                return out->count;

            out->count++;
        }
    } while (q2_events_next_record(events, &rec, &rec));

    return out->count;
}

u32 q2_laserbeams_draw(const q2_laserbeam_set *set, q2_fx_world *w, q2_rng *rng)
{
    u32 i, n = 0;

    if (!set || !w || !rng)
        return 0;

    for (i = 0; i < set->count; i++) {
        const q2_laserbeam *b = &set->beam[i];
        q2_fx_laser_result  r;

        /* ends = 0 — the fifth argument the walk zeroes on the stack at
         * 0x8002EEBC. A level's beams are tube only. */
        if (q2_fx_laser(w, rng, (u32)b->kind, b->from, b->to, b->area, 0, &r)
            && r.queued)
            n++;
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/* VIEW CREDITS — see levelbin.h                                              */
/* ------------------------------------------------------------------------- */
/* Is `p` the start of a printable, NUL-terminated string of at least two
 * characters? The pool is packed and 4-aligned, which is what makes walking it
 * possible at all. */
static bool credit_string_at(const u8 *module, u32 size, u32 off, u32 *len_out)
{
    u32 i = 0;

    while (off + i < size && module[off + i] != 0) {
        u8 ch = module[off + i];

        if (ch < 0x20 || ch > 0x7E)
            return false;
        i++;
        if (i > 63)
            return false;
    }
    if (i < 2 || off + i >= size)
        return false;
    *len_out = i;
    return true;
}

u32 q2_levelbin_credits(const u8 *module, u32 size,
                        const char **out, u32 max)
{
    static const char k_first[] = "HAMMERHEAD LTD";
    static const char k_stop[]  = "PLEASE WAIT WHILE";
    u32 off, start = 0, n = 0;

    if (!module || size == 0)
        return 0;

    /* Find the roll's first line. */
    for (off = 0; off + sizeof(k_first) <= size; off += 4) {
        u32 len;

        if (!credit_string_at(module, size, off, &len))
            continue;
        if (len == sizeof(k_first) - 1 &&
            memcmp(module + off, k_first, len) == 0) {
            start = off;
            break;
        }
    }
    if (!start)
        return 0;

    /*
     * Then walk the pool forward, string by string, to the disc-swap prompt.
     * Strings are packed and each is padded to the next 4 bytes, which is what
     * `+= (len + 4) & ~3` steps over — a run that does not step exactly onto
     * the next string is not this pool and the walk stops.
     */
    for (off = start; off < size; ) {
        u32 len;

        if (!credit_string_at(module, size, off, &len))
            break;
        if (len == sizeof(k_stop) - 1 &&
            memcmp(module + off, k_stop, len) == 0)
            break;

        if (out && n < max)
            out[n] = (const char *)module + off;
        n++;
        if (n >= Q2_LB_CREDITS_MAX)
            break;

        off += (len + 4u) & ~3u;
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/* The title screen's scene — see the derivation in levelbin.h                */
/* ------------------------------------------------------------------------- */

/*
 * Does the module materialise `addr` with a `lui`/`addiu` pair?
 *
 * The two halves are almost never adjacent — the scheduler fills the load
 * delay — so this is the same bounded look-back `q2_levelbin_misevents` uses
 * for its own constants: find the `addiu`, then walk up to eight instructions
 * back for a `lui` into the register it reads.
 *
 * A `lui` whose low half is negative carries a borrow, which is why the sum is
 * taken over sign-extended halves rather than concatenated.
 */
static bool module_materialises(const u8 *module, u32 size, u32 addr)
{
    u32 i;

    for (i = 0; i + 4 <= size; i += 4) {
        u32 w = q2_rd_u32(module + i);
        u32 rs, j;
        s32 lo;

        if ((w >> 26) != 0x09)                 /* addiu rt, rs, imm */
            continue;
        rs = (w >> 21) & 0x1F;
        lo = (s32)(s16)(u16)(w & 0xFFFF);

        for (j = 4; j <= 32 && j <= i; j += 4) {
            u32 p = q2_rd_u32(module + i - j);

            if ((p >> 26) != 0x0F)             /* lui rt, imm */
                continue;
            if (((p >> 16) & 0x1F) != rs)
                continue;
            if ((u32)(((p & 0xFFFF) << 16) + lo) == addr)
                return true;
            break;
        }
    }
    return false;
}

bool q2_levelbin_scene(const u8 *module, u32 size, u32 load_base,
                       q2_lb_scene *out)
{
    u32 off;

    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!module || size < 8)
        return false;

    for (off = 0; off + 8 <= size; off += 4) {
        u32 run = 0, at;

        for (at = off; at + 4 <= size; at += 4) {
            u32 w = q2_rd_u32(module + at);

            if (w == 0xFFFFFFFFu)
                break;
            /*
             * The spawner reads the id with `lh`, so a word is only an id when
             * its upper half is clear as well — which is what stops a run of
             * relocated pointers or small negative constants reading as a
             * short list of low ids.
             */
            if (w >= Q2_ITEM_COUNT)
                break;
            if (++run > Q2_LB_SCENE_MAX)
                break;
        }

        /*
         * Two independent things, as `q2_levelbin_menu_pages` needs two: a
         * terminated run of plausible ids, and an address the module actually
         * names. QFRONT has exactly one such list; a module with none — every
         * other map's — finds nothing rather than the first pair of small
         * words that happens to sit next to a -1.
         */
        if (run < 2 || run > Q2_LB_SCENE_MAX)
            continue;
        if (at + 4 > size || q2_rd_u32(module + at) != 0xFFFFFFFFu)
            continue;
        if (!module_materialises(module, size, load_base + off))
            continue;

        out->offset = off;
        out->count  = run;
        for (at = 0; at < run; at++)
            out->id[at] = (u16)q2_rd_u32(module + off + at * 4);
        return true;
    }

    return false;
}
