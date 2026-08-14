#ifndef Q2PSX_CMD_SURFACES_H
#define Q2PSX_CMD_SURFACES_H

#include "disc.h"

/* Census and check: Scene.flags08, the blend tables, MapMod.clut's low byte,
 * and every zone's SortData stream. Returns non-zero on a failed check. */
int cmd_surfaces(disc *d);

#endif /* Q2PSX_CMD_SURFACES_H */
