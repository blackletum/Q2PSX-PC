/*
 * fxtables.h — the effect system's data tables, read out of the boot
 * executable.
 *
 * ---------------------------------------------------------------------------
 * What the effect system is
 * ---------------------------------------------------------------------------
 * Hammerhead's presentation layer is four separate machines that share nothing
 * but a colour convention. Keeping them apart matters, because "particles" in
 * this engine does not mean what it means in PC Quake II:
 *
 *   PARTICLE GROUPS   0x80030284.  A pool of 288-byte records, each holding up
 *                     to fifteen screen-aligned quads that share one origin,
 *                     one velocity, one acceleration and one pair of colour
 *                     ramps. Blood, sparks, explosion fire and gib puffs are
 *                     all this, differing only in ramp, size and lifetime.
 *
 *   TRANSIENT BEAMS   0x80064E64.  A 32-slot pool that is refilled from scratch
 *                     every frame and drawn once (0x80064F10) before being
 *                     reset. A beam is a six-sided tube between two points with
 *                     two end caps — the laser trap, and anything else that
 *                     wants a solid line in the world.
 *
 *   DEBRIS ENTITIES   0x80064558 / 0x80064398.  Real entities with physics and
 *                     a think function, spawned in a burst out of a Scene
 *                     node's bounding box. Breaking glass is the visible one.
 *
 *   ENTITY GLINTS     0x80064C00 / 0x80064780.  A faceted body attached to an
 *                     entity that carries flag 0x04000000, with a bright band
 *                     sweeping its surface. This is the only one whose geometry
 *                     is not in the executable's rodata: it is the `GlintMod`
 *                     level chunk, installed by 0x800651BC. NOT a weapon
 *                     trail — see effect.h for why the obvious reading is
 *                     wrong and what it actually looks like.
 *
 * This module owns the *data* half of the first two. The behaviour half —
 * which effect fires on which event, how long it lives, how fast it spreads —
 * is immediate operands inside the spawn sites and lives in `src/game/effect.c`
 * with the address of each instruction it came from. This module does not
 * pretend to read that.
 *
 * ---------------------------------------------------------------------------
 * Colour ramps: 19 of them, and the header word is not a count
 * ---------------------------------------------------------------------------
 * 0x8009BA60 holds nineteen 132-byte records ending exactly at 0x8009C42C,
 * where a nineteen-entry pointer table begins. Each record is
 *
 *     u16 abr;  u16 pad;  u32 colour[32];
 *
 * and each `colour` entry is a whole GPU primitive header word in hardware
 * order — `{u8 r, u8 g, u8 b, u8 code}` little-endian — so the code byte comes
 * out of the table with the colour rather than being applied by the renderer.
 * Every ramp on this disc carries code 0x2E, which is POLY_FT4 with ABE set.
 *
 * The first u16 reads as 32 on sixteen of the nineteen records, which is
 * exactly the entry count, and that coincidence is a trap. Records 3, 4 and 13
 * read 64 while still being followed by the next record 132 bytes later, so it
 * cannot be a count. It is the semi-transparency field of a texture-page word:
 * the renderer at 0x80030828 computes
 *
 *     tpage = [0x800DDD5A] | ramp->abr
 *
 * and 0x0020 / 0x0040 are ABR mode 1 (B+F, additive) and mode 2 (B-F,
 * subtractive) in bits 5-6. Sixteen ramps are additive and three subtract.
 *
 * A reader that trusted the "count" would still work, because it would read 32
 * entries out of a record that has 32 entries. It would also silently turn
 * every subtractive effect additive, and one of those three is not decoration:
 *
 *   ramp 3 is CYAN, and it is the second ramp the blood spray uses. Cyan
 *   subtracted from a lit wall leaves red. That is how blood reads as dark on
 *   a bright surface and still shows up on a dark one, and read as additive it
 *   would come out as a cyan flash.
 *
 * The other two are ramp 4, a dark red that dims what it lands on, and ramp 13,
 * which is ramp 12's magenta with red and green transposed — a green ramp that
 * subtracts where ramp 11's identical green adds.
 *
 * THE RAMP IS INDEXED BY AGE, NOT BY LIFE. 0x800307A0 computes
 * `colour[32 - life]`, and life counts down from the value the spawner was
 * given. A group spawned with life 15 therefore uses entries 17..31 — the dim
 * tail of the ramp — and one spawned with life 32 uses the whole thing. That is
 * why the bright head of every ramp is unreachable for most effects and why
 * raising a lifetime changes an effect's colour, not just its duration.
 *
 * ---------------------------------------------------------------------------
 * The pointer table is a permutation, so ids are not offsets
 * ---------------------------------------------------------------------------
 * 0x8009C42C is nineteen pointers into the array above, and they are NOT in
 * address order. Resolved to record indices the table reads
 *
 *     0 6 1 2 3 4 5 7 8 9 10 11 12 13 14 16 17 18 15
 *
 * — id 1 is record 6 and id 18 is record 15, with everything between them
 * shifted by one. Call sites reach ramps both ways — the script
 * effect at 0x80028D9C indexes the table, everything else materialises the
 * record address directly — so both indices exist in the original and this
 * module exposes both. `q2_fx_ramp_by_id` is the table; `q2_fx_ramp_at` is the
 * array. Confusing them shifts every scripted effect's colour by one.
 *
 * ---------------------------------------------------------------------------
 * Beam styles: five in the table, three reachable
 * ---------------------------------------------------------------------------
 * 0x8009D734 holds five 200-byte style records, ending exactly where the
 * blaster bolt's hull begins at 0x8009DB1C (weapontables.h). Each is ten
 * 20-byte faces — `{u8 index[4]; u32 colour[4]}` — split six/two/two: the six
 * sides of a hexagonal tube spanning vertices 0..11, then two quads that close
 * the near hexagon and two that close the far one.
 *
 *     tube      (0,1,6,7) (1,2,7,8) (2,3,8,9) (3,4,9,10) (4,5,10,11) (5,0,11,6)
 *     cap_near  (1,0,2,3) (4,3,5,0)
 *     cap_far   (0,1,3,2) (3,4,0,5)
 *
 * The twelve vertices are not stored: the renderer builds them at draw time
 * from the beam's own direction (0x800634E4 walks the 4096-step circle in
 * thirds, so the hexagon is three axes and their negations) scaled by the
 * radius the caller passed. Only the colours are data.
 *
 * Styles 0..2 are red, blue and yellow at code 0x3A — POLY_G4 with ABE. Style 3
 * is green at 0x3A. Style 4 is a dimmer red at code 0x38, the only OPAQUE style
 * in the table. Nothing in the executable reaches styles 3 or 4: the laser
 * dispatcher at 0x80048DC8 has six cases and they use styles 0, 1 and 2 twice
 * each, once at radius 16 and once at radius 64. They are kept here because a
 * port that dropped them would be unable to reproduce a saved game or a mod
 * that reaches them, and because "two styles are unreferenced" is a finding
 * worth being able to re-derive.
 */
