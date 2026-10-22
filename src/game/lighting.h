/*
 * lighting.h — how the world lights the things standing in it.
 *
 * ---------------------------------------------------------------------------
 * What lights what
 * ---------------------------------------------------------------------------
 * Quake II PSX has two lighting systems and they never meet:
 *
 *   THE WORLD is lit at build time. Every `MapMod` quad carries four indices
 *   into a per-node RGB table and the world renderer just copies them into the
 *   packet (0x800681D8). That baked Gouraud table IS the PlayStation's
 *   lightmap: there is no lightmap texture and no second texturing pass.
 *
 *   THAT IS NOT THE WHOLE STORY, and an earlier version of this comment said
 *   it was. It claimed "no second pass, and nothing at runtime modulates it",
 *   and the second half is wrong. `ProcessMobileLights` (0x80074598, named by
 *   the NTSC disc's LIGHTS.C symbol table) walks the same runtime light list
 *   the entity gather reads and ADDS each light's contribution straight into
 *   the Gouraud bytes of the packets DrawScene has already built this frame,
 *   clamping at 255; `UndoMobileLights` (0x80075F60) puts the baked colours
 *   back at the top of the next frame from a per-draw-buffer undo list. So a
 *   rocket really does light the corridor it flies down. See the
 *   `q2_light_ramp` block below for the arithmetic. The port's world.c applies
 *   it while constructing each frame's packets from the baked vertex colours.
 *
 *   EVERYTHING ELSE — models, creatures, items, the view weapon — is lit per
 *   vertex by the GTE from at most THREE point lights chosen per entity per
 *   frame, plus an ambient. That is what this module reconstructs, from
 *   0x8006AFE8 and the two functions it calls.
 *
 * ---------------------------------------------------------------------------
 * The pipeline, in the original's order
 * ---------------------------------------------------------------------------
 *  1. Four candidate slots and a four-entry ranking are reset (0x8006AFE8).
 *  2. Every light that might reach the entity is offered to the ranker
 *     (0x8006AD4C). Three sources, and which ones run depends on the entity:
 *       - the WORLD dynamic list, 16 slots at 0x800E3D18;
 *       - the STATIC lights of the collision node the entity stands in,
 *         through SpaceLights (see spacelights.h);
 *       - or, when signed entity halfword +0xF4 is negative, the VIEW dynamic
 *         list at 0x800E3ED8 instead of both of the above.
 *     An entity with no collision node gets one synthetic light instead of the
 *     static ones — see Q2_LIGHT_FALLBACK_*.
 *  3. Each offer computes a distance attenuation (0x80076040), scales the
 *     light's colour by it, and keeps the result only if it is brighter than
 *     one of the three already held. The candidate always lands in the slot
 *     ranked last, so the loser is simply overwritten next time.
 *  4. The survivors become GTE state: their DIRECTIONS become the light
 *     matrix's rows, their COLOURS become the colour matrix's columns, and the
 *     entity's own glow colour becomes the back colour (0x8008AD0C).
 *  5. Vertices shade through NCT — normal, no primitive colour, no depth cue —
 *     so a model's texture is modulated purely by the light reaching it.
 *
 * Three lights is not a budget chosen here. It is the GTE's: the light matrix
 * has three rows.
 *
 * ---------------------------------------------------------------------------
 * The arithmetic that has to be exact
 * ---------------------------------------------------------------------------
 * The light-to-entity delta is computed in SIXTEEN-bit wrapping arithmetic
 * (`lhu`/`lhu`/`subu` at 0x8006AD70), exactly like the collision queries. A
 * 32-bit port agrees until a light is more than 32 K units away on some axis,
 * at which point the original wraps and this must wrap with it.
 *
 * The attenuation is linear in the SQUARE of the distance, not in the distance:
 *
 *     atten = ((radiusSq - distSq) << 12) / (radiusSq - innerRadiusSq)
 *
 * clamped to 4096 by the caller but NOT clamped below it inside the flare path,
 * which is what makes a flare grow as you walk into a light.
 */
