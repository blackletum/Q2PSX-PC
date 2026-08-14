/*
 * cmd_ai.h — check the reconstructed creature AI against the executable.
 *
 * Reads back every constant the port's AI was built from and compares it to
 * what the port actually uses, so "the AI matches the disc" is a number rather
 * than a claim.
 */
#ifndef Q2PSX_CMD_AI_H
#define Q2PSX_CMD_AI_H

#include "disc.h"

int cmd_ai(const disc *d);

#endif /* Q2PSX_CMD_AI_H */
