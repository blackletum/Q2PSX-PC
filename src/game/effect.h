/*
 * effect.h — the presentation layer: particles, beams, debris and glints.
 *
 * ---------------------------------------------------------------------------
 * Four machines, not one
 * ---------------------------------------------------------------------------
 * There is no generic "effect" in this engine. What looks like one system is
 * four, with different pools, different lifetimes and different renderers.
 * `src/build/fxtables.h` owns their data; this owns their behaviour.
 *
 *   GROUPS   0x80030284, updated and drawn by 0x800304A8.
 *            Up to fifteen screen-aligned quads sharing one origin, one
 *            velocity, one acceleration and a pair of colour ramps. Every
 *            spark, blood spray, explosion flame and gib puff in the game is
 *            this, differing only in ramp, size and lifetime.
 *
 *   BEAMS    0x80064E64, drawn and reset by 0x80064F10.
 *            A 32-slot pool that is refilled from empty EVERY frame. A beam is
 *            a hexagonal tube between two points with two end caps; the twelve
 *            hull vertices are generated at draw time from the beam's own
 *            direction, so the pool stores only the endpoints and a radius.
 *
 *            Behind it sits a TIMED list of twelve (0x800CABC0), whose entries
 *            outlive the frame and are re-submitted until their timers expire.
 *            That is the BFG's beam — the game's weapon trail. See below.
 *
 *   DEBRIS   0x80064558 spawns a burst, 0x80064398 spawns one piece,
 *            0x80064124 thinks. Real entities with physics and a lifetime.
 *
 *   GLINTS   0x80064C00 / 0x80064780.
 *            A faceted body attached to an entity carrying flag 0x04000000,
 *            with a bright band sweeping across its surface.
 *
 *            THIS IS NOT A WEAPON TRAIL. The obvious reading of "a mesh dragged
 *            behind an entity with a band running along it" is a trail, and it
 *            is wrong: the mesh is the `GlintMod` level chunk, and drawing
 *            BIGGUN's — the only one on the disc — puts a 216-quad SPHERE on
 *            screen, not a ribbon. The band sweeps its surface, which is a
 *            shimmer. It is also not the game's weapon trail — the BFG's beam
 *            is, and that lives in the timed list above.
 *
 * ---------------------------------------------------------------------------
 * The group pool is not a particle pool
 * ---------------------------------------------------------------------------
 * A slot is a whole burst, not one particle, and this is load-bearing: the
 * fifteen quads in a group cannot outlive each other, cannot be culled
 * separately, and all read the same ramp entry, so a burst fades as one object.
 * That is why the original's explosions pulse rather than dissolve.
 *
 * Particle 0 carries the group's absolute position and velocity. Particles
 * 1..14 carry an offset and a velocity RELATIVE to particle 0, which is why the
 * whole burst drifts together while spreading — and why the spawner subtracts
 * `vel[0]` from every other velocity before storing it (0x800303C8).
 *
 * The record's field order is preserved here because the offsets are how the
 * layout was established:
 *
 *     +0x00  s32 origin[3]        particle 0, absolute
 *     +0x0C  s16 offset[14][3]    particles 1..14, relative to particle 0
 *     +0x60  s16 accel[3]         added to vel each tick; ALWAYS ZERO at spawn
 *     +0x66  s16 vel[3]           particle 0, absolute
 *     +0x6C  s16 rel_vel[14][3]   particles 1..14, relative to particle 0
 *     +0xC0  s16 size             world size, scaled by [0x800D5D46] at spawn
 *     +0xC4  u8  view_mask        high nibble: skip this group in viewport n
 *     +0xC5  u8  life             0 means the slot is free
 *     +0xC6  u8  count
 *     +0xC7  u8  area             visibility key, from the area record's +0x20
 *     +0xC8  ramp[0], +0xCC ramp[1]
 *
 * The accel slot sits inside what would otherwise be a fifteenth offset, which
 * is exactly why the burst is capped at fifteen quads and not sixteen.
 *
 * ---------------------------------------------------------------------------
 * Two ramps, alternating in threes
 * ---------------------------------------------------------------------------
 * The renderer swaps its two ramps every three quads (0x80030A14), so a group
 * given two different ramps stripes them 3-3-3-3-3 rather than blending. Every
 * spawner on the disc but one passes the same pointer twice; the blood spray
 * (0x80048BFC) is the exception, and it is why blood has two tones.
 *
 * ---------------------------------------------------------------------------
 * Age indexes the ramp, so lifetime IS colour
 * ---------------------------------------------------------------------------
 * `colour = ramp[32 - life]`. Life counts down from what the spawner asked for,
 * so a 15-tick group only ever uses entries 17..31 — the dim tail. Changing a
 * lifetime changes the colour the effect starts at. See fxtables.h.
 *
 * ---------------------------------------------------------------------------
 * What is READ and what is MODELLED
 * ---------------------------------------------------------------------------
 * READ from the executable: the record layout and every offset in it; the
 * integrator; the ramp lookup; the perspective size divide and its minimum of
 * two pixels; the quad's corner order; the two-ramp alternation; the beam pool's
 * size, its per-frame reset and its 40-byte record; the timed list's 28-byte
 * record, its timer and its dedup; the hexagon generator; the
 * beam face sets; the glint's colour band, its mesh, its two draw paths and its
 * band records; the debris burst's velocity draw, the hull it moves in, and the
 * mover's gravity, terminal velocity and no-gravity flag; every constant in
 * `q2_fx_preset`.
 *
 * MODELLED: nothing in the effect system. Two things are the CALLER's to supply
 * because the original takes them from outside the effect code — the gravity
 * step, which the shared mover reads from the same global the player's does,
 * and the glint's band data, which a level script writes through its import
 * table. Both are passed in rather than invented here.
 */
#ifndef Q2PSX_EFFECT_H
#define Q2PSX_EFFECT_H

#include "fxtables.h"
#include "gpu.h"
#include "gte.h"
#include "weapon.h"
#include "world.h"

/* ------------------------------------------------------------------------- */
/* Pool sizes                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Fifteen. Every spawner on the disc passes exactly fifteen, and the record
 * cannot hold more: the sixteenth offset slot is the acceleration.
 */
