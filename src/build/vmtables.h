/*
 * vmtables.h — the view model's animation bank, read out of the executable.
 *
 * ---------------------------------------------------------------------------
 * What the view model is
 * ---------------------------------------------------------------------------
 * The weapon in the player's hands is not a special case of the entity
 * renderer and it is not a sprite. It is a `CastList` model — the bank calls it
 * "Blaster G", "Supershot G", the same twelve-byte names the weapon tables
 * carry at 0x8009DB9C — driven by its own four-state machine and its own
 * animation bank, which lives in the executable's data segment rather than on
 * the disc.
 *
 * That bank is what this module reads.
 *
 * ---------------------------------------------------------------------------
 * The shape of it
 * ---------------------------------------------------------------------------
 * A pointer table at **0x8009F59C**, twelve entries, indexed by the 1-based
 * weapon id. Slot 0 aliases slot 1, which is the same "firing no weapon is a
 * call that returns" convention the fire-function table uses.
 *
 * Each entry points at a **five-pointer block** (0x8009F4C0, stride 20). Four
 * of them are the clips, one per state; the fifth is the end sentinel, and it
 * is exactly the next weapon's first clip — the clips are laid out contiguously
 * and each one's length is the difference between neighbouring pointers. That
 * is how the engine knows where a clip ends: the driver hands the state machine
 * `(clip_start, clip_end)` and the machine's whole "has the animation finished"
 * test is `clip_start + 20*frame == clip_end` (0x8004F93C).
 *
 *     state 0  RAISE   ends -> IDLE
 *     state 1  FIRE    ends -> IDLE
 *     state 2  IDLE    the resting loop; firing enters FIRE
 *     state 3  LOWER   ends -> the model is swapped and the state resets
 *
 * ---------------------------------------------------------------------------
 * A key
 * ---------------------------------------------------------------------------
 * Twenty bytes, and every field of it is consumed at a named instruction:
 *
 *     +0  s16 rx        local rotation, 4096-step circle (0x8004EF80)
 *     +2  s16 ry
 *     +4  s16 rz
 *     +6  s16 tx        local translation (0x8004F494)
 *     +8  s16 ty
 *     +10 s16 tz
 *
 * WHICH TRIPLE IS WHICH, because it is not guessable and getting it the wrong
 * way round puts the weapon two thousand units away instead of in the hands:
 * the triple at **+6** is the one that reaches `0x8006FD38` at `0x8004F5E0` to
 * be rotated by the view matrix and added to the eye, so +6 is the TRANSLATION.
 * The triple at +0 feeds the angle chain at `0x8004F284` instead. Their
 * magnitudes agree — the blaster's +0 triple runs to 2248, which is half the
 * 4096-step circle, while its +6 triple is 140/157/44, a few units in front of
 * the eye.
 *     +12 s16 duration  in dt units (0x8004F250)
 *     +14 s16 jitter    non-zero randomises the duration (0x8004F1F8)
 *     +16 s16 event     -1 for none; handed to 0x80050454 (0x8004EF34)
 *     +18 s16 pad       zero on every key on the disc
 *
 * Interpolation is linear from the value the previous key left behind to this
 * key's value, over the key's duration:
 *
 *     v = prev + (key - prev) * (total - left) / total
 *
 * and a jittered duration is `((rand() * duration) >> 15 + 1) * 10`, which is
 * what makes the idle loop of a weapon that has one breathe irregularly.
 */
#ifndef Q2PSX_VMTABLES_H
#define Q2PSX_VMTABLES_H

#include "disc.h"
#include "ident.h"
#include "q2psx.h"

#define Q2_VM_STATES        4
#define Q2_VM_SLOTS        12   /* 1-based weapon ids; slot 0 aliases slot 1 */
#define Q2_VM_KEY_SIZE     20
#define Q2_VM_MAX_KEYS   4096   /* a sanity bound, not a format limit        */

/* No event on this key. The engine stores -1 and tests it as a signed half. */
#define Q2_VM_EVENT_NONE   (-1)

typedef enum q2_vm_state {
    Q2_VM_RAISE = 0,
    Q2_VM_FIRE,
    Q2_VM_IDLE,
    Q2_VM_LOWER
} q2_vm_state;

const char *q2_vm_state_name(q2_vm_state s);

typedef struct q2_vm_key {
    s16 r[3];        /* +0  local rotation, 4096-step circle                 */
    s16 t[3];        /* +6  local translation                                */
    s16 duration;    /* +12 dt units                                         */
    s16 jitter;      /* +14 non-zero: randomise the duration                 */
    s16 event;       /* +16 -1 for none                                      */
    s16 pad;         /* +18                                                  */
} q2_vm_key;

typedef struct q2_vm_clip {
    u32              addr;    /* where it was read from, for diagnostics     */
    u32              count;   /* keys                                        */
    const q2_vm_key *key;     /* into the bank's own storage                 */
} q2_vm_clip;

typedef struct q2_vm_tables {
    /* [weapon id][state]. Slot 0 aliases slot 1 exactly as the table does. */
    q2_vm_clip clip[Q2_VM_SLOTS][Q2_VM_STATES];

    /* The 1-based model names — the same array weapontables reads, kept here
     * so that resolving a weapon to the model in the player's hands needs only
     * this module. Two of them fill all twelve bytes with no NUL. */
    char       model_name[Q2_VM_SLOTS][13];

    q2_vm_key *storage;       /* every key, in one allocation                */
    u32        key_count;
} q2_vm_tables;

/*
 * Read the bank out of the disc's boot executable.
 *
 * Fails with Q2_ERR_UNSUPPORTED on a build whose table addresses are not
 * catalogued rather than reading a plausible-looking wrong address: an
 * animation bank read at the wrong offset does not fail, it produces a weapon
 * that waves in a way the console never did.
 */
q2_result q2_vm_tables_load(q2_vm_tables *out, const disc *d,
                            const q2_build_id *id);
void      q2_vm_tables_free(q2_vm_tables *t);

/* The clip for a weapon and state, or NULL. `weapon` is 1-based. */
const q2_vm_clip *q2_vm_clip_get(const q2_vm_tables *t, int weapon,
                                 q2_vm_state state);

#endif /* Q2PSX_VMTABLES_H */
