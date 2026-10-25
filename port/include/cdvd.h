#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sce/libcdvd.h"

enum { CDVD_SECTOR_SIZE = 2048 };

int port_cdvd_init(const char *path);
void port_cdvd_close(void);
int port_cdvd_find(const char *path, struct sceCdlFILE *file);
int port_cdvd_read_file(const struct sceCdlFILE *file, uint64_t offset, void *buffer, size_t size);
void port_cdvd_log_files(int enabled);
