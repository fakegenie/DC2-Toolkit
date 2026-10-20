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

extern PortGraphicsState port_graphics;
void port_graphics_tick(void);
int sceGsSyncV(int mode);
