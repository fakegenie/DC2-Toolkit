#pragma once
#include <stdint.h>

int port_present_init(int enabled);
void port_present_frame(void);
void port_present_close(void);
void port_present_movie(const uint32_t *rgba, unsigned width, unsigned height);
void port_present_movie_clear(void);

