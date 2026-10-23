#define _POSIX_C_SOURCE 200809L
#include "smooth.h"
#include "port_mod.h"
#include "render.h"
#include "port_stats.h"
#include <time.h>

enum { INTERVAL_LOW = 10000000, INTERVAL_HIGH = 200000000 };

static uint64_t presented_at;
static uint64_t shown_at;
static uint64_t interval = 40000000;
static uint64_t next_refresh;
static int rate;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint64_t refresh_period(void) {
    int display = render_refresh_rate();
    int hz = rate > 0 && rate < display ? rate : display;
    return UINT64_C(1000000000) / (uint64_t)(hz > 0 ? hz : 60);
}

void smooth_set_rate(int hz) {
    rate = hz;
}

void smooth_presented(void) {
    uint64_t now = monotonic_ns();
    uint64_t gap = now - presented_at;
    if (presented_at && gap > INTERVAL_LOW && gap < INTERVAL_HIGH) interval = (interval * 3 + gap) / 4;
    presented_at = now;
    next_refresh = shown_at + refresh_period() > now ? shown_at + refresh_period() : now + refresh_period();
}

int smooth_due(void) { return monotonic_ns() >= shown_at + refresh_period(); }

void smooth_shown(void) { shown_at = monotonic_ns(); }

void smooth_until(uint64_t deadline) {
    RenderDisplay display;
    render_display(&display);
    uint64_t margin = display.vsync ? refresh_period() : refresh_period() / 2;
    uint64_t start = monotonic_ns();
    for (uint64_t now = start; now < deadline; now = monotonic_ns()) {
        if (now >= next_refresh && now + margin < deadline) {
            render_replay((float)(now - presented_at) / (float)interval);
            port_stats.replay_ns += monotonic_ns() - now;
            next_refresh += refresh_period();
            if (next_refresh <= now) next_refresh = now + refresh_period();
            continue;
        }
        uint64_t until = next_refresh > now && next_refresh < deadline ? next_refresh : deadline;
        struct timespec pause = {(time_t)((until - now) / UINT64_C(1000000000)), (long)((until - now) % UINT64_C(1000000000))};
        nanosleep(&pause, NULL);
    }
    port_stats.wait_ns += monotonic_ns() - start;
}
