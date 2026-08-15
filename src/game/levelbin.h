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

#include "population.h"
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


#endif /* Q2PSX_LEVELBIN_H */
