#include <assert.h>
#include <string.h>

#include "input.h"
#include "sce/libpad.h"

int sceSifRebootIop(const char *path);
int sceSifSyncIop(void);
int sceSifLoadModule(const char *path, int arg_len, const char *args);

int main(void) {
    unsigned char data[2 * sizeof(struct pad_dualshock2_report)];
    assert(sceSifRebootIop("unused") == 1);
    assert(sceSifSyncIop() == 1);
    assert(sceSifLoadModule("unused", 0, NULL) > 0);
    assert(scePadInit(0) == 1);
    assert(scePadGetState(0, 0) == scePadStateDiscon);
    assert(scePadPortOpen(0, 0, data) == 1);
    assert(scePadPortOpen(1, 0, data) == 1);
    assert(scePadGetState(0, 0) == scePadStateStable);
    assert(scePadGetState(1, 0) == scePadStateDiscon);
    assert(scePadGetState(-1, 0) == scePadStateDiscon);
    assert(scePadRead(0, 0, data) == 1);
    struct pad_dualshock2_report report;
    memcpy(&report, data, sizeof(report));
    assert(report.status == 0);
    assert(report.terminal == PORT_INPUT_TERMINAL_DUALSHOCK && report.halfwords == PORT_INPUT_REPORT_HALFWORDS);
    assert(report.released_high == UINT8_MAX && report.released_low == UINT8_MAX);
    assert(report.right_x == PORT_INPUT_AXIS_CENTER && report.right_y == PORT_INPUT_AXIS_CENTER);
    assert(report.left_x == PORT_INPUT_AXIS_CENTER && report.left_y == PORT_INPUT_AXIS_CENTER);
    for (int i = 0; i < PORT_INPUT_PRESSURES; ++i) assert(report.pressure[i] == 0);
    for (size_t i = 0; i < sizeof(report.unused); ++i) assert(report.unused[i] == 0);
    assert(scePadInfoMode(0, 0, InfoModeCurID, 0) == PORT_INPUT_TERMINAL_DUALSHOCK);
    assert(scePadPortClose(0, 0) == 1 && scePadRead(0, 0, data) == 0);
    return 0;
}
