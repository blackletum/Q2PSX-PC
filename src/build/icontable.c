#include "icontable.h"

#include "itemtable.h"

#include <stdlib.h>
#include <string.h>

/*
 * The PS-EXE's load address less its header, the same bias hudtables.c uses:
 * the file starts 0x800 bytes before the segment, and the segment is at
 * 0x80018000.
 */
#define Q2_ICON_VADDR_BIAS 0x80017800u

static const u8 *at(const q2_buf *exe, u32 vaddr, size_t len)
{
    u32 off = vaddr - Q2_ICON_VADDR_BIAS;

    if (vaddr < Q2_ICON_VADDR_BIAS)
        return NULL;
    if ((size_t)off + len > exe->size)
        return NULL;
    return exe->data + off;
}

q2_result q2_icon_tables_load(q2_icon_tables *out, const disc *d,
                              const q2_build_id *id)
{
    q2_result r;
    const u8 *p;
    u32 i;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /*
     * Gated on identification rather than attempted blind, for the same reason
     * the HUD tables are: a rect table read at the wrong offset does not fail,
     * it yields plausible-looking rectangles that sample the wrong pixels.
     */
    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("status-bar table locations are unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }
    if (!id->exe_name[0])
        return Q2_ERR_NOT_FOUND;

    r = disc_read_file(d, id->exe_name, &out->exe);
    if (r != Q2_OK)
        return r;

    p = at(&out->exe, Q2_ICON_ADDR_RECTS, Q2_ICON_COUNT * Q2_ICON_RECORD);
    if (!p)
        goto bad;
    for (i = 0; i < Q2_ICON_COUNT; i++) {
        out->rect[i].u  = p[i * Q2_ICON_RECORD + 0];
        out->rect[i].v  = p[i * Q2_ICON_RECORD + 1];
        out->rect[i].w  = p[i * Q2_ICON_RECORD + 2];
        out->rect[i].h  = p[i * Q2_ICON_RECORD + 3];
        out->rect[i].id = p[i * Q2_ICON_RECORD + 4];
    }
    out->rect_count = Q2_ICON_COUNT;

    p = at(&out->exe, Q2_ICON_ADDR_AMMO_ICON, Q2_ICON_WEAPONS);
    if (!p)
        goto bad;
    memcpy(out->ammo_icon, p, Q2_ICON_WEAPONS);

    p = at(&out->exe, Q2_ICON_ADDR_AMMO_KIND, Q2_ICON_WEAPONS);
    if (!p)
        goto bad;
    memcpy(out->ammo_kind, p, Q2_ICON_WEAPONS);

    return Q2_OK;

bad:
    q2_buf_free(&out->exe);
    return Q2_ERR_BAD_FORMAT;
}

void q2_icon_tables_free(q2_icon_tables *t)
{
    if (!t)
        return;
    q2_buf_free(&t->exe);
    memset(t, 0, sizeof(*t));
}

const q2_icon_rect *q2_icon_rect_get(const q2_icon_tables *t, u32 index)
{
    if (!t || index >= t->rect_count)
        return NULL;
    return &t->rect[index];
}

u8 q2_icon_ammo_for_weapon(const q2_icon_tables *t, int weapon_id)
{
    if (!t || weapon_id < 0 || weapon_id >= Q2_ICON_WEAPONS)
        return 0;
    return t->ammo_icon[weapon_id];
}

const q2_icon_rect *q2_icon_rect_for_id(const q2_icon_tables *t, u8 effect,
                                        u32 *index_out)
{
    u32 i;

    if (!t || effect == 0)
        return NULL;

    for (i = 0; i < t->rect_count; i++) {
        if (t->rect[i].id != effect)
            continue;
        if (index_out)
            *index_out = i;
        return &t->rect[i];
    }
    return NULL;
}

const q2_icon_rect *q2_icon_ammo_rect(const q2_icon_tables *t, int weapon_id)
{
    return q2_icon_rect_for_id(t, q2_icon_ammo_for_weapon(t, weapon_id), NULL);
}

const char *q2_icon_name_for_id(u8 effect)
{
    const q2_item_table *items = q2_item_table_builtin();
    u32 i;

    if (!items || effect == 0)
        return NULL;

    for (i = 0; i < items->count; i++)
        if (items->def[i].effect == effect)
            return items->def[i].model;
    return NULL;
}

const char *q2_icon_item_name(const q2_icon_tables *t, u32 rect_index)
{
    const q2_icon_rect *r = q2_icon_rect_get(t, rect_index);

    return r ? q2_icon_name_for_id(r->id) : NULL;
}

bool q2_icon_on_grid(const q2_icon_rect *r)
{
    if (!r)
        return false;
    if (r->h != Q2_ICON_CELL_H)
        return false;
    if ((r->u % Q2_ICON_CELL_W) != 0)
        return false;
    if ((r->v % Q2_ICON_CELL_H) != 0)
        return false;

    /* The rightmost column is one texel narrow so that u + w stays inside the
     * 256-texel page — see the note in the header. */
    if (r->u == 224)
        return r->w == Q2_ICON_LAST_COL_W || r->w == Q2_ICON_CELL_W;
    return r->w == Q2_ICON_CELL_W;
}

q2_icon_size q2_icon_draw_size_of(int players, int weapon_id,
                                  u8 src_w, u8 src_h)
{
    q2_icon_size s;

    /*
     * 0x800353B0 onward. A weapon id of zero maps to rect 0, the 1 x 1 blank,
     * and the code collapses the drawn size to match rather than scaling the
     * blank up. Otherwise two players force 24 x 18 and three or more 16 x 12,
     * and single player passes the record's own size straight through.
     */
    if (weapon_id <= 0) {
        s.w = 1;
        s.h = 1;
        return s;
    }

    if (players >= 3) {
        s.w = 16;
        s.h = 12;
    } else if (players == 2) {
        s.w = 24;
        s.h = 18;
    } else {
        s.w = src_w;
        s.h = src_h;
    }
    return s;
}

q2_icon_size q2_icon_draw_size(int players, int weapon_id)
{
    return q2_icon_draw_size_of(players, weapon_id,
                                Q2_ICON_CELL_W, Q2_ICON_CELL_H);
}