#ifndef Q2PSX_FXTABLES_H
#define Q2PSX_FXTABLES_H

#include "exe.h"
#include "ident.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Where everything is                                                        */
/* ------------------------------------------------------------------------- */
#define Q2_FXT_ADDR_RAMPS         0x8009BA60u  /* 19 x 132                    */
#define Q2_FXT_ADDR_RAMP_INDEX    0x8009C42Cu  /* 19 x u32, a permutation     */
#define Q2_FXT_ADDR_BEAM_STYLES   0x8009D734u  /* 5 x 200                     */
#define Q2_FXT_ADDR_LASER_JUMP    0x800ACCD4u  /* 6 x u32, the kind dispatch  */

#define Q2_FX_RAMP_COUNT      19
#define Q2_FX_RAMP_COLOURS    32
#define Q2_FX_RAMP_STRIDE     132u

#define Q2_FX_BEAM_STYLE_COUNT 5
#define Q2_FX_BEAM_STYLE_STRIDE 200u
#define Q2_FX_BEAM_TUBE_FACES  6
#define Q2_FX_BEAM_CAP_FACES   2
#define Q2_FX_BEAM_VERTS      12   /* two hexagons                            */

#define Q2_FX_LASER_KIND_COUNT 6

/* ------------------------------------------------------------------------- */
/* Semi-transparency modes, as the tpage word encodes them (bits 5-6)         */
/* ------------------------------------------------------------------------- */
#define Q2_FX_ABR_HALF   0x0000u  /* B/2 + F/2                                */
#define Q2_FX_ABR_ADD    0x0020u  /* B + F      — eighteen of the ramps       */
#define Q2_FX_ABR_SUB    0x0040u  /* B - F      — one ramp, the darkening one */
#define Q2_FX_ABR_QUART  0x0060u  /* B + F/4                                  */
#define Q2_FX_ABR_MASK   0x0060u

