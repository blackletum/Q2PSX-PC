/*
 * leveltext.h — the `Strings` chunk: a map's text, addressed by name.
 *
 * ---------------------------------------------------------------------------
 * What it is
 * ---------------------------------------------------------------------------
 * Every COMMON.DAT carries a `Strings` chunk (level.h, index 9) that had been
 * catalogued but never decoded. It is a small name-to-text dictionary, and it
 * is what the lookup at `0x800701B4` reads — the call the briefing screen makes
 * with the literal key `MapTitle`, packed four characters to a register.
 *
 * BASE0's, in full, is the whole format in one example:
 *
 *     MapTitle       "Strogg Outpost"
 *     FindLift       "Find the elevator"
 *     FindWeapon     "Look for a better weapon"
 *     PressJump      "Press the jump button to get up here"
 *     FoundASecret   "You have found a secret."
 *     Unit1Miss1     "Establish Communication Link to Command Ship."
 *     Unit1Curr0     "Find entrance to Strogg Base."
 *     Unit1Curr1     "Locate Base Installation Elevator."
 *     Default        "Base0*"
 *
 * Three of those keys are the briefing screen's three fields (briefing.h), and
 * finding them here is what turns that screen from a layout into a working
 * one: `MapTitle` is `Location:`, `UnitNCurrM` is `Current Orders:` and
 * `UnitNMiss1` is `Mission Objective:`. The executable builds the last two keys
 * with `sprintf` — the format `"Unit%dMiss1"` sits at `0x800AB9D0`, and three
 * already-built keys are still lying in memory at `0x800ABA60`.
 *
 * The rest are the HUD's prompts. `FindLift`, `FindWeapon` and `PressJump` are
 * hint lines; `FoundASecret` is the message the secret counter raises.
 *
 * ---------------------------------------------------------------------------
 * The format
 * ---------------------------------------------------------------------------
 *     struct { char name[12]; u32 offset; }
 *
 * repeated until an all-zero record, then the text — NUL-terminated, packed,
 * offsets relative to the start of the chunk. `name` is a fixed 12 bytes and
 * is NOT guaranteed to be terminated: `FoundASecret` fills it exactly. Reading
 * it as a C string is the mistake this decoder exists to not make.
 */
#ifndef Q2PSX_FORMATS_LEVELTEXT_H
#define Q2PSX_FORMATS_LEVELTEXT_H

#include "level.h"
#include "q2psx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define Q2_LEVELTEXT_NAME_LEN   12
#define Q2_LEVELTEXT_MAX        64

typedef struct q2_leveltext_entry {
    char        name[Q2_LEVELTEXT_NAME_LEN + 1];  /* terminated for us  */
    u32         offset;                           /* into the chunk     */
    const char *text;                             /* into the chunk     */
} q2_leveltext_entry;

typedef struct q2_leveltext {
    q2_leveltext_entry entry[Q2_LEVELTEXT_MAX];
    u32                count;
    const u8          *base;      /* the chunk, borrowed not owned      */
    u32                size;
} q2_leveltext;

/* Decode the chunk in place. `data` must outlive `out`. */
q2_result q2_leveltext_parse(q2_leveltext *out, const u8 *data, u32 size);

/* The same, straight from an open COMMON.DAT. */
q2_result q2_leveltext_open(q2_leveltext *out, const q2_common_file *f);

/* NULL when the key is absent — which is normal; not every map has every key. */
const char *q2_leveltext_find(const q2_leveltext *t, const char *name);

/*
 * The three briefing keys, built the way the executable builds them.
 * `out` must hold Q2_LEVELTEXT_NAME_LEN + 1.
 */
void q2_leveltext_key_objective(char *out, int unit);           /* Unit%dMiss1 */
void q2_leveltext_key_orders(char *out, int unit, int step);    /* Unit%dCurr%X */

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_FORMATS_LEVELTEXT_H */
