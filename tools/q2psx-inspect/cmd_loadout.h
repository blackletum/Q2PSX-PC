#ifndef Q2PSX_INSPECT_CMD_LOADOUT_H
#define Q2PSX_INSPECT_CMD_LOADOUT_H

#include "disc.h"

/*
 * The inventory a player is given on launching straight into each campaign
 * level, at one skill (default 1, medium) — the table `--map` applies, printed
 * so it can be read rather than inferred from a screenshot. See loadout.h.
 */
int cmd_loadout(disc *d, const char *skill);

#endif /* Q2PSX_INSPECT_CMD_LOADOUT_H */
