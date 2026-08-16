/*
 * mover.h — doors, lifts and double doors.
 *
 * MOVER_A, MOVER_B and MOVER_C are not three motions. They are ONE motion —
 * an axis-aligned translation of a group of Scene nodes — with three
 * parameterisations. This integrator contains no rotation: no matrix, no angle
 * field, and no GTE call.
 *
 *     MOVER_A  1,006 uses. Vertical, axis hard-wired to Y. Lifts and most doors.
 *     MOVER_B     20 uses. Horizontal, axis taken from the payload.
 *     MOVER_C    292 uses. Double door: two leaves moving opposite ways.
 *
 * CORRECTION — that is true of these three and NOT of the engine.
 *
 * This file used to say "there is no rotation anywhere in the engine". The zone
 * draw refutes it: at 0x800678B4 it calls `RotMatrix` on three s16 Euler angles
 * at the node's runtime object +0x0C, and then adds TWO independent s16 triples,
 * +0x12 and +0x18, to the node's camera-space position. So a node carries a full
 * rotation and two translations, and `ROTHATCH`, `SIMROT`, `SIMROT2` and
 * `ROTBUTTON` — which userfuncs.c has always listed as rotating movers — drive
 * the rotation slots of the same object this module writes the translation of.
 *
 * What is implemented here is the linear family and its +0x12 triple. The
 * rotation slots and the second triple are decoded (see surface.h, which maps
 * the whole object binding out of Scene.flags08 bits 0-9) but no integrator
 * fills them yet, so rotating brush geometry stands still rather than turning.
 *
 * ---------------------------------------------------------------------------
 * Why this port does not reproduce the load-time pre-pass
 * ---------------------------------------------------------------------------
 * The original rewrites the script's s16 slots in place at load, turning Scene
 * node indices into indices into a fixed 48-entry runtime array. That array
 * exists because the console had 2 MB; it is an allocation strategy, not
 * meaning.
 *
 * A native port wants the opposite: read the DISC values, which are Scene node
 * indices, and build objects directly. So this module never performs the
 * rewrite, and the disc payload is the only input. That also sidesteps the
 * subtlety that made movers unimplementable for so long — the same bytes mean
 * different things before and after the pre-pass.
 *
 * ---------------------------------------------------------------------------
 * On-disc payloads
 * ---------------------------------------------------------------------------
 * Offsets are from the start of the event item (op at +0, len at +1).
 *
 *   MOVER_A, len 24:
 *     +2  s16  travel        target displacement is -travel
 *     +4  s16  speed         absolute value taken
 *     +6  s16  portal_node   Scene node carrying the visibility bit; -1 none
 *     +8  s16  node[4]       Scene nodes translated; -1 unused
 *     +16 u16  key_mask      required key bits; 0 means unlocked
 *     +18 u8   delay         seconds before opening
 *     +19 u8   wait          seconds held open; 0xFF never auto-closes
 *     +20 s16  touch         non-zero also opens on touch (65 of 1,006)
 *
 *   MOVER_B, len 24: as A but +8 is the axis (& 3; only 0 and 2 occur), the
 *     nodes move to +10, key_mask to +18, delay/wait to +20/+21, and the
 *     target is +travel when the axis field is 0, -travel otherwise.
 *
 *   MOVER_C, len 32: two leaves at +10 and +18, key_mask +26, delay/wait
 *     +28/+29. Leaf 1's target is the negation of leaf 0's, and leaf 1 owns the
 *     portal node and points at leaf 0 as its partner.
 *
 * ---------------------------------------------------------------------------
 * Timing
 * ---------------------------------------------------------------------------
 * delay and wait are bytes multiplied by 300 and counted down by the same dt
 * the simulation uses, so one byte is one second at the 25 Hz tick.
 *
 * Note this 300 is genuine, unlike TIMER's, which is x30 — see userfuncs.h.
 * The two use different instruction idioms and conflating them makes every
 * door ten times too slow.
 */
#ifndef Q2PSX_MOVER_H
#define Q2PSX_MOVER_H

#include "events.h"
#include "userfuncs.h"
#include "q2psx.h"
#include "scene.h"

#define Q2_MOVER_MAX_PARTS  4
#define Q2_MOVER_TIMEBASE 300
#define Q2_MOVER_WAIT_NEVER 0xFFFFu

