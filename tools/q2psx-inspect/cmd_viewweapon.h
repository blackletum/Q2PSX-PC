#ifndef Q2PSX_INSPECT_CMD_VIEWWEAPON_H
#define Q2PSX_INSPECT_CMD_VIEWWEAPON_H

#include "disc.h"

/*
 * Dump the view weapon's animation bank and check it against the disc.
 *
 * With `weapon` (a name or a 1-based id) print every key of that weapon's four
 * clips rather than the summary.
 *
 * With `out` as well, render a first-person frame of `map` through the
 * reconstructed transform instead — the world from the eye and the weapon in
 * hand, both through the same GTE and the same ordering table. Alignment is the
 * sort of claim you have to look at, and this needs no window.
 */
int cmd_viewweapon(disc *d, const char *weapon, const char *out,
                   const char *map, int zone_index);

#endif /* Q2PSX_INSPECT_CMD_VIEWWEAPON_H */