#ifndef Q2PSX_LIGHTING_H
#define Q2PSX_LIGHTING_H

/* The Lights chunk parser. Spelled out because src/game has an entity.h of its
 * own — the game entity — and this needs the on-disc one. */
#include "../formats/entity.h"

#include "gte.h"
#include "q2psx.h"
#include "weapon.h"
#include "spacelights.h"

/* The GTE holds three lights; the engine ranks four so the fourth can be the
 * scratch slot every candidate is written into before it is judged. */
#define Q2_LIGHT_SLOTS      4
#define Q2_LIGHT_ACTIVE_MAX 3

/* The scale argument 0x8006AD4C passes to the attenuation, and the shift that
 * undoes it. Together they are the identity; the flare path passes a larger
 * scale to widen the same light's reach. */
#define Q2_LIGHT_ATTEN_SCALE 64
#define Q2_LIGHT_ATTEN_SHIFT 6

/* 1.12 one, the value the attenuation saturates at and the neutral value for
 * both entity intensity fields. */
#define Q2_LIGHT_ONE 4096

/*
 * The light an entity gets when it is not in any collision node — 0x8006B150
 * building a record at 0x800A1C48 out of the entity's own position.
 *
 * Everything but the position is fixed data in the executable: a dim grey at
 * 48/48/48, a radius of 32767, and an inner radius squared EQUAL to the outer,
 * which makes the attenuation's denominator zero and so pins it at 1.0. It is a
 * flat fill light, not a falloff.
 */
#define Q2_LIGHT_FALLBACK_OFS_X   500
#define Q2_LIGHT_FALLBACK_OFS_Y (-2000)
#define Q2_LIGHT_FALLBACK_OFS_Z (-1000)
#define Q2_LIGHT_FALLBACK_GREY    0x30

/* The two runtime light lists, 16 records of 28 bytes each at 0x800E3D18 and
 * 0x800E3ED8. The count is the array size, not a soft limit: the engine's own
 * end pointers cannot exceed it. */
#define Q2_DYNLIGHT_MAX 16

/* ------------------------------------------------------------------------- */
/* One candidate slot — the 16-byte record at 0x800DDC60                      */
/* ------------------------------------------------------------------------- */
typedef struct q2_light_slot {
    s16 total;     /* +0x00  r + g + b; the ranking key                     */
    s16 rgb[3];    /* +0x02  1.12 colour after attenuation, clamped to 4096 */
    s16 d[3];      /* +0x08  light - entity, 16-bit wrapped                 */
} q2_light_slot;

/* The ranker's whole state. */
typedef struct q2_light_set {
    q2_light_slot slot[Q2_LIGHT_SLOTS];
    s16           rank[Q2_LIGHT_SLOTS];  /* slot indices, brightest first   */
    u32           count;                 /* gp+1400: accepted lights        */
} q2_light_set;

