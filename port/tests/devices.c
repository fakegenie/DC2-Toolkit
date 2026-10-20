#include <assert.h>
#include <string.h>

#include "sce/libpad.h"
typedef struct sceSifRpcHeader sceSifRpcHeader;
typedef struct sceSifServeData sceSifServeData;
#include "sce/sifrpc.h"
#include "sce/sifdma.h"
#include "sce/libsdr.h"

static int callbacks;

static void completed(void *parameter) {
    callbacks += *(int *)parameter;
}

int main(void) {
    assert(sceSdRemote(1, rSdInit, 0) == 0);
    assert(sceSdRemote(1, rSdVoiceTransStatus, 1, 0) == 1);
    unsigned char adpcm[16] = {0};
    assert(sceSdRemote(1, rSdVoiceTrans, 1, 0, adpcm, 0u, 16) == 16);
    struct sceSdEffectAttr effect = {.mode = 0x101, .depth_L = 16384, .depth_R = 16384};
    assert(sceSdRemote(1, rSdSetEffectAttr, 0u, &effect) == 0);
    assert(sceSdRemote(1, rSdSetCoreAttr, SD_C_EFFECT_ENABLE, 1u) == 0);
    effect.mode = 10;
    assert(sceSdRemote(1, rSdSetEffectAttr, 0u, &effect) == -1);
    struct sceSifClientData client = {0};
    unsigned char data[64];
    int value = 3;
    assert(sceSifRebootIop("unused") == 1);
    assert(sceSifSyncIop() == 1);
    assert(sceSifLoadModule("unused", 0, NULL) > 0);
    assert(sceSifBindRpc(&client, 42, 0) == 0 && client.server);
    memset(data, 255, sizeof(data));
    assert(sceSifCallRpc(&client, 1, 1, data, 16, data, 64, completed, &value) == 0);
    for (int i = 0; i < 64; ++i) assert(data[i] == 0);
    assert(callbacks == 3 && sceSifCheckStatRpc(&client) == 0);
    void *destination = sceSifAllocIopHeap(64);
    assert(destination);
    memset(data, 73, sizeof(data));
    struct sceSifDmaData transfer = {data, destination, 64, 0};
    int id = sceSifSetDma(&transfer, 1);
    assert(id > 0 && sceSifDmaStat(id) < 0);
    assert(memcmp(data, destination, 64) == 0);
    assert(sceSifFreeIopHeap(destination) == 0);
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