#define Q2_FX_GROUP_QUADS      15
#define Q2_FX_GROUP_FOLLOWERS  (Q2_FX_GROUP_QUADS - 1)

/*
 * 0x80030C70 sizes the pool: a negative argument gives zero groups, zero gives
 * thirty-two, anything else is taken as-is. Thirty-two is therefore the shipped
 * default and what this port uses unless told otherwise.
 */
#define Q2_FX_GROUPS_DEFAULT   32
#define Q2_FX_GROUPS_MAX       64

/* 0x800D5730..0x800D5C30, forty bytes each. */
#define Q2_FX_BEAMS_MAX        32

/* ------------------------------------------------------------------------- */
/* The timed beam list — and this IS the weapon trail                         */
/* ------------------------------------------------------------------------- */
/*
 * `0x800CABC0`..`0x800CAD10` is twelve 28-byte records, walked every frame by
 * `0x80048CA8`, which ticks each timer down by the frame delta and re-submits
 * the live ones into the transient pool. So unlike a queued beam — which lives
 * one frame — a timed beam PERSISTS, and it is the only effect in the game that
 * outlives the frame that made it without being an entity.
 *
 *     +0x00  s16 timer     down by [0x800B2DB4] per frame, clamped at 0
 *     +0x04  s16 clip      handed to the pose lookup at 0x8006CC44
 *     +0x06  s16 radius
 *     +0x08  ptr           the thing the near end is derived from
 *     +0x0C  ptr           the entity the far end sits on
 *     +0x10  tube, +0x14 cap_near, +0x18 cap_far
 *
 * **The BFG fills it.** `0x80049CF0`, inside `0x80049B9C`, which the BFG's tick
 * calls at `0x8004BD04` every frame while the ball flies. It runs a visibility
 * test (`0x80051874`) and then either refreshes the existing entry for that
 * pair of things or takes a free slot, with a timer of **45** and a radius of
 * **64**, drawn in beam style **3** — the green one.
 *
 * That is the classic BFG behaviour: the ball shoots a beam at everything it
 * can see, and each beam lingers after the ball has moved on because its timer
 * has to run down first. It is a weapon trail in every sense that matters, and
 * an earlier pass here concluded the game had none — because every *particle*
 * spawn site is an impact or a death, which is true and is not the whole story.
 *
 * It also corrects the claim that beam styles 3 and 4 are unreachable. They are
 * not reached from the laser dispatcher; they are reached from here. Style 4 —
 * the opaque red one — is filled by `0x8004E9F4` inside `0x8004E920`, behind the
 * same visibility test; which weapon or creature drives that one is not yet
 * attributed.
 */
#define Q2_FX_TIMED_BEAMS_MAX    12   /* 0x800CABC0..0x800CAD10, 28 each  */
#define Q2_FX_TIMED_BEAM_LIFE    45   /* 0x80049DC0                       */
#define Q2_FX_TIMED_BEAM_RADIUS  64   /* 0x80049D20                       */
#define Q2_FX_TIMED_BEAM_STYLE    3   /* 0x80049D00, the green style      */

typedef struct q2_fx_timed_beam {
    s16 timer;                    /* +0x00 */
    s16 radius;                   /* +0x06 */

    /*
     * The original keys a record on the two POINTERS it stores at +0x08 and
     * +0x0C and refreshes the matching one rather than allocating a second.
     * The port keys on a caller-supplied pair of ids for the same reason: a
     * BFG ball that can see one target must hold one beam on it, not add a
     * beam every frame until the list is full.
     */
    s32 owner;
    s32 target;

    s32 from[3];
    s32 to[3];

    const q2_fx_face *tube;
    const q2_fx_face *cap_near;
    const q2_fx_face *cap_far;
} q2_fx_timed_beam;

/* MODELLED. The original allocates debris out of the shared entity pool, so it
 * has no private ceiling; this port gives it one so the frame cost is bounded. */
#define Q2_FX_DEBRIS_MAX       48

/* The glint mesh's transform window — 0x80064780 walks 96 vertices in threes
 * and its face loop is bounded at 94. */
#define Q2_FX_GLINT_VERTS      96

/* ------------------------------------------------------------------------- */
/* What turns a glint on, and it is not the executable                        */
/* ------------------------------------------------------------------------- */
/*
 * The draw at 0x8006AA44 gates on bit 0x04000000 of entity +0x10C, and NOTHING
 * IN THE EXECUTABLE EVER SETS IT. All thirty-nine writes to +0x10C were checked
 * and the largest bit any of them raises is 0x8000; the only site in the whole
 * image that materialises 0x04000000 into a general register is the test
 * itself. Read from the executable alone the glint is dead code.
 *
 * It is not dead. BIGGUN's `LevelBin` — the level script, which is a
 * RELOCATABLE MIPS MODULE, not bytecode — carries both halves, at file offsets
 * 0x18650 and 0x187B8 of its COMMON.DAT:
 *
 *     lw   v0, 268(a0)      ; ent+0x10C
 *     lui  v1, 0x0400
 *     or   v0, v0, v1       ; raise the glint flag
 *     sw   v0, 268(a0)
 *
 * and, four instructions earlier, `sb 6, 695(a0)` — ent+0x2B7, the BAND COUNT.
 * The second site tests the same bit and resets `sh 4, 702(a0)` — ent+0x2BE,
 * the PHASE.
 *
 * That last one is the check that the band formula was read right: the phase
 * starts at 4 and 0x80064D3C counts it down, which is exactly what makes
 * `(width / 4) * (4 - phase)` sweep the band from one end of the mesh to the
 * other. A formula with the subtraction the other way round would have looked
 * just as plausible and swept backwards.
 *
 * So the answer to "which entity states raise the flag" is: the level script
 * does, on the one map that ships a mesh, and no engine state does. A port
 * cannot reproduce it without running BIGGUN's script — which is why the
 * client does NOT draw a glint by default. Inventing an entity to wear one
 * would be adding behaviour the console does not have.
 */
