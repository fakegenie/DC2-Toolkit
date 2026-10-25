#define _POSIX_C_SOURCE 200809L
#include "port_graphics.h"
#include "port_game.h"
#include "audio.h"
#include <stddef.h>
#include <time.h>
#include "port_present.h"
#include "port_stats.h"
#include "port_mod.h"
#include "smooth.h"

extern void port_kernel_vblank(void);

PORT_GAME_BEGIN
uint64_t port_fields;
static int (*vblank_callback)(int);
static uint32_t timer_lines;
PORT_GAME_END
static uint64_t next_field;
uint64_t port_field_period = UINT64_C(20000000);
uint64_t port_ticks;
uint64_t port_field_ticks[PORT_FIELD_LOG];
const uint64_t *port_field_script;
size_t port_field_script_count;
size_t port_field_script_next;
uint64_t port_field_script_base;
enum { LINES_PER_FIELD = 312 };

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

void port_timer_reset(void) {
    timer_lines = 0;
}

uint32_t port_timer_lines(void) {
    return timer_lines;
}

int port_display_field(void) {
    return (int)(port_fields & 1);
}

static void graphics_field(void) {
    port_fields++;
    port_field_ticks[port_fields % PORT_FIELD_LOG] = port_ticks;
    timer_lines += LINES_PER_FIELD;
    port_kernel_vblank();
    if (vblank_callback) vblank_callback((int)(port_fields & 1));
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

static void scripted_tick(void) {
    if (port_ticks < port_field_script_base + port_field_script[port_field_script_next]) return;
    port_field_script_next++;
    uint64_t now = monotonic_ns();
    if (next_field > now) sleep_until(next_field);
    next_field = (next_field > now ? next_field : now) + port_field_period;
    graphics_field();
}

void port_graphics_tick(void) {
    port_ticks++;
    if (port_field_script_next < port_field_script_count) {
        scripted_tick();
        return;
    }
    uint64_t now = monotonic_ns();
    if (!next_field) next_field = now + port_field_period;
    if (now < next_field) return;
    next_field += port_field_period;
    if (now >= next_field) {
        next_field = now + port_field_period;
        port_stats.dropped_fields++;
    }
    graphics_field();
}

void port_graphics_wait(void) {
    uint64_t now = monotonic_ns();
    if (!next_field) next_field = now + port_field_period;
    if (render_smoothing()) smooth_until(next_field);
    else sleep_until(next_field);
    port_graphics_tick();
}

int sceGsSyncV(int mode) {
    if (mode == 1) return (int)(port_fields & 1);
    port_graphics_wait();
    return (int)(port_fields & 1);
}

void sceGsSyncVCallback(int (*callback)(int)) {
    vblank_callback = callback;
}
