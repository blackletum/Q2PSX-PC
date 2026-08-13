/*
 * monster.h — the creature animation and AI framework.
 *
 * The PSX AI is a close port of the PC game's monster code, with fixed-point
 * arithmetic and the per-creature parts moved into relocatable modules. That
 * single observation is what made it tractable: the structures, the callback
 * block and the transition logic are all recognisable, so the work was
 * confirming a known shape rather than inventing one.
 *
 * This module implements the FRAMEWORK — animation sequencing, the AI verb
 * table, and the shared state model. Per-creature behaviour lives in the
 * relocated modules (see reloc.h) and is data as far as this file is concerned.
 *
 * ---------------------------------------------------------------------------
 * Animation
 * ---------------------------------------------------------------------------
 * A move is a frame range plus a per-frame script and an optional end callback:
 *
 *     q2_mmove   16 bytes  { s32 first; s32 last; frames*; endfunc }
 *     q2_mframe   3 bytes  { u8 ai; s8 dist; u8 think }
 *
 * The three-byte frame is worth noticing: it is not padded to four. A reader
 * that assumes alignment walks off the end of every animation.
 *
 * Each frame does two things. `ai` selects a movement verb, and `think` selects
 * a per-creature action — firing, a sound, a melee hit.
 *
 *   - `ai < 0x80` indexes the six shared verbs below.
 *   - `ai >= 0x80` indexes the creature module's own table instead.
 *
 * Census over all 22 modules: 160 moves, 2,675 frames, with `ai` in {0..5} and
 * {129..133} and `think` in 0..19.
 *
 * Distance travelled on a frame is scaled by the creature's own speed factor:
 *
 *     dist = (aiflags & AI_HOLD_FRAME) ? 0 : (dist * speed_scale * 12) / 10
 *
 * ---------------------------------------------------------------------------
 * The six shared AI verbs
 * ---------------------------------------------------------------------------
 * Slot 0 is null. There is deliberately no turn verb — a precise negative from
 * reading the table, and worth recording because the PC lineage has one and its
 * absence would otherwise look like an oversight.
 *
 * ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 * The AI clock runs at 10 units per second, established arithmetically: the
 * drowning timer is set to level.time + 120 where the PC equivalent is twelve
 * seconds. So every time constant inherited from the PC game is multiplied by
 * ten here.
 *
 * Note this is NOT the same clock as the simulation tick, which runs at 25 Hz
 * in units of 1/300 s (see sim.h). Two different rates, and conflating them
 * would make every monster act at two and a half times its intended speed.
 */
#ifndef Q2PSX_MONSTER_H
#define Q2PSX_MONSTER_H

#include "q2psx.h"

/* The AI clock: 10 ticks per second. */
#define Q2_AI_HZ 10
#define Q2_AI_SECONDS(s) ((s) * Q2_AI_HZ)

/* ------------------------------------------------------------------------- */
/* Animation                                                                  */
/* ------------------------------------------------------------------------- */
#define Q2_MFRAME_SIZE 3        /* NOT 4 — the record is unpadded */
#define Q2_MMOVE_SIZE 16

/* ai >= this indexes the creature's own verb table rather than the shared one */
#define Q2_AI_LOCAL_FLAG 0x80

typedef struct q2_mframe {
    u8 ai;
    s8 dist;
    u8 think;
} q2_mframe;

typedef struct q2_mmove {
    s32 first_frame;
    s32 last_frame;
    u32 frames_offset;    /* module-relative pointer to the frame array */
    u32 endfunc_offset;   /* module-relative; 0 when the move just loops */
} q2_mmove;

/* The six shared movement verbs. Slot 0 is null; there is no turn verb. */
typedef enum q2_ai_verb {
    Q2_AI_NONE = 0,
    Q2_AI_STAND,
    Q2_AI_WALK,
    Q2_AI_RUN,
    Q2_AI_CHARGE,
    Q2_AI_MOVE,
    Q2_AI_VERB_COUNT
} q2_ai_verb;

extern const char *const q2_ai_verb_names[Q2_AI_VERB_COUNT];

/* ------------------------------------------------------------------------- */
/* AI flags, matching the PC lineage's values                                 */
/* ------------------------------------------------------------------------- */
enum {
    Q2_AI_SOUND_TARGET  = 0x0004,
    Q2_AI_HOLD_FRAME    = 0x0080,   /* freeze positional advance this frame */
    Q2_AI_GOOD_GUY      = 0x0100,
    Q2_AI_COMBAT_POINT  = 0x1000
};

/* attack_state, using the PC enum's values. */
typedef enum q2_attack_state {
    Q2_AS_STRAIGHT = 1,
    Q2_AS_SLIDING  = 2,
    Q2_AS_MELEE    = 3,
    Q2_AS_MISSILE  = 4
} q2_attack_state;

/* ------------------------------------------------------------------------- */
/* A live creature                                                            */
/* ------------------------------------------------------------------------- */
typedef struct q2_monster {
    s32 pos[3];
    s32 velocity[3];
    s32 angles[3];
    s32 ideal_yaw;

    s16 health;
    s16 max_health;
    s16 gib_health;     /* negative; below this the body is destroyed */

    s16 class_id;       /* indexes the 38-entry module registry */
    u8  class_byte;     /* the local descriptor key, 64..94 for monsters */

    s32 frame;
    s32 current_move;   /* index into the module's move table, -1 when idle */
    u32 aiflags;
    u8  attack_state;
    u8  speed_scale;    /* multiplies every frame's dist */

    s32 next_think;     /* on the 10 Hz AI clock */
    s32 attack_finished;

    s32 enemy;          /* index of the target, -1 when none */
    bool in_use;
    bool dead;
} q2_monster;

/* The registry is 38 slots, which is exactly Population's class_id range. */
#define Q2_MONSTER_CLASS_COUNT 38

void q2_monster_init(q2_monster *m);

/* Positional advance for one animation frame, with the speed scale and the
 * hold-frame flag applied. */
s32 q2_monster_frame_dist(const q2_monster *m, const q2_mframe *frame);

/* True when `target` lies within the creature's forward cone.
 *
 * The threshold is a dot product of 1230/4096, i.e. about 0.30 — a wide cone of
 * roughly 72 degrees either side, which is why monsters notice you from well
 * off to the side. */
#define Q2_INFRONT_DOT 1230
bool q2_monster_infront(const q2_monster *m, const s32 target[3]);

/* Squared distance to a point, for the range tiers. */
s64 q2_monster_dist_sq(const q2_monster *m, const s32 target[3]);

/* Apply damage. Returns true when this killed the creature. Health below
 * gib_health destroys the body rather than leaving a corpse. */
bool q2_monster_damage(q2_monster *m, s16 amount);

/* Decode a move and a frame out of a relocated module image. `offset` is
 * module-relative. Returns false when out of range. */
bool q2_mmove_read(const u8 *image, size_t size, u32 offset, q2_mmove *out);
bool q2_mframe_read(const u8 *image, size_t size, u32 offset, q2_mframe *out);

#endif /* Q2PSX_MONSTER_H */
