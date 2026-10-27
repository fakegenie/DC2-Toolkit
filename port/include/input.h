#pragma once

#include <stddef.h>
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

enum { PORT_INPUT_TERMINAL_DUALSHOCK = 7, PORT_INPUT_AXIS_CENTER = 0x80, PORT_INPUT_PRESSURE_FULL = UINT8_MAX };

enum {
    PORT_INPUT_PRESSURE_RIGHT,
    PORT_INPUT_PRESSURE_LEFT,
    PORT_INPUT_PRESSURE_UP,
    PORT_INPUT_PRESSURE_DOWN,
    PORT_INPUT_PRESSURE_TRIANGLE,
    PORT_INPUT_PRESSURE_CIRCLE,
    PORT_INPUT_PRESSURE_CROSS,
    PORT_INPUT_PRESSURE_SQUARE,
    PORT_INPUT_PRESSURE_L1,
    PORT_INPUT_PRESSURE_R1,
    PORT_INPUT_PRESSURE_L2,
    PORT_INPUT_PRESSURE_R2,
    PORT_INPUT_PRESSURES
};

struct pad_dualshock2_report {
    uint8_t status;
    uint8_t halfwords : 4;
    uint8_t terminal : 4;
    uint8_t released_high;
    uint8_t released_low;
    uint8_t right_x;
    uint8_t right_y;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t pressure[PORT_INPUT_PRESSURES];
    uint8_t unused[12];
};
_Static_assert(sizeof(struct pad_dualshock2_report) == 32, "DualShock 2 pad report");

enum { PORT_INPUT_REPORT_HALFWORDS = (offsetof(struct pad_dualshock2_report, unused) - offsetof(struct pad_dualshock2_report, released_high)) / 2 };

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
int port_input_square(int value, int other);
void port_input_report(const struct port_input_state *state, struct pad_dualshock2_report *report);
int port_input_init(void);
void port_input_close(void);
void port_input_poll(void);
void port_input_read(struct pad_dualshock2_report *report);
