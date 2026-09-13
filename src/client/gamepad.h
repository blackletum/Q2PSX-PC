#ifndef Q2PSX_CLIENT_GAMEPAD_H
#define Q2PSX_CLIENT_GAMEPAD_H

#include <SDL3/SDL.h>
#include "pad.h"

/* Host device assignment is separate from retail pad decoding. Disconnecting
 * a device vacates its slot; it never moves another player's controller. */
typedef struct q2_gamepads {
    SDL_Gamepad *device[Q2_SIM_MAX_PLAYERS];
    SDL_JoystickID id[Q2_SIM_MAX_PLAYERS];
    u32 held[Q2_SIM_MAX_PLAYERS];
    u32 pressed[Q2_SIM_MAX_PLAYERS];
    s16 axis[Q2_SIM_MAX_PLAYERS][SDL_GAMEPAD_AXIS_COUNT];
    bool changed[Q2_SIM_MAX_PLAYERS];
    int first_slot; /* 1 reserves player one for keyboard/mouse; 0 uses four pads. */
} q2_gamepads;

void q2_gamepads_open(q2_gamepads *pads, int first_slot);
void q2_gamepads_close(q2_gamepads *pads);
int q2_gamepads_event(q2_gamepads *pads, const SDL_Event *event);
/* Current buttons plus taps since the last read. Axes are centred signed bytes.
 * The caller rolls buttons once per simulation tick and clears pressed. */
u32 q2_gamepads_read(const q2_gamepads *pads, int player, int style,
                     q2_pad_state *axes);

#endif