#define Q2_FX_GLINT_BANDS       6   /* ent+0x2B7, set by BIGGUN's script */
#define Q2_FX_GLINT_PHASE_START 4   /* ent+0x2BE, and the 4 in the formula */

/* ------------------------------------------------------------------------- */
/* One band, and where the bands come from                                    */
/* ------------------------------------------------------------------------- */
/*
 * 0x80064C00 has TWO paths and they are not variations of each other.
 *
 *   ent+0x2B7 == 0   one band, using the entity's own colour (+0x2B4), its own
 *                    phase (+0x2BE) and its own matrix (+0x2C0), at width 8192.
 *
 *   ent+0x2B7 != 0   that many bands, each a 12-byte record in a SHARED array
 *                    at 0x800D565C with its own orientation, its own phase and
 *                    its own colour, at width 4096 — so a band in the multi
 *                    path is half as wide and twice as bright as one in the
 *                    single path (see q2_fx_glint_shade: the two uses of width
 *                    do not cancel).
 *
 * Each band's record is
 *
 *     +0x00  s16 angle[3]   its own orientation, composed with the entity's
 *     +0x06  u8  phase      decremented by the DRAW, per band
 *     +0x07  u8  pad
 *     +0x08  u32 colour     {r,g,b,code}, as everywhere else here
 *
 * and 0x800D565C..0x800D56B0 holds SEVEN of them, which is the ceiling however
 * many the count asks for.
 *
 * The array is not filled by the executable either: 0x8007A074 exports it to
 * relocated modules through their import table at +0x3BC, next to `T_Damage`
 * and the rest of the engine's services. So the level script owns both the flag
 * and the band data — the effect system's job is to consume them, and that is
 * what is reconstructed here.
 *
 * The per-band phase decrement is a PLAIN byte step that underflows: 0x80064DEC
 * is `addiu v0, a3, 255` with no clamp and no reload, so a band that runs past
 * zero wraps to 255 and lights nothing for the next ~250 ticks. Refreshing it is
 * the script's job too, and wrapping it back to the start here would be the port
 * inventing a reload the console does not perform.
 *
 * It happens inside the DRAW in the original, which is why a two-viewport frame
 * would advance every band twice. This port steps it in `q2_fx_glint_advance`
 * on the tick instead — the glint's one divergence.
 */
#define Q2_FX_GLINT_BANDS_MAX   7
#define Q2_FX_GLINT_BAND_WIDTH  4096   /* 0x80064DFC, the multi-band path  */
#define Q2_FX_GLINT_ONE_WIDTH   8192   /* 0x80064D48, the single-band path */

/* The decoded `GlintMod` mesh. Defined here rather than beside its decoder
 * because the glint state below holds one by value; the decode contract is
 * documented with q2_fx_glint_mesh_decode. */
typedef struct q2_fx_glint_mesh {
    const s16 (*vert)[4];   /* {x, y, z, band} */
    u32         vert_count;
    const u8   *index;      /* 4 per face */
    u32         face_count;
} q2_fx_glint_mesh;

typedef struct q2_fx_glint_band {
    s16 angle[3];   /* +0x00 composed with the entity's own matrix */
    u8  phase;      /* +0x06 */
    u8  pad;        /* +0x07 */
    u32 colour;     /* +0x08 */
} q2_fx_glint_band;

typedef struct q2_fx_glint {
    q2_fx_glint_mesh mesh;
    bool             ready;

    /* True when the map's own LevelBin raises the flag. See q2_fx_glint_scan. */
    bool             raised;

    /* ent+0x2B7. Zero selects the single-band path. */
    u8               band_count;

    /* The shared array at 0x800D565C. */
    q2_fx_glint_band band[Q2_FX_GLINT_BANDS_MAX];

    /* The single-band path's own state: ent+0x2B4 and ent+0x2BE. */
    u8               tint[3];
    u16             phase;
} q2_fx_glint;

/* ------------------------------------------------------------------------- */
/* Asking the level script whether it raises a glint                          */
/* ------------------------------------------------------------------------- */
/*
 * `LevelBin` is a relocatable MIPS module, and this project does not execute
 * one. It does not have to: the question "does this level raise a glint, and
 * with what" is answerable by READING the module, and reading modules is
 * something the project already does.
 *
 * `q2_fx_glint_scan` looks for the instruction triple that raises the flag —
 *
 *     lw   rX, 0x10C(rY)
 *     lui  rZ, 0x0400
 *     or   rX, rX, rZ
 *
 * — and, when it finds one, picks up the two immediates the surrounding code
 * writes: `sb <n>, 0x2B7(rY)` for the band count and `sh <n>, 0x2BE(rY)` for
 * the phase. So the port takes the script's own numbers rather than a
 * hardcoded 6 and 4, and a map whose script uses different ones gets those.
 *
 * This is general rather than a special case for BIGGUN: any build, any map,
 * any script. A map whose script does not raise a glint gets none, which is
 * every map but one on this disc.
 *
 * WHICH ENTITY it attaches to is the one part that still needs the script to
 * run. The search loop at chunk `+0x1868` walks 48 records of 768 bytes and
 * takes the one whose `+0xD2` is 46 and whose `+0xDA` is 20 — a kind tag and a
 * sub-id, both written by the spawner (`0x8007D700` stores 47 into `+0xD2` for
 * a different kind). The rule is recorded here so that the lookup becomes exact
 * the moment the port's entity records carry those two fields; until then the
 * caller supplies the position and this module does not guess one.
 */
#define Q2_FX_GLINT_TARGET_KIND  46   /* ent+0xD2, the search key   */
#define Q2_FX_GLINT_TARGET_ID    20   /* ent+0xDA                   */
#define Q2_FX_GLINT_ENT_STRIDE  768   /* the script's own step      */
#define Q2_FX_GLINT_ENT_MAX      48   /* and its own bound          */

typedef struct q2_fx_glint_script {
    bool raises;        /* the flag is raised somewhere in this module  */
    u32  raise_offset;  /* where, so the finding can be re-checked      */
    u8   band_count;    /* the `sb` near it, or 0 if none was found     */
    u8   phase;         /* the `sh` near it, or 0                       */
} q2_fx_glint_script;

