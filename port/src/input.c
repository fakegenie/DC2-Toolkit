#include "port_mod.h"
#include "render.h"
#include <stdlib.h>
#include <string.h>
#include "input.h"
#include "port_graphics.h"

static struct port_input_state input;

unsigned char port_input_axis(int value) {
    if (value < -32768) value = -32768;
    if (value > 32767) value = 32767;
    return (unsigned char)((value + 32768) / 256);
}

void port_input_report(const struct port_input_state *state, unsigned char data[32]) {
    static const uint16_t pressure_buttons[12] = {
        PORT_INPUT_RIGHT, PORT_INPUT_LEFT, PORT_INPUT_UP, PORT_INPUT_DOWN,
        PORT_INPUT_TRIANGLE, PORT_INPUT_CIRCLE, PORT_INPUT_CROSS, PORT_INPUT_SQUARE,
        PORT_INPUT_L1, PORT_INPUT_R1, PORT_INPUT_L2, PORT_INPUT_R2
    };
    uint16_t buttons = state->buttons;
    int left = state->left_trigger;
    int right = state->right_trigger;
    if (left < 0) left = 0;
    if (left > 32767) left = 32767;
    if (right < 0) right = 0;
    if (right > 32767) right = 32767;
    if (left > 4096) buttons |= PORT_INPUT_L2;
    if (right > 4096) buttons |= PORT_INPUT_R2;
    memset(data, 0, 32);
    data[1] = 0x79;
    data[2] = (unsigned char)(~buttons >> 8);
    data[3] = (unsigned char)~buttons;
    data[4] = port_input_axis(state->right_x);
    data[5] = port_input_axis(state->right_y);
    data[6] = port_input_axis(state->left_x);
    data[7] = port_input_axis(state->left_y);
    for (int i = 0; i < 12; ++i) {
        if (buttons & pressure_buttons[i]) data[8 + i] = 255;
    }
    if (!(state->buttons & PORT_INPUT_L2)) data[18] = (unsigned char)(left * 255 / 32767);
    if (!(state->buttons & PORT_INPUT_R2)) data[19] = (unsigned char)(right * 255 / 32767);
}

void port_input_read(unsigned char data[32]) {
    struct port_input_state state = input;
    port_mods_input(&state);
    port_input_report(&state, data);
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

void port_input_poll(void) {
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
    memset(&input, 0, sizeof(input));
    if (!initialized) return;
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
        input.left_x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        input.left_y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        input.right_x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        input.right_y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
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
