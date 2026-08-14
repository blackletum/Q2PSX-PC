#ifndef Q2PSX_INSPECT_CMD_MENU_H
#define Q2PSX_INSPECT_CMD_MENU_H

#include "disc.h"

/*
 * Dump the reconstructed menu and check it against the disc's executable.
 *
 * With `out`, instead draw the named page to a PPM — the same "no window
 * needed" route the geometry pipeline uses — at the console's own 512 x 248,
 * with the console's own font. `map` names the level directory the atlases are
 * decoded out of and defaults to BASE1; three of the forty-nine ship no
 * `frontend.lbm` and two ship no `chars.lbm`.
 */
int cmd_menu(const disc *d, const char *page, const char *out,
             const char *map);

#endif /* Q2PSX_INSPECT_CMD_MENU_H */