/* ------------------------------------------------------------------------- */
/* The sounds — 0x80025658's three, and they are per OPCODE FAMILY            */
/* ------------------------------------------------------------------------- */
/*
 * A mover made no sound at all, and the reason is that this module never had a
 * sound path: `q2_movers_tick` reaches all three of the transitions the
 * original plays on and plays nothing at any of them.
 *
 * The linear handler at 0x80025658 makes exactly three `jal 0x80073704` calls:
 *
 *   0x800257A8  msc_keyuse   a keyed door accepted the player's key
 *   0x8002585C  msc_keytry   it refused, and only the first time — the same
 *                            once-only latch `announced` already models
 *   0x80025A5C  pt1__strt    the motion starts, reached from BOTH the
 *                            DELAY->OPENING arm (0x800258F0) and the
 *                            OPEN->CLOSING arm (0x800259BC)
 *
 * There is NO arrival sound and NO looping move sound on a linear door. Do not
 * assume the start/loop/stop shape: only ROTHATCH has it (0x8002B250 plays
 * pt1__strt, then pt1__mid through the LOOPING entry point 0x80073734, and
 * pt1__end on arrival, stopping the loop with 0x8007398C).
 *
 * The six handles are all `lw a0, N(gp)` GLOBALS registered by name at
 * 0x8002D4E0..0x8002D800 — not one is loaded from the runtime object, and no
 * payload carries a sound operand. So the sound is a property of the opcode
 * family, and this port folds several families into one set:
 *
 *   MOVER_A/B/C, LIFT1, CAGELIFT1, PLATFORM -> the three above
 *   BUTTON                                  -> amb_butn2 and nothing else
 *   PISTON, DISH                            -> silent, deliberately
 *
 * WHERE it comes from: every call passes the CENTRE of the mover's collision
 * box, and the CLOSING one adds the live displacement (0x80025A00 onward), so
 * the sound follows the door.
 */
typedef enum q2_mover_sound {
    Q2_MVSND_NONE = -1,
    Q2_MVSND_KEY_USE = 0,   /* msc_keyuse, 0x800B27CC */
    Q2_MVSND_KEY_TRY,       /* msc_keytry, 0x800B27D0 */
    Q2_MVSND_START,         /* pt1__strt,  0x800B27D8 */
    Q2_MVSND_BUTTON,        /* amb_butn2,  0x800B27E4 */
    Q2_MVSND_COUNT
} q2_mover_sound;

/* The bank keys, in that order. */
extern const char *const q2_mover_sound_name[Q2_MVSND_COUNT];

/* obj+0x52 in the original. */
typedef enum q2_mover_state {
    Q2_MV_IDLE = 0,
    Q2_MV_OPENING,
    Q2_MV_ARRIVED,
    Q2_MV_CLOSING,
    Q2_MV_BLOCKED,
    Q2_MV_DELAY,
    Q2_MV_OPEN
} q2_mover_state;

enum {
    Q2_MV_BLK_IGNORE_OPENING = 1,
    Q2_MV_BLK_IGNORE_CLOSING = 2
};

/* `prim` for a mover built from a MOVER_A/B/C opcode rather than a CALL. */
#define Q2_MOVER_PRIM_OPCODE 0xFFu

