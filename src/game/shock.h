/*
 * shock.h — the DualShock's two motors: QUAKE.C's SetShockData (0x80040800),
 * ProcessShockData (0x80040B28) and KillDualShocks (0x800411F0), and the
 * sixteen patterns they play.
 *
 * ---------------------------------------------------------------------------
 * This replaces a misreading
 * ---------------------------------------------------------------------------
 * The tree called 0x80040800 "the SPU voice emitter", on the strength of its
 * shape: an id, a volume, an optional position, a near and a far radius, a
 * linear falloff and a floor of 513 under which a request is dropped. That
 * shape is real, but nothing in it reaches the SPU. MAIN.SYM names the function
 * SetShockData, and what it writes is a PATTERN POINTER into one of four slots
 * at GameWindows[player]+732 (0x800D5C30, 784 bytes a player). The only reader
 * of those slots is ProcessShockData, and its only caller is PadDualShock
 * (0x80031728), which hands the two bytes it produces to PadSetAct — the
 * actuator command. So every "sound" the tree recorded through 0x80040800 is
 * a rumble: the hit on a player (0x8005859C), the train's running and arrival
 * (0x8002C778, 0x8002C8D8), an explosion (0x8005AA7C), and — the reason this
 * file exists — the kick of every player weapon, the machinegun's included.
 *
 * ---------------------------------------------------------------------------
 * A pattern is two envelopes
 * ---------------------------------------------------------------------------
 * ShockDataAvailable (0x8009D4E0) is a NULL-terminated list of sixteen
 * 12-byte records `{ big, little, u16 flags, u16 id }` at 0x8009D420. `big`
 * and `little` each point at a run of `{ s16 level, s16 duration }` points
 * ending in one whose duration is zero. The level is 1.0.12; durations are
 * level ticks, because the time a slot has been playing advances by
 * PalNtscRate (0x800B2DD8) — the same per-field step the vertical-blank ISR
 * adds to the level clock — once per field.
 *
 * Between two points the level is interpolated linearly. On the terminating
 * point the level is held for exactly one sample — the one whose remaining
 * time is zero — and then the envelope is spent. A slot is freed on the first
 * sample at which NEITHER envelope is still interpolating.
 *
 * ---------------------------------------------------------------------------
 * The two motors are not driven the same way
 * ---------------------------------------------------------------------------
 * Every live slot contributes `level * strength >> 12` to a sum per motor, and
 * each sum is clamped to 4096 and scaled to a byte (`* 255 >> 12`). The BIG
 * motor gets that byte. The LITTLE one is on/off on a DualShock, so its byte
 * is fed to a first-order sigma-delta at GameWindows+764: added to a running
 * halfword, and the motor runs on every sample where that reaches 129, with
 * 256 taken back off. A half-strength buzz is the small motor pulsing at half
 * the field rate.
 *
 * PadSetAct gets the little motor in act[0] and the big one in act[1]
 * (0x80031850..0x80031860) — the DualShock's own order.
 *
 * ---------------------------------------------------------------------------
 * Not the console's
 * ---------------------------------------------------------------------------
 * SetShockData's position test takes a square root through SquareRootNR
 * (0x80055CBC): two Newton steps seeded by libgte's SquareRoot0. The seed here
 * is an exact floor root rather than SquareRoot0's table, so a distance can
 * come out one unit different from the console's. That moves a positional
 * rumble's strength by at most `strength / (far - near)`, which for the only
 * positional caller wired (the train, 1024 over 2048) is half a unit.
 */
#ifndef Q2PSX_SHOCK_H
#define Q2PSX_SHOCK_H

#include "q2psx.h"

