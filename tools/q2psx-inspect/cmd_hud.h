#ifndef Q2PSX_INSPECT_CMD_HUD_H
#define Q2PSX_INSPECT_CMD_HUD_H

#include "disc.h"

/*
 * Dump the HUD's tables, check them against the disc, and optionally render the
 * overlay to a PPM so the atlas binding can be seen rather than asserted.
 *
 *   hud <disc>                    the tables, and a disc-wide atlas check
 *   hud <disc> <map> [out.ppm]    draw the overlay over a flat backdrop
 */
int cmd_hud(const disc *d, const char *map, const char *out_path);

#endif /* Q2PSX_INSPECT_CMD_HUD_H */
