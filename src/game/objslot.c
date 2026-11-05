#include "objslot.h"

static s8  g_stack[Q2_OBJ_SLOTS];   /* 0x800D4A28: free slot numbers        */
static u32 g_used;                  /* the cursor's depth, 0x800B2BAC       */
static u8  g_owner[Q2_OBJ_SLOTS];
static s32 g_ref[Q2_OBJ_SLOTS];

void q2_obj_slots_reset(void)
{
    u32 i;

    /* 0x8006D0CC..0x8006D0F0: entry i holds record i, the cursor at entry 0. */
    for (i = 0; i < Q2_OBJ_SLOTS; i++) {
        g_stack[i] = (s8)i;
        g_owner[i] = Q2_OBJ_FREE;
        g_ref[i]   = -1;
    }
    g_used = 0;
}

s32 q2_obj_slot_alloc(q2_obj_owner owner, s32 ref)
{
    s32 slot;

    /* 0x8006C0B8: `slti 48`. The full arm's eviction (a0 = 1) is not
     * modelled; the caller's object simply has no slot and runs last. */
    if (g_used >= Q2_OBJ_SLOTS)
        return -1;
    slot = g_stack[g_used++];             /* 0x8006C17C `lw s0, 0(v1)` */
    g_owner[slot] = (u8)owner;
    g_ref[slot]   = ref;
    return slot;
}

void q2_obj_slot_free(s32 slot)
{
    if (slot < 0 || slot >= Q2_OBJ_SLOTS || g_owner[slot] == Q2_OBJ_FREE ||
        g_used == 0)
        return;
    /* 0x8006D2C8..0x8006D2D4: the cursor steps back and the record goes on
     * top. */
    g_stack[--g_used] = (s8)slot;
    g_owner[slot] = Q2_OBJ_FREE;
    g_ref[slot]   = -1;
}

q2_obj_owner q2_obj_slot_owner(s32 slot, s32 *ref)
{
    if (slot < 0 || slot >= Q2_OBJ_SLOTS) {
        if (ref) *ref = -1;
        return Q2_OBJ_FREE;
    }
    if (ref) *ref = g_ref[slot];
    return (q2_obj_owner)g_owner[slot];
}

u32 q2_obj_slots_used(void)
{
    return g_used;
}
