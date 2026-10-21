#pragma once

#include <stddef.h>
#include <stdint.h>

struct sceCdlFILE {
    uint32_t lsn;
    uint32_t size;
    char name[16];
    unsigned char date[8];
    uint32_t flag;
};

struct sceCdRMode {
    unsigned char trycount;
    unsigned char spindlctrl;
    unsigned char datapattern;
    unsigned char pad;
};

struct sce_stat {
    uint32_t st_mode;
    uint32_t st_attr;
    uint32_t st_size;
    unsigned char st_ctime[8];
    unsigned char st_atime[8];
    unsigned char st_mtime[8];
    uint32_t st_hisize;
    uint32_t st_private[6];
};

int port_cdvd_init(const char *path);
void port_cdvd_close(void);
int port_cdvd_find(const char *path, struct sceCdlFILE *file);
int port_cdvd_read_file(const struct sceCdlFILE *file, uint64_t offset, void *buffer, size_t size);
void port_cdvd_log_files(int enabled);
