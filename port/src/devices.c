#include <string.h>
#include "sce/libpad.h"
#include "input.h"
#include "port_game.h"

enum { PAD_PORTS = 2 };

PORT_GAME_BEGIN
static unsigned char pad_open[PAD_PORTS];
PORT_GAME_END

int scePadInit(int mode) {
    memset(pad_open, 0, sizeof(pad_open));
    return 1;
}

int scePadEnd(void) {
    memset(pad_open, 0, sizeof(pad_open));
    return 1;
}

int scePadPortOpen(int port, int slot, unsigned char *buffer) {
    if (port < 0 || port >= PAD_PORTS || slot != 0 || !buffer) return 0;
    pad_open[port] = 1;
    return 1;
}

int scePadPortClose(int port, int slot) {
    if (port < 0 || port >= PAD_PORTS || slot != 0) return 0;
    pad_open[port] = 0;
    return 1;
}

int scePadGetState(int port, int slot) {
    if (port != 0 || slot != 0 || !pad_open[port]) return scePadStateDiscon;
    return scePadStateStable;
}

int scePadRead(int port, int slot, unsigned char *data) {
    if (!data || scePadGetState(port, slot) != scePadStateStable) return 0;
    struct pad_dualshock2_report report;
#ifdef PORT_GRAPHICS
    port_input_read(&report);
#else
    report = (struct pad_dualshock2_report){
        .halfwords = PORT_INPUT_REPORT_HALFWORDS,
        .terminal = PORT_INPUT_TERMINAL_DUALSHOCK,
        .released_high = UINT8_MAX,
        .released_low = UINT8_MAX,
        .right_x = PORT_INPUT_AXIS_CENTER,
        .right_y = PORT_INPUT_AXIS_CENTER,
        .left_x = PORT_INPUT_AXIS_CENTER,
        .left_y = PORT_INPUT_AXIS_CENTER,
    };
#endif
    memcpy(data, &report, sizeof(report));
    return 1;
}

int scePadInfoMode(int port, int slot, int info, int index) {
    if (scePadGetState(port, slot) != scePadStateStable) return 0;
    if (info == InfoModeCurID || info == InfoModeCurExID) return PORT_INPUT_TERMINAL_DUALSHOCK;
    return 0;
}

int scePadSetMainMode(int port, int slot, int mode, int lock) {
    return scePadGetState(port, slot) == scePadStateStable;
}

int scePadInfoAct(int port, int slot, int actuator, int command) {
    if (scePadGetState(port, slot) != scePadStateStable) return 0;
    return actuator == -1 ? 2 : 0;
}

int scePadSetActAlign(int port, int slot, unsigned char *alignment) {
    return scePadGetState(port, slot) == scePadStateStable;
}

int scePadSetActDirect(int port, int slot, unsigned char *values) {
    return scePadGetState(port, slot) == scePadStateStable;
}

void sceDevConsInit(void) {}
int sceDevConsOpen(unsigned int x, unsigned int y, unsigned int width, unsigned int height) { return 0; }
void sceDevConsClose(int handle) {}
void sceDevConsAttribute(int handle, unsigned char attributes) {}
void sceDevConsDraw(int handle) {}
