/* Shared gameplay random stream, replacing the host C library's rand().
 * The generator is BIOS A(2Fh)'s 15-bit LCG, implemented by q2_rng_next.
 * Keep the complete 32-bit state: the returned 15 bits cannot resume it.
 * Weapon/effect streams remain owned and saved by their simulation objects. */
#ifndef Q2PSX_RANDOM_H
#define Q2PSX_RANDOM_H

#include "q2psx.h"

s32 q2_random_next(void);
u32 q2_random_state(void);
/* Also restores a saved state. Zero is a valid seed/state. */
void q2_random_seed(u32 state);

#endif
