/*
 * cmd_pmove.h — the player movement frame, checked against the disc.
 *
 * Everything `0x8003A1C8` does, exercised rather than asserted: the nine control
 * styles and what each one binds, the jump arc tick by tick, the view height's
 * ease between its three targets, the fall-damage curve against the executable's
 * own thresholds, and a census of which maps actually author the environment
 * volumes that drive crouching, swimming and the no-jump zones.
 *
 * The last of those is the part that needs a real disc: the port can reproduce a
 * crouch, but whether the game contains one is a question only the data answers.
 */
#ifndef Q2PSX_CMD_PMOVE_H
#define Q2PSX_CMD_PMOVE_H

#include "disc.h"

/* `map` may be NULL, in which case the volume census covers the whole disc. */
int cmd_pmove(const disc *d, const char *map, int zone_index);

#endif /* Q2PSX_CMD_PMOVE_H */