/*
 * Read a `LevelBin` chunk and report whether its script turns a glint on.
 * Returns false when it does not, leaving `*out` zeroed.
 */
bool q2_fx_glint_scan(q2_fx_glint_script *out, const u8 *levelbin, u32 size);

/* Advance every live band's phase by one tick, wrapping at the start value.
 * The original does this inside the draw; doing it here is what keeps split
 * screen from advancing the bands once per viewport. */
void q2_fx_glint_advance(q2_fx_glint *g);

/*
 * Draw a glint, taking whichever path its band count selects. Returns the
 * number of primitives emitted, and zero when the mesh is not loaded.
 */
u32 q2_fx_glint_draw(const q2_fx_glint *g, const s32 origin[3], s32 yaw,
                     const q2_camera *cam, psx_ot *ot, gte_state *gte);

/* ------------------------------------------------------------------------- */
/* The per-frame particle budget                                              */
/* ------------------------------------------------------------------------- */
/*
 * 0x80030CB4 sets a quad allowance the renderer spends and the spawner ignores:
 * one viewport gets `groups * 15`, more than one gets `(groups / 2) * views *
 * 15`. A group whose count exceeds what is left is skipped ENTIRELY rather than
 * partially drawn (0x80030650), so a crowded frame drops whole bursts. Keeping
 * that is the difference between the original's behaviour under load and a
 * smooth degradation it never had.
 */
u32 q2_fx_budget(u32 group_count, u32 viewport_count);

/* ------------------------------------------------------------------------- */
/* One group                                                                  */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_group {
    s32 origin[3];                              /* +0x00 */
    s16 offset[Q2_FX_GROUP_FOLLOWERS][3];       /* +0x0C */
    s16 accel[3];                               /* +0x60 */
    s16 vel[3];                                 /* +0x66 */
    s16 rel_vel[Q2_FX_GROUP_FOLLOWERS][3];      /* +0x6C */
    s16 size;                                   /* +0xC0 */
    u8  view_mask;                              /* +0xC4 high nibble */
    u8  life;                                   /* +0xC5 */
    u8  count;                                  /* +0xC6 */
    u8  area;                                   /* +0xC7 */
    const q2_fx_ramp *ramp[2];                  /* +0xC8, +0xCC */
} q2_fx_group;

/* ------------------------------------------------------------------------- */
/* One transient beam                                                         */
/* ------------------------------------------------------------------------- */
/*
 * The forty-byte record at 0x800D5730. The original stores the three face
 * lists as three independent pointers, so a caller could in principle mix a
 * tube from one style with caps from another. Nothing does, but the three
 * pointers are kept rather than collapsed to a style index, because collapsing
 * them would quietly remove a degree of freedom the format has.
 */
typedef struct q2_fx_beam {
    s32 from[3];                  /* +0x00 */
    s32 to[3];                    /* +0x0C */
    s16 radius;                   /* +0x18 */
    s16 area;                     /* +0x1A */
    const q2_fx_face *tube;       /* +0x1C, six faces  */
    const q2_fx_face *cap_near;   /* +0x20, two faces  */
    const q2_fx_face *cap_far;    /* +0x24, two faces  */
} q2_fx_beam;

/* ------------------------------------------------------------------------- */
/* One debris piece                                                           */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_debris {
    bool in_use;
    s32  pos[3];       /* entity +0xA4 and +0x54, both set to the spawn point */
    s16  vel[3];       /* entity +0xE0 */
    s16  spin[3];      /* entity +0xE6, three raw rand() draws               */
    s16  life;         /* entity +0xF4, 2100 at spawn, -300 per impact       */
    s16  model;        /* which registered debris model this piece uses      */
    u32  flags;        /* entity +0x10C; Q2_FX_ENT_NO_GRAVITY lives here    */
    u8   area;         /* entity +0x9E */
    s32  node;         /* collision cell, -1 = unknown (same as projectiles) */
} q2_fx_debris;

/* ------------------------------------------------------------------------- */
/* The effect world                                                           */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_stats {
    u32 groups_live;
    u32 groups_drawn;
    u32 groups_skipped_budget;  /* the whole-burst drop described above */
    u32 groups_skipped_near;    /* projected in front of the near plane */
    u32 quads_emitted;
    u32 beams_queued;
    u32 beams_dropped;          /* the pool was full when one was offered */
    u32 beam_faces_emitted;
    u32 debris_live;
    u32 ot_overflow;
} q2_fx_stats;

typedef struct q2_fx_world {
    const q2_fx_tables *tab;

    q2_fx_group  group[Q2_FX_GROUPS_MAX];
    u32          group_count;      /* the pool size, not the live count */

    q2_fx_beam   beam[Q2_FX_BEAMS_MAX];
    u32          beam_count;       /* how many are queued THIS frame    */

    /* The persistent list at 0x800CABC0. Unlike `beam`, these survive the
     * frame and are re-submitted until their timers run out. */
    q2_fx_timed_beam timed[Q2_FX_TIMED_BEAMS_MAX];

    q2_fx_debris debris[Q2_FX_DEBRIS_MAX];

    /*
     * The registration list at 0x800D56B0, capped at 32 by 0x80064F80. A level
     * fills it from its own class table ("Debris1".."Debris3" all register
     * through 0x80064F70) and the burst picks uniformly from it.
     */
    s16          debris_model[32];
    u32          debris_model_count;

    u32          viewport_count;   /* [0x800B2C2C] */
    u32          budget;           /* recomputed by q2_fx_world_resize   */

    /*
     * 0x800D5D46, the multiplier every spawner applies to its size argument
     * before storing it: `size = (arg * scale) / 512`, rounding toward zero.
     * It is a level-scale knob, and the port defaults it to the value that
     * makes the divide a no-op so a caller that never sets it gets the
     * argument through unchanged.
     */
    s16          size_scale;

    /*
     * The particle image's registration, from the level's VRAM upload. The
     * ramp's ABR field is OR-ed into `tpage_base` at draw time, exactly as
     * 0x80030830 does — which is why the base is stored without it.
     */
    u16          tpage_base;    /* [0x800DDD5A] */
    u16          clut;          /* [0x800E3F2C + 150] = palette 75 */

    /*
     * Draw flat quads instead of textured ones. Set automatically when no
     * image has been registered, because the alternative is a burst that emits
     * fifteen primitives and shows nothing. Clearing it without registering an
     * image gives you the console's behaviour against an empty texture page,
     * which is nothing at all.
     */
    bool         untextured;

    q2_fx_stats  stats;
} q2_fx_world;

