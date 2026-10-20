#pragma once
#include <stdint.h>

typedef struct {
    uint64_t display[5];
    uint64_t draw[30];
    int buffer;
    short mode;
    short interlace;
    short output;
    short frame_mode;
    uint64_t fields;
} PortGraphicsState;

#ifdef __cplusplus
extern "C" {
#endif
extern PortGraphicsState port_graphics;
extern uint64_t port_field_period;
void port_graphics_tick(void);
int sceGsSyncV(int mode);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C"
#endif
void port_depth_download(int x, int y, int width, int height, unsigned *pixels);
