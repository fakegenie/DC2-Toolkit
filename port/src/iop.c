#include <stddef.h>
#include "audio.h"
#include "port_game.h"

typedef struct sceSifRpcHeader sceSifRpcHeader;
typedef struct sceSifServeData sceSifServeData;
#include "sce/sifrpc.h"
#include "sce/sifdma.h"
#include "sce/libsdr.h"

PORT_GAME_BEGIN
static int module_id;
PORT_GAME_END

void sceSifInitRpc(int mode) {}

int sceSifRebootIop(const char *path) {
    module_id = 0;
    audio_reset();
    return 1;
}

int sceSifSyncIop(void) { return 1; }

int sceSifLoadModule(const char *path, int arg_len, const char *args) {
    return ++module_id;
}

void sceSifExitCmd(void) {}

int sceSifBindRpc(struct sceSifClientData *client, unsigned int number, unsigned int mode) { return -1; }

int sceSifCallRpc(struct sceSifClientData *client, unsigned int number, unsigned int mode, void *send, int send_size,
                  void *receive, int receive_size, void (*end_callback)(void *), void *end_parameter) { return -1; }

int sceSifCheckStatRpc(struct sceSifClientData *client) { return 0; }

int sceSifSetDma(struct sceSifDmaData *transfer, int count) { return 0; }

int sceSifDmaStat(int id) { return -1; }

int sceSifInitIopHeap(void) { return -1; }

void *sceSifAllocIopHeap(unsigned int size) { return NULL; }

int sceSifFreeIopHeap(void *address) { return -1; }

void *sceSifAllocSysMemory(int mode, unsigned int size, void *address) { return NULL; }

int sceSdRemote(int arg, int command, ...) { return -1; }
