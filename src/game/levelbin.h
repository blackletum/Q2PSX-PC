/*
 * levelbin.h — reading a map's own `LevelBin` module without running it.
 *
 * ---------------------------------------------------------------------------
 * The problem this solves
 * ---------------------------------------------------------------------------
 * `population.h` states it plainly: a Population group is not spawned because
 * it exists, it is spawned because a script SELECTED it. `0x80056C60` takes a
 * twelve-byte name and sets bit 0 of that group's flags; the spawn pass runs
 * only the selected ones. **The flags word is zero on disc for all 222 groups
 * of all 49 maps**, so at level load nothing is selected — and which groups a
 * level starts with therefore lives in its `LevelBin`, which is MIPS code.
 *
 * This port has no MIPS interpreter and does not want one. But it does not need
 * to RUN the module to know what it asks for, and there is precedent: the glint
 * is turned on by `q2_fx_glint_scan` reading the same kind of module for the
 * instruction that raises its flag.
 *
 * ---------------------------------------------------------------------------
 * What the module does, from BASE1's
 * ---------------------------------------------------------------------------
 * `q2psx-inspect levdisasm BASE1` relocates it to 0x80100000. Its init (export
 * 0) opens with a VERSION CHECK on the engine's import block — `lw v1, 0(v0)`
 * against 1268, the block's own first word (FORMATS §15.5) — and the mismatch
 * arm builds a name and prints *"Please get a programmer to recompile level
 * %s"*. The real work is past that branch:
 *
 *     80100344  addiu a0, a0, 124      ; module+0x7C = "Base1Batches"
 *     80100348  lw    v0, 40(v0)       ; block +40
 *     80100350  jalr  v0               ; select that group
 *
 *     8010039C  addiu t0, v0, 136      ; module+0x88 = "MapTitle"
 *     801003A4..80100430               ; twelve bytes loaded into a0/a1/a2
 *     80100424  lw    v0, 292(v0)      ; block +292
 *     8010042C  jalr  v0               ; a Strings lookup
 *
 * A twelve-byte name is passed BY VALUE in three registers, which is the
 * convention `0x80056C60` takes. And `+40` settles the block's numbering, which
 * §15.5 left ambiguous: it lists the selector as slot 9, and 4 + 9*4 = 40 — so
 * slot N sits at `+4 + 4*N`, past the size word, not at `4*N`.
 *
 * ---------------------------------------------------------------------------
 * Why this reads STRINGS rather than instructions
 * ---------------------------------------------------------------------------
 * The scan below looks for a map's own Population group NAMES inside its
 * `LevelBin`, rather than decoding the call. That is weaker in principle and
 * stronger in practice, for a reason worth stating: the operand is a twelve-byte
 * name loaded from the module's own data, and the ONLY thing a module does with
 * a group name is select the group. Decoding the call site instead would mean
 * tracking a `lui`/`addiu` pair through whatever order the scheduler left them
 * in — which `q2_fx_glint_scan` already had to do for one immediate and found
 * fragile enough to warn about.
 *
 * It is also checkable, which the instruction decode would not be: every name
 * it finds must be a group the map actually ships, and the count of maps with
 * at least one is a number that can be compared against the 222 groups the disc
 * carries. `q2psx-inspect zonescript` prints both.
 */
#ifndef Q2PSX_LEVELBIN_H
#define Q2PSX_LEVELBIN_H

#include "../formats/collision.h"
#include "effect.h"
#include "events.h"
#include "population.h"
#include "userfuncs.h"
#include "q2psx.h"

/* The group selector, `0x80056C60`. See the sweep above. */
#define Q2_LEVELBIN_SLOT_SELECT 36

/*
 * Does `module` name Population group `g`?
 *
 * Matched as a whole twelve-byte field, NUL-padded as the disc stores it, so a
 * group called `Zone1` cannot match inside a `Zone1Key`. Names are compared
 * case-sensitively: the module and the Population chunk are produced by the
 * same tool from the same source and agree exactly on the 89 CREBATCH calls
 * already checked (#79).
 */
bool q2_levelbin_names_group(const u8 *module, u32 size, const q2_pop_group *g);

