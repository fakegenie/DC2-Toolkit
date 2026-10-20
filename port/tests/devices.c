#include <assert.h>
#include <string.h>

#include "sce/libpad.h"

int sceSifRebootIop(const char *path);
int sceSifSyncIop(void);
int sceSifLoadModule(const char *path, int arg_len, const char *args);

int main(void) {
    unsigned char data[64];
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
    assert(data[0] == 0 && data[1] == 0x79 && data[2] == 255 && data[3] == 255);
    for (int i = 4; i < 8; ++i) assert(data[i] == 128);
    for (int i = 8; i < 32; ++i) assert(data[i] == 0);
    assert(scePadPortClose(0, 0) == 1 && scePadRead(0, 0, data) == 0);
    return 0;
}
