#define _GNU_SOURCE
#include "port_packets.h"
#include "port_graphics.h"
#include <assert.h>
#include <time.h>

void sceGsSyncVCallback(int (*callback)(int));
static int interrupts;
static int callbacks;

void port_present_frame(void) {}
void audio_frame(void) {}
int render_smoothing(void) { return 0; }
void smooth_until(uint64_t deadline) { (void)deadline; }

void port_kernel_vblank(void) {
    interrupts++;
}

static int vblank(int field) {
    assert(field == (interrupts & 1));
    callbacks++;
    return 0;
}

static void vif_packet(void) {
    uint32_t words[32] = {0};
    PortVifPacket packet = {0};
    sceVif1PkInit(&packet, words);
    sceVif1PkReserve(&packet, 12);
    assert(sceVif1PkTerminate(&packet) == words + 12);
    sceVif1PkReset(&packet);
    assert(packet.current == words && packet.base == words);
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
    pal_timing();
    return 0;
}
