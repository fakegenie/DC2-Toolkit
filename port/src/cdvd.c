#define _POSIX_C_SOURCE 200809L
#include "cdvd.h"
#include "port_kernel.h"
#include "port_overrides.h"
#include "port_stats.h"
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#undef st_ctime
#undef st_atime
#undef st_mtime

static int disc = -1;
static uint64_t disc_size;
static struct sceCdlFILE root;
static int cd_error;
static char missing_paths[64][1024];
static size_t missing_count;
static unsigned char archive_index[1024 * 1024];
static uint32_t archive_size;
static uint32_t archive_sector;
static uint32_t archive_last_sector;
static int archive_checked;
static struct {
    int used;
    struct sceCdlFILE file;
    uint32_t position;
    int override;
} handles[32];

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_disc(uint64_t offset, void *buffer, size_t size) {
    if (disc < 0 || offset > disc_size || size > disc_size - offset || (size && !buffer)) return 0;
    port_stats.disc_reads++;
    port_stats.disc_bytes += (uint32_t)size;
    unsigned char *out = buffer;
    while (size) {
        ssize_t result = pread(disc, out, size, (off_t)offset);
        if (result <= 0) return 0;
        out += result;
        offset += (size_t)result;
        size -= (size_t)result;
    }
    return 1;
}

static int record_file(const unsigned char *record, struct sceCdlFILE *file) {
    if (record[0] < 34 || record[32] > record[0] - 33 || record[25] & 0x80) return 0;
    memset(file, 0, sizeof(*file));
    file->lsn = le32(record + 2);
    file->size = le32(record + 10);
    file->flag = record[25];
    size_t length = record[32];
    if (length >= sizeof(file->name)) length = sizeof(file->name) - 1;
    memcpy(file->name, record + 33, length);
    memcpy(file->date, record + 18, 7);
    return (uint64_t)file->lsn * 2048 <= disc_size && file->size <= disc_size - (uint64_t)file->lsn * 2048;
}

void port_cdvd_close(void) {
    if (disc >= 0) close(disc);
    disc = -1;
    disc_size = 0;
    memset(&root, 0, sizeof(root));
    memset(handles, 0, sizeof(handles));
    missing_count = 0;
    archive_size = 0;
    archive_checked = 0;
    archive_last_sector = UINT32_MAX;
}

int port_cdvd_init(const char *path) {
    port_cdvd_close();
    if (!path) return 0;
    disc = open(path, O_RDONLY);
    struct stat info;
    if (disc < 0 || fstat(disc, &info) || info.st_size < 17 * 2048) goto fail;
    disc_size = (uint64_t)info.st_size;
    unsigned char sector[2048];
    for (uint32_t number = 16; number < 256; number++) {
        if (!read_disc((uint64_t)number * 2048, sector, sizeof(sector))) break;
        if (memcmp(sector + 1, "CD001", 5) || sector[6] != 1) break;
        if (sector[0] == 255) break;
        if (sector[0] != 1) continue;
        if (sector[128] != 0 || sector[129] != 8 || !record_file(sector + 156, &root) || !(root.flag & 2)) break;
        cd_error = 0;
        fprintf(stderr, "disc: %s (%llu bytes)\n", path, (unsigned long long)disc_size);
        return 1;
    }
fail:
    fprintf(stderr, "cannot read ISO 9660 image: %s\n", path);
    port_cdvd_close();
    return 0;
}

static int same_name(const unsigned char *record, const char *name, size_t length) {
    size_t record_length = record[32];
    for (size_t i = 0; i < record_length; i++) {
        if (record[33 + i] == ';') {
            record_length = i;
            break;
        }
    }
    if (record_length != length) return 0;
    for (size_t i = 0; i < length; i++) {
        if (toupper(record[33 + i]) != toupper((unsigned char)name[i])) return 0;
    }
    return 1;
}

