/*
 * reloc.h — relocating the CreAIBin and LevelBin modules.
 *
 * CreAIBin holds a map's creature AI and LevelBin its level script, both as
 * position-independent MIPS R3000 modules with a fixup stream alongside
 * (CreAIRel, LevelRel). Until they are relocated they cannot be read at all,
 * which is why this was the gate standing in front of monsters.
 *
 * ---------------------------------------------------------------------------
 * The fixup stream
 * ---------------------------------------------------------------------------
 * An array of u32, terminated by 0xFFFFFFFF. Each entry packs a byte offset and
 * a relocation type:
 *
 *     offset = entry & ~3
 *     type   = entry &  3
 *
 * That is why an earlier survey found "only 31% of entries are 4-aligned" and
 * concluded the encoding was not offset-based. The low bits were never
 * alignment — they are the type tag, and the residue histogram decomposes
 * exactly into the four type counts.
 *
 *     0  WORD32    *t += base
 *     1  HI16      consumes ONE EXTRA raw word as an addend, then
 *                  *t = (*t & 0xFFFF0000) | (((addend + base + 0x8000) >> 16) & 0xFFFF)
 *     2  LO16      *t = (*t & 0xFFFF0000) | ((*t + base) & 0xFFFF)
 *     3  TARGET26  *t = (*t & 0xFC000000)
 *                     | ((((( *t & 0x03FFFFFF) << 2) + base) >> 2) & 0x03FFFFFF)
 *
 * THE HI16 ADDEND WORD IS THE TRAP. It is a raw value, not a tagged entry, and
 * a decoder that does not consume it stays in step almost all the time: a flat
 * scan scores 99.78% against 100% for the correct parse, because only the 216
 * addend words whose low bits happen to be 0b10 get misread as LO16 entries.
 * That is close enough to look like success and wrong enough to corrupt a
 * module. The +0x8000 is the usual MIPS HI16 bias, compensating for LO16 being
 * sign-extended.
 *
 * HI16 needs a separate addend because the modules carry no high half at all —
 * every one of the 28,191 HI16 sites is a `lui` whose immediate is zero. LO16
 * keeps its addend in the instruction, so it needs no extra word.
 *
 * ---------------------------------------------------------------------------
 * Bases
 * ---------------------------------------------------------------------------
 * CreAI modules have a 16-byte preamble that is NOT part of the relocated
 * image: both the code base and the fixup stream start at module + 16.
 * LevelBin/LevelRel have no preamble and start at + 0.
 *
 * ---------------------------------------------------------------------------
 * The module ABI, for what comes after
 * ---------------------------------------------------------------------------
 * A CreAI module's header sits at module+16, i.e. fixup offset 0:
 *
 *     +0x00, +0x08, +0x0C   exported pointers, relocated, present in all 166
 *     +0x04                 a fourth, present in 104 and NULL in 62
 *     +0x10, +0x12          zero on disc; the loader writes 304 and 1
 *     +0x14 .. +0x12C       71 imported engine function pointers, written by
 *                           the loader — no fixup ever targets this range
 *     +0x130                code begins
 *
 * The engine registers each module by class: it stores the module header into a
 * 38-entry table indexed by the instance's class id. That table's size matches
 * Population's observed class_id range of 0..37 exactly, which is the link from
 * a spawn record to the AI that runs it.
 */
#ifndef Q2PSX_RELOC_H
#define Q2PSX_RELOC_H

#include "level.h"
#include "q2psx.h"

#define Q2_RELOC_TERMINATOR 0xFFFFFFFFu

/* CreAI modules skip a 16-byte preamble; Level modules do not. */
#define Q2_RELOC_CREAI_PREAMBLE 16
#define Q2_RELOC_LEVEL_PREAMBLE 0

/* Header layout inside a relocated CreAI module. */
#define Q2_AI_HDR_EXPORT0     0x00
#define Q2_AI_HDR_EXPORT1     0x04
#define Q2_AI_HDR_EXPORT2     0x08
#define Q2_AI_HDR_EXPORT3     0x0C
#define Q2_AI_HDR_SIZE_FIELD  0x10
#define Q2_AI_HDR_VERSION     0x12
#define Q2_AI_HDR_IMPORTS     0x14
#define Q2_AI_HDR_IMPORT_COUNT  71
#define Q2_AI_HDR_CODE_START  0x130

