#define _POSIX_C_SOURCE 200809L
#include "cdvd.h"
#include "port_game.h"
#include "sce/sifdev.h"
#include "port_kernel.h"
#include "port_overrides.h"
#include "port_stats.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#undef st_ctime
#undef st_atime
#undef st_mtime

enum { ISO_FIRST_DESCRIPTOR = 16, ISO_DESCRIPTOR_LIMIT = 256 };
enum { ISO_DESCRIPTOR_PRIMARY = 1, ISO_DESCRIPTOR_TERMINATOR = 255, ISO_DESCRIPTOR_VERSION = 1 };
enum { ISO_FLAG_DIRECTORY = 0x02, ISO_FLAG_MULTI_EXTENT = 0x80 };
enum { HANDLE_LIMIT = 32, FIRST_HANDLE = 3, MISSING_LIMIT = 64, CD_SYNC_WAIT = 0 };

static const char iso_identifier[5] = {'C', 'D', '0', '0', '1'};

struct __attribute__((packed)) iso_dir_record {
    uint8_t length;
    uint8_t extended_length;
    uint32_t extent;
    uint32_t extent_big_endian;
    uint32_t size;
    uint32_t size_big_endian;
    uint8_t date[7];
    uint8_t flags;
    uint8_t unit_size;
    uint8_t gap_size;
    uint16_t volume_sequence;
    uint16_t volume_sequence_big_endian;
    uint8_t name_length;
    char name[];
};
_Static_assert(sizeof(struct iso_dir_record) == 33, "ISO 9660 directory record");

struct __attribute__((packed)) iso_primary_volume {
    uint8_t type;
    char identifier[5];
    uint8_t version;
    uint8_t unused1;
    char system_identifier[32];
    char volume_identifier[32];
    uint8_t unused2[8];
    uint32_t volume_space_size;
    uint32_t volume_space_size_big_endian;
    uint8_t unused3[32];
    uint16_t volume_set_size;
    uint16_t volume_set_size_big_endian;
    uint16_t volume_sequence_number;
    uint16_t volume_sequence_number_big_endian;
    uint16_t logical_block_size;
    uint16_t logical_block_size_big_endian;
    uint32_t path_table_size;
    uint32_t path_table_size_big_endian;
    uint32_t type_l_path_table;
    uint32_t optional_type_l_path_table;
    uint32_t type_m_path_table;
    uint32_t optional_type_m_path_table;
    uint8_t root_directory_record[34];
    char volume_set_identifier[128];
    char publisher_identifier[128];
    char preparer_identifier[128];
    char application_identifier[128];
    char copyright_file_identifier[37];
    char abstract_file_identifier[37];
    char bibliographic_file_identifier[37];
    char creation_date[17];
    char modification_date[17];
    char expiration_date[17];
    char effective_date[17];
    uint8_t file_structure_version;
    uint8_t unused4;
    uint8_t application_use[512];
    uint8_t reserved[653];
};
_Static_assert(sizeof(struct iso_primary_volume) == CDVD_SECTOR_SIZE, "ISO 9660 primary volume descriptor");

struct data_index_entry {
    uint32_t name_offset;
    uint32_t size;
    uint32_t sector;
};
_Static_assert(sizeof(struct data_index_entry) == 12, "DATA.HD4 entry");

enum { DATA_INDEX_LIMIT = 1024 * 1024 };

static int disc = -1;
static uint64_t disc_size;
static struct sceCdlFILE root;
static char missing_paths[MISSING_LIMIT][1024];
static size_t missing_count;
static union {
    struct data_index_entry entries[DATA_INDEX_LIMIT / sizeof(struct data_index_entry)];
    char bytes[DATA_INDEX_LIMIT];
} archive_index;
static uint32_t archive_size;
static uint32_t archive_sector;
static uint32_t archive_last_sector;
static int archive_checked;
PORT_GAME_BEGIN
static int cd_error;
static struct {
    int used;
    struct sceCdlFILE file;
    uint32_t position;
    int override;
} handles[HANDLE_LIMIT];
PORT_GAME_END

