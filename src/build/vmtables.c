#include "vmtables.h"

#include "exe.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Addresses                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * SLES-01534's, translated into whichever build is on the disc (exe.h). Reading
 * a moved address does not fail — it produces a bank of plausible nonsense —
 * so an executable whose layout is not catalogued is refused rather than
 * guessed at.
 */
#define VM_ADDR_CLIP_INDEX 0x8009F59Cu  /* the 12-entry pointer table        */
#define VM_ADDR_NAME_TABLE 0x8009DB9Cu  /* the 1-based 12-byte model names   */

const char *q2_vm_state_name(q2_vm_state s)
{
    switch (s) {
    case Q2_VM_RAISE: return "raise";
    case Q2_VM_FIRE:  return "fire";
    case Q2_VM_IDLE:  return "idle";
    case Q2_VM_LOWER: return "lower";
    default:          return "?";
    }
}

/* ------------------------------------------------------------------------- */
static bool read_key(const q2_exe *e, u32 addr, q2_vm_key *k)
{
    int i;

    for (i = 0; i < 3; i++) {
        /* +0 is the rotation and +6 the translation — see vmtables.h. */
        if (!q2_exe_s16(e, addr + (u32)i * 2u, &k->r[i]))
            return false;
        if (!q2_exe_s16(e, addr + 6u + (u32)i * 2u, &k->t[i]))
            return false;
    }
    return q2_exe_s16(e, addr + 12u, &k->duration)
        && q2_exe_s16(e, addr + 14u, &k->jitter)
        && q2_exe_s16(e, addr + 16u, &k->event)
        && q2_exe_s16(e, addr + 18u, &k->pad);
}

q2_result q2_vm_tables_load(q2_vm_tables *out, const disc *d,
                            const q2_build_id *id)
{
    q2_exe e;
    q2_result r;
    u32 clip_index, name_table;
    u32 block[Q2_VM_SLOTS][Q2_VM_STATES + 1];
    u32 total = 0;
    u32 cursor = 0;
    int w, s;

    if (!out || !d)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    r = q2_exe_load(&e, d, id && id->exe_name[0] ? id->exe_name : NULL);
    if (r != Q2_OK)
        return r;
    if (!q2_exe_has_layout(&e)) {
        q2_exe_free(&e);
        return Q2_ERR_UNSUPPORTED;
    }
    clip_index = q2_exe_addr(&e, VM_ADDR_CLIP_INDEX);
    name_table = q2_exe_addr(&e, VM_ADDR_NAME_TABLE);

    /*
     * Two passes. The first reads every clip's bounds and totals the keys so
     * they can live in one allocation; the second fills it. The bounds are
     * pointer differences — clip N runs from block[N] to block[N+1] — which is
     * the format's own way of expressing a length and the reason the fifth
     * pointer exists.
     */
    for (w = 0; w < Q2_VM_SLOTS; w++) {
        u32 blockaddr;

        if (!q2_exe_u32(&e, clip_index + (u32)w * 4u, &blockaddr)) {
            q2_exe_free(&e);
            return Q2_ERR_BAD_FORMAT;
        }

        for (s = 0; s <= Q2_VM_STATES; s++) {
            if (!q2_exe_u32(&e, blockaddr + (u32)s * 4u, &block[w][s])) {
                q2_exe_free(&e);
                return Q2_ERR_BAD_FORMAT;
            }
        }

        for (s = 0; s < Q2_VM_STATES; s++) {
            u32 lo = block[w][s], hi = block[w][s + 1];
            u32 n;

            if (hi < lo || ((hi - lo) % Q2_VM_KEY_SIZE) != 0) {
                q2_exe_free(&e);
                return Q2_ERR_BAD_FORMAT;
            }
            n = (hi - lo) / Q2_VM_KEY_SIZE;
            if (n > Q2_VM_MAX_KEYS) {
                q2_exe_free(&e);
                return Q2_ERR_BAD_FORMAT;
            }
            out->clip[w][s].addr  = lo;
            out->clip[w][s].count = n;
            total += n;
        }
    }

    out->storage = (q2_vm_key *)calloc(total ? total : 1, sizeof(q2_vm_key));
    if (!out->storage) {
        q2_exe_free(&e);
        return Q2_ERR_NO_MEMORY;
    }
    out->key_count = total;

    for (w = 0; w < Q2_VM_SLOTS; w++) {
        for (s = 0; s < Q2_VM_STATES; s++) {
            q2_vm_clip *c = &out->clip[w][s];
            u32 k;

            c->key = out->storage + cursor;
            for (k = 0; k < c->count; k++) {
                if (!read_key(&e, c->addr + k * Q2_VM_KEY_SIZE,
                              out->storage + cursor + k)) {
                    q2_vm_tables_free(out);
                    q2_exe_free(&e);
                    return Q2_ERR_BAD_FORMAT;
                }
            }
            cursor += c->count;
        }

        /* The model name, width-limited rather than NUL-terminated: two of the
         * twelve fill all twelve bytes. */
        {
            u32 base = name_table + (u32)w * 12u;
            int i;
            for (i = 0; i < 12; i++) {
                u8 ch = 0;
                if (!q2_exe_u8(&e, base + (u32)i, &ch))
                    ch = 0;
                out->model_name[w][i] = (char)ch;
            }
            out->model_name[w][12] = '\0';
        }
    }

    q2_exe_free(&e);
    return Q2_OK;
}

void q2_vm_tables_free(q2_vm_tables *t)
{
    if (!t)
        return;
    free(t->storage);
    t->storage = NULL;
    t->key_count = 0;
}

const q2_vm_clip *q2_vm_clip_get(const q2_vm_tables *t, int weapon,
                                 q2_vm_state state)
{
    if (!t || weapon < 0 || weapon >= Q2_VM_SLOTS)
        return NULL;
    if ((int)state < 0 || (int)state >= Q2_VM_STATES)
        return NULL;
    if (t->clip[weapon][state].count == 0)
        return NULL;
    return &t->clip[weapon][state];
}
