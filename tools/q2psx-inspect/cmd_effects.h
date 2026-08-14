#ifndef Q2PSX_INSPECT_CMD_EFFECTS_H
#define Q2PSX_INSPECT_CMD_EFFECTS_H

#include "disc.h"

/*
 * Read the effect system's tables out of the disc's executable, print them, and
 * check the structural claims `src/build/fxtables.h` makes about them.
 *
 * Then exercise the runtime with no screen attached: spawn every named preset,
 * run the integrator, and report what came out — so "the effects are
 * reconstructed" is a statement the build can evaluate rather than an assertion
 * in a document.
 */
/*
 * `out_ppm` optionally names an image to write: one frame with a burst of each
 * preset and a beam of each style, drawn through the real GTE, the real
 * ordering table and the real rasteriser. It is the end-to-end check the
 * assertions cannot be — a burst can pass every numeric test and still land off
 * screen, at one pixel, or in the wrong sort order.
 */
int cmd_effects(const disc *d, const char *out_ppm);

#endif /* Q2PSX_INSPECT_CMD_EFFECTS_H */