int port_cdvd_find(const char *path, struct sceCdlFILE *file) {
    if (disc < 0 || !path || !file) return 0;
    if (!strncmp(path, "cdrom0:", 7)) path += 7;
    else if (!strncmp(path, "cdrom:", 6)) path += 6;
    if (strchr(path, ':')) return 0;
    struct sceCdlFILE current = root;
    while (*path == '/' || *path == '\\') path++;
    if (!*path) return 0;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/' && *end != '\\') end++;
        size_t length = (size_t)(end - path);
        const char *version = memchr(path, ';', length);
        if (version) {
            if (end - version != 2 || version[1] != '1') return 0;
            length = (size_t)(version - path);
        }
        if (!length || length > 255 || (length == 1 && *path == '.') || (length == 2 && !memcmp(path, "..", 2))) return 0;
        if (!(current.flag & 2)) return 0;
        int found = 0;
        unsigned char sector[2048];
        for (uint64_t offset = 0; offset < current.size; offset += 2048) {
            if (!read_disc((uint64_t)current.lsn * 2048 + offset, sector, sizeof(sector))) return 0;
            size_t limit = current.size - offset < 2048 ? (size_t)(current.size - offset) : 2048;
            for (size_t position = 0; position < limit && sector[position]; position += sector[position]) {
                const unsigned char *record = sector + position;
                if (record[0] < 34 || record[0] > limit - position || record[32] > record[0] - 33) return 0;
                if (same_name(record, path, length)) {
                    if (!record_file(record, &current)) return 0;
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return 0;
        path = end;
        while (*path == '/' || *path == '\\') path++;
    }
    *file = current;
    return 1;
}

int port_cdvd_read_file(const struct sceCdlFILE *file, uint64_t offset, void *buffer, size_t size) {
    return file && offset <= file->size && size <= file->size - offset && read_disc((uint64_t)file->lsn * 2048 + offset, buffer, size);
}

static void log_file(const char *path, const struct sceCdlFILE *file) {
    fprintf(stderr, "file: %s (%u bytes)\n", path, file->size);
}

static void log_archive_read(uint32_t sector) {
    if (!archive_checked) {
        archive_checked = 1;
        struct sceCdlFILE data;
        struct sceCdlFILE index;
        if (port_cdvd_find("DATA.DAT", &data) && port_cdvd_find("DATA.HD4", &index) &&
            index.size >= 12 && index.size <= sizeof(archive_index) &&
            port_cdvd_read_file(&index, 0, archive_index, index.size)) {
            archive_size = index.size;
            archive_sector = data.lsn;
        }
    }
    if (!archive_size || sector == archive_last_sector || sector < archive_sector) return;
    uint32_t entries_size = le32(archive_index);
    if (entries_size > archive_size || entries_size % 12) return;
    for (uint32_t offset = 0; offset < entries_size; offset += 12) {
        const unsigned char *entry = archive_index + offset;
        if (le32(entry + 8) != sector - archive_sector) continue;
        uint32_t name = le32(entry);
        if (name < entries_size || name >= archive_size ||
            !memchr(archive_index + name, 0, archive_size - name)) return;
        fprintf(stderr, "archive: %s (%u bytes)\n", archive_index + name, le32(entry + 4));
        archive_last_sector = sector;
        return;
    }
}

static void log_missing(const char *path) {
    if (!path) return;
    for (size_t i = 0; i < missing_count; i++) {
        if (!strcmp(path, missing_paths[i])) return;
    }
    size_t length = strlen(path);
    if (missing_count < 64 && length < sizeof(missing_paths[0])) {
        memcpy(missing_paths[missing_count++], path, length + 1);
    }
    fprintf(stderr, "file missing: %s\n", path);
}

int sceCdInit(int mode) { (void)mode; return disc >= 0; }
int sceCdMmode(int media) { return media == 1 || media == 2; }
int sceCdSearchFile(struct sceCdlFILE *file, const char *name) {
    if (!port_cdvd_find(name, file) || file->flag & 2) {
        log_missing(name);
        return 0;
    }
    log_file(name, file);
    return 1;
}
int sceCdRead(unsigned int lsn, unsigned int sectors, void *buffer, struct sceCdRMode *mode) {
    if (mode && mode->datapattern) { cd_error = 0x22; return 0; }
    int result = read_disc((uint64_t)lsn * 2048, buffer, (size_t)sectors * 2048);
    cd_error = result ? 0 : 0x30;
    if (result && sectors) log_archive_read(lsn);
    return result;
}
int sceCdSync(int mode) {
    if (mode == 0) port_kernel_yield();
    return 0;
}
int sceCdGetError(void) { return cd_error; }
int sceCdBreak(void) { cd_error = 0; return 1; }
int sceCdSeek(unsigned int lsn) { return disc >= 0 && (uint64_t)lsn * 2048 < disc_size; }
int sceCdDiskReady(int mode) { (void)mode; return disc >= 0 ? 2 : 6; }
int sceCdStInit(int sectors, int banks, void *buffer) { return sectors > 0 && banks > 0 && buffer != NULL; }
int sceCdStStart(unsigned int sector, struct sceCdRMode *mode) { (void)mode; return sceCdSeek(sector); }
int sceCdStSeekF(unsigned int sector) { return sceCdSeek(sector); }
int sceCdStStop(void) { return 1; }
int sceCdStRead(unsigned int sectors, void *buffer, unsigned int mode, unsigned int *error) {
    (void)sectors; (void)buffer; (void)mode;
    if (error) *error = 0;
    return 0;
}

void sceFsReset(void) { memset(handles, 0, sizeof(handles)); }
int sceOpen(const char *name, int flags, ...) {
    struct sceCdlFILE file;
    if ((flags & 3) != 1 || flags & (0x200 | 0x400)) return -1;
    int override = port_overrides_find(name);
    memset(&file, 0, sizeof file);
    file.size = port_overrides_size(override);
    if (override < 0 && (!port_cdvd_find(name, &file) || file.flag & 2)) {
        log_missing(name);
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        if (handles[i].used) continue;
        handles[i].used = 1;
        handles[i].file = file;
        handles[i].position = 0;
        handles[i].override = override;
        log_file(name, &file);
        return i + 3;
    }
    return -1;
}
int sceClose(int fd) {
    if (fd < 3 || fd >= 35 || !handles[fd - 3].used) return -1;
    handles[fd - 3].used = 0;
    return 0;
}
int sceLseek(int fd, int offset, int whence) {
    if (fd < 3 || fd >= 35 || !handles[fd - 3].used || whence < 0 || whence > 2) return -1;
    int index = fd - 3;
    int64_t position = offset;
    if (whence == 1) position += handles[index].position;
    if (whence == 2) position += handles[index].file.size;
    if (position < 0 || position > INT_MAX) return -1;
    handles[index].position = (uint32_t)position;
    return (int)position;
}
int sceRead(int fd, void *buffer, int size) {
    if (fd < 3 || fd >= 35 || !handles[fd - 3].used || size < 0 || (size && !buffer)) return -1;
    int index = fd - 3;
    if (handles[index].position >= handles[index].file.size) return 0;
    uint32_t available = handles[index].file.size - handles[index].position;
    if ((uint32_t)size > available) size = (int)available;
    int read = handles[index].override >= 0
                   ? port_overrides_read(handles[index].override, handles[index].position, buffer, (size_t)size)
                   : port_cdvd_read_file(&handles[index].file, handles[index].position, buffer, (size_t)size);
    if (!read) return -1;
    handles[index].position += (uint32_t)size;
    return size;
}
int sceWrite(int fd, void *buffer, int size) { (void)fd; (void)buffer; (void)size; return -1; }
int sceIoctl(int fd, int request, void *argument) {
    if (fd < 3 || fd >= 35 || !handles[fd - 3].used || request != 1 || !argument) return -1;
    *(int *)argument = 0;
    return 0;
}
int sceGetstat(const char *name, struct sce_stat *stat) {
    struct sceCdlFILE file;
    int override = port_overrides_find(name);
    memset(&file, 0, sizeof file);
    file.size = port_overrides_size(override);
    if (!stat || (override < 0 && !port_cdvd_find(name, &file))) return -1;
    memset(stat, 0, sizeof(*stat));
    stat->st_mode = file.flag & 2 ? 0x1000 : 0x2000;
    stat->st_size = file.size;
    memcpy(stat->st_ctime, file.date, 8);
    memcpy(stat->st_mtime, file.date, 8);
    return 0;
}
