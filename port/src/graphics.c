#define _POSIX_C_SOURCE 200809L
#include "port_graphics.h"
#include "port_window.h"
#include "audio.h"
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "port_present.h"
#include "port_stats.h"

extern void port_kernel_vblank(void);

PortGraphicsState port_graphics;
static int (*vblank_callback)(int);
static uint64_t next_field;
uint64_t port_field_period = UINT64_C(20000000);
static uint32_t dma_channels[10][36];
static unsigned char dma_environment[24];
static int dma_enabled;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void graphics_field(void) {
    port_graphics.fields++;
    *(volatile uint64_t *)(PORT_WINDOW + 0x12001000) = (port_graphics.fields & 1) << 13;
    *(volatile uint32_t *)(PORT_WINDOW + 0x10000000) += 312;
    port_kernel_vblank();
    if (vblank_callback) vblank_callback((int)(port_graphics.fields & 1));
    audio_frame();
    port_present_frame();
}

static void sleep_until(uint64_t deadline) {
    uint64_t start = monotonic_ns();
    for (uint64_t now = start; now < deadline; now = monotonic_ns()) {
        struct timespec pause = {(time_t)((deadline - now) / UINT64_C(1000000000)),
                                 (long)((deadline - now) % UINT64_C(1000000000))};
        nanosleep(&pause, NULL);
    }
    port_stats.wait_ns += monotonic_ns() - start;
}

void port_graphics_tick(void) {
    uint64_t now = monotonic_ns();
    if (!next_field) next_field = now + port_field_period;
    if (now < next_field) return;
    next_field += port_field_period;
    if (now >= next_field) next_field = now + port_field_period;
    graphics_field();
}

void port_graphics_wait(void) {
    uint64_t now = monotonic_ns();
    if (!next_field) next_field = now + port_field_period;
    sleep_until(next_field);
    port_graphics_tick();
}

int sceGsSyncV(int mode) {
    if (mode == 1) return (int)(port_graphics.fields & 1);
    port_graphics_wait();
    return (int)(port_graphics.fields & 1);
}

void sceGsSyncVCallback(int (*callback)(int)) {
    vblank_callback = callback;
}

void sceGsResetGraph(short mode, short inter, short output, short frame_mode) {
    port_graphics.mode = mode;
    port_graphics.interlace = inter;
    port_graphics.output = output;
    port_graphics.frame_mode = frame_mode;
}

void sceGsResetPath(void) {}

int sceGsSyncPath(int mode, unsigned short timeout) {
    (void)mode;
    (void)timeout;
    return 0;
}

static uint64_t gs_test(int test) {
    return test ? UINT64_C(0x10000) | (uint64_t)(test & 3) << 17 : UINT64_C(0x30000);
}

void sceGsSetDefDBuff(void *buffer, int psm, int width, int height, int test, int zpsm, int clear) {
    uint64_t *db = buffer;
    memset(db, 0, 0x230);
    int pages = ((width + 63) / 64) * ((height + ((psm & 2) ? 63 : 31)) / ((psm & 2) ? 64 : 32));
    if (!(port_graphics.interlace == 1 && port_graphics.frame_mode == 0)) pages *= 2;
    for (int i = 0; i < 2; i++) {
        uint64_t *disp = db + i * 5;
        disp[0] = 0x66;
        disp[1] = port_graphics.interlace ? (port_graphics.frame_mode ? 3 : 1) : 2;
        disp[2] = (uint64_t)((width + 63) / 64) << 9 | (uint64_t)(psm & 15) << 15;
        if (i) disp[2] |= (uint64_t)(pages / 2);
        int magnify = (width + 2559) / width;
        int dx = (port_graphics.output == 2 ? 0x27c : 0x290);
        int dy = (port_graphics.output == 2 ? 50 : 72);
        if (!port_graphics.interlace) dy /= 2;
        int display_height = port_graphics.interlace && port_graphics.frame_mode ? height * 2 : height;
        disp[3] = (uint64_t)dx | (uint64_t)dy << 12 | (uint64_t)(magnify - 1) << 23 |
                  (uint64_t)(width * magnify - 1) << 32 | (uint64_t)(display_height - 1) << 44;
        uint64_t *tag = db + 10 + i * 30;
        tag[0] = UINT64_C(0x1000000000008000) | (clear ? 14 : 8);
        tag[1] = 14;
        uint64_t *draw = tag + 2;
        static const uint64_t regs[8] = {0x4c, 0x4e, 0x18, 0x40, 0x1a, 0x46, 0x45, 0x47};
        for (int j = 0; j < 8; j++) draw[j * 2 + 1] = regs[j];
        draw[0] = (uint64_t)((width + 63) / 64) << 16 | (uint64_t)(psm & 15) << 24;
        if (!i && (!port_graphics.interlace || port_graphics.frame_mode)) draw[0] |= (uint64_t)(pages / 2);
        draw[2] = (uint64_t)pages | (uint64_t)(zpsm & 15) << 24 | (test ? 0 : UINT64_C(1) << 32);
        draw[4] = (uint64_t)((2048 - width / 2) * 16) | (uint64_t)((2048 - height / 2) * 16) << 32;
        draw[6] = (uint64_t)(width - 1) << 16 | (uint64_t)(height - 1) << 48;
        draw[8] = draw[10] = 1;
        draw[12] = (psm & 2) ? 1 : 0;
        draw[14] = gs_test(test);
        if (clear) {
            uint64_t *c = draw + 16;
            c[0] = 0x30000; c[1] = 0x47;
            c[2] = 6; c[3] = 0;
            c[4] = UINT64_C(0x3f80000000000000); c[5] = 1;
            c[6] = (uint64_t)((2048 - width / 2) * 16) | (uint64_t)((2048 - height / 2) * 16) << 16;
            c[7] = 5;
            c[8] = (uint64_t)((2048 + width / 2) * 16) | (uint64_t)((2048 + height / 2) * 16) << 16;
            c[9] = 5; c[10] = gs_test(test); c[11] = 0x47;
        }
    }
}

