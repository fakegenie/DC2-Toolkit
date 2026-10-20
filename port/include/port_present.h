#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int port_present_init(int enabled);
void port_present_frame(void);
void port_present_close(void);
void port_present_movie(const uint32_t *rgba, unsigned width, unsigned height);
void port_present_movie_clear(void);
void port_present_show(int target, int width, int height);
int port_display_target(int *width, int *height);
void port_present_save(const char *path);

#ifdef __cplusplus
}
#endif