/* ------------------------------------------------------------------------- */
/* One colour ramp                                                            */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_ramp {
    u16 abr;                          /* +0x00 OR-ed into the draw-mode word  */
    u16 reserved;                     /* +0x02 zero on every record           */
    u32 colour[Q2_FX_RAMP_COLOURS];   /* +0x04 {r,g,b,code} in hardware order */
} q2_fx_ramp;

/* Split one ramp entry the way the hardware reads it. */
Q2PSX_INLINE u8 q2_fx_colour_r   (u32 c) { return (u8)( c        & 0xFFu); }
Q2PSX_INLINE u8 q2_fx_colour_g   (u32 c) { return (u8)((c >>  8) & 0xFFu); }
Q2PSX_INLINE u8 q2_fx_colour_b   (u32 c) { return (u8)((c >> 16) & 0xFFu); }
Q2PSX_INLINE u8 q2_fx_colour_code(u32 c) { return (u8)((c >> 24) & 0xFFu); }

/*
 * The entry a group of the given age uses. `life` is the group's remaining
 * lifetime, counting down; the original computes 32 - life and does not clamp,
 * so a lifetime above 32 would index before the ramp. Nothing on this disc
 * spawns one — the largest is 25 — but the clamp is here because a save game
 * or a mod could, and reading behind the table is not behaviour worth
 * reproducing.
 */
Q2PSX_INLINE u32 q2_fx_ramp_index_for_life(int life)
{
    int i = Q2_FX_RAMP_COLOURS - life;
    if (i < 0)
        i = 0;
    if (i >= Q2_FX_RAMP_COLOURS)
        i = Q2_FX_RAMP_COLOURS - 1;
    return (u32)i;
}

/* ------------------------------------------------------------------------- */
/* One beam style                                                             */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_face {
    u8  v[4];        /* indices into the twelve generated hull vertices */
    u32 colour[4];   /* one {r,g,b,code} word per corner — gouraud      */
} q2_fx_face;

typedef struct q2_fx_beam_style {
    q2_fx_face tube[Q2_FX_BEAM_TUBE_FACES];
    q2_fx_face cap_near[Q2_FX_BEAM_CAP_FACES];
    q2_fx_face cap_far[Q2_FX_BEAM_CAP_FACES];
} q2_fx_beam_style;

/* ------------------------------------------------------------------------- */
/* One laser kind                                                             */
/*                                                                            */
/* Transcribed from the six arms of the dispatch at 0x80048DC8, not read from  */
/* a table: every field is an immediate operand. The addresses are the         */
/* instruction each number came from, so the transcription can be re-checked   */
/* against the disc without re-deriving the control flow.                      */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_laser_kind {
    s16 radius;      /* the hexagon's radius in world units      */
    u8  style;       /* index into the beam style table          */
    u8  ramp;        /* ARRAY index of the end-burst ramp        */
    s16 damage;      /* handed to T_Damage at 0x80048FB4         */
    s16 mod;         /* means of death, see combat.h             */
    u32 arm_addr;    /* where this arm starts, for the record    */
} q2_fx_laser_kind;

