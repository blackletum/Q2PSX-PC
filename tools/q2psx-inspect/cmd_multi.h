#ifndef Q2PSX_INSPECT_CMD_MULTI_H
#define Q2PSX_INSPECT_CMD_MULTI_H

#include "disc.h"

/*
 * Check the multiplayer reconstruction against the disc. `map` is optional; pass
 * an arena name to also list its MultiSpawn points and run the spawn selector
 * over them.
 */
int cmd_multi(const disc *d, const char *map);

#endif /* Q2PSX_INSPECT_CMD_MULTI_H */
