#include "port_mod.h"
#include "render.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"
#include "port_graphics.h"
#include "touch.h"

enum { TRIGGER_PRESS = 4096, AXIS_STEP = 256 };

static struct port_input_state input;

unsigned char port_input_axis(int value) {
    if (value < INT16_MIN) value = INT16_MIN;
    if (value > INT16_MAX) value = INT16_MAX;
    return (unsigned char)((value - INT16_MIN) / AXIS_STEP);
}

int port_input_square(int value, int other) {
    float along = value / (float)INT16_MAX, across = other / (float)INT16_MAX;
    float longest = fmaxf(fabsf(along), fabsf(across));
    if (longest == 0) return 0;
    float radius = fminf(hypotf(along, across), 1);
    return (int)lroundf(along * radius / longest * INT16_MAX);
}

static int trigger_depth(int value) {
    if (value < 0) return 0;
    return value > INT16_MAX ? INT16_MAX : value;
}

void port_input_report(const struct port_input_state *state, struct pad_dualshock2_report *report) {
    static const uint16_t pressure_buttons[PORT_INPUT_PRESSURES] = {
        [PORT_INPUT_PRESSURE_RIGHT] = PORT_INPUT_RIGHT, [PORT_INPUT_PRESSURE_LEFT] = PORT_INPUT_LEFT,
        [PORT_INPUT_PRESSURE_UP] = PORT_INPUT_UP, [PORT_INPUT_PRESSURE_DOWN] = PORT_INPUT_DOWN,
        [PORT_INPUT_PRESSURE_TRIANGLE] = PORT_INPUT_TRIANGLE, [PORT_INPUT_PRESSURE_CIRCLE] = PORT_INPUT_CIRCLE,
        [PORT_INPUT_PRESSURE_CROSS] = PORT_INPUT_CROSS, [PORT_INPUT_PRESSURE_SQUARE] = PORT_INPUT_SQUARE,
        [PORT_INPUT_PRESSURE_L1] = PORT_INPUT_L1, [PORT_INPUT_PRESSURE_R1] = PORT_INPUT_R1,
        [PORT_INPUT_PRESSURE_L2] = PORT_INPUT_L2, [PORT_INPUT_PRESSURE_R2] = PORT_INPUT_R2,
    };
    uint16_t buttons = state->buttons;
    int left = trigger_depth(state->left_trigger);
    int right = trigger_depth(state->right_trigger);
    if (left > TRIGGER_PRESS) buttons |= PORT_INPUT_L2;
    if (right > TRIGGER_PRESS) buttons |= PORT_INPUT_R2;
    uint16_t released = (uint16_t)~buttons;
    *report = (struct pad_dualshock2_report){
        .halfwords = PORT_INPUT_REPORT_HALFWORDS,
        .terminal = PORT_INPUT_TERMINAL_DUALSHOCK,
        .released_high = (uint8_t)(released >> 8),
        .released_low = (uint8_t)released,
        .right_x = port_input_axis(state->right_x),
        .right_y = port_input_axis(state->right_y),
        .left_x = port_input_axis(state->left_x),
        .left_y = port_input_axis(state->left_y),
    };
    for (int i = 0; i < PORT_INPUT_PRESSURES; ++i) {
        if (buttons & pressure_buttons[i]) report->pressure[i] = PORT_INPUT_PRESSURE_FULL;
    }
    if (!(state->buttons & PORT_INPUT_L2)) report->pressure[PORT_INPUT_PRESSURE_L2] = (uint8_t)(left * PORT_INPUT_PRESSURE_FULL / INT16_MAX);
    if (!(state->buttons & PORT_INPUT_R2)) report->pressure[PORT_INPUT_PRESSURE_R2] = (uint8_t)(right * PORT_INPUT_PRESSURE_FULL / INT16_MAX);
}

void port_input_read(struct pad_dualshock2_report *report) {
    struct port_input_state state = input;
    port_mods_input(&state);
    port_input_report(&state, report);
}

#ifdef PORT_GRAPHICS
#include <SDL3/SDL.h>

static SDL_Gamepad *gamepad;
static int initialized;

int port_input_init(void) {
    if (initialized) return 1;
    memset(&input, 0, sizeof(input));
    initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    return initialized;
}

void port_input_close(void) {
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    memset(&input, 0, sizeof(input));
    if (initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    initialized = 0;
}

static void read_devices(void) {
    static const SDL_Scancode keys[16] = {
        SDL_SCANCODE_1, SDL_SCANCODE_3, SDL_SCANCODE_Q, SDL_SCANCODE_E,
        SDL_SCANCODE_I, SDL_SCANCODE_L, SDL_SCANCODE_K, SDL_SCANCODE_J,
        SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT
    };
    static const SDL_GamepadButton buttons[16] = {
        SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT
    };
    if (gamepad && !SDL_GamepadConnected(gamepad)) {
        SDL_CloseGamepad(gamepad);
        gamepad = NULL;
    }
    if (!gamepad) {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count && !gamepad; ++i) gamepad = SDL_OpenGamepad(ids[i]);
        SDL_free(ids);
    }
    if (SDL_GetKeyboardFocus() != render_window()) return;
    if (gamepad) {
        for (int i = 2; i < 16; ++i) {
            if (SDL_GetGamepadButton(gamepad, buttons[i])) input.buttons |= (uint16_t)(1u << i);
        }
        int left_x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        int left_y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        int right_x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        int right_y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
        input.left_x = port_input_square(left_x, left_y);
        input.left_y = port_input_square(left_y, left_x);
        input.right_x = port_input_square(right_x, right_y);
        input.right_y = port_input_square(right_y, right_x);
        input.left_trigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        input.right_trigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    }
    const bool *keyboard = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < 16; ++i) {
        if (keyboard[keys[i]]) input.buttons |= (uint16_t)(1u << i);
    }
    int x = keyboard[SDL_SCANCODE_D] - keyboard[SDL_SCANCODE_A];
    int y = keyboard[SDL_SCANCODE_S] - keyboard[SDL_SCANCODE_W];
    if (keyboard[SDL_SCANCODE_D] || keyboard[SDL_SCANCODE_A]) input.left_x = x * 32767;
    if (keyboard[SDL_SCANCODE_S] || keyboard[SDL_SCANCODE_W]) input.left_y = y * 32767;
    x = keyboard[SDL_SCANCODE_H] - keyboard[SDL_SCANCODE_F];
    y = keyboard[SDL_SCANCODE_G] - keyboard[SDL_SCANCODE_T];
    if (keyboard[SDL_SCANCODE_H] || keyboard[SDL_SCANCODE_F]) input.right_x = x * 32767;
    if (keyboard[SDL_SCANCODE_G] || keyboard[SDL_SCANCODE_T]) input.right_y = y * 32767;
}

void port_input_poll(void) {
    memset(&input, 0, sizeof(input));
    if (!initialized) return;
    read_devices();
    port_touch_read(&input);
}
#else
int port_input_init(void) {
    memset(&input, 0, sizeof(input));
    return 1;
}

void port_input_close(void) {
    memset(&input, 0, sizeof(input));
}

void port_input_poll(void) {}
#endif
