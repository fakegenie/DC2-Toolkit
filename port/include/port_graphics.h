#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t port_fields;
extern uint64_t port_field_period;
enum { PORT_FIELD_LOG = 65536 };
extern uint64_t port_ticks;
extern uint64_t port_field_ticks[PORT_FIELD_LOG];
extern const uint64_t *port_field_script;
extern size_t port_field_script_count;
extern size_t port_field_script_next;
extern uint64_t port_field_script_base;
void port_graphics_tick(void);
void port_graphics_wait(void);
void port_timer_reset(void);
uint32_t port_timer_lines(void);
int port_display_field(void);
int sceGsSyncV(int mode);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C"
#endif
void port_depth_download(int x, int y, int width, int height, unsigned *pixels);