typedef struct q2_mover {
    s16 node[Q2_MOVER_MAX_PARTS];   /* Scene nodes this group translates */
    u32 part_count;

    s16 portal_node;    /* -1 when the group has none */
    s16 target;         /* signed displacement along `axis` */
    s16 speed;
    u16 key_mask;

    u16 delay_timer;
    u16 wait_timer;     /* Q2_MOVER_WAIT_NEVER means it never auto-closes */

    u8  axis;           /* 0 = X, 1 = Y, 2 = Z */
    u8  state;
    u8  saved_state;
    u8  block_timer;
    u8  block_flags;
    u8  triggered;      /* latch, cleared each tick */
    u8  announced;      /* so a locked door only complains once */
    u8  touch_opens;

    /*
     * The portal node's visibility bit — `flags08` bit 15, written at
     * 0x80025C5C when the leaf settles fully closed and cleared otherwise,
     * with the write skipped while the partner leaf is still moving.
     *
     * Kept here rather than pushed into the caller's hide array because that
     * array has a second writer (the script's OBJDRAWOFF) and a per-tick
     * unconditional write from here would undo it.
     *
     * NO CONSUMER YET, and said so rather than left looking wired: the port's
     * zone draw has no portal-visibility test for it to feed. What is fixed is
     * that the value is now COMPUTED — `partner_busy` was calculated and then
     * `(void)`-cast away under a comment claiming the renderer consumed it,
     * and no renderer did.
     */
    u8  sealed;

    /*
     * Which family's sounds this mover uses. BUTTON has its own single sound;
     * PISTON and DISH have none. Set at build from the primitive, because the
     * handler a constructor installs is what decides it.
     */
    u8  silent;        /* PISTON, DISH: no sound at all      */
    u8  is_button;     /* amb_butn2 instead of the three     */

    /*
     * The sound this tick asked for, or Q2_MVSND_NONE. Drained by the owner,
     * which is also the only thing that knows where the mover's box is and can
     * therefore position the voice.
     */
    s8  sound_pending;

    s32 offset;         /* current displacement along the axis */

    /* Which primitive built it, or Q2_MOVER_PRIM_OPCODE for the MOVER_A/B/C
     * opcodes, which are not UserFuncs calls at all. Only for reporting: a
     * build that drops a mover and one that never saw it look identical in a
     * count. */
    u8  prim;

    s32 partner;        /* index of the other leaf, -1 when single */

    /*
     * The byte offset of the event item this mover was built from — its
     * IDENTITY.
     *
     * The runtime reports a MOVER item when a script reaches one, and the
     * owner has to say which of the built movers that is. An ordinal would
     * work only while build order and execution order agree and nothing
     * guarantees they do; the item's own offset (`q2_event_item.offset`)
     * cannot drift.
     */
    u32 item_offset;
} q2_mover;

typedef struct q2_mover_set {
    q2_mover *movers;
    u32       count;
    u32       capacity;
} q2_mover_set;

/*
 * Build every mover the MOVER_A/B/C opcodes declare.
 *
 * `ops` carries the two-buffer rebase (#56, userfuncs.h): the object SLOTS are
 * read out of the pristine copy of the Events chunk, not the one being walked.
 * Pass NULL to read in place, which is right only for zone 0 — COMMON.DAT's
 * Events snapshot agrees with ZONE0's and with no other, so 239 mover items
 * disc-wide otherwise build with the wrong nodes or with none at all.
 */
q2_result q2_movers_build(q2_mover_set *out, const q2_events *events,
                          const q2_uf_operands *ops);
void      q2_movers_free(q2_mover_set *set);

/* Latch a mover so it acts on the next tick. Reversing a closing door is
 * immediate, which is why this is not just a flag set. */
void q2_mover_trigger(q2_mover_set *set, u32 index);

/*
 * Trigger every mover built from the item at `item_offset`.
 *
 * "Every" because MOVER_C builds TWO movers from one item — the two leaves of
 * a double door — and a script that opens the door means both.
 *
 * This is the piece that was missing. `q2_movers_build` had no caller anywhere
 * in the port and `q2_mover_trigger` had none either, so every door and lift
 * on the disc stood still: 1,006 MOVER_A items, 20 MOVER_B and 292 MOVER_C.
 * Returns how many were triggered.
 */
u32 q2_movers_trigger_item(q2_mover_set *set, u32 item_offset);