/* ------------------------------------------------------------------------- */
/* The TIMED light table — sixteen 36-byte slots at 0x800E1218                */
/* ------------------------------------------------------------------------- */
/*
 * There are TWO appenders in LIGHTS.C, not one, and the port had only ever
 * traced the first:
 *
 *   AddLight      0x80075C34 — push one light onto the world list, now.
 *   AddTimedLight 0x80075D14 — park one light in a SLOT, and let
 *                              ProcessTimedLights (0x800741AC) push a shrinking
 *                              copy of it onto the world list once a tick until
 *                              the slot's counter runs out.
 *
 * The fourth argument of AddTimedLight is therefore a LIFETIME IN TICKS, not a
 * radius and not a style. It is stored into BOTH halfwords at slot+32 and
 * slot+34 (0x80075DD8 / 0x80075DDC) — remaining and total — and every frame
 * ProcessTimedLights emits
 *
 *     inner = base_inner * remaining / total
 *     outer = base_outer * remaining / total
 *
 * (0x800741EC..0x80074270, MIPS `div`, so both truncate toward zero) and THEN
 * decrements remaining (0x80074358). The decrement after the emit is why a
 * slot given N ticks is emitted exactly N times, at N/N, (N-1)/N … 1/N, and
 * never at 0/N. A four-tick light is 1600, 1200, 800, 400 and gone.
 *
 * AddTimedLight takes the FIRST slot whose remaining is zero (0x80075D28) and,
 * when all sixteen are busy, returns 0 and silently drops the light — it never
 * replaces one. InitLights (0x80074194) zeroes the sixteen remaining counters
 * and nothing else, so a level load frees the table without touching the rest.
 *
 * WHAT THIS STRUCT LEAVES OUT, and why. AddTimedLight also writes slot+18 (the
 * outer radius), slot+20 (innerSq) and slot+24 (outerSq) — the same three
 * fields AddLight fills in a real light record. ProcessTimedLights never reads
 * any of them: it reads slot+0..+8 (position), slot+12..+15 (the colour word),
 * slot+28/+30 (the radii) and slot+32/+34 (the counters), and squares the
 * SCALED radii afresh each frame. The three are dead storage, the shared shape
 * of a light record showing through, and reproducing them here would invite a
 * later reader to use them.
 */
#define Q2_TIMEDLIGHT_SLOTS 16

typedef struct q2_timed_light {
    s32 pos[3];     /* slot +0x00  the position, copied verbatim            */
    u8  rgb[3];     /* slot +0x0C  the low three bytes of the colour word   */
    u8  colour_hi;  /* slot +0x0F  the fourth; 0 in both presets on the disc*/
    s16 inner;      /* slot +0x1C  a2's LOW half, read back with `lh`       */
    s16 outer;      /* slot +0x1E  a2's HIGH half, read back with `lh`      */
    s16 remaining;  /* slot +0x20  counts down; 0 means the slot is free    */
    s16 total;      /* slot +0x22  the divisor, fixed for the slot's life   */
} q2_timed_light;

/* ------------------------------------------------------------------------- */
/* What the world offers a light gather                                       */
/* ------------------------------------------------------------------------- */
typedef struct q2_light_world {
    /* COMMON.DAT's Lights array and the zone's per-node index lists. Both may
     * be NULL, which simply means no static lights. */
    const q2_light_list  *statics;
    const q2_spacelights *space;

    /*
     * The two runtime lists.
     *
     * `world` lights everything standing in the map, and is refilled from empty
     * every frame (0x80075B94). `view` is the list the **negative +0xF4** branch
     * reaches instead — and **nothing in the image ever appends to it.** Its end pointer
     * at `0x800B2EC4` has exactly two writers, both of which reset it to the
     * start of the array. The two arrays are also contiguous, and the world
     * appender's full test is a compare against the view array's base, so what
     * is really there is one 16-slot array with a second, permanently empty,
     * view over its end.
     *
     * The consequence is a real behaviour and not a gap in this port: an entity
     * whose signed +0xF4 is negative gets NO lights at all and is drawn by its
     * back colour alone. The field is kept because the engine's storage is kept.
     */
    q2_light dynamic_world[Q2_DYNLIGHT_MAX];
    u32      dynamic_world_count;
    q2_light dynamic_view[Q2_DYNLIGHT_MAX];
    u32      dynamic_view_count;

    /*
     * The timed table. Separate storage in the original (0x800E1218, nowhere
     * near the two lists) and separate storage here, but carried in the same
     * object because ProcessTimedLights' only output is an append to
     * `dynamic_world` — keeping them together is what stops a caller ticking
     * one and emptying the other.
     *
     * A zeroed q2_light_world is a table of sixteen free slots, which is
     * exactly what InitLights leaves behind.
     */
    q2_timed_light timed[Q2_TIMEDLIGHT_SLOTS];
} q2_light_world;

