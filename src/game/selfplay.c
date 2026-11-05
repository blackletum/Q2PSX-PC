#include "selfplay.h"

#include <string.h>

/* 0x8009D228 (US 0x8009D47C), three twelve-byte names and a zero entry. */
static const char *const k_levels[Q2_SELFPLAY_LEVELS] = {
    "Base3", "Jail2", "Waste2"
};

const char *q2_selfplay_level(u32 index)
{
    return index < Q2_SELFPLAY_LEVELS ? k_levels[index] : NULL;
}

/*
 * 0x800413B8..0x80041494. A NULL pointer is pointed at Base3 first; the
 * current name is copied out; then the pointer steps 12 bytes and, if the
 * entry it lands on starts with a zero word, goes back to Base3.
 */
u32 q2_selfplay_next(u32 *cursor)
{
    u32 at;

    if (!cursor)
        return 0;
    at = *cursor < Q2_SELFPLAY_LEVELS ? *cursor : 0;
    *cursor = at + 1 < Q2_SELFPLAY_LEVELS ? at + 1 : 0;
    return at;
}

u16 q2_selfplay_buttons(u16 word)
{
    return (u16)(word & Q2_SELFPLAY_BUTTON_MASK);
}

/*
 * 0x8003271C..0x800327FC: NewVcounts starts at zero and each bit adds a
 * multiple of PalNtscRate — bit 3 once, bit 0 twice, bit 1 four times and
 * bit 2 eight times. The record path at 0x80032950 divides by the rate and
 * stores the quotient's bits 0..3 into bits 3, 0, 1 and 2, which is the same
 * permutation the other way round.
 */
u32 q2_selfplay_fields(u16 word)
{
    return ((word >> 3) & 1u) | ((word & 1u) << 1) |
           ((word & 2u) << 1) | ((word & 4u) << 1);
}

u16 q2_selfplay_encode(u16 buttons, u32 fields)
{
    u16 w = (u16)(buttons & Q2_SELFPLAY_BUTTON_MASK);

    if (fields & 1u) w |= 0x8;
    if (fields & 2u) w |= 0x1;
    if (fields & 4u) w |= 0x2;
    if (fields & 8u) w |= 0x4;
    return w;
}

/* ------------------------------------------------------------------------- */
/* Reading the init                                                           */
/* ------------------------------------------------------------------------- */

typedef enum {
    K_UNKNOWN = 0,
    K_CONST,       /* a value built from an immediate                  */
    K_MODULE,      /* module + value                                   */
    K_ENGINE,      /* the engine block, module+0x8                     */
    K_SLOT,        /* the word at engine + value                       */
    K_ENTITY,      /* *engine+0xE0, actor 0                            */
    K_CLIENT       /* actor 0's client record, entity+0xC              */
} sp_kind;

typedef struct { sp_kind k; s32 v; } sp_reg;

#define SP_OFF_INSELF     0x78
#define SP_OFF_SELFPLAYON 0x58
#define SP_OFF_NOINPUT    0x6C
#define SP_OFF_ROUTINE    0x70
#define SP_OFF_ENTITIES   0xE0
#define SP_OFF_SKILL      0x366

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* The store of one register into a known record. */
static void sp_store(q2_selfplay_setup *o, const sp_reg *base, s32 off,
                     const sp_reg *val, bool in_block)
{
    if (!in_block || val->k != K_CONST)
        return;
    if (base->k == K_CLIENT) {
        if (off == 0x66) {
            o->weapon = (s16)val->v;
            o->set |= Q2_SELFPLAY_SET_WEAPON;
        } else if (off == 0x68) {
            o->weapons = (u32)val->v;
            o->set |= Q2_SELFPLAY_SET_WEAPONS;
        } else if (off >= 0x6C && off < 0x6C + 12 && !(off & 1)) {
            int n = (off - 0x6C) / 2;

            o->ammo[n] = (s16)val->v;
            o->set |= Q2_SELFPLAY_SET_AMMO0 << n;
        }
    } else if (base->k == K_ENTITY && off == 0x108) {
        o->health = (s16)val->v;
        o->set |= Q2_SELFPLAY_SET_HEALTH;
    } else if (base->k == K_ENGINE && off == SP_OFF_SKILL) {
        o->skill = val->v;
    } else if (base->k == K_SLOT && base->v == SP_OFF_NOINPUT && off == 0) {
        o->no_input = val->v;
    }
}

/*
 * A straight-line walk of the module that tracks what each register holds
 * well enough to recognise the self-play block: it starts at the load through
 * *engine+0x78 and ends at the call through engine+0x70. Branches are ignored,
 * which is safe because the block itself has none, and a delay slot is
 * applied before the call that owns it, as the processor applies it.
 */
