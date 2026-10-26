#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void port_overrides_scan(void);
int port_overrides_count(void);
const char *port_data_folder(void);
const char *port_overrides_folder(void);
const char *port_overrides_path(int index);
int port_overrides_find(const char *path);
uint32_t port_overrides_size(int index);
int port_overrides_read(int index, uint32_t offset, void *buffer, size_t size);
const char *port_paks_folder(void);
int port_paks_list(void);
const char *port_pak_text(int index, const char *key);
int port_pak_files(int index);
int port_pak_clash(int a, int b, char *why, size_t size);
int port_pak_enabled(int index);

#ifdef __cplusplus
}
#endif
