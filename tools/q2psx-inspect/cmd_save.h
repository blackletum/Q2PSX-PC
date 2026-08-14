#ifndef Q2PSX_INSPECT_CMD_SAVE_H
#define Q2PSX_INSPECT_CMD_SAVE_H

#include "disc.h"

/*
 * Exercise the save system against a real map.
 *
 * Loads a zone, spawns a player, runs the world forward, captures a save,
 * writes it, reads it back and compares — which is the round trip the unit test
 * does, but against disc data rather than a synthetic sim, so the entity set,
 * the event script and the trigger volumes are the real ones.
 *
 * Then drives the memory-card front end through a save and a load, printing the
 * state transitions by the console's own numbers.
 *
 * With `out`, also draws each screen the front end shows to `<out>.NAME.ppm`,
 * at the console's own 512 x 248 and with the console's own font. `map` names
 * the level directory the atlases come from and the world is loaded from.
 *
 * Returns non-zero when something did not survive the round trip.
 */
int cmd_save(const disc *d, const char *map, const char *out);

#endif /* Q2PSX_INSPECT_CMD_SAVE_H */
