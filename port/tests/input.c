#include <assert.h>
#include <math.h>
#include <string.h>
#include "input.h"
#include "port_graphics.h"

uint64_t port_fields;

enum { NO_PRESSURE = -1, ALL_BUTTONS = UINT16_MAX, REPORT_FILL = 73, ENGINE_DEAD_ZONE = 50, ENGINE_SPAN = 78, ANGLES = 64 };

#define TOWN_RUN 0.8f

struct button_pressure {
    uint16_t button;
    int pressure;
};

static const struct button_pressure buttons[] = {
    {PORT_INPUT_L2, PORT_INPUT_PRESSURE_L2},
    {PORT_INPUT_R2, PORT_INPUT_PRESSURE_R2},
    {PORT_INPUT_L1, PORT_INPUT_PRESSURE_L1},
    {PORT_INPUT_R1, PORT_INPUT_PRESSURE_R1},
    {PORT_INPUT_TRIANGLE, PORT_INPUT_PRESSURE_TRIANGLE},
    {PORT_INPUT_CIRCLE, PORT_INPUT_PRESSURE_CIRCLE},
    {PORT_INPUT_CROSS, PORT_INPUT_PRESSURE_CROSS},
    {PORT_INPUT_SQUARE, PORT_INPUT_PRESSURE_SQUARE},
    {PORT_INPUT_SELECT, NO_PRESSURE},
    {PORT_INPUT_L3, NO_PRESSURE},
    {PORT_INPUT_R3, NO_PRESSURE},
    {PORT_INPUT_START, NO_PRESSURE},
    {PORT_INPUT_UP, PORT_INPUT_PRESSURE_UP},
    {PORT_INPUT_RIGHT, PORT_INPUT_PRESSURE_RIGHT},
    {PORT_INPUT_DOWN, PORT_INPUT_PRESSURE_DOWN},
    {PORT_INPUT_LEFT, PORT_INPUT_PRESSURE_LEFT},
};

static float deflection(int value) {
    int offset = port_input_axis(value) - PORT_INPUT_AXIS_CENTER;
    if (offset > -ENGINE_DEAD_ZONE && offset < ENGINE_DEAD_ZONE) return 0;
    int outside = offset > 0 ? offset - (ENGINE_DEAD_ZONE - 1) : offset + ENGINE_DEAD_ZONE;
    return (float)(outside * 128 / ENGINE_SPAN) / 128;
}

static float stick_strength(float radius, float angle) {
    int x = (int)(radius * cosf(angle) * INT16_MAX), y = (int)(radius * sinf(angle) * INT16_MAX);
    return hypotf(deflection(port_input_square(x, y)), deflection(port_input_square(y, x)));
}

static unsigned int pressed(const struct pad_dualshock2_report *report) {
    return ((report->released_high << 8) | report->released_low) ^ ALL_BUTTONS;
}

static int unused_clear(const struct pad_dualshock2_report *report) {
    for (size_t i = 0; i < sizeof(report->unused); ++i) {
        if (report->unused[i]) return 0;
    }
    return 1;
}

