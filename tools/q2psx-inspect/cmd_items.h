#ifndef Q2PSX_INSPECT_CMD_ITEMS_H
#define Q2PSX_INSPECT_CMD_ITEMS_H

#include "disc.h"

/*
 * Dump the item table and check it disc-wide.
 *
 * Three things, in order:
 *
 *   1. The 64-record table read out of the executable, diffed field by field
 *      against the port's built-in copy — so a drift in the transcription is a
 *      command that fails rather than an assertion in a document. The 55-entry
 *      touch dispatch is read too, so which effects are inert is evidence.
 *
 *   2. Every place record on the disc walked through the same lookup the
 *      spawner uses, checking that the id names a record and the record names a
 *      model the same map ships. That is the item-side equivalent of what
 *      `classes` does for creatures.
 *
 *   3. A census: which place ids are used, how many items carry an effect that
 *      does nothing, and which table records no map ever places.
 */
int cmd_items(disc *d);

#endif /* Q2PSX_INSPECT_CMD_ITEMS_H */
