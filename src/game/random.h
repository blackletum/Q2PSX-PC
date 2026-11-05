/* Shared gameplay random stream, replacing the host C library's rand().
 * The generator is BIOS A(2Fh)'s 15-bit LCG, implemented by q2_rng_next.
 * Keep the complete 32-bit state: the returned 15 bits cannot resume it.
 * Every subsystem's q2_rng is bound to this one state, as every caller of
 * 0x80089E28 shares the BIOS's. */
#ifndef Q2PSX_RANDOM_H
#define Q2PSX_RANDOM_H

#include "q2psx.h"

s32 q2_random_next(void);
u32 q2_random_state(void);
/* Also restores a saved state. Zero is a valid seed/state. */
void q2_random_seed(u32 state);

/* THE state: BIOS rand()'s, the one every subsystem's stream is bound to
 * (weapon.h, q2_rng). Never NULL. */
struct q2_rng;
struct q2_rng *q2_random_rng(void);

#endif
