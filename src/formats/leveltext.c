#include "leveltext.h"

#include <stdio.h>
#include <string.h>

q2_result q2_leveltext_parse(q2_leveltext *out, const u8 *data, u32 size)
{
    u32 off = 0;

    if (!out || !data)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->base = data;
    out->size = size;

    /*
     * The directory is the same shape as the archive's own — 12 bytes of name
     * then a u32 — which is not a coincidence: the tool that wrote the .DAT
     * container wrote this too (dat.h). It ends at the first all-zero record,
     * and the text begins wherever the first entry points.
     */
    while (off + 16 <= size) {
        const u8 *rec = data + off;
        u32 text_off;
        u32 i;
        bool empty = true;

        for (i = 0; i < 16; i++)
            if (rec[i]) { empty = false; break; }
        if (empty)
            break;

        text_off = (u32)rec[12] | ((u32)rec[13] << 8) |
                   ((u32)rec[14] << 16) | ((u32)rec[15] << 24);
        if (text_off >= size)
            return Q2_ERR_BAD_FORMAT;

        if (out->count >= Q2_LEVELTEXT_MAX)
            return Q2_ERR_BAD_FORMAT;

        {
            q2_leveltext_entry *e = &out->entry[out->count++];

            /* The name is a fixed 12 bytes and need not be terminated —
             * `FoundASecret` fills it exactly. Copy the field, then terminate. */
            memcpy(e->name, rec, Q2_LEVELTEXT_NAME_LEN);
            e->name[Q2_LEVELTEXT_NAME_LEN] = '\0';
            e->offset = text_off;
            e->text   = (const char *)(data + text_off);
        }
        off += 16;
    }

    /* Every text must be terminated inside the chunk, or a reader walks off
     * the end of the buffer the first time it prints one. */
    {
        u32 i;
        for (i = 0; i < out->count; i++) {
            u32 p = out->entry[i].offset;
            while (p < size && data[p])
                p++;
            if (p >= size)
                return Q2_ERR_BAD_FORMAT;
        }
    }
    return Q2_OK;
}

q2_result q2_leveltext_open(q2_leveltext *out, const q2_common_file *f)
{
    const dat_chunk *c;

    if (!out || !f)
        return Q2_ERR_INVALID_ARG;

    c = f->chunk[Q2_COMMON_STRINGS];
    if (!c || !c->data)
        return Q2_ERR_BAD_FORMAT;

    return q2_leveltext_parse(out, c->data, c->size);
}

const char *q2_leveltext_find(const q2_leveltext *t, const char *name)
{
    u32 i;

    if (!t || !name)
        return NULL;

    for (i = 0; i < t->count; i++)
        if (strcmp(t->entry[i].name, name) == 0)
            return t->entry[i].text;
    return NULL;
}

void q2_leveltext_key_objective(char *out, int unit)
{
    /* 0x800AB9D0. */
    if (out)
        sprintf(out, "Unit%dMiss1", unit);
}

void q2_leveltext_key_orders(char *out, int unit, int step)
{
    /*
     * The counterpart the game leaves built in memory at 0x800ABA60
     * ("Unit3Curr4", "Unit3Curr3").
     *
     * The step is HEX, not decimal: SECURITY's dictionary runs Unit2Curr1
     * through Unit2CurrA, which is eleven steps and only reads as one sequence
     * if 10 formats as `A`. Decimal agrees for the first ten and then silently
     * stops finding anything, which is exactly the kind of miss that looks
     * like "this map has no orders" rather than like a bug.
     */
    if (out)
        sprintf(out, "Unit%dCurr%X", unit, step);
}