/*
 * ---------------------------------------------------------------------------
 * The string scan establishes the VOCABULARY, not the timing
 * ---------------------------------------------------------------------------
 * `q2_levelbin_names_group` finds every group a module mentions, and that turns
 * out to be a superset of the ones it selects at load: JAIL3's `Bridge` is
 * named by its LevelBin AND by a `CREBATCH`, so the mention is the name being
 * held for a later call rather than a selection. Useful — it confirms #79's
 * zone rule on 71 of the 75 groups the disc's modules name — and not enough.
 *
 * Separating the two needs the CALL SITE, which is what the scan below reads.
 */

/*
 * Groups the module SELECTS, by decoding the calls rather than the strings.
 *
 * The shape, from BASE1's init:
 *
 *     801000A0  addiu a3, v0, 12     ; a3 = module + 0xC = "Zone0"
 *     801000A4  lbu   v1, 1(a3)      ; ...and twenty more, into a0/a1/a2
 *     ...       lw    vX, 36(rY)     ; the engine block's slot 9
 *     ...       jalr  vX
 *
 * `+36` is the selector, and it was SWEPT rather than assumed. For every offset
 * a module calls with a name-shaped argument, count how many of those names are
 * groups the map actually ships:
 *
 *     +36    71 / 83      <- the selector
 *     +136    4 / 5
 *     +28     1 / 20
 *     +32     1 / 20
 *     everything else  0 / n
 *
 * One offset accounts for essentially every hit and the rest for none, which is
 * what makes this a measurement rather than a guess. 36 = 4 * 9, so §15.5's
 * slot numbering indexes the block directly — slot 0 IS the size word — and the
 * "4 + 4*N" reading an earlier pass here used was wrong.
 *
 * `out` receives up to `max` module offsets, each the start of a twelve-byte
 * name. Returns how many were written. A caller resolves each against the map's
 * Population, which is also the check: an offset that does not name a real
 * group means the decode is wrong, and there is no reason for it to be right by
 * accident.
 */
u32 q2_levelbin_selected(const u8 *module, u32 size, u32 *out, u32 max);

/* The same, for an arbitrary slot offset — which is how the selector's slot was
 * FOUND rather than assumed: sweep every offset a module calls with a name and
 * see which one's names are all real groups. */
u32 q2_levelbin_selected_slot(const u8 *module, u32 size, s32 slot_off,
                              u32 *out, u32 max);



