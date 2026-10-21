#include "port_debug.h"
#include "port_kernel.h"
#include "port_stats.h"

PortStats port_stats;

__attribute__((weak)) void port_kernel_yield(void) {}

__attribute__((weak)) void debug_start(void) {}

__attribute__((weak)) int debug_build(void) { return 0; }

__attribute__((weak)) int debug_event(const void *event) {
    (void)event;
    return 0;
}

__attribute__((weak)) void debug_field(void) {}

__attribute__((weak)) void debug_frame_enter(void) {}

__attribute__((weak)) void debug_frame_drawn(void) {}

__attribute__((weak)) void debug_overlay(void) {}

__attribute__((weak)) void debug_audio(const short *samples, int frames) {
    (void)samples;
    (void)frames;
}

__attribute__((weak)) void debug_frame_exit(void) {}

__attribute__((weak)) void debug_present(int target, int width, int height) {
    (void)target;
    (void)width;
    (void)height;
}

__attribute__((weak)) void debug_input(struct port_input_state *state) {
    (void)state;
}
