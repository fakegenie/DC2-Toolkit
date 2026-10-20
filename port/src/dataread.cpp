#include "dataread.hpp"
#include "port_overrides.h"
#include <cstdio>
#include <cstring>

int LoadFile2Retail(char *path, void *buffer, int *out_size, int mode);
int LoadFileBGRetail(char *name, u_long128 *buffer, int *out_size);
extern BG_READ_INFO bg_read_info[BG_READ_MAX];

static int member_path(const char *path, char *out) {
    if (!path || !*path || strchr(path, ':') || GetMainFileDev() != FILE_DEV_CDROM) return 0;
    char directory[256];
    GetCurrentDir(directory);
    return snprintf(out, 256, "%s%s", directory, path) < 256;
}

static int member_override(const char *path, char *out) {
    if (!port_overrides_count() || !member_path(path, out)) return -1;
    return port_overrides_find(out);
}

int LoadFile2(char *path, void *buffer, int *out_size, int mode) {
    char full[256];
    int index = SearchFileCache(path, NULL) ? -1 : member_override(path, full);
    if (index < 0) return LoadFile2Retail(path, buffer, out_size, mode);
    int size = (int)port_overrides_size(index);
    if (out_size) *out_size = size;
    if (mode == LOAD_FILE_SIZE) return 1;
    return port_overrides_read(index, 0, buffer, (size_t)size);
}

int LoadFileBG(char *name, u_long128 *buffer, int *out_size) {
    char full[256];
    int index = member_override(name, full);
    if (index < 0) return LoadFileBGRetail(name, buffer, out_size);
    for (int i = 0; i < BG_READ_MAX; i++) {
        BG_READ_INFO *info = &bg_read_info[i];
        if (info->busy) continue;
        int size = (int)port_overrides_size(index);
        if (!port_overrides_read(index, 0, buffer, (size_t)size)) return 0;
        strcpy(info->name, full);
        info->busy = 1;
        info->dev = FILE_DEV_CDROM;
        info->issued = 1;
        info->done = 1;
        info->buffer = buffer;
        info->size = size;
        info->sector = 0;
        info->sectors = size_to_sector(size);
        if (out_size) *out_size = size;
        return 1;
    }
    return 0;
}