void sceGsSwapDBuff(const void *buffer, int id) {
    const uint64_t *db = buffer;
    port_graphics.buffer = id & 1;
    memcpy(port_graphics.display, db + (id & 1) * 5, sizeof(port_graphics.display));
    memcpy(port_graphics.draw, db + 10 + (id & 1) * 30, sizeof(port_graphics.draw));
}

void sceGsSetHalfOffset(void *environment, short x, short y, short field) {
    uint64_t *draw = environment;
    draw[4] = (uint64_t)(uint16_t)(x * 16) | (uint64_t)(uint16_t)(y * 16 + (field ? 8 : 0)) << 32;
}

static unsigned image_qwords(int psm, int width, int height) {
    unsigned pixels = (unsigned)width * (unsigned)height;
    if (psm == 1 || psm == 49) return (pixels * 3 + 15) / 16;
    if (psm == 2 || psm == 10 || psm == 50 || psm == 58) return (pixels + 7) / 8;
    if (psm == 19 || psm == 27) return (pixels + 15) / 16;
    if (psm == 20 || psm == 36 || psm == 44) return (pixels + 31) / 32;
    return (pixels + 3) / 4;
}

void sceGsSetDefLoadImage(uint64_t *packet, short bp, short bw, short psm, short x, short y, short width, short height) {
    unsigned qwords = image_qwords(psm, width, height);
    if (qwords > 0x7fff) return;
    memset(packet, 0, 96);
    packet[0] = UINT64_C(0x1000000000000004); packet[1] = 14;
    packet[2] = (uint64_t)(uint16_t)bp << 32 | (uint64_t)(uint16_t)bw << 48 | (uint64_t)(uint16_t)psm << 56;
    packet[3] = 0x50;
    packet[4] = (uint64_t)(uint16_t)x << 32 | (uint64_t)(uint16_t)y << 48; packet[5] = 0x51;
    packet[6] = (uint16_t)width | (uint64_t)(uint16_t)height << 32; packet[7] = 0x52;
    packet[9] = 0x53;
    packet[10] = UINT64_C(0x0800000000008000) | qwords;
}

void sceGsSetDefStoreImage(uint64_t *packet, short bp, short bw, short psm, short x, short y, short width, short height) {
    memset(packet, 0, 112);
    packet[0] = UINT64_C(0x0600800000000000);
    packet[1] = UINT64_C(0x5000000613000000);
    packet[2] = UINT64_C(0x1000000000008005); packet[3] = 14;
    packet[4] = (uint16_t)bp | (uint64_t)(uint16_t)bw << 16 | (uint64_t)(uint16_t)psm << 24;
    packet[5] = 0x50;
    packet[6] = (uint16_t)x | (uint64_t)(uint16_t)y << 16; packet[7] = 0x51;
    packet[8] = (uint16_t)width | (uint64_t)(uint16_t)height << 32; packet[9] = 0x52;
    packet[11] = 0x61; packet[12] = 1; packet[13] = 0x53;
}

void sceGsExecLoadImage(const void *packet, const void *source) {
    (void)packet;
    (void)source;
}

void sceGsExecStoreImage(const uint64_t *packet, void *destination) {
    int psm = (int)(packet[4] >> 24) & 63;
    int width = (short)packet[8], height = (short)(packet[8] >> 32);
    unsigned size = image_qwords(psm, width, height) * 16;
    memset(destination, 0, size);
    if (psm == 48 || psm == 49)
        port_depth_download((short)packet[6], (short)(packet[6] >> 16), width, height, destination);
}

void *sceDmaGetChan(int channel) {
    return (unsigned)channel < 10 ? dma_channels[channel] : NULL;
}

int sceDmaReset(int mode) {
    int previous = dma_enabled;
    memset(dma_channels, 0, sizeof(dma_channels));
    memset(dma_environment, 0, sizeof(dma_environment));
    if (mode == 1) dma_enabled = 1;
    return previous;
}

int sceDmaGetEnv(void *environment) {
    memcpy(environment, dma_environment, sizeof(dma_environment));
    return 0;
}

int sceDmaPutEnv(const void *environment) {
    memcpy(dma_environment, environment, sizeof(dma_environment));
    return 0;
}

int sceDmaSend(void *channel, void *data) {
    uint32_t *registers = channel;
    registers[12] = port_address(data) & UINT32_C(0x7fffffff);
    registers[8] = 0;
    registers[0] = (registers[0] & ~UINT32_C(0x10c)) | 5;
    (void)data;
    return 0;
}

int sceDmaSync(void *channel, int mode, int timeout) {
    (void)channel;
    (void)mode;
    (void)timeout;
    return 0;
}
