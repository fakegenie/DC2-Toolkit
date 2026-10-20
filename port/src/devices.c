#include <string.h>
#include "sce/libpad.h"
#ifdef PORT_GRAPHICS
#include "input.h"
#endif

static unsigned char pad_open[2];

int scePadInit(int mode) {
    memset(pad_open, 0, sizeof(pad_open));
    return 1;
}

int scePadEnd(void) {
    memset(pad_open, 0, sizeof(pad_open));
    return 1;
}

int scePadPortOpen(int port, int slot, unsigned char *buffer) {
    if (port < 0 || port > 1 || slot != 0 || !buffer) return 0;
    pad_open[port] = 1;
    return 1;
}

int scePadPortClose(int port, int slot) {
    if (port < 0 || port > 1 || slot != 0) return 0;
    pad_open[port] = 0;
    return 1;
}

int scePadGetState(int port, int slot) {
    if (port != 0 || slot != 0 || !pad_open[port]) return scePadStateDiscon;
    return scePadStateStable;
}

int scePadRead(int port, int slot, unsigned char *data) {
    if (!data || scePadGetState(port, slot) != scePadStateStable) return 0;
#ifdef PORT_GRAPHICS
    port_input_read(data);
#else
    memset(data, 0, 32);
    data[1] = 0x79;
    data[2] = 0xff;
    data[3] = 0xff;
    memset(data + 4, 0x80, 4);
#endif
    return 1;
}

int scePadInfoMode(int port, int slot, int info, int index) {
    if (scePadGetState(port, slot) != scePadStateStable) return 0;
    if (info == InfoModeCurID || info == InfoModeCurExID) return 7;
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
