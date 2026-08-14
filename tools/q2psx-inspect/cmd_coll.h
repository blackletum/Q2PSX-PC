#ifndef Q2PSX_INSPECT_CMD_COLL_H
#define Q2PSX_INSPECT_CMD_COLL_H

#include "disc.h"

/*
 * Census both collision hulls of every zone on the disc and check the model
 * that was read out of the executable against the data it has to run on.
 *
 * Every check here is one the engine would divide by zero, index out of range
 * or silently mis-walk on if the reading were wrong — they are not statistics
 * about how plausible the format looks.
 */
int cmd_coll(disc *d, const char *map, int zone_index);

#endif /* Q2PSX_INSPECT_CMD_COLL_H */
