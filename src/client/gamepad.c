#include "gamepad.h"
#include <string.h>

static u32 button_mask(Uint8 button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return Q2_PAD_CROSS;
    case SDL_GAMEPAD_BUTTON_EAST: return Q2_PAD_CIRCLE;
    case SDL_GAMEPAD_BUTTON_WEST: return Q2_PAD_SQUARE;
    case SDL_GAMEPAD_BUTTON_NORTH: return Q2_PAD_TRIANGLE;
    case SDL_GAMEPAD_BUTTON_BACK: return Q2_PAD_SELECT;
    case SDL_GAMEPAD_BUTTON_START: return Q2_PAD_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return Q2_PAD_L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return Q2_PAD_R3;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Q2_PAD_L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Q2_PAD_R1;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return Q2_PAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Q2_PAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Q2_PAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Q2_PAD_RIGHT;
    default: return 0;
    }
}

static int find_slot(const q2_gamepads *pads, SDL_JoystickID id)
{
    int pi;
    if (!id) return -1;
    for (pi = 0; pi < Q2_SIM_MAX_PLAYERS; pi++)
        if (pads->id[pi] == id) return pi;
    return -1;
}

static int add_device(q2_gamepads *pads, SDL_JoystickID id)
{
    int pi;
    if (find_slot(pads, id) >= 0 || !SDL_IsGamepad(id)) return -1;
    for (pi = pads->first_slot; pi < Q2_SIM_MAX_PLAYERS; pi++) {
        if (pads->id[pi]) continue;
        pads->device[pi] = SDL_OpenGamepad(id);
        if (!pads->device[pi]) return -1;
        pads->id[pi] = id;
        pads->changed[pi] = true;
        return pi;
    }
    return -1;
}

void q2_gamepads_open(q2_gamepads *pads, int first_slot)
{
    int count = 0, i;
    SDL_JoystickID *ids;
    memset(pads, 0, sizeof(*pads));
    pads->first_slot = first_slot == 0 ? 0 : 1;
    ids = SDL_GetGamepads(&count);
    for (i = 0; i < count; i++) add_device(pads, ids[i]);
    SDL_free(ids);
}

void q2_gamepads_close(q2_gamepads *pads)
{
    int pi;
    for (pi = 0; pi < Q2_SIM_MAX_PLAYERS; pi++)
        if (pads->device[pi]) SDL_CloseGamepad(pads->device[pi]);
    memset(pads, 0, sizeof(*pads));
}

int q2_gamepads_event(q2_gamepads *pads, const SDL_Event *event)
{
    int pi;
    u32 mask;
    if (event->type == SDL_EVENT_GAMEPAD_ADDED)
        return add_device(pads, event->gdevice.which);
    if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        pi = find_slot(pads, event->gdevice.which);
        if (pi < 0) return -1;
        if (pads->device[pi]) SDL_CloseGamepad(pads->device[pi]);
        pads->device[pi] = NULL;
        pads->id[pi] = 0;
        pads->held[pi] = pads->pressed[pi] = 0;
        memset(pads->axis[pi], 0, sizeof(pads->axis[pi]));
        pads->changed[pi] = true;
        return pi;
    }
    if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        pi = find_slot(pads, event->gaxis.which);
        if (pi < 0 || event->gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT) return -1;
        pads->axis[pi][event->gaxis.axis] = event->gaxis.value;
        mask = event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? Q2_PAD_L2 :
               event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? Q2_PAD_R2 : 0;
        if (mask) {
            if (event->gaxis.value > 8192) {
                pads->pressed[pi] |= mask & ~pads->held[pi];
                pads->held[pi] |= mask;
            } else pads->held[pi] &= ~mask;
        }
        return pi;
    }
    if (event->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
        event->type != SDL_EVENT_GAMEPAD_BUTTON_UP) return -1;
    pi = find_slot(pads, event->gbutton.which);
    if (pi < 0) return -1;
    mask = button_mask(event->gbutton.button);
    if (event->gbutton.down) {
        pads->pressed[pi] |= mask & ~pads->held[pi];
        pads->held[pi] |= mask;
    } else pads->held[pi] &= ~mask;
    return pi;
}

static s8 stick(s16 value)
{
    int magnitude = value < 0 ? -(int)value : value;
    int scaled;
    if (magnitude <= 8000) return 0;
    scaled = (magnitude - 8000) * Q2_PAD_FULL / (32767 - 8000);
    if (scaled > Q2_PAD_FULL) scaled = Q2_PAD_FULL;
    return (s8)(value < 0 ? -scaled : scaled);
}

u32 q2_gamepads_read(const q2_gamepads *pads, int pi, int style,
                     q2_pad_state *axes)
{
    u32 buttons;
    if (pi < 0 || pi >= Q2_SIM_MAX_PLAYERS || !pads->id[pi]) return 0;
    axes->lx = stick(pads->axis[pi][SDL_GAMEPAD_AXIS_LEFTX]);
    axes->ly = stick(pads->axis[pi][SDL_GAMEPAD_AXIS_LEFTY]);
    axes->rx = stick(pads->axis[pi][SDL_GAMEPAD_AXIS_RIGHTX]);
    axes->ry = stick(pads->axis[pi][SDL_GAMEPAD_AXIS_RIGHTY]);
    buttons = pads->held[pi] | pads->pressed[pi];
    /* Retail RIGHT STICK moves with the D-pad. The otherwise unused left
     * stick also feeds those four buttons on the host. BOTH STICKS retains
     * the original split-axis layout, including yaw on the left stick. */
    if (style == Q2_PAD_STYLE_RIGHT_STICK) {
        if (axes->lx < 0) buttons |= Q2_PAD_LEFT;
        if (axes->lx > 0) buttons |= Q2_PAD_RIGHT;
        if (axes->ly < 0) buttons |= Q2_PAD_UP;
        if (axes->ly > 0) buttons |= Q2_PAD_DOWN;
    }
    return buttons;
}
