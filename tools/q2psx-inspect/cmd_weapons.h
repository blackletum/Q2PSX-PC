#ifndef Q2PSX_INSPECT_CMD_WEAPONS_H
#define Q2PSX_INSPECT_CMD_WEAPONS_H

#include "disc.h"

/*
 * Dump the weapon, armour and sound tables and check the port's built-in copy
 * against the disc's executable, field by field. Also re-checks that every
 * address the behaviour table claims to have been transcribed from is the one
 * the fire-function pointer array actually holds — so a documentation drift
 * against the real executable fails the command.
 */
int cmd_weapons(const disc *d);

#endif /* Q2PSX_INSPECT_CMD_WEAPONS_H */