/* The `id` field of each record, which is what every caller passes. */
enum {
    Q2_SHOCK_NONE             =  0,
    Q2_SHOCK_BLASTER          =  1,   /* 0x8009D420 ShockData_Blaster          */
    Q2_SHOCK_HYPERBLASTER     =  2,   /* 0x8009D42C ShockData_HyperBlaster     */
    Q2_SHOCK_SHOTGUN          =  3,   /* 0x8009D438 ShockData_Shotgun          */
    Q2_SHOCK_SSHOTGUN         =  4,   /* 0x8009D444 ShockData_SShotgun         */
    Q2_SHOCK_MACHINEGUN       =  5,   /* 0x8009D450 ShockData_MachineGun       */
    Q2_SHOCK_CHAINGUN         =  6,   /* 0x8009D45C ShockData_ChainGun         */
    Q2_SHOCK_GRENADE_OPEN     =  7,   /* 0x8009D480 ShockData_GrenadeOpen      */
    Q2_SHOCK_GRENADE_THROW    =  8,   /* 0x8009D48C ShockData_GrenadeThrow     */
    Q2_SHOCK_GRENADEL         =  9,   /* 0x8009D474 ShockData_GrenadeL         */
    Q2_SHOCK_ROCKETL          = 10,   /* 0x8009D468 ShockData_RocketL          */
    Q2_SHOCK_RAILGUN          = 11,   /* 0x8009D498 ShockData_RailGun          */
    Q2_SHOCK_BFG              = 12,   /* 0x8009D4A4 ShockData_BFG              */
    Q2_SHOCK_EXPLOSION        = 13,   /* 0x8009D4B0 ShockData_Explosion        */
    Q2_SHOCK_STOMP            = 14,   /* 0x8009D4BC ShockData_Stomp            */
    Q2_SHOCK_IMPACT_DISTINCT  = 15,   /* 0x8009D4C8 ShockData_Impact_Distinct  */
    Q2_SHOCK_IMPACT_MERGEABLE = 16    /* 0x8009D4D4 ShockData_Impact_Mergeable */
};

#define Q2_SHOCK_PATTERNS      16
#define Q2_SHOCK_SLOTS          4     /* GameWindows+732, eight bytes each      */
#define Q2_SHOCK_FULL        4096     /* every weapon's strength, 1.0.12        */
#define Q2_SHOCK_MIN_STRENGTH 513     /* `slti v0, a2, 513` at 0x800409E0       */
#define Q2_SHOCK_ALL_PLAYERS    4     /* SetShockData's a0 for "every window"   */

/* Record flag bit 0: a second request RESTARTS the slot already playing this
 * pattern instead of taking another (0x80040A38). Stomp and Impact_Distinct
 * are the two that stack. */
#define Q2_SHOCK_FLAG_MERGE     1u

typedef struct q2_shock_point {
    s16 level;      /* +0: 1.0.12 drive at the start of this span             */
    s16 duration;   /* +2: ticks to the next point; 0 ends the envelope       */
} q2_shock_point;

typedef struct q2_shock_data {
    const q2_shock_point *big;      /* +0: the large, variable-speed motor    */
    const q2_shock_point *little;   /* +4: the small, on/off one              */
    u16 flags;                      /* +8                                     */
    u16 id;                         /* +10                                    */
    const char *name;               /* MAIN.SYM's, for reports                */
    u32 addr;                       /* the record, PAL                        */
} q2_shock_data;

/* ShockDataAvailable, in the order SetShockData searches it, NULL-ended. */
extern const q2_shock_data *const q2_shock_available[Q2_SHOCK_PATTERNS + 1];

/* The first record carrying `id`, as SetShockData's walk finds it, or NULL. */
const q2_shock_data *q2_shock_find(int id);

typedef struct q2_shock_slot {
    const q2_shock_data *data;  /* +0: NULL is a free slot                    */
    s16 time;                   /* +4: ticks into the pattern                 */
    s16 strength;               /* +6: 1.0.12, after any distance falloff     */
} q2_shock_slot;

/* One player's share of GameWindows: the four slots and the little motor's
 * sigma-delta accumulator at +764. */
typedef struct q2_shock {
    q2_shock_slot slot[Q2_SHOCK_SLOTS];
    u16 little_accum;
} q2_shock;