/*
 * 0x80075C34 — append a runtime light to the world list.
 *
 * The caller passes RADII and this squares them, which is why the on-disc
 * records carry the squares and a redundant radius: the disc format is this
 * function's output, written out by the build tool.
 *
 * It also confirms the `type` byte's bit fields from the other side. It masks
 * `0x3800` and ORs `(style & 7) << 11`, then masks `0xC000` and ORs
 * `(size_shift & 3) << 14` — and never touches bits 0..2, which is why a
 * runtime light's type has the low bits clear while every light on the disc has
 * them set to 7. Nothing reads them.
 *
 * Returns false when the list is full: sixteen lights, and the seventeenth is
 * silently dropped rather than replacing anything.
 */
bool q2_light_add_dynamic(q2_light_world *w, const s32 pos[3],
                          const u8 rgb[3], s32 inner_radius, s32 outer_radius,
                          u32 style, u32 size_shift);

/*
 * 0x80075D14 — park a light in the timed table for `ticks` frames.
 *
 * Returns false when all sixteen slots are busy, which is a DROP: the original
 * scans for the first slot whose counter is zero and gives up if there is none
 * (0x80075D28..0x80075D58). Passing `ticks == 0` stores a zero counter, so the
 * slot reads as free and the light is never emitted — that is the original's
 * behaviour too, not a guard added here.
 */
bool q2_light_add_timed(q2_light_world *w, const s32 pos[3], const u8 rgb[3],
                        s32 inner_radius, s32 outer_radius, s32 ticks);

/*
 * 0x800741AC — one tick of the timed table.
 *
 * Emits every live slot into the WORLD dynamic list at its scaled radii, then
 * decrements it. Returns how many lights it appended, which is not always how
 * many slots were live: the list holds sixteen and a slot whose append is
 * refused still counts down (0x800742A8 branches to the decrement, not past
 * it).
 *
 * WHERE THIS BELONGS IN A FRAME. ProcessLights (0x80075B94) is three calls:
 * ProcessTimedLights, then ProcessMobileLights (the world repaint), then it
 * resets both list pointers — and it runs AFTER every model has been drawn. So
 * a timed light NEVER reaches the per-entity GTE gather; it is raised and
 * consumed by the world pass inside one call and thrown away. Calling this at
 * the TOP of a frame instead would light models the console leaves dark.
 */
u32 q2_light_timed_tick(q2_light_world *w);

/* 0x80074194 — InitLights' whole treatment of the table: zero the sixteen
 * counters, leave everything else. For a level load. */
void q2_light_timed_reset(q2_light_world *w);

/* How many slots are counting down. Nothing in the original asks, but a
 * headless run has no other way to see that a timed light exists. */
u32 q2_light_timed_active(const q2_light_world *w);

/* ------------------------------------------------------------------------- */
/* The muzzle flashes, all four of them                                       */
/* ------------------------------------------------------------------------- */
/*
 * `q2_weapon_has_muzzle_light` says the machinegun and the chaingun are "the
 * two weapons that carry one". They are the two that carry an ADDLIGHT one.
 * The shotgun and the super shotgun each raise a TIMED light, which is why
 * enumerating 0x80075C34's call sites missed them:
 *
 *   QPlayerFireShotGun       0x8004C3FC  a0 = entity+0xA4, ticks = 3
 *   QPlayerFireSuperShotGun  0x8004C6C0  a0 = entity+0x54, ticks = 4
 *
 * Both take their colour from 0x800AE9D4 — the same `C8 64 64` the machinegun
 * uses — and their radii from fixed halfword pairs rather than a rand() draw:
 * 0x800AE9D8/0x800AE9DA is 600/1200 and 0x800AE9E4/0x800AE9E6 is 800/1600. The
 * super shotgun therefore has the largest and longest flash the player owns,
 * and it was the one that raised nothing at all.
 *
 * THE POSITION ARGUMENTS ARE DIFFERENT AND MUST NOT BE COLLAPSED. The shotgun
 * passes entity+0xA4 and the super shotgun entity+0x54 — two distinct fields
 * on the same entity (entity.h maps them). `origin_offset` carries which one,
 * because this module has no entity to read it out of.
 */
