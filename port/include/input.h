#pragma once

#include <stdint.h>

enum {
    PORT_INPUT_L2 = 0x0001,
    PORT_INPUT_R2 = 0x0002,
    PORT_INPUT_L1 = 0x0004,
    PORT_INPUT_R1 = 0x0008,
    PORT_INPUT_TRIANGLE = 0x0010,
    PORT_INPUT_CIRCLE = 0x0020,
    PORT_INPUT_CROSS = 0x0040,
    PORT_INPUT_SQUARE = 0x0080,
    PORT_INPUT_SELECT = 0x0100,
    PORT_INPUT_L3 = 0x0200,
    PORT_INPUT_R3 = 0x0400,
    PORT_INPUT_START = 0x0800,
    PORT_INPUT_UP = 0x1000,
    PORT_INPUT_RIGHT = 0x2000,
    PORT_INPUT_DOWN = 0x4000,
    PORT_INPUT_LEFT = 0x8000
};

struct port_input_state {
    uint16_t buttons;
    int left_x;
    int left_y;
    int right_x;
    int right_y;
    int left_trigger;
    int right_trigger;
};

unsigned char port_input_axis(int value);
void port_input_report(const struct port_input_state *state, unsigned char data[32]);
int port_input_init(void);
void port_input_close(void);
void port_input_poll(void);
void port_input_read(unsigned char data[32]);
