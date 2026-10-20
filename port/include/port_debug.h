#pragma once

#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

void debug_start(void);
int debug_build(void);
int debug_event(const void *event);
void debug_field(void);
void debug_frame_enter(void);
void debug_frame_drawn(void);
void debug_frame_exit(void);
void debug_present(int target, int width, int height);
void debug_input(struct port_input_state *state);
void debug_host(void);

#ifdef __cplusplus
}
#endif
