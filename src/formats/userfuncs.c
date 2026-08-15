/*
 * userfuncs.c — see userfuncs.h for the derivation.
 *
 * The operand tables below were built two ways and cross-checked: by tracking
 * the item pointer through each primitive's entry point in the EXE and
 * recording every load off it, and by a census of all 3,760 CALL items on the
 * disc. Where a primitive was never called on this disc its layout comes from
 * code alone and is marked in the table comments.
 */
#include "userfuncs.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The 43 primitives, in EXE binding-table order.                             */
/* ------------------------------------------------------------------------- */
#define OP_NONE   {0, 0, Q2_UF_OP_NONE, NULL, NULL}

static const q2_uf_prim_info uf_table[Q2_UF_PRIM_COUNT] = {

/* ---- text, level flow, spawning ---------------------------------------- */
{Q2_UF_STRING, "STRING", 16, true, false, 1, {
    {4, 1, Q2_UF_OP_NAME12, "key",
     "Strings chunk key; resolves on 165/363 uses, so a miss is normal"}}},

{Q2_UF_LOADMAP, "LOADMAP", 28, true, false, 2, {
    {4,  1, Q2_UF_OP_NAME12, "map",
     "EXE level table at 0x8009C6C8; 129/135. Ignored when it equals the "
     "current map name at 0x800E46B4"},
    /* Resolved against the TARGET map's StartPos (named at +4), not the map the
     * item lives in. Against the containing map it matches only 104/135;
     * against the target, 129/129. Getting the namespace wrong drops the
     * player at the wrong arrival point on a quarter of transitions. */
    {16, 1, Q2_UF_OP_NAME12, "start_pos",
     "StartPos entry of the TARGET map named at +4; 129/129"}}},

{Q2_UF_MISCOMPLETE, "MISCOMPLETE", 4, false, false, 0, {OP_NONE}},

{Q2_UF_TELEPORT, "TELEPORT", 16, true, false, 1, {
    {4, 1, Q2_UF_OP_NAME12, "start_pos",
     "StartPos entry name; 28/28. Switches zone first if the target is in "
     "another one, then sets entity position and drops it by 286"}}},

{Q2_UF_CREBATCH, "CREBATCH", 16, true, true, 1, {
    {4, 1, Q2_UF_OP_NAME12, "group",
     "Population group name; 458/472. Spawns that group's list"}}},

{Q2_UF_MISEVENT, "MISEVENT", 16, true, true, 1, {
    {4, 1, Q2_UF_OP_NAME12, "key",
     "Namespace UNLOCATED: 0/93 resolve against Strings. Consumer is "
     "0x800419A0, which walks the entity array at 0x800D5C30"}}},

{Q2_UF_HELPCOMPUTER, "HELPCOMPUTER", 32, true, false, 3, {
    {4,  1, Q2_UF_OP_NAME12, "key_a", "Strings key; 122/127 with key_b"},
    {16, 1, Q2_UF_OP_NAME12, "key_b", "Strings key"},
    {28, 1, Q2_UF_OP_U32,    "param",  "passed through to 0x80021250"}}},

{Q2_UF_INSECRET, "INSECRET", 4, true, false, 0, {OP_NONE}},

{Q2_UF_SIMPLESOUND, "SIMPLESOUND", 32, false, false, 3, {
    {4,  1, Q2_UF_OP_VEC3_S32, "origin", "absolute world position"},
    {16, 1, Q2_UF_OP_U32,      "unknown",
     "UNKNOWN and NOT PADDING: no instruction in the handler reads it, yet it "
     "is non-zero on 135 of 151 items and takes 20 distinct small values. "
     "Preserve it; do not act on it"},
    {20, 1, Q2_UF_OP_NAME12,   "sound",
     "SNDVRAM sound-bank name; 129/151. Resolved by 0x80073518, played by "
     "0x80073704(id, &origin)"}}},

/* ---- conditionals and flow control ------------------------------------- */
{Q2_UF_ONKEYDO, "ONKEYDO", 12, false, false, 4, {
    {4,  1, Q2_UF_OP_U16, "require_all_set",
     "vs the inventory bitfield at 0x800B29D0; 0 disables the test"},
    {6,  1, Q2_UF_OP_U16, "require_any_set",   "0 disables"},
    {8,  1, Q2_UF_OP_U16, "require_all_clear", "0 disables"},
    {10, 1, Q2_UF_OP_U16, "require_any_clear", "0 disables"}}},

{Q2_UF_TIMER, "TIMER", 12, false, true, 4, {
    {4,  1, Q2_UF_OP_U16, "delay_base",
     "ticks = (base + ((range * rand()) >> 15)) * 30 -- NOT 300"},
    {6,  1, Q2_UF_OP_U16, "delay_range", "rand() is BIOS A(0x2F)"},
    {8,  1, Q2_UF_OP_U16, "slot_arg", "copied to timer slot +8"},
    {10, 1, Q2_UF_OP_U16, "slot_arg2", "copied to timer slot +6"}}},

{Q2_UF_DISABLEME, "DISABLEME", 4, false, true, 0, {OP_NONE}},

/* ---- entity environment predicates -------------------------------------- */
{Q2_UF_INWATER,     "INWATER",     4, false, false, 0, {OP_NONE}},
{Q2_UF_UNDERWATER,  "UNDERWATER",  4, false, false, 0, {OP_NONE}},
{Q2_UF_INCROUCH,    "INCROUCH",    4, false, false, 0, {OP_NONE}},
{Q2_UF_INLOWCROUCH, "INLOWCROUCH", 4, false, false, 0, {OP_NONE}},
{Q2_UF_INACID,      "INACID",      4, false, false, 0, {OP_NONE}},
{Q2_UF_UNDERACID,   "UNDERACID",   4, false, false, 0, {OP_NONE}},
{Q2_UF_INLAVA,      "INLAVA",      4, false, false, 0, {OP_NONE}},
{Q2_UF_UNDERLAVA,   "UNDERLAVA",   4, false, false, 0, {OP_NONE}},
{Q2_UF_DONTJUMP,    "DONTJUMP",    4, false, true,  0, {OP_NONE}},

/* ---- damage volumes ----------------------------------------------------- */
{Q2_UF_LASERWALL, "LASERWALL", 24, true, true, 2, {
    {18, 1, Q2_UF_OP_OBJSLOT, "object", NULL},
    {20, 1, Q2_UF_OP_S16,     "damage",
     "T_Damage(entity, entity, damage, Q2_MOD_LASER, &entity.origin)"}}},

{Q2_UF_LASERBEAM, "LASERBEAM", 36, true, true, 4, {
    {4,  1, Q2_UF_OP_VEC3_S32, "origin_a",
     "absolute; restored from the pristine buffer by the constructor. Bit 0 "
     "of the first word doubles as an enable flag"},
    {18, 1, Q2_UF_OP_OBJSLOT,  "object", NULL},
    {20, 1, Q2_UF_OP_VEC3_S32, "origin_b", "absolute"},
    {34, 1, Q2_UF_OP_S16,      "counter",
     "constructor zeroes it when it is below 6"}}},

/* ---- movers ------------------------------------------------------------- */
{Q2_UF_LIFT1, "LIFT1", 20, true, true, 5, {
    {4,  1, Q2_UF_OP_U16,     "param_a", "constructor writes -value to obj+0x44"},
    {6,  1, Q2_UF_OP_S16,     "param_b", "abs() to obj+0x3A"},
    {8,  4, Q2_UF_OP_OBJSLOT, "objects", "up to four; negative terminates"},
    {16, 1, Q2_UF_OP_U8,      "time_a",  "* 300 into obj+0x4C"},
    {17, 1, Q2_UF_OP_U8,      "time_b",  "* 300 into obj+0x4E; 0xFF => never"}}},

/*
 * CORRECTION — CAGELIFT1 has a SPEED and this table did not list it.
 *
 * Its constructor at `0x80029794` is LIFT1's, operand for operand. With
 * `s0 = obj + 0x38` — pinned by `sw a3, -12(s0)` writing the per-frame tick
 * `0x80025658`, which mover.h already records both lifts install:
 *
 *     800298BC  lhu v0, 4(s5)     ; item +4
 *     800298C4  subu v0, zero, v0 ; negated
 *     800298C8  sh  v0, 12(s0)    ; -> obj+0x44, the TARGET
 *     800298CC  lh  v0, 6(s5)     ; item +6
 *     800298D4  bgez / subu       ; abs()
 *     800298E0  sh  v0, 2(s0)     ; -> obj+0x3A, the SPEED
 *
 * The omission is why `q2_movers_build_calls` could not build one and said so:
 * "a mover with speed zero is triggered, ticks and never arrives". It has a
 * speed; the table was short an entry.
 */
{Q2_UF_CAGELIFT1, "CAGELIFT1", 20, true, true, 4, {
    {4,  1, Q2_UF_OP_U16,     "param_a", "negated into obj+0x44 (0x800298C4)"},
    {6,  1, Q2_UF_OP_S16,     "param_b", "abs() into obj+0x3A (0x800298DC)"},
    {8,  4, Q2_UF_OP_OBJSLOT, "objects", "up to four"},
    {18, 1, Q2_UF_OP_U8,      "time_b",  "* 300; 0xFF => never"}}},

{Q2_UF_ROTHATCH, "ROTHATCH", 20, true, true, 3, {
    {16, 1, Q2_UF_OP_U8,      "time_a", "* 300 into obj+0x4C"},
    {17, 1, Q2_UF_OP_U8,      "time_b", "* 300; 0xFF => never"},
    {18, 1, Q2_UF_OP_OBJSLOT, "object", NULL}}},

{Q2_UF_SIMROT, "SIMROT", 24, true, false, 3, {
    {4,  1, Q2_UF_OP_S16,     "speed",
     "angular speed into obj+0x3A (0x8002867C). The integrator at 0x8002F1A8 "
     "adds speed*dt to a 32-bit accumulator and takes the angle from its bits "
     "8..19, so this is 1/256 of an angle step per unit of dt"},
    {12, 4, Q2_UF_OP_OBJSLOT, "objects",
     "sets obj+0x50 bit 24 on each; no timing operands"},
    {20, 1, Q2_UF_OP_U16,     "axis",
     "low 2 bits into obj+0x50 bits 14-15 (0x80028664): 0 = X, 1 = Y, 2 = Z. "
     "Only obj[0x0C + 2*axis] is ever written, so one Euler angle is non-zero"}}},

{Q2_UF_SIMROT2, "SIMROT2", 24, true, false, 3, {
    {4,  1, Q2_UF_OP_S16,     "speed", "as SIMROT"},
    {12, 4, Q2_UF_OP_OBJSLOT, "objects", NULL},
    {20, 1, Q2_UF_OP_U16,     "axis",  "as SIMROT"}}},

/*
 * PLATFORM's speed is at +18 and this table did not list it either, and its
 * `origin` is not merely "read by the constructor" — it is what the TARGET is
 * computed from. `0x8002CBB0`, with `s0` the object itself here rather than
 * `obj + 0x38`:
 *
 *     8002CDB0  lh   v0, 18(t2)   ; t2 = the item (saved at 0x8002CC30)
 *     8002CDB8  bgez / subu       ; abs()
 *     8002CDC4  sh   v0, 58(s0)   ; -> obj+0x3A, the SPEED
 *     8002CDB4  sh   s4, 68(s0)   ; -> obj+0x44, the TARGET
 *
 * and `s4` is a DISTANCE: three squared deltas summed and passed through
 * `0x80055CBC`, between the `origin` operand and the object's own position. So
 * a platform's travel is not authored as a length, it is the gap between where
 * the node is and where the script says it should end up. That is why this
 * port does not build one yet — the target needs the node's position, which
 * the mover builder does not have.
 */
{Q2_UF_PLATFORM, "PLATFORM", 32, true, true, 5, {
    {4,  1, Q2_UF_OP_VEC3_S32, "origin",
     "the far END of the travel; the target is |origin - node| (0x8002CCE8)"},
    {18, 1, Q2_UF_OP_S16,      "speed",  "abs() into obj+0x3A (0x8002CDC4)"},
    {20, 1, Q2_UF_OP_OBJSLOT,  "object", NULL},
    {28, 1, Q2_UF_OP_U8,       "time_a", "written to obj+0x4C UNSCALED"},
    {29, 1, Q2_UF_OP_U8,       "time_b", "* 300; 0xFF => never"}}},

{Q2_UF_PISTON, "PISTON", 20, true, true, 4, {
    {5,  1, Q2_UF_OP_U8,      "param_a", "obj+0x3A"},
    {6,  1, Q2_UF_OP_U16,     "param_b", "obj+0x44"},
    {8,  1, Q2_UF_OP_OBJSLOT, "object",  NULL},
    {16, 1, Q2_UF_OP_U16,     "time",    "obj+0x4E, UNSCALED"}}},

/* ---- buttons and breakables -------------------------------------------- */
{Q2_UF_ROTBUTTON, "ROTBUTTON", 12, true, true, 2, {
    {6,  1, Q2_UF_OP_S16,     "time_b",
     "* 300 into obj+0x4E; -1 => never. Rotation target is a fixed 0x800"},
    {10, 1, Q2_UF_OP_OBJSLOT, "object", NULL}}},

{Q2_UF_BUTTON, "BUTTON", 16, true, true, 4, {
    {4,  1, Q2_UF_OP_S8,      "invert", "non-zero negates obj+0x44"},
    {8,  1, Q2_UF_OP_U16,     "time_b", "* 300 into obj+0x4E"},
    {12, 1, Q2_UF_OP_OBJSLOT, "object", NULL},
    {14, 1, Q2_UF_OP_S16,     "travel",
     "sign selects obj+0x3A = +1 or -1; magnitude goes to obj+0x44"}}},

{Q2_UF_SIMBUTTON, "SIMBUTTON", 8, true, true, 0, {OP_NONE}},

{Q2_UF_GLASS, "GLASS", 16, true, true, 4, {
    {4,  1, Q2_UF_OP_OBJSLOT, "object", NULL},
    {6,  1, Q2_UF_OP_U16,     "hit_points",
     "decremented by the damage callback; mutated in place at run time"},
    {10, 1, Q2_UF_OP_U8,      "param_a", "obj+0x56"},
    {12, 1, Q2_UF_OP_U8,      "param_b", "obj+0x57"}}},

{Q2_UF_SHOOTTHEN, "SHOOTTHEN", 8, true, true, 2, {
    {4, 1, Q2_UF_OP_OBJSLOT, "object", NULL},
    {6, 1, Q2_UF_OP_U16,     "hit_points",
     "decremented by the damage amount; at zero, runs the record whose byte "
     "offset is in obj+0x40. Mutated in place at run time"}}},

{Q2_UF_DISH, "DISH", 8, true, true, 2, {
    {5, 1, Q2_UF_OP_S8,      "travel", "<< 5 into obj+0x44"},
    {6, 1, Q2_UF_OP_OBJSLOT, "object",
     "obj+0x4E is set to the global clock plus 300"}}},

{Q2_UF_B3ROCKS, "B3ROCKS", 12, true, true, 3, {
    {4, 1, Q2_UF_OP_OBJSLOT, "object",   "gets clock + 300 in obj+0x4E"},
    {6, 1, Q2_UF_OP_OBJSLOT, "object_b", "pointer parked in gp+0x4200"},
    {8, 1, Q2_UF_OP_OBJSLOT, "object_c", "pointer parked in gp+0x4204"}}},

/* ---- lights and visuals ------------------------------------------------- */
{Q2_UF_TIMEDLIGHT, "TIMEDLIGHT", 28, false, false, 5, {
    {4,  1, Q2_UF_OP_VEC3_S32, "origin", "absolute"},
    {16, 1, Q2_UF_OP_U16,      "unknown",
     "UNKNOWN and NOT PADDING: unread by the handler, but non-zero on 72 of "
     "78 items across 7 distinct values. Preserve it; do not act on it"},
    {18, 1, Q2_UF_OP_U16,      "radius",  "tripled before the call"},
    {20, 1, Q2_UF_OP_U32,      "param_a", NULL},
    {24, 1, Q2_UF_OP_U32,      "colour",  "packed; consumer is 0x80075D14"}}},

{Q2_UF_FLKLIGHT, "FLKLIGHT", 24, true, false, 4, {
    {4,  1, Q2_UF_OP_VEC3_S32, "origin",
     "restored from the pristine buffer; bit 0 of the first word gates the "
     "whole primitive, which is INFERRED, not confirmed"},
    {16, 1, Q2_UF_OP_S16,      "light_id", "matched by 0x800689C0"},
    {18, 1, Q2_UF_OP_U8,       "colour_r", NULL},
    {19, 1, Q2_UF_OP_U8,       "colour_g",
     "colour_b follows at +20. The ((rand()*500)>>15)+400 and +1000 formulas "
     "this note once called on/off TIMES are the light's RADII: 0x80028858 "
     "stores the first into a2's low half, 0x8002888C the second into its "
     "high half, and 0x800288C8 hands that a2 to 0x80075C34. The flicker's "
     "actual durations are unread"}}},

{Q2_UF_SETWIBBLE, "SETWIBBLE", 8, true, false, 2, {
    {4, 1, Q2_UF_OP_S16, "scene_node",
     "a Scene NODE index, NOT an object slot — the constructor only restores "
     "the original bytes, it never rewrites them"},
    {6, 1, Q2_UF_OP_U16, "wibble",
     "low 4 bits are written to Scene.flags08 bits 10..13"}}},

{Q2_UF_OBJDRAWOFF, "OBJDRAWOFF", 12, true, true, 1, {
    {4, 4, Q2_UF_OP_OBJSLOT, "objects", "up to four; negative terminates"}}},

{Q2_UF_OBJDRAWON, "OBJDRAWON", 12, false, false, 1, {
    {4, 4, Q2_UF_OP_S16, "scene_nodes",
     "UNUSED on this disc. Reads Scene node indices directly and clears "
     "flags08 bit 15 — note it does NOT go through the object array, unlike "
     "its OFF counterpart"}}},

{Q2_UF_EXPLO, "EXPLO", 24, true, false, 0, {OP_NONE}}
};