typedef struct q2_muzzle_light {
    u8   rgb[3];         /* 0x800AE9D4, shared by all four weapons        */
    s16  inner;          /* 0 when `random_radii` is set                  */
    s16  outer;          /* likewise                                      */
    s16  ticks;          /* 0 = AddLight, one frame; > 0 = AddTimedLight  */
    u16  origin_offset;  /* the entity field the console passes: 0x54/0xA4*/
    bool random_radii;   /* the machinegun/chaingun two rand() draws       */
} q2_muzzle_light;

/* True and fills `out` for the four weapons that flash; false for the rest.
 * `out` may be NULL when only the predicate is wanted. */
bool q2_light_muzzle_preset(u8 weapon_id, q2_muzzle_light *out);

/* 0x800AE9D4 — the one muzzle colour. weapon.h names the same three bytes
 * Q2_MUZZLE_LIGHT_*; repeated here so the preset table is readable on its own. */
#define Q2_MUZZLE_FLASH_R 200
#define Q2_MUZZLE_FLASH_G 100
#define Q2_MUZZLE_FLASH_B 100

/* 0x800AE9D8 / 0x800AE9DA and the literal 3 at 0x8004C3BC. */
#define Q2_SHOTGUN_LIGHT_INNER   600
#define Q2_SHOTGUN_LIGHT_OUTER  1200
#define Q2_SHOTGUN_LIGHT_TICKS     3

/* 0x800AE9E4 / 0x800AE9E6 and the literal 4 at 0x8004C680. */
#define Q2_SSHOTGUN_LIGHT_INNER   800
#define Q2_SSHOTGUN_LIGHT_OUTER  1600
#define Q2_SSHOTGUN_LIGHT_TICKS     4

/* ------------------------------------------------------------------------- */
/* FLKLIGHT — a script-placed light that blinks                               */
/* ------------------------------------------------------------------------- */
/*
 * `FLKLIGHT` is one of the two script light primitives (userfuncs.h).
 *
 * **The two randomised formulas are RADII, not durations.** `userfuncs.h`
 * records them as "on/off times ... ((rand()*500)>>15)+400 and +1000", and an
 * earlier version of this file implemented them that way. Reading the handler
 * at `0x800287A0` shows where each value goes:
 *
 *     8002884C  sra   v0, v0, 15
 *     80028850  addiu v0, v0, 400     ; ((r1*500)>>15) + 400
 *     80028858  sh    v0, 16(sp)      ; ...into a2's LOW half  = INNER radius
 *     80028888  addiu v0, v0, 1000    ; ((r2*500)>>15) + 1000
 *     8002888C  sh    v0, 18(sp)      ; ...into a2's HIGH half = OUTER radius
 *
 * and `0x800288C8` passes that `a2` straight to `0x80075C34`. So a flicker is
 * 400..899 inner and 1000..1499 outer, redrawn per flash. Its colour comes from
 * the item at +18/+19/+20, which is what the operand table already said.
 *
 * **There is no on/off phase, and this port no longer invents one.** The exec at
 * `0x800287A0` loops the primitive's objects, adds one dynamic light each and
 * returns — no timer, no state, nothing that could turn it off. What makes a
 * flicker flicker is that BOTH radii are redrawn from `rand()` on every call, so
 * the same script record casts a different-sized light each time it runs.
 *
 * An earlier version of this file carried a `q2_flklights` set with an on/off
 * phase and a placeholder duration. That machinery had no counterpart in the
 * original and is gone; FLKLIGHT is raised as a transient and the two radius
 * functions below are all that is needed.
 *
 * FLKLIGHT AND TIMEDLIGHT ARE NOT THE SAME SHAPE, and this note used to say
 * they were ("raised as a transient, exactly like TIMEDLIGHT"). 0x800288C8
 * hands FLKLIGHT's light to AddLight, which is one frame and gone; PSXTLIGHT
 * (0x8002E9D0) hands TIMEDLIGHT's to AddTimedLight, which parks it in a slot
 * and fades it out over 3 * item[+18] frames. A flicker is genuinely
 * instantaneous; a timed light is genuinely not.
 *
 * The disc carries exactly ONE FLKLIGHT call, so this is deliberately small: a
 * fixed set, no allocation, and a tick that costs nothing when empty.
 */
