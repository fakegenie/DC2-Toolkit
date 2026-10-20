#include <stdlib.h>
#include <stdarg.h>
#include "iop_sound.h"
#include <string.h>

typedef struct sceSifRpcHeader sceSifRpcHeader;
typedef struct sceSifServeData sceSifServeData;
#include "sce/sifrpc.h"
#include "sce/sifdma.h"
#include "sce/libsdr.h"

static int module_id;
static unsigned char rpc_server;

void sceSifInitRpc(int mode) {}

int sceSifRebootIop(const char *path) {
    module_id = 0;
    port_sound_reset();
    return 1;
}

int sceSifSyncIop(void) { return 1; }

int sceSifLoadModule(const char *path, int arg_len, const char *args) {
    return ++module_id;
}

void sceSifExitCmd(void) {}

int sceSifBindRpc(struct sceSifClientData *client, unsigned int number, unsigned int mode) {
    if (!client) return -1;
    memset(client, 0, sizeof(*client));
    client->header.rpc_id = number;
    client->server = (sceSifServeData *)&rpc_server;
    return 0;
}

int sceSifCheckStatRpc(struct sceSifClientData *client) { return 0; }

int sceSifCallRpc(struct sceSifClientData *client, unsigned int number, unsigned int mode,
                  void *send, int send_size, void *receive, int receive_size,
                  void (*end_callback)(void *), void *end_parameter) {
    if (!client || !client->server || receive_size < 0 || send_size < 0) return -1;
    client->command = number;
    if (!port_sound_rpc(client->header.rpc_id, number, send, send_size, receive, receive_size) && receive && receive_size) memset(receive, 0, (size_t)receive_size);
    if (end_callback) end_callback(end_parameter);
    return 0;
}

int sceSifInitIopHeap(void) { return 0; }

void *sceSifAllocIopHeap(unsigned int size) { return (void *)(uintptr_t)iop_memory_alloc(size ? size : 1); }

int sceSifFreeIopHeap(void *address) {
    iop_memory_free(address);
    return 0;
}

void *sceSifAllocSysMemory(int mode, unsigned int size, void *address) {
    if (mode == 2) return NULL;
    return sceSifAllocIopHeap(size);
}

int sceSifFreeSysMemory(void *address) { return sceSifFreeIopHeap(address); }

int sceSifSetDma(struct sceSifDmaData *transfer, int count) {
    if (!transfer || count <= 0) return 0;
    for (int i = 0; i < count; ++i) {
        if (transfer[i].size < 0 || !transfer[i].data || !transfer[i].addr) return 0;
    }
    for (int i = 0; i < count; ++i) {
        memmove(transfer[i].addr, transfer[i].data, (size_t)transfer[i].size);
    }
    return 1;
}

int sceSifDmaStat(int id) { return -1; }

int sceSdRemoteInit(void) { return 0; }

int sceSdRemote(int arg, int command, ...) {
    va_list arguments;
    va_start(arguments, command);
    int result = port_sound_remote(command, arguments);
    va_end(arguments);
    return result;
}