bool q2_selfplay_scan(const u8 *module, u32 size, q2_selfplay_setup *out)
{
    sp_reg r[32];
    q2_selfplay_setup o;
    bool in_block = false, have_record = false;
    u32 at;

    if (!module || !out || size < 16)
        return false;
    memset(r, 0, sizeof(r));
    memset(&o, 0, sizeof(o));
    o.skill = -1;
    o.no_input = 0;

    for (at = 0; at + 4 <= size; at += 4) {
        u32 w = rd32(module + at);
        u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
        u32 rd = (w >> 11) & 31, fn = w & 63;
        s32 imm = (s16)(w & 0xFFFF);
        sp_reg nv = { K_UNKNOWN, 0 };

        if (op == 0 && fn == 9) {                    /* jalr rs           */
            sp_reg target = r[rs];

            /* The delay slot first: it is where a1 is often finished. */
            if (at + 8 <= size) {
                u32 d = rd32(module + at + 4);
                u32 dop = d >> 26, drs = (d >> 21) & 31, drt = (d >> 16) & 31;
                s32 dimm = (s16)(d & 0xFFFF);

                if (dop == 9 && drt != 0) {
                    sp_reg s = r[drs];

                    if (drs == 0) { r[drt].k = K_CONST; r[drt].v = dimm; }
                    else if (s.k == K_CONST || s.k == K_MODULE) {
                        r[drt] = s;
                        r[drt].v += dimm;
                    } else {
                        r[drt].k = K_UNKNOWN;
                    }
                }
            }
            if (in_block && target.k == K_SLOT) {
                if (target.v == SP_OFF_SELFPLAYON && r[5].k == K_MODULE &&
                    r[5].v > 0 && (u32)r[5].v < size) {
                    o.record = (u32)r[5].v;
                    have_record = true;
                } else if (target.v == SP_OFF_ROUTINE) {
                    break;
                }
            }
            r[2].k = K_UNKNOWN;
            r[31].k = K_UNKNOWN;
            continue;
        }

        switch (op) {
        case 0x0F:                                   /* lui               */
            if ((w & 0xFFFF) == 0x8010u || (w & 0xFFFF) == 0) {
                nv.k = K_MODULE; nv.v = 0;
            } else {
                nv.k = K_CONST; nv.v = (s32)((w & 0xFFFF) << 16);
            }
            if (rt) r[rt] = nv;
            break;
        case 0x09:                                   /* addiu             */
            if (rs == 0) { nv.k = K_CONST; nv.v = imm; }
            else if (r[rs].k == K_CONST || r[rs].k == K_MODULE) {
                nv = r[rs];
                nv.v += imm;
            }
            if (rt) r[rt] = nv;
            break;
        case 0x23: {                                 /* lw                */
            const sp_reg *b = &r[rs];

            if (b->k == K_MODULE && b->v + imm == 8) {
                nv.k = K_ENGINE;
            } else if (b->k == K_ENGINE && imm == SP_OFF_ENTITIES) {
                nv.k = K_ENTITY;
            } else if (b->k == K_ENGINE) {
                nv.k = K_SLOT; nv.v = imm;
            } else if (b->k == K_ENTITY && imm == 0xC) {
                nv.k = K_CLIENT;
            } else if (b->k == K_SLOT && b->v == SP_OFF_INSELF && imm == 0) {
                in_block = true;          /* lw v0, 0(&InSelfPlayMode)    */
            }
            if (rt) r[rt] = nv;
            break;
        }
        case 0x29:                                   /* sh                */
        case 0x2B:                                   /* sw                */
            sp_store(&o, &r[rs], imm, &r[rt], in_block);
            break;
        case 0x00:
            if (fn == 0x21 || fn == 0x25) {          /* addu / or         */
                if (rs == 0 && rt == 0) { nv.k = K_CONST; nv.v = 0; }
                else if (rt == 0)       nv = r[rs];
                else if (rs == 0)       nv = r[rt];
            }
            if (rd) r[rd] = nv;
            break;
        default:
            /* Every other load writes rt with something untracked. */
            if (op >= 0x20 && op <= 0x26 && rt) r[rt].k = K_UNKNOWN;
            else if ((op == 0x0A || op == 0x0B || op == 0x0C || op == 0x0D ||
                      op == 0x0E) && rt)
                r[rt].k = K_UNKNOWN;
            break;
        }
        r[0].k = K_CONST;
        r[0].v = 0;
    }

    if (!have_record)
        return false;
    *out = o;
    return true;
}

