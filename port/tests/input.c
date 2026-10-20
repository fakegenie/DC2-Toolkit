#include <assert.h>
#include <string.h>
#include "input.h"

static unsigned int pressed(const unsigned char data[32]) {
    return ((data[2] << 8) | data[3]) ^ 0xffff;
}

int main(void) {
    unsigned char data[32];
    struct port_input_state state = {0};
    memset(data, 73, sizeof(data));
    port_input_report(&state, data);
    assert(data[0] == 0 && data[1] == 0x79);
    assert(pressed(data) == 0);
    for (int i = 4; i < 8; ++i) assert(data[i] == 128);
    for (int i = 8; i < 32; ++i) assert(data[i] == 0);
    static const int offsets[16] = {18, 19, 16, 17, 12, 13, 14, 15, -1, -1, -1, -1, 10, 8, 11, 9};
    for (int i = 0; i < 16; ++i) {
        state.buttons = (uint16_t)(1u << i);
        port_input_report(&state, data);
        assert(pressed(data) == (1u << i));
        for (int j = 8; j < 20; ++j) assert(data[j] == (j == offsets[i] ? 255 : 0));
    }
    state.buttons = 0xffff;
    port_input_report(&state, data);
    assert(pressed(data) == 0xffff);
    for (int i = 8; i < 20; ++i) assert(data[i] == 255);
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
    for (int i = -32768; i < 32767; ++i) assert(port_input_axis(i) <= port_input_axis(i + 1));
    state = (struct port_input_state){0, -32768, 32767, 32767, -32768, 0, 0};
    port_input_report(&state, data);
    assert(data[4] == 255 && data[5] == 0 && data[6] == 0 && data[7] == 255);
    state.left_trigger = 4096;
    state.right_trigger = 4097;
    port_input_report(&state, data);
    assert(pressed(data) == PORT_INPUT_R2);
    assert(data[18] == 31 && data[19] == 31);
    state.left_trigger = 32767;
    state.right_trigger = -32768;
    port_input_report(&state, data);
    assert(pressed(data) == PORT_INPUT_L2);
    assert(data[18] == 255 && data[19] == 0);
    state.buttons = PORT_INPUT_R2;
    port_input_report(&state, data);
    assert(pressed(data) == (PORT_INPUT_L2 | PORT_INPUT_R2));
    assert(data[19] == 255);
    assert(port_input_init() == 1);
    port_input_read(data);
    assert(pressed(data) == 0);
    port_input_close();
    return 0;
}