/* ------------------------------------------------------------------------- */
/* LASERBEAM — the beams a level ships                                        */
/* ------------------------------------------------------------------------- */
/*
 * 72 `LASERBEAM` items on the disc and not one trigger volume reaches any of
 * them. That looked like GLASS's problem (#66) — a primitive with no caller —
 * and it is not. Nothing needs to reach a LASERBEAM. Which beams burn is a
 * property of WHICH ZONE THE PLAYER IS STANDING IN, and it is carried in the
 * bottom bit of a coordinate.
 *
 * THE CONSTRUCTOR, 0x8002E718, runs at every zone load:
 *
 *     8002E744  lw   v0, 372(gp)    ; the chunk being walked (COMMON's)
 *     8002E748  lw   v1, 376(gp)    ; the chunk the engine reads from (the ZONE's)
 *     8002E74C  subu v0, s0, v0     ; this item's offset...
 *     8002E750  addu v0, v0, v1     ; ...rebased into the zone's copy
 *     8002E754  lw   v1, 4(v0)      ; origin_a's first word, TAKEN FROM THERE
 *     8002E760  lw   v0, 20(v0)     ; origin_b's, likewise
 *     8002E758  addiu a2, zero, -1  ; hint  = -1
 *     8002E764  addiu a3, zero, 1   ; brute = true
 *     8002E768  jal  0x80044F54     ; q2_coll_find_node(PrimaryColl, &origin_a)
 *     8002E780  sh   v0, 18(s0)     ; the NODE INDEX into item +18
 *     8002E778  slti v1, v1, 6      ; and if +34 is NOT below six...
 *     8002E784  sh   zero, 34(s0)   ; ...zero it
 *
 * THE EXEC, 0x8002E694, registers the beam if that word says to:
 *
 *     8002E6A8  lh   v0, 16924(gp)  ; the registration-pass gate, below
 *     8002E6B8  lw   v0, 4(a2)      ; origin_a word 0...
 *     8002E6C0  andi v0, v0, 1      ; ...bit 0 is the ENABLE FLAG
 *     8002E6D4  slti v0, v1, 32     ; the list holds 32
 *     8002E6F8  lhu  a1, 16936(gp)  ; the record now executing
 *     8002E700  sh   a1, 0(v1)      ; -> 0x800C7014[n].raiser
 *     8002E704  sh   v0, 2(v1)      ; -> 0x800C7014[n].item
 *
 * and the two halves together are the whole mechanism. The X the exec tests is
 * not COMMON's X: it is the ZONE's, which the constructor has just copied over
 * it. JAIL2's corridor grid is X=7352 in COMMON and in zone 0, and X=7353 in
 * zones 1 and 2 — the same coordinate with the bottom bit set, one unit wide of
 * nothing and invisible in a world this size. So the level author lights a grid
 * in the rooms it guards by nudging one number, and the beam is dark everywhere
 * else without a trigger, a timer or a script.
 *
 * Across the disc that reads: 71 of the 72 beams are lit in at least one zone,
 * NONE is lit in every zone, and the single beam lit nowhere is JAIL2's
 * (0,0,0)->(0,0,0), which is a dead entry. Reading COMMON's copy instead calls
 * 41 of the 72 dark and is simply the wrong buffer — which is the same mistake,
 * in the same field, that #56 was about.
 *
 * THE WALK, 0x8002EE38, runs every frame over the registered list. Nothing
 * clears it — `gp+0x420C` is only ever incremented — so a beam registered at
 * zone load burns until the zone changes:
 *
 *     8002EE88  lbu  v0, 3(base + entry.raiser)
 *     8002EE90  andi v0, v0, 128    ; the raiser record's dead bit...
 *     8002EE94  bne  -> skip        ; ...is the one off switch there is
 *     8002EEB0  s0 = item + 4       ; from
 *     8002EEC0  s1 = item + 20      ; to
 *     8002EEB4  s2 = lh 18(item)    ; area
 *     8002EEB8  s3 = lh 34(item)    ; kind
 *     8002EEBC  jal  0x80089E18     ; the fifth argument, zeroed on the stack
 *     8002EED0  jal  0x80048DC8     ; q2_fx_laser(from, to, area, kind, 0)
 *
 * and the zeroed fifth argument is why a level's beams do not spit particles:
 * `ends = 0`, so neither end burst fires. Only the tube is drawn.
 *
 * `gp+0x421C` decides register-or-act, and LASERWALL is what proves it: the
 * same flag, read at 0x8002E228, sends it to the same kind of list when set and
 * straight to T_Damage (0x80057D54, mod 11) when clear. A port has no reason to
 * model the flag. It can raise the beams at zone load, which is when the
 * console's registration pass raises them.
 */
typedef struct q2_laserbeam {
    s32 from[3];        /* item +4;  bit 0 of [0] is the enable flag  */
    s32 to[3];          /* item +20                                   */
    s16 area;           /* item +18, written by the constructor       */
    s16 kind;           /* item +34, clamped below six there          */
    u32 raiser;         /* the record's offset; its dead bit gates us */
} q2_laserbeam;

/* `slti v0, v1, 32` at 0x8002E6D4. */
#define Q2_LASERBEAM_MAX 32

typedef struct q2_laserbeam_set {
    q2_laserbeam beam[Q2_LASERBEAM_MAX];
    u32          count;
    u32          declined;   /* declared, but dark in THIS zone */
} q2_laserbeam_set;

/*
 * Raise every LASERBEAM this zone lights, as the registration pass does.
 *
 * `ops` is not a refinement here but the mechanism: `base_b` must be the
 * RESIDENT ZONE's Events chunk, because that is the buffer holding the enable
 * bit, and passing COMMON's for both leaves most of a level's lasers dark.
 *
 * `coll` should be PrimaryColl and may be NULL, in which case the area is left
 * at the disc's value rather than resolved — the beam still draws, it is just
 * not sorted into a room.
 *
 * Returns the number raised.
 */
u32 q2_laserbeams_build(q2_laserbeam_set *out, const q2_events *events,
                        const q2_userfuncs *uf, const q2_uf_operands *ops,
                        const q2_collision *coll);

/*
 * Queue every raised beam into this frame's pool. The transient pool empties
 * every frame (effect.h), which is why the console's walk re-submits its whole
 * list every frame too. Returns how many were queued.
 */
u32 q2_laserbeams_draw(const q2_laserbeam_set *set, q2_fx_world *w,
                       q2_rng *rng);

#endif /* Q2PSX_LEVELBIN_H */
