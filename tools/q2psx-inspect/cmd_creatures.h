/*
 * cmd_creatures.h — decode every creature module on the disc and report it.
 *
 * Walks each map's CreAIBin chain, relocates each module, follows its own code
 * to its class bytes, callbacks, moves and frames, and prints what the port
 * covers against what the disc has.
 */
#ifndef Q2PSX_CMD_CREATURES_H
#define Q2PSX_CMD_CREATURES_H

#include "disc.h"

int cmd_creatures(const disc *d);

#endif /* Q2PSX_CMD_CREATURES_H */
