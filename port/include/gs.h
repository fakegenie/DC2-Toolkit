#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void port_gs_reset(void);
int port_gs_sync(void);
const char *port_gs_error(void);
int port_gif_feed(const void *data, size_t bytes, unsigned path);
int port_gif_submit(const void *data, size_t bytes);
int port_gs_write_register(unsigned address, uint64_t value);
uint64_t port_gs_read_register(unsigned address);
int port_gs_image_load(const uint64_t *packet, const void *source, size_t bytes);
int port_gs_image_store(const uint64_t *packet, void *destination, size_t bytes);
int port_gs_read_rgba(unsigned base256, unsigned width64, unsigned psm,
                      unsigned x, unsigned y, unsigned width, unsigned height,
                      uint32_t *rgba, size_t stride_pixels);

#ifdef __cplusplus
}
#endif