/*
 * Register the particle image. `tpage` is the page word WITHOUT its
 * semi-transparency field — the ramps supply that per effect — and `clut` is
 * the palette. Turns `untextured` off.
 */
void q2_fx_set_texture(q2_fx_world *w, u16 tpage_base, u16 clut);

/*
 * The same, from a map's uploaded HUD atlas — which is the page the particles
 * are actually on, with built-in palette 75 as their CLUT. Does nothing when the
 * font is not resident, so a map with no `chars.lbm` (two of the forty-nine)
 * keeps the flat-quad fallback.
 */
#define Q2_FX_CLUT_PALETTE_ID 75
struct q2_hud_font;
void q2_fx_use_hud_atlas(q2_fx_world *w, const struct q2_hud_font *font);

/* 512, so that `size_scale == Q2_FX_SIZE_SCALE_UNITY` passes the argument
 * through. The disc's own value lives in level data, not in the executable. */
#define Q2_FX_SIZE_SCALE_UNITY 512

/* ------------------------------------------------------------------------- */
/* What a particle quad is textured with                                      */
/* ------------------------------------------------------------------------- */
/*
 * A quad is a POLY_FT4, and the renderer at 0x80030958 writes only its four
 * corners and its draw-mode word. The UVs and the CLUT are baked into the
 * primitive pool once, by the allocator at 0x80030DB8:
 *
 *     uv0 = (240, 240)   uv1 = (255, 240)
 *     uv2 = (240, 255)   uv3 = (255, 255)
 *
 * — a 16x16 patch in the bottom-right corner of a page. Both the page and the
 * palette are zero-filled bss, written when images are registered at load time,
 * and both are resolvable anyway.
 *
 * THE PAGE. `0x8001AD14` picks between two of those globals by a menu item's
 * face size: size 8 takes `[0x800DDD5A]` and anything else `[0x800DDD56]`. The
 * 8-pixel face is `chars.lbm` — hudtables.h — so the particle and the small
 * menu font are on the SAME PAGE, and a particle is the 16x16 corner of the HUD
 * atlas rather than an image of its own.
 *
 * THE PALETTE. `0x800E3FC2` is not a standalone global, which is why an
 * address-level sweep finds no writer for it: the allocator materialises
 * `0x800E3F2C` and reads `+150`. `0x800E3F2C` is the CLUT-id table the boot
 * palette loop at `0x8007610C` fills — one entry per built-in palette, indexed
 * by the record's own id, and hudtables.h already decodes the whole bank. So
 * the particle's palette is **built-in palette 75**, three below the box ramp's
 * 76 and three above the font's 72.
 *
 * `q2_fx_use_hud_atlas` wires up both from a map's own VRAM.
 *
 * A caller with no VRAM at all gets `untextured`, which emits a flat quad of
 * the ramp's colour instead — visible, honest, and NOT what the hardware drew.
 * It is a fallback rather than a reconstruction, and it is a separate flag so
 * nobody has to guess which they are looking at.
 */
#define Q2_FX_QUAD_U0 240
#define Q2_FX_QUAD_V0 240
#define Q2_FX_QUAD_U1 255
#define Q2_FX_QUAD_V1 255

void q2_fx_world_init(q2_fx_world *w, const q2_fx_tables *tab);

/* Change the group pool size and recompute the budget. `groups` follows
 * 0x80030C70: zero means the shipped default of thirty-two. */
void q2_fx_world_resize(q2_fx_world *w, u32 groups, u32 viewports);

/* Free every group, beam and debris piece. 0x80048C54 + 0x80064EF4. */
void q2_fx_world_clear(q2_fx_world *w);

/* ------------------------------------------------------------------------- */
/* Groups                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * Spawn one group. Faithful to 0x80030284:
 *
 *   - the first free slot wins, scanning from the bottom of the pool
 *   - `vel[0]` is stored absolute; `vel[1..]` are stored as differences
 *   - offsets and the acceleration start at zero
 *   - `size = (size_arg * world->size_scale) / 512`, rounding toward zero
 *   - the low byte of the flags word is cleared, so view_mask starts empty
 *
 * `vel` must have `count` entries. Returns the slot, or -1 when the pool is
 * full — which the original also has to cope with, and which it handles by
 * silently dropping the effect.
 */
s32 q2_fx_group_spawn(q2_fx_world *w,
                      const s32 origin[3],
                      const s16 (*vel)[3], u32 count,
                      const q2_fx_ramp *ramp0, const q2_fx_ramp *ramp1,
                      u32 life, s32 size, u8 area);

/*
 * One tick of the integrator, exactly as the tail of 0x800304A8 runs it:
 *
 *     life--
 *     origin += vel
 *     vel    += accel
 *     offset[i] += rel_vel[i]      for i in 1..count-1
 *
 * Note the order: the position uses the velocity BEFORE the acceleration is
 * applied, so a group is always one tick behind a naive integrator. That is
 * visible on the long-lived bursts and is reproduced rather than corrected.
 *
 * The original runs this inside the draw, once per frame rather than once per
 * viewport. Splitting it out is what lets a headless caller advance effects
 * without a screen.
 */
void q2_fx_tick(q2_fx_world *w);

/* Where quad `i` of a group is this tick, in world units. */
void q2_fx_group_point(const q2_fx_group *g, u32 i, s32 out[3]);

/* The colour word a group reads this tick from ramp `which` (0 or 1). */
u32 q2_fx_group_colour(const q2_fx_group *g, u32 which);

/* ------------------------------------------------------------------------- */
/* Beams                                                                      */
/* ------------------------------------------------------------------------- */
/*
 * Queue one beam for this frame. 0x80064E64. Returns false when the 32-slot
 * pool is full, which is what the original returns too — the caller is expected
 * to carry on regardless.
 */
