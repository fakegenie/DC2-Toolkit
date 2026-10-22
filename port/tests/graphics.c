#define _GNU_SOURCE
#include "port_packets.h"
#include "port_graphics.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "port_window.h"

void sceGsSetDefDBuff(void *, int, int, int, int, int, int);
void sceGsSwapDBuff(const void *, int);
void sceGsResetGraph(short, short, short, short);
void sceGsSetDefLoadImage(uint64_t *, short, short, short, short, short, short, short);
void sceGsSetDefStoreImage(uint64_t *, short, short, short, short, short, short, short);
void sceGsSyncVCallback(int (*callback)(int));
void *sceDmaGetChan(int);
int sceDmaSend(void *, void *);
static int interrupts;
static int callbacks;

void port_present_frame(void) {}
void audio_frame(void) {}
int render_smoothing(void) { return 0; }
void smooth_until(uint64_t deadline) { (void)deadline; }

void port_depth_download(int x, int y, int width, int height, unsigned *pixels) {
    (void)x;
    (void)y;
    memset(pixels, 0, (size_t)width * (size_t)height * sizeof *pixels);
}

void port_kernel_vblank(void) {
    interrupts++;
}

static int vblank(int field) {
    assert(field == (interrupts & 1));
    callbacks++;
    return 0;
}

static void vif_packet(void) {
    _Alignas(16) uint32_t words[32] = {0};
    PortVifPacket packet = {0};
    sceVif1PkInit(&packet, words);
    sceVif1PkCnt(&packet, 0);
    sceVif1PkOpenDirectCode(&packet, 0);
    sceVif1PkOpenGifTag(&packet, (PortQword)14 << 64 | (PortQword)1 << 60 | 0x8000);
    sceVif1PkAddGsAD(&packet, 0x4c, UINT64_C(0x123456789abcdef0));
    sceVif1PkCloseGifTag(&packet);
    sceVif1PkCloseDirectCode(&packet);
    sceVif1PkEnd(&packet, 0);
    assert(sceVif1PkTerminate(&packet) == words + 16);
    const uint32_t expected[16] = {
        0x10000002, 0, 0, 0x50000002,
        0x8001, 0x10000000, 14, 0,
        0x9abcdef0, 0x12345678, 0x4c, 0,
        0x70000000, 0, 0, 0
    };
    assert(memcmp(words, expected, sizeof(expected)) == 0);
    sceVif1PkReset(&packet);
    assert(packet.current == words && !packet.dma);
    sceVif1PkCall(&packet, (PortQword *)(uintptr_t)0x23456780, 0);
    sceVif1PkTerminate(&packet);
    assert(words[0] == 0x50000000 && words[1] == 0x03456780);
}

static void gif_packet(void) {
    _Alignas(16) uint32_t words[48] = {0};
    PortGifPacket packet = {0};
    sceGifPkInit(&packet, (PortQword *)words);
    sceGifPkCnt(&packet, 0, 0, 0);
    sceGifPkOpenGifTag(&packet, (PortQword)14 << 64 | (PortQword)1 << 60 | 0x8000);
    sceGifPkAddGsAD(&packet, 0x50, UINT64_C(0x1122334455667788));
    sceGifPkCloseGifTag(&packet);
    sceGifPkRef(&packet, (PortQword *)(uintptr_t)0x23456780, 17, 0, 0, 0);
    sceGifPkEnd(&packet, 0, 0, 0);
    assert(sceGifPkTerminate(&packet) == words + 20);
    const uint32_t expected[20] = {
        0x10000002, 0, 0, 0,
        0x8001, 0x10000000, 14, 0,
        0x55667788, 0x11223344, 0x50, 0,
        0x30000011, 0x03456780, 0, 0,
        0x70000000, 0, 0, 0
    };
    assert(memcmp(words, expected, sizeof(expected)) == 0);
    sceGifPkReset(&packet);
    sceGifPkOpenGifTag(&packet, (PortQword)2 << 60 | (PortQword)1 << 58);
    uint32_t *data = sceGifPkReserve(&packet, 6);
    for (int i = 0; i < 6; i++) data[i] = (uint32_t)i + 1;
    sceGifPkCloseGifTag(&packet);
    assert(words[0] == 2 && packet.current == words + 12);
    assert(words[10] == 0 && words[11] == 0);
}

static void graph_packets(void) {
    uint64_t db[70];
    sceGsResetGraph(0, 1, 3, 1);
    sceGsSetDefDBuff(db, 0, 640, 448, 2, 49, 1);
    assert(db[10] == UINT64_C(0x100000000000800e));
    assert(db[11] == 14);
    assert(db[12] == ((uint64_t)10 << 16 | 140));
    assert(db[14] == ((uint64_t)1 << 24 | 280));
    assert(db[26] == 0x50000);
    assert(db[40] == db[10]);
    sceGsSwapDBuff(db, 1);
    assert(port_graphics.buffer == 1);
    assert(memcmp(port_graphics.display, db + 5, 40) == 0);
    assert(memcmp(port_graphics.draw, db + 40, 240) == 0);
    assert(port_graphics.draw[28] == 0x50000 && port_graphics.draw[29] == 0x47);
    uint64_t image[14];
    sceGsSetDefLoadImage(image, 512, 10, 0, 12, 34, 128, 256);
    assert(image[2] == ((uint64_t)512 << 32 | (uint64_t)10 << 48));
    assert(image[4] == ((uint64_t)12 << 32 | (uint64_t)34 << 48));
    assert(image[10] == (UINT64_C(0x0800000000008000) | 8192));
    sceGsSetDefStoreImage(image, 512, 10, 0, 12, 34, 128, 256);
    assert(image[4] == (512 | (uint64_t)10 << 16));
    assert(image[6] == (12 | (uint64_t)34 << 16));
    assert(image[12] == 1 && image[13] == 0x53);
}

static void dma_registers(void) {
    uint32_t *channel = sceDmaGetChan(1);
    channel[0] = 0x80ff;
    channel[8] = 123;
    assert(sceDmaSend(channel, (void *)(uintptr_t)0x23456780) == 0);
    assert(channel[0] == 0x80f7);
    assert(channel[8] == 0 && channel[12] == 0x23456780);
    assert(sceDmaGetChan(-1) == NULL && sceDmaGetChan(10) == NULL);
}

static void pal_timing(void) {
    struct timespec before, after;
    sceGsSyncVCallback(vblank);
    clock_gettime(CLOCK_MONOTONIC, &before);
    for (int i = 0; i < 5; i++) assert(sceGsSyncV(0) == ((i + 1) & 1));
    clock_gettime(CLOCK_MONOTONIC, &after);
    double elapsed = after.tv_sec - before.tv_sec + (after.tv_nsec - before.tv_nsec) * 1e-9;
    assert(elapsed >= 0.095 && elapsed < 0.5);
    assert(interrupts == 5 && callbacks == 5);
}

int main(void) {
    vif_packet();
    gif_packet();
    graph_packets();
    dma_registers();
    pal_timing();
    return 0;
}