int main(void) {
    struct pad_dualshock2_report report;
    struct port_input_state state = {0};
    memset(&report, REPORT_FILL, sizeof(report));
    port_input_report(&state, &report);
    assert(report.status == 0);
    assert(report.terminal == PORT_INPUT_TERMINAL_DUALSHOCK && report.halfwords == PORT_INPUT_REPORT_HALFWORDS);
    assert(pressed(&report) == 0);
    assert(report.right_x == PORT_INPUT_AXIS_CENTER && report.right_y == PORT_INPUT_AXIS_CENTER);
    assert(report.left_x == PORT_INPUT_AXIS_CENTER && report.left_y == PORT_INPUT_AXIS_CENTER);
    for (int i = 0; i < PORT_INPUT_PRESSURES; ++i) assert(report.pressure[i] == 0);
    assert(unused_clear(&report));
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        state.buttons = buttons[i].button;
        port_input_report(&state, &report);
        assert(pressed(&report) == buttons[i].button);
        for (int j = 0; j < PORT_INPUT_PRESSURES; ++j) {
            assert(report.pressure[j] == (j == buttons[i].pressure ? PORT_INPUT_PRESSURE_FULL : 0));
        }
    }
    state.buttons = ALL_BUTTONS;
    port_input_report(&state, &report);
    assert(pressed(&report) == ALL_BUTTONS);
    for (int i = 0; i < PORT_INPUT_PRESSURES; ++i) assert(report.pressure[i] == PORT_INPUT_PRESSURE_FULL);
    assert(port_input_axis(-40000) == 0);
    assert(port_input_axis(-32768) == 0);
    assert(port_input_axis(-16384) == 64);
    assert(port_input_axis(-6000) == 104);
    assert(port_input_axis(0) == 128);
    assert(port_input_axis(6000) == 151);
    assert(port_input_axis(16384) == 192);
    assert(port_input_axis(-12801) == 77);
    assert(port_input_axis(-12800) == 78);
    assert(port_input_axis(12799) == 177);
    assert(port_input_axis(12800) == 178);
    assert(port_input_axis(32767) == 255);
    assert(port_input_axis(40000) == 255);
    for (int i = INT16_MIN; i < INT16_MAX; ++i) assert(port_input_axis(i) <= port_input_axis(i + 1));
    assert(port_input_square(0, 0) == 0);
    assert(port_input_square(INT16_MAX, 0) == INT16_MAX && port_input_square(0, INT16_MAX) == 0);
    assert(port_input_square(INT16_MIN, 0) == -INT16_MAX);
    assert(port_input_square(23170, 23170) == INT16_MAX && port_input_square(-23170, 23170) == -INT16_MAX);
    assert(port_input_square(INT16_MAX, INT16_MAX) == INT16_MAX);
    assert(port_input_square(16384, 0) == 16384);
    for (int i = 0; i < ANGLES; ++i) {
        float angle = 2 * (float)M_PI * i / ANGLES;
        assert(stick_strength(1, angle) >= 1);
        assert(stick_strength(0.9f, angle) >= TOWN_RUN);
        assert(stick_strength(0.25f, angle) == 0);
    }
    state = (struct port_input_state){
        .left_x = INT16_MIN,
        .left_y = INT16_MAX,
        .right_x = INT16_MAX,
        .right_y = INT16_MIN,
    };
    port_input_report(&state, &report);
    assert(report.right_x == UINT8_MAX && report.right_y == 0 && report.left_x == 0 && report.left_y == UINT8_MAX);
    state.left_trigger = 4096;
    state.right_trigger = 4097;
    port_input_report(&state, &report);
    assert(pressed(&report) == PORT_INPUT_R2);
    assert(report.pressure[PORT_INPUT_PRESSURE_L2] == 31 && report.pressure[PORT_INPUT_PRESSURE_R2] == 31);
    state.left_trigger = INT16_MAX;
    state.right_trigger = INT16_MIN;
    port_input_report(&state, &report);
    assert(pressed(&report) == PORT_INPUT_L2);
    assert(report.pressure[PORT_INPUT_PRESSURE_L2] == PORT_INPUT_PRESSURE_FULL && report.pressure[PORT_INPUT_PRESSURE_R2] == 0);
    state.buttons = PORT_INPUT_R2;
    port_input_report(&state, &report);
    assert(pressed(&report) == (PORT_INPUT_L2 | PORT_INPUT_R2));
    assert(report.pressure[PORT_INPUT_PRESSURE_R2] == PORT_INPUT_PRESSURE_FULL);
    assert(port_input_init() == 1);
    port_input_read(&report);
    assert(pressed(&report) == 0);
    port_input_close();
    return 0;
}
