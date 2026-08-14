#ifndef Q2PSX_CMD_ZONESCRIPT_H
#define Q2PSX_CMD_ZONESCRIPT_H

#include "disc.h"

/* Which Events chunk a trigger volume fires, and what a zone's script names.
 * `only_map` limits the run to one map and turns on the per-map listing. */
int cmd_zonescript(const disc *d, const char *only_map);

#endif /* Q2PSX_CMD_ZONESCRIPT_H */