/* ------------------------------------------------------------------------- */
static const q2_uf_prim_info *uf_lookup(q2_uf_prim prim)
{
    u32 i;

    if (prim < 0 || prim >= Q2_UF_PRIM_COUNT)
        return NULL;

    /* The table is written in readability order, not enum order, so this is a
     * scan rather than an index. 43 entries; it is not worth an index array. */
    for (i = 0; i < Q2_UF_PRIM_COUNT; i++) {
        if (uf_table[i].prim == prim)
            return &uf_table[i];
    }

    return NULL;
}

const q2_uf_prim_info *q2_uf_info(q2_uf_prim prim)
{
    return uf_lookup(prim);
}

q2_uf_prim q2_uf_prim_from_name(const char *name)
{
    u32 i;

    if (!name)
        return Q2_UF_PRIM_UNKNOWN;

    for (i = 0; i < Q2_UF_PRIM_COUNT; i++) {
        if (strncmp(uf_table[i].name, name, Q2_UF_NAME_LEN + 1) == 0)
            return uf_table[i].prim;
    }

    return Q2_UF_PRIM_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
q2_result q2_userfuncs_parse(q2_userfuncs *out, const q2_common_file *f)
{
    const dat_chunk *chunk;
    u32 count;

    if (!out || !f)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = f->chunk[Q2_COMMON_USER_FUNCS];
    if (!chunk || !chunk->data)
        return Q2_ERR_NOT_FOUND;

    if (chunk->size < 4 || (chunk->size & 3u))
        return Q2_ERR_BAD_FORMAT;

    /* The binder reads the count with lhu, so only the low halfword is real.
     * Every count on this disc is 0..18 and the high half is zero, but reading
     * it the way the engine does is free and cannot be wrong. */
    count = q2_rd_u16(chunk->data);

    if (chunk->size != 4u + Q2_UF_RECORD_SIZE * count)
        return Q2_ERR_BAD_FORMAT;

    out->data  = chunk->data;
    out->size  = chunk->size;
    out->count = count;

    return Q2_OK;
}

bool q2_userfuncs_name(const q2_userfuncs *uf, u32 index, char *out)
{
    if (!uf || !uf->data || !out || index >= uf->count)
        return false;

    memcpy(out, uf->data + 4 + Q2_UF_RECORD_SIZE * index, Q2_UF_NAME_LEN);
    out[Q2_UF_NAME_LEN] = '\0';

    return true;
}

q2_uf_prim q2_userfuncs_prim(const q2_userfuncs *uf, u32 index)
{
    char name[Q2_UF_NAME_LEN + 1];

    if (!q2_userfuncs_name(uf, index, name))
        return Q2_UF_PRIM_UNKNOWN;

    return q2_uf_prim_from_name(name);
}

/* ------------------------------------------------------------------------- */
q2_result q2_uf_decode_call(q2_uf_call *out, const q2_userfuncs *uf,
                            const q2_event_item *item)
{
    u8 index;

    if (!out || !uf || !item)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->prim = Q2_UF_PRIM_UNKNOWN;

    if (item->opcode != Q2_EVOP_CALL)
        return Q2_ERR_INVALID_ARG;

    /* The shortest legal CALL is four bytes: op, len, index, pad. */
    if (item->len < 4 || !item->payload)
        return Q2_ERR_BAD_FORMAT;

    index = q2_rd_u8(item->payload);        /* payload is item + 2 */

    out->func_index = index;
    out->item       = item->payload - 2;
    out->item_len   = item->len;

    /* The engine's own bounds check. Past the end is a silent no-op there, so
     * the caller gets a distinguishable code rather than a hard failure. */
    if (index >= uf->count)
        return Q2_ERR_RANGE;

    out->prim = q2_userfuncs_prim(uf, index);
    out->info = uf_lookup(out->prim);

    if (!out->info)
        return Q2_ERR_UNSUPPORTED;   /* bound to a stub by this build */

    if (out->info->item_len != item->len)
        return Q2_ERR_BAD_FORMAT;

    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
static u32 uf_optype_width(q2_uf_optype t)
{
    switch (t) {
    case Q2_UF_OP_U8:
    case Q2_UF_OP_S8:      return 1;
    case Q2_UF_OP_U16:
    case Q2_UF_OP_S16:
    case Q2_UF_OP_OBJSLOT: return 2;
    case Q2_UF_OP_U32:
    case Q2_UF_OP_S32:     return 4;
    case Q2_UF_OP_NAME12:  return Q2_UF_NAME_LEN;
    case Q2_UF_OP_VEC3_S32:return 12;
    default:               return 0;
    }
}

/* Resolve an operand to a byte pointer, or NULL if anything is out of range.
 * Everything below funnels through here so no read can ever escape the item. */
static const u8 *uf_operand_at(const q2_uf_call *c, u32 op, u32 element,
                               u32 elem_size, q2_uf_optype want)
{
    const q2_uf_operand *o;
    u32 offset;

    if (!c || !c->info || !c->item || op >= c->info->operand_count)
        return NULL;

    o = &c->info->operands[op];

    if (want != Q2_UF_OP_NONE && o->type != want)
        return NULL;

    if (element >= o->count)
        return NULL;

    offset = (u32)o->offset + element * elem_size;

    if (offset + elem_size > c->item_len)
        return NULL;

    return c->item + offset;
}

bool q2_uf_operand_u32(const q2_uf_call *c, u32 op, u32 element, u32 *out)
{
    const q2_uf_operand *o;
    const u8 *p;
    u32 w;

    if (!c || !c->info || !out || op >= c->info->operand_count)
        return false;

    o = &c->info->operands[op];
    w = uf_optype_width(o->type);

    /* A VEC3 has no single unsigned value; ask for it with the vec3 reader. */
    if (w == 0 || w > 4)
        return false;

    p = uf_operand_at(c, op, element, w, Q2_UF_OP_NONE);
    if (!p)
        return false;

    switch (w) {
    case 1: *out = q2_rd_u8(p);  return true;
    case 2: *out = q2_rd_u16(p); return true;
    default:*out = q2_rd_u32(p); return true;
    }
}

bool q2_uf_operand_s32(const q2_uf_call *c, u32 op, u32 element, s32 *out)
{
    const q2_uf_operand *o;
    u32 raw;

    if (!c || !c->info || !out || op >= c->info->operand_count)
        return false;

    if (!q2_uf_operand_u32(c, op, element, &raw))
        return false;

    o = &c->info->operands[op];

    switch (o->type) {
    case Q2_UF_OP_S8:      *out = (s8)raw;  return true;
    case Q2_UF_OP_S16:
    case Q2_UF_OP_OBJSLOT: *out = (s16)raw; return true;
    default:               *out = (s32)raw; return true;
    }
}

bool q2_uf_operand_name(const q2_uf_call *c, u32 op, char *out)
{
    const u8 *p;

    if (!out)
        return false;

    p = uf_operand_at(c, op, 0, Q2_UF_NAME_LEN, Q2_UF_OP_NAME12);
    if (!p)
        return false;

    /* NOT NUL-terminated on disc when the name is exactly 12 characters —
     * HELPCOMPUTER is one — so terminate here and never strlen the raw bytes. */
    memcpy(out, p, Q2_UF_NAME_LEN);
    out[Q2_UF_NAME_LEN] = '\0';

    return true;
}

bool q2_uf_operand_vec3(const q2_uf_call *c, u32 op, s32 out[3])
{
    const u8 *p;
    u32 i;

    if (!out)
        return false;

    p = uf_operand_at(c, op, 0, 12, Q2_UF_OP_VEC3_S32);
    if (!p)
        return false;

    for (i = 0; i < 3; i++)
        out[i] = q2_rd_s32(p + 4 * i);

    return true;
}

bool q2_uf_operand_slot_raw(const q2_uf_call *c, u32 op, u32 element, s16 *out)
{
    const u8 *p;

    if (!out)
        return false;

    p = uf_operand_at(c, op, element, 2, Q2_UF_OP_OBJSLOT);
    if (!p)
        return false;

    *out = q2_rd_s16(p);

    return true;
}

const u8 *q2_uf_operand_at(const q2_uf_operands *src, const u8 *p, u32 need)
{
    size_t off;

    if (!src || !src->base_a || !src->base_b || !p)
        return p;
    if (p < src->base_a)
        return p;

    off = (size_t)(p - src->base_a);
    if (off + need > src->b_size)
        return p;

    return src->base_b + off;
}
