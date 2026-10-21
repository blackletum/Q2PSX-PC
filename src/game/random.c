#include "random.h"
#include "weapon.h"

static q2_rng gameplay_rng = { 1u };

s32 q2_random_next(void)
{
    return q2_rng_next(&gameplay_rng);
}

u32 q2_random_state(void)
{
    return gameplay_rng.state;
}

void q2_random_seed(u32 state)
{
    q2_rng_seed(&gameplay_rng, state);
}