/* The flicker's radii, as 0x8002882C and 0x8002885C compute them. */
s32 q2_flklight_inner_radius(s32 rand_0_32767);
s32 q2_flklight_outer_radius(s32 rand_0_32767);


/* ------------------------------------------------------------------------- */
/* MOBILE lights — the runtime list repainting the world's own vertex colours */
/* ------------------------------------------------------------------------- */
/*
 * ProcessMobileLights, 0x80074598. For every light on the world list it walks
 * the brushes DrawScene registered this frame (52-byte records at 0x800DA578,
 * count at 0x800B2C80), rejects the ones the light's box misses, and for every
 * point of the surviving node's Points list (0x800B2CA0, 12 bytes each) adds a
 * distance-ramped share of the light's colour into the four Gouraud bytes the
 * point names — clamped at 255, and replicated to every drawn instance of the
 * same quad (0x80074B38).
 *
 * The four functions below are that inner loop, and nothing else. They take
 * numbers, not scene pointers, because the brush list and the Points table are
 * world.c's storage; this module owns the arithmetic and world.c owns the walk.
 *
 * THE SHAPE OF THE RAMP. The two radii squared are first divided by four,
 * repeatedly, until the OUTER one fits in 32767 (0x80074658), because the
 * distance is squared through the GTE's SQR with lm = 1 and each component
 * saturates at 0x7FFF. The count of those halvings is the shift GetDist2Shift_
 * (0x800B0FB4) applies to each axis delta before squaring it. Then
 *
 *     den  = (outerSq - innerSq) / 256           0x8007468C, truncating
 *     t    = distSq - innerSq
 *     t < 0                 -> the light's full colour
 *     otherwise  s = 255 - t / den, contribution[i] = s * rgb[i] / 256
 *
 * with both divides truncating TOWARD ZERO — the `addiu +255` before each
 * `sra 8` at 0x80074A30, 0x80074A4C and 0x80074A68 is there precisely because
 * `s` can come out negative at the very edge of the outer radius.
 */
typedef struct q2_light_ramp {
    s32 inner_sq;   /* fp       innerRadiusSq after the /4 loop              */
    s32 outer_sq;   /* s6       radiusSq after the same                      */
    s32 den;        /* 120(sp)  (outer_sq - inner_sq) / 256                  */
    s32 shift;      /* 132(sp)  how many /4 steps it took                    */
    s32 radius;     /* the light's own radius, for the box test              */
    u8  rgb[3];     /* 96(sp), 128(sp), 124(sp)                              */
} q2_light_ramp;

/* 0x80074628..0x800746E4 — hoist everything the brush loop reuses. */
void q2_light_ramp_begin(q2_light_ramp *out, const q2_light *l);

/*
 * 0x80074734..0x800747BC — does the light's pos +/- radius box overlap this
 * brush's world AABB? Six strict `slt` compares in the order x, z, y, the same
 * axis order q2_light_set_add rejects in. `obj_flags` is the dword at
 * object+0x0C; bit 0x00800000 opts the object out entirely (0x800747D4).
 */
bool q2_light_ramp_reaches(const q2_light_ramp *r, const s32 light_pos[3],
                           const s32 box_min[3], const s32 box_max[3],
                           u32 obj_flags);

/* The opt-out bit, so a caller can name it. */
#define Q2_LIGHT_NO_MOBILE 0x00800000u

