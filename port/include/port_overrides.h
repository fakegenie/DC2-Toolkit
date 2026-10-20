#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void port_overrides_scan(void);
int port_overrides_count(void);
const char *port_overrides_folder(void);
const char *port_overrides_path(int index);
int port_overrides_find(const char *path);
uint32_t port_overrides_size(int index);
int port_overrides_read(int index, uint32_t offset, void *buffer, size_t size);

#ifdef __cplusplus
}
#endif