bool q2_fx_beam_add(q2_fx_world *w,
                    const s32 from[3], const s32 to[3],
                    s16 radius, s16 area,
                    const q2_fx_face *tube,
                    const q2_fx_face *cap_near,
                    const q2_fx_face *cap_far);

/* The same, naming a style out of the tables. */
bool q2_fx_beam_add_style(q2_fx_world *w,
                          const s32 from[3], const s32 to[3],
                          s16 radius, s16 area, u32 style);

/* Empty the beam pool. 0x80064EF4, and the tail of 0x80064F10. */
void q2_fx_beams_reset(q2_fx_world *w);

/*
 * Hold a persistent beam between two things — the BFG's, and the mechanism for
 * any weapon trail.
 *
 * `owner` and `target` are the caller's ids for the two ends. 0x80049D30
 * searches the list for a record whose stored pair matches and REFRESHES it
 * rather than allocating a second, so a ball that can see one creature holds
 * one beam on it however many frames it is in view. Passing a fresh pair every
 * frame would fill all twelve slots in twelve frames and then stop drawing.
 *
 * `life` is in the same units the frame delta is subtracted in; pass
 * Q2_FX_TIMED_BEAM_LIFE for the BFG's own 45. Returns false when the list is
 * full of live beams, which the original also just tolerates.
 */
bool q2_fx_beam_timed(q2_fx_world *w, s32 owner, s32 target,
                      const s32 from[3], const s32 to[3],
                      s16 radius, u32 style, s16 life);

/*
 * Age the timed list by one frame's worth. 0x80048CE8 subtracts the frame delta
 * and clamps at zero — it does not free the slot, so a record stays matchable
 * by its owner/target pair until something else claims it.
 */
void q2_fx_timed_tick(q2_fx_world *w, s32 frame_delta);

/* How many timed beams are still alive. */
u32 q2_fx_timed_live(const q2_fx_world *w);

/*
 * The twelve hull vertices of a beam, in world units.
 *
 * 0x800634E4 builds an orthonormal basis around the beam's direction, then
 * walks the 4096-step circle in thirds — three axes at 120 degrees — and emits
 * each axis and its negation, giving a hexagon. The near hexagon is written to
 * out[0..5] and the far one to out[6..11].
 *
 * Returns false when `from` and `to` coincide, which is when the basis cannot
 * be built; the original bails at 0x8006350C for the same reason and draws
 * nothing.
 */
bool q2_fx_beam_hull(const q2_fx_beam *b, s32 out[Q2_FX_BEAM_VERTS][3]);

/* ------------------------------------------------------------------------- */
/* Debris                                                                     */
/* ------------------------------------------------------------------------- */
/* Register a debris model. 0x80064F70: appends, capped at 32, silently. */
bool q2_fx_debris_register(q2_fx_world *w, s16 model);

/*
 * The burst at 0x80064558.
 *
 * `bmin`/`bmax` are the Scene node's bounding box. When `at` is non-NULL every
 * piece starts there; when it is NULL each piece starts at a uniformly random
 * point inside the box — which is how a shattering window scatters along its
 * whole surface rather than out of its centre.
 *
 * Each piece's velocity is drawn as
 *
 *     x = (3 * (rand() - 16384)) >> 5                 in -1536..1535
 *     y = (3 * (rand() - 16384)) >> 5  - 1536         biased upward
 *     z = (3 * (rand() - 16384)) >> 5
 *
 * The Y bias is the whole reason debris arcs: -1536 is up in this engine's
 * frame. Its model is picked uniformly from the registration list.
 *
 * Returns how many pieces were actually spawned, which is less than `count`
 * when the pool fills.
 */
u32 q2_fx_debris_burst(q2_fx_world *w, q2_rng *rng,
                       const s32 bmin[3], const s32 bmax[3],
                       const s32 *at, u32 count, u8 area);

/*
 * WHICH HULL DEBRIS MOVES IN, and it is not the player's.
 *
 * The think hands each piece to `0x80046DA0`, which parks `0x800C8E90` —
 * PrimaryColl — in the mover's hull slot and the entity's `+0xA0` in its node
 * slot, then calls the shared mover. Its twin at `0x80046DDC` is the one that
 * uses SecondaryCol and `+0xA2`, and that is the player's path.
 *
 * So debris is traced against the UNERODED hull with its own cell index, while
 * the player is traced against the hull eroded by 286 (sim.h). Using the
 * player's hull for debris would stop every shard 286 units short of the floor
 * and leave it hanging in mid-air — which reads as a physics bug rather than a
 * hull mix-up.
 *
 * The radius is `a2` at the call site: 2048.
 *
 * GRAVITY IS READ, not modelled. The shared mover applies it at 0x80046464:
 *
 *     if (ent+0x10C & 0x2000) skip          ; a per-entity no-gravity flag
 *     vel.y += [0x800AE924] * frame_delta   ; the SAME global the player uses
 *     if (vel.y > 8192) vel.y = 8192        ; terminal velocity
 *
 * Three things follow that a plausible model would have got wrong: the step is
 * gravity times the FRAME DELTA rather than a flat per-tick add; the fall speed
 * is capped, so a shard dropped down a lift shaft stops accelerating; and the
 * value is the simulation's own, which is why `q2_fx_debris_step_one` takes it
 * from the caller instead of keeping a second copy that could drift from the
 * player's when the GAME VARIABLES menu changes it (sim.h).
 */
#define Q2_FX_DEBRIS_RADIUS 2048

/* 0x80046490. The downward speed a falling entity is clamped to. */
#define Q2_FX_TERMINAL_VELOCITY 8192

/* 0x80046458. Set in ent+0x10C, this suppresses gravity entirely. */
#define Q2_FX_ENT_NO_GRAVITY 0x2000u

/* What a debris piece wants to do this tick. Same contract as q2_proj_step:
 * the caller traces `from`..`to` and either commits or reports an impact. */
typedef struct q2_fx_debris_step {
    s32  from[3];
    s32  to[3];
    bool expired;
} q2_fx_debris_step;

