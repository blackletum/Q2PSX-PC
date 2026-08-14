#ifndef Q2PSX_CMD_LIGHTS_H
#define Q2PSX_CMD_LIGHTS_H

#include "disc.h"

/* `lights <disc>` — the whole lighting model, checked against the disc:
 * the SpaceLights partition on every zone, the light type byte's three fields,
 * the four flare element tables and the reciprocal-square-root table, both read
 * back out of the executable and compared against what the port carries. */
int cmd_lights(const disc *d);

/* `lit <disc> [map]` — drop the player at each map's start and report what the
 * light gather actually finds there: how many lights reach the cell, which
 * three win, and how many of them would put a flare on the screen. */
int cmd_lit(const disc *d, const char *map);

#endif /* Q2PSX_CMD_LIGHTS_H */
