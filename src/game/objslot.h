#ifndef Q2PSX_OBJSLOT_H
#define Q2PSX_OBJSLOT_H

#include "q2psx.h"

/*
 * THE CONSOLE'S OBJECT SLOTS — the 48 records at 0x800CBA28 (stride 0x300)
 * that every player, creature actor, pickup, gib and model object lives in,
 * and the order MoveModels (0x8006A4F0) runs their handlers in: slot 0 to 47,
 * each in-use record's +0x3C.
 *
 * The slots are handed out from a stack of free pointers at 0x800D4A28 whose
 * cursor is 0x800B2BAC. 0x8006D0BC fills it with slot 0 on top down to slot
 * 47; 0x8006C098 pops the top (so a fresh level allocates 0, 1, 2 ... in
 * order) and 0x8006D280 pushes the freed record back on top, so the slot freed
 * last is the next one taken.
 *
 * The port keeps players, creatures and entities in lists of their own, so
 * this is a MIRROR of that stack and of who holds each slot: the walk order is
 * the only thing taken from it. A gib thrown into a slot below its thrower's
 * is not moved until the next frame, and one above it is moved in the frame
 * it was thrown (BASE3's attract run, record frames 30 and 99).
 */

#define Q2_OBJ_SLOTS 48

typedef enum q2_obj_owner {
    Q2_OBJ_FREE = 0,
    Q2_OBJ_RESERVED,     /* a player, or a record the port does not model */
    Q2_OBJ_CREATURE,     /* `ref` is the creature-set index                 */
    Q2_OBJ_ENTITY        /* `ref` is the entity-set index                   */
} q2_obj_owner;

/* 0x8006D0BC: every slot free, slot 0 on top. */
void q2_obj_slots_reset(void);
/* 0x8006C098: the top of the stack, or -1 when all 48 are taken. */
s32  q2_obj_slot_alloc(q2_obj_owner owner, s32 ref);
/* 0x8006D280: back on top. A negative or free slot is ignored. */
void q2_obj_slot_free(s32 slot);
/* Who holds a slot; `ref` receives the owner's index. */
q2_obj_owner q2_obj_slot_owner(s32 slot, s32 *ref);
/* How many are taken (the cursor's depth). */
u32  q2_obj_slots_used(void);

#endif