/* ------------------------------------------------------------------------- */
/* The loaded set                                                             */
/* ------------------------------------------------------------------------- */
typedef struct q2_fx_tables {
    q2_fx_ramp       ramp[Q2_FX_RAMP_COUNT];
    u32              ramp_addr[Q2_FX_RAMP_COUNT];   /* where each record sits */

    /* 0x8009C42C, resolved to ARRAY indices. `ramp_id_to_index[id]` is the
     * record `q2_fx_ramp_by_id` returns. */
    u8               ramp_id_to_index[Q2_FX_RAMP_COUNT];
    bool             ramp_index_is_permutation;

    q2_fx_beam_style beam[Q2_FX_BEAM_STYLE_COUNT];

    /* Which styles the dispatcher can actually reach. Derived, not read. */
    bool             beam_style_reachable[Q2_FX_BEAM_STYLE_COUNT];

    q2_fx_laser_kind laser[Q2_FX_LASER_KIND_COUNT];
    u32              laser_jump[Q2_FX_LASER_KIND_COUNT]; /* the raw table     */

    bool             loaded;
} q2_fx_tables;

/* ------------------------------------------------------------------------- */
/* Loading                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Read every table out of `exe`. Fails only when an address escapes the
 * segment, which for an uncatalogued build is the honest answer: the tables are
 * at fixed addresses for this release and a different release moves them.
 */
q2_result q2_fx_tables_load(q2_fx_tables *out, const q2_exe *exe);

/*
 * The same, from a disc: identifies the build, loads its boot executable and
 * reads the tables out of it.
 *
 * Refuses a build it does not have addresses for, rather than reading whatever
 * happens to be at 0x8009BA60 on a localised release — the tables move with the
 * executable and a wrong read here would produce plausible-looking gradients
 * that are somebody else's data. The caller gets Q2_ERR_UNSUPPORTED and can
 * carry on with no effects, which is what the client does.
 */
q2_result q2_fx_tables_load_disc(q2_fx_tables *out, const disc *d,
                                 const q2_build_id *id);

/* Ramp by ARRAY index (0..18 in address order), or NULL. */
const q2_fx_ramp *q2_fx_ramp_at(const q2_fx_tables *t, u32 index);

/* Ramp by the id the pointer table at 0x8009C42C uses, or NULL. */
const q2_fx_ramp *q2_fx_ramp_by_id(const q2_fx_tables *t, u32 id);

/* Beam style by index, or NULL. */
const q2_fx_beam_style *q2_fx_beam_style_at(const q2_fx_tables *t, u32 index);

/* Laser kind 0..5, or NULL. */
const q2_fx_laser_kind *q2_fx_laser_kind_at(const q2_fx_tables *t, u32 kind);

/* ------------------------------------------------------------------------- */
/* Checking                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * There is no hand-written copy of the ramp colours to diff against, and there
 * should not be: nineteen 32-entry gradients transcribed by hand would be a
 * second source of truth that could drift from the disc without anyone
 * noticing. What IS structural — and what a mis-decode would break — is
 * checked instead:
 *
 *   - every ramp's ABR field is one of the four hardware modes
 *   - every ramp entry carries a legal GPU polygon code
 *   - the pointer table at 0x8009C42C is a permutation of 0..18, i.e. every
 *     record is reachable exactly once (a stride or base error breaks this
 *     immediately, because the pointers stop landing on record boundaries)
 *   - the five beam styles use the documented index sets, so a wrong stride
 *     shows up as a hull that does not close
 *   - each beam face's four corners share one colour word, which is what makes
 *     the "gouraud" quads flat in practice
 *   - the laser dispatch table's six arms are inside the text segment and hit
 *     the three distinct bodies the transcription assumes
 *
 * Returns the number of complaints; zero means the decode holds up.
 */
typedef void (*q2_fx_report_fn)(void *user, const char *what,
                                s64 got, s64 want);
u32 q2_fx_tables_check(const q2_fx_tables *t, const q2_exe *exe,
                       q2_fx_report_fn report, void *user);

/*
 * Compare two loaded sets field by field, reporting each mismatch. Returns the
 * number of differences; zero means the two builds agree. Used to tell a
 * localised release apart from a moved table.
 */
u32 q2_fx_tables_compare(const q2_fx_tables *a, const q2_fx_tables *b,
                         q2_fx_report_fn report, void *user);

#endif /* Q2PSX_FXTABLES_H */