void q2_fx_debris_step_one(q2_fx_world *w, u32 index, s32 gravity,
                           q2_fx_debris_step *out);
void q2_fx_debris_commit(q2_fx_world *w, u32 index, const s32 to[3]);

/*
 * Resolve an impact. 0x80064228: a piece that hits something advances its spin
 * by one and loses 300 of its lifetime, and is freed when that reaches zero. It
 * does NOT bounce off geometry in the original — it slides, because it is
 * handed to the shared mover — so this stops it at the contact point.
 */
void q2_fx_debris_impact(q2_fx_world *w, u32 index, const s32 point[3]);

/* ------------------------------------------------------------------------- */
/* Glints                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * The colour band, 0x80064780.
 *
 * A glint is a fixed mesh whose vertices carry a surface coordinate in
 * their Z. The renderer lights a band of it and slides the band along as the
 * entity's counter runs down:
 *
 *     centre = (width / 4) * (4 - phase) + 1024
 *     d      = |vertex.z - centre|
 *     w      = (d >= width) ? 0 : (width - d) * phase
 *     rgb    = (tint * w) >> 15,  tint = (channel << 13) / width
 *
 * `width` is the caller's 8192 or 4096 and `phase` the entity's own countdown.
 * Writing it out matters because the shape is not obvious from the arithmetic:
 * the band is triangular, its brightness scales with the phase, and the two
 * `width` uses do not cancel — halving the width both narrows the band and
 * doubles the tint.
 *
 * `vertex_band` is the FOURTH halfword of each mesh vertex, not its Z. See the
 * GlintMod notes below.
 *
 * Fills `out_rgb[i]` for each of `count` vertices.
 */
void q2_fx_glint_shade(const s16 *vertex_band, u32 count,
                       const u8 tint_rgb[3], s32 width, s32 phase,
                       u8 (*out_rgb)[3]);

/* ------------------------------------------------------------------------- */
/* The glint mesh — the `GlintMod` chunk                                      */
/* ------------------------------------------------------------------------- */
/*
 * The mesh is not in the executable, and for a while that looked like a dead
 * end. It is a NAMED LEVEL CHUNK: `0x8007AB44` looks up "GlintMod" in the
 * .DAT directory and hands the chunk pointer to `0x800651BC`, which splits it
 * at a fixed offset and parks the halves at `gp+0x4614` and `gp+0x4610`.
 *
 *     [0, 864)      216 faces, four u8 indices each, in the GPU's Z order
 *     [864, end)    (size - 864) / 8 vertices: s16 x, y, z, band
 *
 * The split is a literal 864 (`addiu v0, a0, 864`), not a header field, so a
 * chunk of any size has its faces in the first 864 bytes and its vertices after.
 *
 * THE FOURTH HALFWORD IS THE BAND COORDINATE, not padding. `0x80064938` reads
 * `+6` off each source vertex — the slot an SVECTOR normally wastes — and that
 * is what the travelling band is measured along. On BIGGUN it starts at 1024 on
 * the tip vertex and rises with distance, and 1024 is exactly the constant the
 * band's centre is offset by (`0x800648EC`), which is the check that says the
 * field was read right rather than plausibly.
 *
 * Reading the Z instead gets you a band that sweeps the wrong axis and looks
 * like a lighting bug.
 *
 * `GlintMod` is OPTIONAL and present on exactly ONE map of the forty-nine —
 * BIGGUN — where the chunk is 2,608 bytes: 216 quads over 218 vertices, with
 * the highest index reaching exactly 217. So the glint is one effect on one
 * level, not a general weapon trail, and a port that never loads BIGGUN will
 * never draw one.
 */
#define Q2_FX_GLINT_INDEX_BYTES 864
#define Q2_FX_GLINT_FACE_COUNT  (Q2_FX_GLINT_INDEX_BYTES / 4)   /* 216 */

/*
 * What one call to the renderer can actually cover. `0x800649C8` transforms
 * vertices in threes while its counter is under 94 — 96 vertices — into the
 * shared scratch at 0x800B2DFC, and `0x80064BC4` stops emitting at 79
 * primitives. Both are literal bounds in the original, so the port honours them
 * rather than drawing the whole 216-face mesh and calling it an improvement.
 */
#define Q2_FX_GLINT_MAX_FACES   79

/*
 * Decode a `GlintMod` chunk. `data`/`size` are the chunk's own bytes; the mesh
 * points into them, so they must outlive it.
 *
 * Fails for anything at or under 864 bytes, which is a chunk with no vertices —
 * the split is unconditional in the original and would hand the renderer a
 * pointer past the end.
 */
bool q2_fx_glint_mesh_decode(q2_fx_glint_mesh *out, const u8 *data, u32 size);

/*
 * One glint, appended to `ot`.
 *
 * `origin` and `yaw` place the mesh; `tint_rgb` and `width` are the entity's
 * own colour and band width (`0x04000000` entities carry them at +0x2B4 and
 * are handed 8192 or 4096); `phase` is the countdown at +0x2BE, which the
 * caller decrements — 0x80064D3C does it in the draw, but doing it there would
 * make a two-viewport frame advance the band twice.
 *
 * Returns the number of primitives emitted, and zero for a NULL or empty mesh
 * rather than inventing one.
 */
u32 q2_fx_glint_build_ot(const q2_fx_glint_mesh *mesh,
                         const s32 origin[3], s32 yaw,
                         const u8 tint_rgb[3], s32 width, s32 phase,
                         const q2_camera *cam, psx_ot *ot, gte_state *gte);

/* ------------------------------------------------------------------------- */
/* The named effects                                                          */
/* ------------------------------------------------------------------------- */
/*
 * Every burst in the game is `q2_fx_group_spawn` with a different set of five
 * numbers. They are immediates inside their spawn sites, so they are
 * transcribed with the address each came from rather than read from a table.
 *
 * `spread_shift` is the shift applied to `rand() - 16384` when drawing each
 * quad's velocity, so a smaller shift is a wider burst: shift 9 gives
 * components in -32..31, shift 10 gives -16..15.
 */