/*
 * 0x800749A8..0x80074A70 — one point against one light.
 *
 * `point` is the point record's three s16 coordinates and `light_local` is the
 * light's position already made relative to the brush's own origin (the three
 * subtractions at 0x80074940..0x80074980 against object+0x28/+0x2C/+0x30).
 * Returns false when the point is outside the outer radius, which is the
 * original's skip to the next point.
 *
 * `out` IS SIGNED, and that is not tidiness. `den` is truncated DOWN by the
 * divide at 0x8007468C, so `255 - t / den` undershoots zero for the last
 * shell of points before the outer radius: a light of 600/1200 has den = 65
 * and a point exactly on the outer radius gives 255 - 259 = -4. The original
 * keeps those in registers (t5, t4, 136(sp)) and adds them as signed words at
 * 0x80074AC8, so the outermost ring of a mobile light very slightly DARKENS
 * the world instead of leaving it alone. Rounding this to a byte would turn a
 * -3 into a +253 and paint that ring white.
 */
bool q2_light_ramp_point(const q2_light_ramp *r, const s16 point[3],
                         const s32 light_local[3], s32 out[3]);

/*
 * 0x80074AC0..0x80074B0C — add a contribution into a vertex.
 *
 * `addu` in 32 bits, then `slti 256` per component, then `sb`. The clamp is
 * ONE-SIDED: there is no floor, so a negative contribution on an already-dark
 * vertex wraps through the byte store. Reproduced, because it is visible.
 */
void q2_light_ramp_add(u8 vertex_rgb[3], const s32 contrib[3]);

/*
 * The four slot bytes at point+6..+9, decoded through the table InitLights
 * builds at 0x800E1018 (0x80074134..0x80074174): the halfword at index b is
 *
 *     52 * ((b >> 2) & 31) + 4 + 12 * (b & 3)
 *
 * which is the byte offset of corner `b & 3` of POLY_GT4 `(b >> 2) & 31` in a
 * 32-poly packet block, and bit 7 of b picks the second block's base pointer
 * (0x80074A8C tests the sign of b << 24). A slot of 0xFF ends the point's list
 * early (0x80074A84), so a point can paint one, two, three or four corners.
 *
 * Returns false for 0xFF. `block`, `poly` and `corner` may each be NULL.
 */
bool q2_light_point_slot(u8 slot, u32 *block, u32 *poly, u32 *corner);

/* The table entry itself, for anyone checking this against 0x800E1018. */
s32 q2_light_packet_offset(u8 slot);

/* 0x80075BAC/0x80075BC0 — empty the runtime lists.
 *
 * ProcessLights does this at its END, after the timed pass has filled the list
 * and the mobile pass has drained it; this port calls it at the top of a frame
 * instead, which is the same instant one frame earlier. It deliberately does
 * NOT touch the timed table — that is InitLights' job and a level's, not a
 * frame's. */
void q2_light_world_begin_frame(q2_light_world *w);

/* ------------------------------------------------------------------------- */
/* What a gather produces                                                     */
/* ------------------------------------------------------------------------- */
typedef struct q2_light_env {
    /* The GTE colour matrix: row is the colour component, COLUMN is the light.
     * Row 0 is therefore the three lights' red. */
    gte_matrix colour;

    /* Three world-space directions, normalised to 1.3.12 and then scaled by
     * the entity's intensity. Rows of the light matrix once the caller has
     * composed them with the model's own rotation. */
    s16 dir[Q2_LIGHT_ACTIVE_MAX][3];

    /* Back colour, 0..255 per component — pass straight to
     * gte_set_back_colour, which does the <<4 the hardware wants. */
    s32 back[3];

    u32 active;   /* how many of `dir` and the colour columns are real, 0..3 */
} q2_light_env;

/* ------------------------------------------------------------------------- */
/* The pieces, each named for the function it came from                       */
/* ------------------------------------------------------------------------- */

