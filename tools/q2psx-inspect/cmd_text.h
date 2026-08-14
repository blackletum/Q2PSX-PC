#ifndef Q2PSX_INSPECT_CMD_TEXT_H
#define Q2PSX_INSPECT_CMD_TEXT_H

#include "disc.h"

/*
 * The `Strings` chunk and the briefing screen it feeds, plus the two pieces of
 * UI chrome that are art rather than text — the panel frame and the button
 * prompts.
 *
 *   text <disc>          every map's dictionary, one line each
 *   text <disc> <map>    one map in full, with the briefing it would show
 *   text <disc> <map> <out.ppm>   and draw it
 */
int cmd_text(const disc *d, const char *map, const char *out_path);

#endif /* Q2PSX_INSPECT_CMD_TEXT_H */