static uint64_t sector_offset(uint32_t sector) {
    return (uint64_t)sector * CDVD_SECTOR_SIZE;
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

static int record_name_fits(const struct iso_dir_record *record) {
    return record->length > sizeof *record && record->name_length <= record->length - sizeof *record;
}

static int record_file(const struct iso_dir_record *record, struct sceCdlFILE *file) {
    if (!record_name_fits(record) || record->flags & ISO_FLAG_MULTI_EXTENT) return 0;
    memset(file, 0, sizeof(*file));
    file->lsn = record->extent;
    file->size = record->size;
    file->flag = record->flags;
    size_t length = record->name_length;
    if (length >= sizeof(file->name)) length = sizeof(file->name) - 1;
    memcpy(file->name, record->name, length);
    memcpy(file->date, record->date, sizeof record->date);
    return sector_offset(file->lsn) <= disc_size && file->size <= disc_size - sector_offset(file->lsn);
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

static int open_disc(const char *path) {
    SDL_IOStream *stream = SDL_IOFromFile(path, "rb");
    SDL_PropertiesID properties = stream ? SDL_GetIOProperties(stream) : 0;
    int descriptor = (int)SDL_GetNumberProperty(properties, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1);
    int kept = descriptor >= 0 ? dup(descriptor) : -1;
    if (stream) SDL_CloseIO(stream);
    return kept;
}

int port_cdvd_init(const char *path) {
    port_cdvd_close();
    if (!path) return 0;
    disc = open_disc(path);
    struct stat info;
    if (disc < 0 || fstat(disc, &info) || info.st_size < (off_t)sector_offset(ISO_FIRST_DESCRIPTOR + 1)) goto fail;
    disc_size = (uint64_t)info.st_size;
    struct iso_primary_volume volume;
    for (uint32_t number = ISO_FIRST_DESCRIPTOR; number < ISO_DESCRIPTOR_LIMIT; number++) {
        if (!read_disc(sector_offset(number), &volume, sizeof(volume))) break;
        if (memcmp(volume.identifier, iso_identifier, sizeof iso_identifier) || volume.version != ISO_DESCRIPTOR_VERSION) break;
        if (volume.type == ISO_DESCRIPTOR_TERMINATOR) break;
        if (volume.type != ISO_DESCRIPTOR_PRIMARY) continue;
        const struct iso_dir_record *root_record = (const struct iso_dir_record *)volume.root_directory_record;
        if (volume.logical_block_size != CDVD_SECTOR_SIZE || !record_file(root_record, &root) || !(root.flag & ISO_FLAG_DIRECTORY)) break;
        cd_error = SCECdErNO;
        fprintf(stderr, "disc: %s (%llu bytes)\n", path, (unsigned long long)disc_size);
        return 1;
    }
fail:
    fprintf(stderr, "cannot read ISO 9660 image: %s\n", path);
    port_cdvd_close();
    return 0;
}

static int same_name(const struct iso_dir_record *record, const char *name, size_t length) {
    size_t record_length = record->name_length;
    for (size_t i = 0; i < record_length; i++) {
        if (record->name[i] == ';') {
            record_length = i;
            break;
        }
    }
    if (record_length != length) return 0;
    for (size_t i = 0; i < length; i++) {
        if (toupper((unsigned char)record->name[i]) != toupper((unsigned char)name[i])) return 0;
    }
    return 1;
}

static const char *after_prefix(const char *path, const char *prefix) {
    size_t length = strlen(prefix);
    return strncmp(path, prefix, length) ? path : path + length;
}

int port_cdvd_find(const char *path, struct sceCdlFILE *file) {
    if (disc < 0 || !path || !file) return 0;
    const char *device_path = after_prefix(path, "cdrom0:");
    path = device_path != path ? device_path : after_prefix(path, "cdrom:");
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
        if (!length || length > UINT8_MAX || (length == 1 && *path == '.') || (length == 2 && !memcmp(path, "..", 2))) return 0;
        if (!(current.flag & ISO_FLAG_DIRECTORY)) return 0;
        int found = 0;
        unsigned char sector[CDVD_SECTOR_SIZE];
        for (uint64_t offset = 0; offset < current.size; offset += CDVD_SECTOR_SIZE) {
            if (!read_disc(sector_offset(current.lsn) + offset, sector, sizeof(sector))) return 0;
            size_t limit = current.size - offset < CDVD_SECTOR_SIZE ? (size_t)(current.size - offset) : CDVD_SECTOR_SIZE;
            for (size_t position = 0; position < limit;) {
                const struct iso_dir_record *record = (const struct iso_dir_record *)(sector + position);
                if (!record->length) break;
                if (record->length > limit - position || !record_name_fits(record)) return 0;
                if (same_name(record, path, length)) {
                    if (!record_file(record, &current)) return 0;
                    found = 1;
                    break;
                }
                position += record->length;
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
    return file && offset <= file->size && size <= file->size - offset && read_disc(sector_offset(file->lsn) + offset, buffer, size);
}

static int log_files;

void port_cdvd_log_files(int enabled) {
    log_files = enabled;
}

static void log_file(const char *path, const struct sceCdlFILE *file) {
    if (!log_files) return;
    fprintf(stderr, "file: %s (%u bytes)\n", path, file->size);
}

static void log_archive_read(uint32_t sector) {
    if (!log_files) return;
    if (!archive_checked) {
        archive_checked = 1;
        struct sceCdlFILE data;
        struct sceCdlFILE index;
        if (port_cdvd_find("DATA.DAT", &data) && port_cdvd_find("DATA.HD4", &index) &&
            index.size >= sizeof(struct data_index_entry) && index.size <= sizeof(archive_index.bytes) &&
            port_cdvd_read_file(&index, 0, archive_index.bytes, index.size)) {
            archive_size = index.size;
            archive_sector = data.lsn;
        }
    }
    if (!archive_size || sector == archive_last_sector || sector < archive_sector) return;
    uint32_t names = archive_index.entries[0].name_offset;
    if (names > archive_size || names % sizeof(struct data_index_entry)) return;
    for (uint32_t i = 0; i < names / sizeof(struct data_index_entry); i++) {
        const struct data_index_entry *entry = &archive_index.entries[i];
        if (entry->sector != sector - archive_sector) continue;
        if (entry->name_offset < names || entry->name_offset >= archive_size ||
            !memchr(archive_index.bytes + entry->name_offset, 0, archive_size - entry->name_offset)) return;
        fprintf(stderr, "archive: %s (%u bytes)\n", archive_index.bytes + entry->name_offset, entry->size);
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
    if (missing_count < MISSING_LIMIT && length < sizeof(missing_paths[0])) {
        memcpy(missing_paths[missing_count++], path, length + 1);
    }
    fprintf(stderr, "file missing: %s\n", path);
}

int sceCdInit(int mode) { (void)mode; return disc >= 0; }
int sceCdMmode(int media) { return media == SCECdCD || media == SCECdDVD; }
int sceCdSearchFile(struct sceCdlFILE *file, const char *name) {
    if (!port_cdvd_find(name, file) || file->flag & ISO_FLAG_DIRECTORY) {
        log_missing(name);
        return 0;
    }
    log_file(name, file);
    return 1;
}
int sceCdRead(unsigned int lsn, unsigned int sectors, void *buffer, struct sceCdRMode *mode) {
    if (mode && mode->datapattern != SCECdSecS2048) { cd_error = SCECdErPRM; return 0; }
    int result = read_disc(sector_offset(lsn), buffer, (size_t)sectors * CDVD_SECTOR_SIZE);
    cd_error = result ? SCECdErNO : SCECdErREAD;
    if (result && sectors) log_archive_read(lsn);
    return result;
}
int sceCdSync(int mode) {
    if (mode == CD_SYNC_WAIT) port_kernel_yield();
    return 0;
}
int sceCdGetError(void) { return cd_error; }
int sceCdBreak(void) { cd_error = SCECdErNO; return 1; }
int sceCdSeek(unsigned int lsn) { return disc >= 0 && sector_offset(lsn) < disc_size; }
int sceCdDiskReady(int mode) { (void)mode; return disc >= 0 ? SCECdComplete : SCECdNotReady; }
int sceCdStInit(int sectors, int banks, void *buffer) { return sectors > 0 && banks > 0 && buffer != NULL; }
int sceCdStStart(unsigned int sector, struct sceCdRMode *mode) { (void)mode; return sceCdSeek(sector); }
int sceCdStSeekF(unsigned int sector) { return sceCdSeek(sector); }
int sceCdStStop(void) { return 1; }
int sceCdStRead(unsigned int sectors, void *buffer, unsigned int mode, unsigned int *error) {
    (void)sectors; (void)buffer; (void)mode;
    if (error) *error = SCECdErNO;
    return 0;
}

static int handle_index(int fd) {
    int index = fd - FIRST_HANDLE;
    return index >= 0 && index < HANDLE_LIMIT && handles[index].used ? index : -1;
}

void sceFsReset(void) { memset(handles, 0, sizeof(handles)); }
int sceOpen(const char *name, int flags, ...) {
    struct sceCdlFILE file;
    if ((flags & SCE_RDWR) != SCE_RDONLY || flags & (SCE_CREAT | SCE_TRUNC)) return -1;
    int override = port_overrides_find(name);
    memset(&file, 0, sizeof file);
    file.size = port_overrides_size(override);
    if (override < 0 && (!port_cdvd_find(name, &file) || file.flag & ISO_FLAG_DIRECTORY)) {
        log_missing(name);
        return -1;
    }
    for (int i = 0; i < HANDLE_LIMIT; i++) {
        if (handles[i].used) continue;
        handles[i].used = 1;
        handles[i].file = file;
        handles[i].position = 0;
        handles[i].override = override;
        log_file(name, &file);
        return i + FIRST_HANDLE;
    }
    return -1;
}
int sceClose(int fd) {
    int index = handle_index(fd);
    if (index < 0) return -1;
    handles[index].used = 0;
    return 0;
}
int sceLseek(int fd, int offset, int whence) {
    int index = handle_index(fd);
    if (index < 0 || whence < SCE_SEEK_SET || whence > SCE_SEEK_END) return -1;
    int64_t position = offset;
    if (whence == SCE_SEEK_CUR) position += handles[index].position;
    if (whence == SCE_SEEK_END) position += handles[index].file.size;
    if (position < 0 || position > INT_MAX) return -1;
    handles[index].position = (uint32_t)position;
    return (int)position;
}
int sceRead(int fd, void *buffer, int size) {
    int index = handle_index(fd);
    if (index < 0 || size < 0 || (size && !buffer)) return -1;
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
    if (handle_index(fd) < 0 || request != SCE_FS_EXECUTING || !argument) return -1;
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
    stat->st_mode = file.flag & ISO_FLAG_DIRECTORY ? SCE_STM_FDIR : SCE_STM_FREG;
    stat->st_size = file.size;
    memcpy(stat->st_ctime, file.date, sizeof stat->st_ctime);
    memcpy(stat->st_mtime, file.date, sizeof stat->st_mtime);
    return 0;
}