/* The engine's class table has this many slots, and Population class ids run
 * 0..37 — the same range. */
#define Q2_AI_CLASS_COUNT 38

typedef enum q2_reloc_type {
    Q2_RELOC_WORD32   = 0,
    Q2_RELOC_HI16     = 1,
    Q2_RELOC_LO16     = 2,
    Q2_RELOC_TARGET26 = 3
} q2_reloc_type;

typedef struct q2_reloc_stats {
    u32 fixups;
    u32 by_type[4];
    u32 addend_words;
    u32 out_of_range;     /* offsets past the end of the image */
} q2_reloc_stats;

/*
 * Apply a fixup stream to a writable copy of a module.
 *
 * `base` is the value a module-relative address is biased by; pass the address
 * the image will live at. `image` and `stream` must already point past their
 * preambles.
 *
 * Fails on a malformed stream — an unterminated one, or an offset outside the
 * image — rather than relocating partially, because a half-relocated module
 * disassembles into plausible nonsense.
 */
q2_result q2_reloc_apply(u8 *image, size_t image_size,
                         const u8 *stream, size_t stream_size,
                         u32 base, q2_reloc_stats *stats);

/*
 * Count and classify a stream without modifying anything. Useful for auditing a
 * whole disc, and it performs the same structural checks.
 */
q2_result q2_reloc_scan(const u8 *stream, size_t stream_size,
                        size_t image_size, q2_reloc_stats *stats);

/* ------------------------------------------------------------------------- */
/* Module access                                                              */
/* ------------------------------------------------------------------------- */
typedef struct q2_ai_module {
    q2_buf image;        /* owned: a relocated, writable copy */
    u32    base;
    bool   empty;        /* a 4-byte all-zero chunk: this map has no creatures */
} q2_ai_module;

/*
 * Load and relocate a COMMON.DAT's CreAIBin using its CreAIRel.
 *
 * An empty pair (both exactly 4 zero bytes) is not an error: 29 of 98 zone
 * files legitimately have no creatures. `empty` is set and the image is NULL.
 */
q2_result q2_ai_module_load(q2_ai_module *out, const q2_common_file *common,
                            u32 base);
void      q2_ai_module_free(q2_ai_module *m);

/* An exported pointer from the relocated header, or 0 when absent. */
u32 q2_ai_module_export(const q2_ai_module *m, u32 slot);

/* ------------------------------------------------------------------------- */
/* The LEVEL module                                                           */
/* ------------------------------------------------------------------------- */
/*
 * LevelBin/LevelRel are the same format with a different header, and they are
 * where a map's own game rules live. The installer at 0x8007A330 is what says
 * so, and it is worth reading because it names every field:
 *
 *     0x80018000(module, fixups)      apply the relocations
 *     0x800B2F58 = module             the level module pointer the rest of the
 *                                     engine reaches the map's rules through
 *     0x800B2F6C = *(module + 0x00)   export 0, called once per level load at
 *                                     0x80070F5C
 *     *(module + 0x08) = 0x800B2FE4   an IMPORT the loader writes: the settings
 *                                     block, so the module can read the menu
 *
 * So the header is exports first, then a writable import area, exactly as the
 * CreAI header is — but shorter, and with no preamble to skip.
 *
 * Export 1 (+0x04) is the one this project came here for: the player-killed-
 * player hook at 0x8003978C calls `(*(0x800B2F58))->[4](killer, victim)` with
 * two player indices, both bounds-checked against 4 first. Deathmatch scoring
 * is therefore NOT in the executable; every arena carries its own copy.
 */
#define Q2_LEVEL_HDR_INIT     0x00   /* export 0 — level init                 */
#define Q2_LEVEL_HDR_FRAG     0x04   /* export 1 — (killer, victim)           */
#define Q2_LEVEL_HDR_SETTINGS 0x08   /* import  — written with 0x800B2FE4     */

/*
 * Load and relocate a COMMON.DAT's LevelBin using its LevelRel.
 *
 * Two maps ship an empty pair; `empty` is set for those exactly as it is for a
 * creature-less CreAIBin, and the image is NULL.
 */
q2_result q2_level_module_load(q2_ai_module *out, const q2_common_file *common,
                               u32 base);

#endif /* Q2PSX_RELOC_H */
