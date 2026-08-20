/*
 * cmd_explosive.h — opcode 0x08, this engine's `func_explosive`, censused
 * against every Events chunk on the disc and exercised through the module that
 * reconstructs it.
 *
 * Two questions, and the command answers them separately because they are
 * different: does the 28-byte layout in explosive.h hold across all 224 items,
 * and does destroying one produce the geometry swap the constructor set up for?
 */
#ifndef Q2PSX_CMD_EXPLOSIVE_H
#define Q2PSX_CMD_EXPLOSIVE_H

#include "disc.h"

/* `map` may be NULL, in which case the census covers the whole disc. */
int cmd_explosive(const disc *d, const char *map);

#endif /* Q2PSX_CMD_EXPLOSIVE_H */
