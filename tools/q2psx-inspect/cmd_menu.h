#ifndef Q2PSX_INSPECT_CMD_MENU_H
#define Q2PSX_INSPECT_CMD_MENU_H

#include "disc.h"

/*
 * Dump the reconstructed menu and check it against the disc's executable.
 * With `out`, instead draw the named page to a PPM — the same "no window
 * needed" route the geometry pipeline uses — at the console's own 512x248, or
 * at `size` ("320x256") when one is given.
 */
int cmd_menu(const disc *d, const char *page, const char *out,
             const char *size);

#endif /* Q2PSX_INSPECT_CMD_MENU_H */