/*
 * 0x80076040 — the distance attenuation, 1.12.
 *
 * `scale` multiplies both radii squared before the comparison (the entity path
 * passes Q2_LIGHT_ATTEN_SCALE, i.e. the identity; the flare path passes the
 * flare's own size). Returns 0 outside the outer radius and 4096 when the two
 * radii are equal. The result is NOT clamped: inside the inner radius it
 * exceeds 4096, which the flare path relies on and the entity path clamps.
 */
s32 q2_light_attenuation(const q2_light *l, const s16 d[3], s32 scale);

/* The 16-bit wrapping delta the whole system is defined in terms of. */
void q2_light_delta(const q2_light *l, const s32 origin[3], s16 out[3]);

/* 0x8006AFE8's prologue: four empty slots ranked 0,1,2,3. */
void q2_light_set_begin(q2_light_set *set);

/*
 * 0x8006AD4C — offer one light. Returns true when it displaced one of the three
 * being held. Rejects on any axis delta at or beyond the light's radius before
 * it computes anything expensive, which is the original's own early-out and is
 * why the radius field exists alongside radiusSq.
 */
bool q2_light_set_add(q2_light_set *set, const s32 origin[3], const q2_light *l);

/*
 * 0x8006AFE8 — the whole gather for one entity.
 *
 * `coll_node` is the SecondaryCol node the entity stands in, or negative for
 * none (which selects the fallback light). `entity_f4` is the signed halfword
 * at entity +0xF4: values >= 0 pick the world/static path; values < 0 pick the
 * view dynamic list and skip both. Passing the field rather than a boolean is
 * deliberate — the viewmodel writes literal +1 and therefore takes WORLD.
 */
void q2_light_gather(q2_light_set *set, const q2_light_world *w,
                     const s32 origin[3], s32 coll_node, s16 entity_f4);

/*
 * 0x8006B184…0x8006B4B8 — turn a finished gather into GTE state.
 *
 * `intensity_a` and `intensity_b` are the entity's two 1.12 scale fields
 * (+0xFC and +0xFE); pass Q2_LIGHT_ONE for both to get the original's neutral
 * behaviour, where the back colour comes out as exactly the glow bytes. `glow`
 * is the entity's own ambient colour, 0..255 per component, or NULL for black.
 */
void q2_light_env_build(q2_light_env *env, const q2_light_set *set,
                        s32 intensity_a, s32 intensity_b, const u8 glow[3]);

/* Load an environment into a GTE. The light matrix is the three directions as
 * rows, so a caller that needs them in model space must compose first and call
 * gte_set_light_matrix itself; this is the world-space form. */
void q2_light_env_apply(const q2_light_env *env, gte_state *g);

/*
 * 0x8008A5B8 / 0x8008A5E8 — libgte's VectorNormal, reproduced.
 *
 * Normalises an integer vector to 1.3.12. The original squares through the GTE
 * (SQR with sf = 0, so the magnitude is NOT pre-divided), rounds the leading
 * zero count DOWN TO EVEN so the square root's exponent stays an integer,
 * shifts the magnitude into the resulting [64,256) window, looks a reciprocal
 * square root up in a 192-entry table at 0x800A9C54, and shifts the product
 * back by half the exponent.
 *
 * This computes the same table from `32768 / sqrt(i + 64)`;
 * `q2psx-inspect lights` measures the two against each other entry by entry.
 */
void q2_vector_normal(const s16 in[3], s16 out[3]);

/* The reciprocal-square-root table, for the tool that compares it. */
s16 q2_light_rsqrt_table(u32 index);
#define Q2_LIGHT_RSQRT_ENTRIES 192

/*
 * 0x80075E14 — the per-entity ambient fade.
 *
 * `current` walks one `steps`-th of the way toward `target` per call, per
 * component, in integer arithmetic that truncates — so it converges only when
 * the gap exceeds `steps`, and the last few units arrive when the gap finally
 * drops below it. Passing steps == 0 is read as 1, as the original does.
 */
void q2_light_glow_fade(u8 current[3], const u8 target[3], s32 steps);

#endif /* Q2PSX_LIGHTING_H */