/*
 * One player's pass through SetShockData's loop.
 *
 * `pos` is the source, or NULL for a request that is not positional — every
 * weapon passes NULL. `listener` is that player's GameWindows+0 position and
 * is only read when `pos` is given; `near_r` and `far_r` are the fifth and
 * sixth arguments: full strength inside `near_r`, nothing beyond `far_r`, and
 * `strength * (far - d) / (far - near)` between, never more than `strength`.
 *
 * Returns true when a slot took the request. A strength under 513 after the
 * falloff, an unknown id or four busy slots all drop it.
 */
bool q2_shock_set(q2_shock *s, int id, s32 strength,
                  const s32 pos[3], const s32 listener[3],
                  s16 near_r, s16 far_r);

/*
 * ProcessShockData: one sample of both motors, then advance every live slot
 * by `dt` ticks (PalNtscRate). `out[0]` is the big motor's byte, `out[1]` the
 * little motor's 0 or 1 — the console's own order on the stack at sp+16.
 *
 * `alive` is the window's player having health above zero (0x80040E4C..
 * 0x80040E64): a dead player's pad is silenced, but the slots still advance.
 */
void q2_shock_process(q2_shock *s, s32 dt, bool alive, u8 out[2]);

/* KillDualShocks: free every slot. The accumulator is left alone, as there. */
void q2_shock_kill(q2_shock *s);

/* True while any slot is playing. */
bool q2_shock_active(const q2_shock *s);

/*
 * ---------------------------------------------------------------------------
 * SetShockData as the rest of the game library reaches it
 * ---------------------------------------------------------------------------
 * The console has one SetShockData and every caller jumps straight to it. In
 * the port the slots and the pads are the client's — a q2_shock per window,
 * and SDL behind it — so a caller inside this library hands its six arguments
 * to whatever registered here instead. Three do:
 *
 *   - T_Damage's tail, a hit on a player that survived (0x8005859C), from
 *     combat.c;
 *   - QuakeGenExplosion (0x8005AA7C), from the model-entity spawn in
 *     modelent.c, which is that function;
 *   - the Tank Commander's footstep, through its module's import +0x12C
 *     (module+0x157C on COMMAND and WASTE4), from cre_tankcomm.c.
 *
 * The weapons and the train reach the client in results it already reads, and
 * it routes those itself. With nothing attached a request is dropped, which is
 * what a unit test that does not attach a spy wants.
 *
 * `player` is a window, or Q2_SHOCK_ALL_PLAYERS; `pos` is NULL for a request
 * that is not positional, and then the two radii are not read — exactly
 * q2_shock_set's terms, one window at a time.
 */
typedef void (*q2_shock_sink_fn)(void *user, int player, int id, s32 strength,
                                 const s32 pos[3], s16 near_r, s16 far_r);

/* Register the sink, or detach with NULL. One at a time, as there is one
 * SetShockData. */
void q2_shock_attach(q2_shock_sink_fn fn, void *user);

/* SetShockData (0x80040800) with the console's own arguments. */
void q2_shock_request(int player, int id, s32 strength, const s32 pos[3],
                      s16 near_r, s16 far_r);

/*
 * QuakeGenExplosion's request, 0x8005AA5C..0x8005AA7C:
 *
 *     a0 = 4, a1 = 13, a2 = size, a3 = the point it was handed,
 *     sh (size / 2) -> sp+16, sh size -> sp+18
 *
 * Every window, ShockData_Explosion at the explosion's own size, full within
 * half of it and nothing past it. `size` is the spawn's third argument, the
 * one that also scales its matrix (0x80055F3C): 4096 from a rocket, both
 * grenades and an explosive brush, 2048 from the gib dispatcher's two
 * Explosion arms. The halving is the compiler's signed divide (`srl 31`,
 * `addu`, `sra 1` at 0x8005AA68), so it rounds toward zero.
 */
void q2_shock_explosion(const s32 at[3], s32 size);

/* The Tank Commander's footstep at module+0x1550..+0x157C: every window,
 * ShockData_Stomp at full strength, full within 2048 of the Tank and nothing
 * past 8192. */
#define Q2_SHOCK_TANK_STEP_NEAR 2048
#define Q2_SHOCK_TANK_STEP_FAR  8192

#endif /* Q2PSX_SHOCK_H */
