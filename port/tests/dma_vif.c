#include "dma_chain.h"
#include "vif.h"
#include "vu1.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

unsigned char port_vu1_code[16384];
unsigned char port_vu1_data[16384];
uint16_t port_vu1_top;
uint16_t port_vu1_itop;
static unsigned starts;
static unsigned continues;
static unsigned gif_bytes;
static unsigned gif_path;

int port_vu1_start(unsigned pc) {
    assert(pc == 7);
    starts++;
    return 0;
}

int port_vu1_continue(void) {
    continues++;
    return 0;
}

int port_gif_feed(const void *data, size_t bytes, unsigned path) {
    assert(data);
    gif_bytes += bytes;
    gif_path = path;
    return 0;
}

static uint32_t lane(unsigned qword, unsigned component) {
    uint32_t value;
    memcpy(&value, port_vu1_data + qword * 16 + component * 4, 4);
    return value;
}

static void vif(void) {
    port_vif_reset();
    uint32_t stream[] = {
        0x01000204, 0x0300003c, 0x02000078, 0x04000009,
        0x6c048000, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        0x14000007, 0x17000000,
        0x4a010003, 0x12345678, 0xabcdef01,
        0x50000001, 0, 0, 0, 0
    };
    for (size_t i = 0; i < sizeof(stream); i++) assert(!port_vif_feed((unsigned char *)stream + i, 1));
    assert(!port_vif_finish());
    assert(lane(60, 0) == 1 && lane(61, 3) == 8);
    assert(lane(64, 0) == 9 && lane(65, 3) == 16);
    assert(starts == 1 && continues == 1 && port_vu1_top == 180 && port_vu1_itop == 9);
    assert(port_vu1_code[24] == 0x78 && port_vu1_code[31] == 0xab);
    assert(gif_bytes == 16 && gif_path == 2);
    port_vif_reset();
    uint32_t masked[] = {
        0x30000000, 10, 20, 30, 40,
        0x31000000, 50, 60, 70, 80,
        0x20000000, 0xe4,
        0x7c01400a, 1, 2, 3, 4
    };
    assert(!port_vif_feed(masked, sizeof(masked)));
    assert(!port_vif_finish());
    assert(lane(10, 0) == 1 && lane(10, 1) == 20 && lane(10, 2) == 50 && lane(10, 3) == 0);
    port_vif_reset();
    uint32_t narrow[] = {0x62010014, 0x000000ff, 0x65010015, 0x00020001, 0x6f010016, 0x0000ffff};
    assert(!port_vif_feed(narrow, sizeof(narrow)));
    assert(!port_vif_finish());
    assert(lane(20, 0) == UINT32_MAX && lane(20, 3) == UINT32_MAX);
    assert(lane(21, 0) == 1 && lane(21, 1) == 2 && lane(21, 2) == 1);
    assert(lane(22, 0) == 248 && lane(22, 3) == 128);
    port_vif_reset();
    uint32_t fill[] = {0x30000000, 20, 30, 40, 50, 0x01000301, 0x6c03001e, 1, 2, 3, 4};
    assert(!port_vif_feed(fill, sizeof(fill)));
    assert(!port_vif_finish());
    assert(lane(30, 0) == 1 && lane(31, 0) == 20 && lane(32, 3) == 50);
    port_vif_reset();
    uint32_t difference[] = {
        0x30000000, 10, 20, 30, 40, 0x05000002,
        0x6c020028, 1, 2, 3, 4, 5, 6, 7, 8
    };
    assert(!port_vif_feed(difference, sizeof(difference)));
    assert(!port_vif_finish());
    assert(lane(40, 0) == 11 && lane(41, 0) == 16 && port_vif.row[3] == 52);
    port_vif_reset();
    uint32_t many[257] = {0x600003ff};
    for (unsigned i = 1; i < 257; i++) many[i] = i;
    assert(!port_vif_feed(many, sizeof(many)));
    assert(!port_vif_finish());
    assert(lane(1023, 0) == 1 && lane(254, 3) == 256);
    port_vif_reset();
    uint32_t short_stream = 0x6c010000;
    assert(!port_vif_feed(&short_stream, 4));
    assert(port_vif_finish() == -1);
    port_vif_reset();
    uint32_t invalid = 0x63010000;
    assert(port_vif_feed(&invalid, 4) == -1);
    port_vif_reset();
}

static void dma(void) {
    _Alignas(16) uint32_t chain[32] = {0};
    _Alignas(16) uint32_t external[4] = {0};
    chain[0] = 0x10000001;
    chain[4] = 0x01000101;
    chain[8] = 0x50000000;
    chain[9] = 0x1060;
    chain[12] = 0x30000001;
    chain[13] = 0x20002000;
    chain[16] = 0x20000000;
    chain[17] = 0x1070;
    chain[24] = 0x60000000;
    chain[28] = 0x70000000;
    port_dma_unmap();
    assert(!port_dma_map(0x1000, chain, sizeof(chain)));
    assert(!port_dma_map(0x2000, external, sizeof(external)));
    assert(!port_dma_chain(1, (void *)(uintptr_t)0x30001000, 0));
    assert(!port_dma_send(2, (void *)(uintptr_t)0x20002000, 1));
    assert(gif_path == 3);
    chain[0] = 0x40000001;
    chain[1] = 0x2000;
    chain[4] = 0x70000000;
    chain[6] = 0x01000203;
    assert(!port_dma_chain(1, (void *)(uintptr_t)0x1000, 1));
    assert(port_vif.cl == 3 && port_vif.wl == 2);
    chain[4] = 0;
    chain[6] = 0;
    chain[0] = 0x00000001;
    chain[1] = 0x80000010;
    assert(!port_dma_map(0x70000000, chain, sizeof(chain)));
    assert(!port_dma_chain(1, (void *)(uintptr_t)0x1000, 0));
    chain[28] = 0x70000002;
    assert(port_dma_chain(1, (void *)(uintptr_t)0x1070, 0) == -1);
    assert(port_dma_chain(1, (void *)(uintptr_t)0x1001, 0) == -1);
    chain[0] = 0x50000000;
    chain[1] = 0x1000;
    assert(port_dma_chain(1, (void *)(uintptr_t)0x1000, 0) == -1);
    port_dma_unmap();
}

int main(void) {
    vif();
    dma();
    return 0;
}
