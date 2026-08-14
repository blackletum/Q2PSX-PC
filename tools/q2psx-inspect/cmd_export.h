/*
 * cmd_export.h — bulk asset extraction to interchange formats.
 *
 * `extract` writes the disc's files back out byte for byte. This is the other
 * half: it decodes them and writes geometry as Wavefront OBJ, textures as PCX
 * (plus an indexed PNG, which is the only one of the two that can carry the
 * PlayStation's transparent texel) and every sample as a RIFF WAV.
 */
#ifndef Q2PSX_CMD_EXPORT_H
#define Q2PSX_CMD_EXPORT_H

#include "disc.h"

/*
 * `what` is a comma-separated subset of {maps, models, textures, sounds,
 * music, cdda}, or NULL / "all" for everything. `only_map` restricts the level
 * walk to one directory, or is NULL for every map on the disc.
 */
int cmd_export(disc *d, const char *outdir, const char *what,
               const char *only_map);

#endif /* Q2PSX_CMD_EXPORT_H */
