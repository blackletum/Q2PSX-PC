#ifndef Q2PSX_INSPECT_CMD_SCREEN_H
#define Q2PSX_INSPECT_CMD_SCREEN_H

#include "disc.h"

/*
 * Print the reconstructed screen — display state, ordering-table slicing and
 * every viewport layout — and check each constant against the instruction in
 * the boot executable it was read from. Any mismatch is a non-zero exit.
 *
 * With `out`, instead compose one real frame through the screen path and write
 * it as a PPM: the whole point of a table-slicing, draw-env-clipping screen is
 * something you have to look at to believe, and this needs no window. `layout`
 * is one of the names q2_screen_layout_name prints.
 */
int cmd_screen(disc *d, const char *out, const char *layout,
               const char *map, int zone_index);

#endif /* Q2PSX_INSPECT_CMD_SCREEN_H */