/*
 * THE OTHER HALF OF THE LIFTS: the ones a CALL builds.
 *
 * `MOVER_A/B/C` are opcodes in the record stream and `q2_movers_build` reads
 * them. `LIFT1` is a CALL PRIMITIVE that builds the same kind of runtime
 * object, and userfuncs.c's operand table maps one onto the other exactly:
 *
 *     LIFT1, len 20        this module's field
 *     +4  u16 param_a      target, NEGATED (the ctor writes -value to obj+0x44)
 *     +6  s16 param_b      speed, abs() (obj+0x3A)
 *     +8  s16 objects[4]   node[], negative terminates
 *     +16 u8  time_a       delay,  x300 (obj+0x4C)
 *     +17 u8  time_b       wait,   x300 (obj+0x4E); 0xFF means never
 *
 * The axis is Y, as `MOVER_A`'s is: these are lifts. The object slots are
 * OBJSLOTs and therefore subject to the two-buffer rebase (#56), so an `ops` is
 * taken; pass NULL to read them in place.
 *
 * BUTTON and PISTON are built here too: both name a target AND a speed, which
 * is what a mover needs. A button's speed is literally one unit a tick — the
 * table says `travel`'s SIGN selects obj+0x3A = +1 or -1 and its magnitude goes
 * to obj+0x44 — and a PISTON is a crusher, so it ignores obstruction in both
 * directions rather than one.
 *
 * STALE TEXT REMOVED. This block used to say PLATFORM, DISH and CAGELIFT1 were
 * "deliberately NOT built here" because none of them names both a target and a
 * speed. All three ARE built — the code below has read each constructor since
 * — and the paragraph survived to contradict it, which is the sort of thing
 * that makes the next reader distrust the file rather than the sentence.
 *
 * What is still true of CAGELIFT1: it takes TWO boxes, not one. Its
 * constructor at 0x80029794 calls the slot allocator twice (0x80029A78 and
 * 0x80029B1C) and chains them through +0x3C — a top slab whose max[1] is
 * min[1] + item[+17] and a bottom slab whose min[1] is max[1] - item[+16]. It
 * is a cage with a floor and a ceiling. The collision registration in
 * q2_sim_attach_movers gives every part one box, so a cage lift is currently
 * solid through its middle. Written down rather than approximated.
 *
 * Appends to `set`, so call it after `q2_movers_build` with the same set: the
 * two families share a tick, a draw offset and a trigger.
 */
q2_result q2_movers_build_calls(q2_mover_set *out, const q2_events *events,
                                const q2_userfuncs *uf,
                                const q2_uf_operands *ops,
                                const q2_scene *scene);

/*
 * Advance every mover by `dt` in the simulation's own units.
 *
 * `player_keys` gates locked doors. Returns the number that moved, so a caller
 * can tell a static frame from a busy one.
 */
u32 q2_movers_tick(q2_mover_set *set, s32 dt, u16 player_keys);

/* Take the sound a mover asked for this tick, or Q2_MVSND_NONE. */
s8 q2_mover_take_sound(q2_mover_set *set, u32 index);

/*
 * The same tick, with an OBSTRUCTION TEST — the half of the state machine that
 * was decoded and dead.
 *
 * `Q2_MV_BLOCKED` had a `case` and a countdown and nothing anywhere assigned
 * it, and `block_flags` had three writers and no reader, so a door closing on
 * the player neither stopped, nor reversed, nor crushed: it passed through
 * them, which is the other half of "movers are non-solid".
 *
 * `blocked(index, step, user)` is asked, before a step is committed, whether
 * moving mover `index` by `step` along its own axis would sweep through
 * something. That is 0x80051EC0's return — it reports 0 when 0x800519B0 finds
 * an entity in the swept box — and 0x80025CBC is what acts on it:
 *
 *   opening (state 1) and bit 0 of obj+0x58 clear -> stop
 *   closing (state 3) and bit 1 clear             -> stop
 *   the matching bit SET                          -> carry on regardless
 *
 * and "stop" means: save the state, enter state 4, load the timer at obj+0x56
 * with 16. A PISTON sets both bits, which is what makes it a crusher.
 *
 * Passing NULL is the old behaviour and never blocks.
 */
typedef bool (*q2_mover_blocked_fn)(u32 index, s32 step, void *user);

u32 q2_movers_tick_blocked(q2_mover_set *set, s32 dt, u16 player_keys,
                           q2_mover_blocked_fn blocked, void *user);

/* How long a blocked mover waits before trying again — obj+0x56, loaded with
 * 16 at 0x80025D08 on both the stopped and the crushing arm. */
#define Q2_MOVER_BLOCK_TICKS 16

/*
 * The displacement to add to a Scene node's origin when drawing it, or zero.
 *
 * This is how movement reaches the renderer, and it is the original's own
 * mechanism rather than a convenience: the per-frame handler at 0x80025658
 * accumulates the displacement in the mover's runtime object at +0x12, and the
 * zone draw at 0x800678EC adds that triple to the node's camera-space position
 * as it draws it. The geometry is never modified, which is why every node in a
 * zone can share one origin and doors still move.
 *
 * Linear scan because a zone has at most a few dozen movers and this is called
 * once per node per frame.
 */
void q2_movers_node_offset(const q2_mover_set *set, u32 scene_node, s32 out[3]);

#endif /* Q2PSX_MOVER_H */
