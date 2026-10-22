/*
 * launcher.h — choose the disc before anything is read from one.
 *
 * Shown when the client starts without --disc. It lists the disc played last
 * (selected), the ones before it, disc images found beside the executable and
 * in its disc/ folder, and every optical drive; Browse opens the system's own
 * file picker, and an image dropped on the window joins the list. Whatever is
 * played is remembered (launchcfg.h) and selected again next time.
 *
 * It needs nothing from the disc to draw itself — SDL's built-in 8x8 font and
 * flat shapes — because until it returns there is no disc to read a font from.
 */
#ifndef Q2PSX_LAUNCHER_H
#define Q2PSX_LAUNCHER_H

#include <SDL3/SDL.h>

#include "q2psx.h"

/*
 * Run until the player picks a disc or closes the window.
 *
 * True: `out` holds the chosen path — opened and identified as Quake II PSX,
 * and saved as the last choice — and `*window` the launcher's window, its
 * renderer destroyed, for the game to resize and draw into so there is no gap
 * between the two. False: the player quit, and nothing is left open.
 */
bool q2_launcher_run(char *out, size_t cap, SDL_Window **window);

#endif /* Q2PSX_LAUNCHER_H */