typedef enum q2_fx_preset_id {
    Q2_FX_EXPLOSION = 0,   /* 0x800486EC — grenades, rockets, dying monsters */
    Q2_FX_BLOOD,           /* 0x80048C08 — the only two-ramp effect          */
    Q2_FX_BFG_BURST,       /* 0x8004BDC4 — the biggest quads in the game     */
    Q2_FX_GIB,             /* 0x800596B0 — thrown when a body comes apart    */
    Q2_FX_SCRIPTED,        /* 0x80028DC8 — the UserFuncs effect primitive    */
    Q2_FX_SPARK,           /* 0x8003E0C0 — a surface being struck            */
    Q2_FX_LASER_END,       /* 0x80049074 — both ends of a laser beam         */
    Q2_FX_PRESET_COUNT
} q2_fx_preset_id;

typedef struct q2_fx_preset {
    u8   count;          /* quads in the burst                        */
    u8   life;           /* ticks, and therefore where the ramp starts */
    s32  size;           /* world size before size_scale               */
    u8   spread_shift;
    u8   ramp0;          /* ARRAY index into the ramp table            */
    u8   ramp1;
    u32  site;           /* the spawn site this was read out of        */
} q2_fx_preset;

const q2_fx_preset *q2_fx_preset_at(q2_fx_preset_id id);

/*
 * Spawn a named effect at `at`. Draws `count` velocities from `rng` with the
 * preset's shift, exactly as the spawn sites do, and hands them to
 * q2_fx_group_spawn. Returns the slot or -1.
 */
s32 q2_fx_spawn(q2_fx_world *w, q2_rng *rng, q2_fx_preset_id id,
                const s32 at[3], u8 area);

/*
 * The scripted effect (0x80028DC8) is the one preset whose life and size are
 * not constant: life is `(rand() + 24) & 15` and size is
 * `(rand() & 0x2047) + 12000`. Both draws happen before the velocities, so the
 * generator's sequence matters; q2_fx_spawn reproduces the order.
 */

/* ------------------------------------------------------------------------- */
/* Blood colour                                                               */
/* ------------------------------------------------------------------------- */
/*
 * A gib burst's ramp comes from the creature, not from the effect: 0x80059648
 * tests three bits of the flag word in this order and takes the first that is
 * set. So the game has red, green and blue blood, and which one a monster
 * bleeds is a property of its class.
 *
 * The chain has no final else. A creature with none of the three bits reaches
 * the spawn with the ramp register still holding whatever it last held, which
 * is an engine defect of the same kind as the uninitialised T_Damage argument
 * in userfuncs.h. `q2_fx_gib_ramp` returns red in that case and says so; the
 * alternative is not reproducible.
 */
#define Q2_FX_BLOOD_RED    0x10u
#define Q2_FX_BLOOD_GREEN  0x20u
#define Q2_FX_BLOOD_BLUE   0x40u

/* Which ramp a creature with these flags bleeds. */
u8 q2_fx_gib_ramp(u32 creature_flags);

/* The gib burst, with the creature's blood colour applied. */
s32 q2_fx_gib(q2_fx_world *w, q2_rng *rng, const s32 at[3], u8 area,
              u32 creature_flags);

/* ------------------------------------------------------------------------- */
/* The laser                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * 0x80048DC8. Queues the beam, reports the damage the caller should apply
 * along it, and spawns a burst at each end.
 *
 * The original traces for a victim itself and calls T_Damage; here the trace
 * and the damage stay with the caller, because this module has no collision
 * hull and should not grow one. `out_damage` and `out_mod` carry what to apply.
 *
 * `ends` selects which end bursts fire, matching the two independent halfwords
 * the fifth argument packs (0x80048FF0 and 0x8004909C test them separately):
 * bit 0 is the `from` end, bit 1 the `to` end. Each end that fires spawns FOUR
 * groups of fifteen, so a laser with both ends lit costs eight pool slots.
 *
 * Returns false for an unknown kind, in which case nothing was queued.
 */
typedef struct q2_fx_laser_result {
    bool queued;        /* the beam made it into the pool                */
    s16  damage;
    s16  mod;
    u32  groups;        /* how many end-burst groups were spawned        */
} q2_fx_laser_result;

#define Q2_FX_LASER_END_FROM 1u
#define Q2_FX_LASER_END_TO   2u

/* Four groups per lit end. 0x8004908C / 0x80049138 both loop `s2 < 4`. */
#define Q2_FX_LASER_END_GROUPS 4

/*
 * `area` is s16 and not u8 because the original's is: the script's own beams
 * reach here with a raw collision-node index (0x8002EEB4 loads it with `lh`),
 * and a level with more than 256 nodes would fold two rooms onto one otherwise.
 * The end bursts still take a byte, which is the original's own narrowing.
 */
bool q2_fx_laser(q2_fx_world *w, q2_rng *rng, u32 kind,
                 const s32 from[3], const s32 to[3],
                 s16 area, u32 ends, q2_fx_laser_result *out);

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Append every live group and every queued beam to `ot`.
 *
 * The table is NOT cleared: effects share the frame's table with the world and
 * the models, which is the whole point of an ordering table and the reason a
 * spark can sort behind a crate.
 *
 * `viewport` selects which view_mask bit is honoured. Beams are drawn once and
 * the pool is left alone; call q2_fx_beams_reset after the last viewport, which
 * is where 0x80064F10 does it.
 *
 * Returns the number of primitives emitted.
 */
u32 q2_fx_build_ot(q2_fx_world *w,
                   const q2_camera *cam,
                   u32 viewport,
                   psx_ot *ot,
                   gte_state *gte);

/*
 * The minimum on-screen size of a quad. 0x80030850 clamps the perspective
 * divide up to two pixels, so a distant spark never disappears — it becomes a
 * 2x2 dot and stays one. Reproducing the clamp is what stops long-range
 * gunfire from looking like it produced no effect at all.
 */
#define Q2_FX_QUAD_MIN_PIXELS 2

/*
 * The near cutoff. A group whose projected origin lands closer than this is
 * dropped whole (0x80030708). It is in GTE depth units, not world units.
 */
#define Q2_FX_NEAR_DEPTH 128

#endif /* Q2PSX_EFFECT_H */