u32 q2_selfplay_length(const u8 *module, u32 size, u32 record)
{
    u32 at, n = 0;

    if (!module || record >= size)
        return 0;
    for (at = record; at + 2 <= size; at += 2, n++)
        if (((u32)module[at] | ((u32)module[at + 1] << 8)) ==
            Q2_SELFPLAY_END_WORD)
            return n;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Playback                                                                   */
/* ------------------------------------------------------------------------- */

void q2_selfplay_start(q2_selfplay *sp, const u8 *module, u32 size,
                       const q2_selfplay_setup *setup)
{
    if (!sp)
        return;
    memset(sp, 0, sizeof(*sp));
    if (!module || !setup || setup->record >= size)
        return;
    sp->module   = module;
    sp->size     = size;
    sp->cursor   = setup->record;      /* +0x20 and +0x24 = data          */
    sp->playing  = true;               /* +0x1C = 1                       */
    sp->routine  = true;               /* SetNoInputTimeOutRoutine        */
    sp->no_input = setup->no_input;    /* NoInputTime                     */
    /* ChrisCount and LastInputTime are both zeroed by the level's frame
     * setup (0x800708C0 / 0x800708E8) before the module runs. */
}

static u16 sp_word(const q2_selfplay *sp)
{
    if (!sp->module || sp->cursor + 2 > sp->size)
        return Q2_SELFPLAY_END_WORD;
    return (u16)(sp->module[sp->cursor] |
                 ((u16)sp->module[sp->cursor + 1] << 8));
}

u32 q2_selfplay_peek_fields(const q2_selfplay *sp)
{
    u16 w;

    if (!sp || !sp->playing)
        return 0;
    w = sp_word(sp);
    return w == Q2_SELFPLAY_END_WORD ? 0 : q2_selfplay_fields(w);
}

bool q2_selfplay_frame(q2_selfplay *sp, u16 real, bool dead)
{
    bool fire = false;
    bool ended = false;

    if (!sp)
        return false;

    sp->buttons = 0;
    sp->fields  = 0;

    if (sp->playing) {
        u16 w = sp_word(sp);

        /* 0x80032704: `lh`, so the terminator reads as -1 and the frame's
         * buttons become -1 for the test below. */
        if (w == Q2_SELFPLAY_END_WORD) {
            ended = true;
            sp->end = Q2_SELFPLAY_END_DATA;
            sp->no_input = 0;                       /* 0x80032820 */
        } else {
            sp->buttons = q2_selfplay_buttons(w);
            sp->fields  = q2_selfplay_fields(w);
            sp->cursor += 2;
        }

        /* 0x80032878..0x800328AC, in this order: the terminator, the real
         * pad, the dead player. */
        if (!ended && real) {
            ended = true;
            sp->end = Q2_SELFPLAY_END_INPUT;
        } else if (!ended && dead) {
            ended = true;
            sp->end = Q2_SELFPLAY_END_DEATH;
        }
        if (ended) {
            sp->buttons  = 0;
            sp->no_input = 0;
            sp->playing  = false;                   /* mode 3 */
        }
    }

    /* 0x80032A3C: any button this frame is the last input. */
    if (sp->buttons)
        sp->last_input = sp->frames;

    /* 0x80033058..0x80033088: ChrisCount < LastInputTime + NoInputTime
     * skips the routine; anything else calls it. */
    if (sp->routine &&
        !((s64)sp->frames < (s64)sp->last_input + (s64)sp->no_input)) {
        /* The routine asks for the title and leaves the record where it was:
         * on an idle timeout mode 1 and InSelfPlayMode both survive the
         * frame, and it is the level going away that ends them. */
        fire = true;
        if (sp->end == Q2_SELFPLAY_END_NONE)
            sp->end = Q2_SELFPLAY_END_IDLE;
    }

    /* The world frame that follows: 0x800706F4 `addiu a1, a1, 1`. */
    sp->frames++;
    return fire;
}

u32 q2_selfplay_overlay_step(u32 step, u32 fields)
{
    step += fields;                                  /* 0x80038974 */
    while (step >= Q2_SELFPLAY_OVERLAY_STEPS)        /* 0x80038990 */
        step -= Q2_SELFPLAY_OVERLAY_STEPS;
    return step;
}

void q2_selfplay_overlay_cell(u32 step, u32 *slot, u8 *u, u8 *v)
{
    u32 s = Q2_SELFPLAY_OVERLAY_SLOT_A, row;

    step %= Q2_SELFPLAY_OVERLAY_STEPS;
    row = step;
    if (step >= Q2_SELFPLAY_OVERLAY_HALF) {          /* 0x800389A4 */
        s = Q2_SELFPLAY_OVERLAY_SLOT_B;
        row = step - Q2_SELFPLAY_OVERLAY_HALF;
    }
    if (slot) *slot = s;
    /* u from the whole counter's low two bits (0x800389DC), v from the
     * half's row (0x800389BC..0x800389D4). */
    if (u) *u = (u8)((step & 3u) << 6);
    if (v) *v = (u8)((row >> 2) * 20u);
}
