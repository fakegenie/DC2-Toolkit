#define _POSIX_C_SOURCE 200809L
#include "port_overrides.h"
#include "port_mod.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

enum { OVERRIDE_LIMIT = 4096, OVERRIDE_PATH = 256, PAK_LIMIT = 32, PAK_PATH = 1024, PAK_TEXT = 256 };
enum { PAK_FILE_LIMIT = 512, PAK_NUMBER_LIMIT = 64, ZIP_STORED = 0, FROM_FOLDER = -1 };
enum { ZIP_LOCAL_SIGNATURE = 0x04034b50, ZIP_CENTRAL_SIGNATURE = 0x02014b50, ZIP_END_SIGNATURE = 0x06054b50 };

struct __attribute__((packed)) zip_local_header {
    uint32_t signature;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression;
    uint16_t modified_time;
    uint16_t modified_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t name_length;
    uint16_t extra_length;
};
_Static_assert(sizeof(struct zip_local_header) == 30, "ZIP local file header");

struct __attribute__((packed)) zip_central_header {
    uint32_t signature;
    uint16_t version_made;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression;
    uint16_t modified_time;
    uint16_t modified_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t name_length;
    uint16_t extra_length;
    uint16_t comment_length;
    uint16_t disk_start;
    uint16_t internal_attributes;
    uint32_t external_attributes;
    uint32_t local_header_offset;
};
_Static_assert(sizeof(struct zip_central_header) == 46, "ZIP central directory header");

struct __attribute__((packed)) zip_end_record {
    uint32_t signature;
    uint16_t disk;
    uint16_t central_disk;
    uint16_t disk_entries;
    uint16_t total_entries;
    uint32_t central_size;
    uint32_t central_offset;
    uint16_t comment_length;
};
_Static_assert(sizeof(struct zip_end_record) == 22, "ZIP end of central directory record");

enum { ZIP_TAIL = sizeof(struct zip_end_record) + UINT16_MAX };

typedef struct {
    char file[PAK_PATH];
    char id[PAK_TEXT];
    char name[PAK_TEXT];
    char version[PAK_TEXT];
    char description[PAK_TEXT];
    char authors[PAK_TEXT];
    char game[PAK_TEXT];
    char settings[PAK_TEXT];
    int file_count;
    char files[PAK_FILE_LIMIT][OVERRIDE_PATH];
    uint32_t sizes[PAK_FILE_LIMIT];
    uint32_t offsets[PAK_FILE_LIMIT];
    int number_count;
    char numbers[PAK_NUMBER_LIMIT][PAK_TEXT];
} Pak;

static const char *const settings_file = "dc2tk-port.cfg";
static const char *const config_member = "pak.cfg";
static const char *const files_prefix = "files/";

static char folder[1024];
static char paks_folder[1024];
static char paths[OVERRIDE_LIMIT][OVERRIDE_PATH];
static uint32_t sizes[OVERRIDE_LIMIT];
static uint32_t offsets[OVERRIDE_LIMIT];
static int sources[OVERRIDE_LIMIT];
static int count;
static int scanned;
static Pak paks[PAK_LIMIT];
static int pak_count;
static int paks_listed;
static int settings_applied;

static const char *const bundle_resources = ".app/Contents/Resources/";

const char *port_data_folder(void) {
    static char data[1024];
    if (data[0]) return data;
    const char *base = SDL_GetBasePath();
    size_t length = base ? strlen(base) : 0;
    size_t suffix = strlen(bundle_resources);
    int bundled = length > suffix && !strcmp(base + length - suffix, bundle_resources);
    char *preferred = bundled ? SDL_GetPrefPath("DC2 Toolkit", "Dark Chronicle") : NULL;
    snprintf(data, sizeof data, "%s", preferred ? preferred : base ? base : "");
    SDL_free(preferred);
    return data;
}

const char *port_overrides_folder(void) {
    if (folder[0]) return folder;
    const char *chosen = getenv("DC2TK_OVERRIDES");
    if (chosen && *chosen) snprintf(folder, sizeof folder, "%s", chosen);
    else snprintf(folder, sizeof folder, "%soverrides", port_data_folder());
    return folder;
}

#if TARGET_OS_IPHONE
#include "ios_files.h"
static const char *mods_parent(void) { return port_ios_documents(); }
#elif defined(__ANDROID__)
#include "android_files.h"
static const char *mods_parent(void) { return port_android_documents(); }
#else
static const char *mods_parent(void) { return port_data_folder(); }
#endif

const char *port_paks_folder(void) {
    if (!paks_folder[0]) snprintf(paks_folder, sizeof paks_folder, "%smods", mods_parent());
    return paks_folder;
}

static int read_at(int file, uint64_t offset, void *buffer, size_t size) {
    unsigned char *out = buffer;
    while (size) {
        ssize_t result = pread(file, out, size, (off_t)offset);
        if (result <= 0) return 0;
        out += result;
        offset += (uint64_t)result;
        size -= (size_t)result;
    }
    return 1;
}

static void copy_text(char *out, const char *text) { snprintf(out, PAK_TEXT, "%s", text); }

static void read_config(Pak *pak, char *text) {
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = 0;
        const char *value = equals + 1;
        if (!strcmp(line, "id")) copy_text(pak->id, value);
        if (!strcmp(line, "name")) copy_text(pak->name, value);
        if (!strcmp(line, "version")) copy_text(pak->version, value);
        if (!strcmp(line, "description")) copy_text(pak->description, value);
        if (!strcmp(line, "authors")) copy_text(pak->authors, value);
        if (!strcmp(line, "game")) copy_text(pak->game, value);
        if (!strcmp(line, "number") && pak->number_count < PAK_NUMBER_LIMIT) copy_text(pak->numbers[pak->number_count++], value);
    }
}

static int member_data(int file, uint32_t local, uint32_t *offset) {
    struct zip_local_header header;
    if (!read_at(file, local, &header, sizeof header) || header.signature != ZIP_LOCAL_SIGNATURE) return 0;
    *offset = local + sizeof header + header.name_length + header.extra_length;
    return 1;
}

static const struct zip_end_record *find_end_record(const unsigned char *tail, size_t size) {
    for (size_t at = size >= sizeof(struct zip_end_record) ? size - sizeof(struct zip_end_record) + 1 : 0; at-- > 0;) {
        const struct zip_end_record *record = (const struct zip_end_record *)(tail + at);
        if (record->signature == ZIP_END_SIGNATURE) return record;
    }
    return NULL;
}

static void add_member(Pak *pak, int file, const char *name, uint32_t size, uint32_t local) {
    uint32_t offset;
    if (!member_data(file, local, &offset)) return;
    if (!strcmp(name, config_member)) {
        char *text = calloc(1, (size_t)size + 1);
        if (text && read_at(file, offset, text, size)) read_config(pak, text);
        free(text);
        return;
    }
    size_t prefix = strlen(files_prefix);
    if (strncmp(name, files_prefix, prefix) || pak->file_count == PAK_FILE_LIMIT) return;
    if (!strcmp(name + prefix, settings_file)) {
        snprintf(pak->settings, sizeof pak->settings, "%u %u", offset, size);
        return;
    }
    snprintf(pak->files[pak->file_count], OVERRIDE_PATH, "%s", name + prefix);
    pak->sizes[pak->file_count] = size;
    pak->offsets[pak->file_count] = offset;
    pak->file_count++;
}

static int read_pak(Pak *pak, const char *path) {
    memset(pak, 0, sizeof *pak);
    snprintf(pak->file, sizeof pak->file, "%s", path);
    int file = open(path, O_RDONLY);
    if (file < 0) return 0;
    struct stat info;
    unsigned char tail[ZIP_TAIL];
    size_t tail_size = fstat(file, &info) || info.st_size < (off_t)sizeof(struct zip_end_record) ? 0
                       : info.st_size < ZIP_TAIL                                    ? (size_t)info.st_size
                                                                                    : ZIP_TAIL;
    uint64_t tail_at = (uint64_t)info.st_size - tail_size;
    int found = tail_size && read_at(file, tail_at, tail, tail_size);
    const struct zip_end_record *record = found ? find_end_record(tail, tail_size) : NULL;
    if (!record) {
        close(file);
        return 0;
    }
    uint32_t entries = record->total_entries, central_size = record->central_size, central_at = record->central_offset;
    unsigned char *central = malloc(central_size ? central_size : 1);
    int ok = central && read_at(file, central_at, central, central_size);
    for (uint32_t i = 0, at = 0; ok && i < entries && at + sizeof(struct zip_central_header) <= central_size; i++) {
        const struct zip_central_header *entry = (const struct zip_central_header *)(central + at);
        uint32_t name_length = entry->name_length;
        if (entry->signature != ZIP_CENTRAL_SIGNATURE || at + sizeof *entry + name_length > central_size) break;
        char name[OVERRIDE_PATH + 16];
        snprintf(name, sizeof name, "%.*s", (int)name_length, (const char *)(entry + 1));
        if (entry->compression == ZIP_STORED) add_member(pak, file, name, entry->uncompressed_size, entry->local_header_offset);
        at += sizeof *entry + name_length + entry->extra_length + entry->comment_length;
    }
    free(central);
    close(file);
    return ok && pak->id[0];
}

static int has_extension(const char *name, const char *extension) {
    size_t length = strlen(name), wanted = strlen(extension);
    return length > wanted && !strcasecmp(name + length - wanted, extension);
}

int port_paks_list(void) {
    paks_listed = 1;
    pak_count = 0;
    DIR *listing = opendir(port_paks_folder());
    struct dirent *item;
    while (listing && (item = readdir(listing)) && pak_count < PAK_LIMIT) {
        if (item->d_name[0] == '.' || !has_extension(item->d_name, ".dc2pak")) continue;
        char path[PAK_PATH];
        snprintf(path, sizeof path, "%s/%s", port_paks_folder(), item->d_name);
        if (!read_pak(&paks[pak_count], path)) continue;
        int at = pak_count++;
        while (at > 0 && strcasecmp(paks[at - 1].name, paks[at].name) > 0) {
            Pak swap = paks[at];
            paks[at] = paks[at - 1];
            paks[at - 1] = swap;
            at--;
        }
    }
    if (listing) closedir(listing);
    return pak_count;
}

const char *port_pak_text(int index, const char *key) {
    if (!paks_listed) port_paks_list();
    if (index < 0 || index >= pak_count) return "";
    const Pak *pak = &paks[index];
    if (!strcmp(key, "id")) return pak->id;
    if (!strcmp(key, "name")) return pak->name[0] ? pak->name : pak->id;
    if (!strcmp(key, "version")) return pak->version;
    if (!strcmp(key, "description")) return pak->description;
    if (!strcmp(key, "authors")) return pak->authors;
    if (!strcmp(key, "file")) return pak->file;
    return "";
}

int port_pak_files(int index) {
    if (!paks_listed) port_paks_list();
    return index >= 0 && index < pak_count ? paks[index].file_count : 0;
}

static int same_number(const char *a, const char *b) {
    char kind_a[64], kind_b[64];
    long number_a, number_b;
    return sscanf(a, "%63s %ld", kind_a, &number_a) == 2 && sscanf(b, "%63s %ld", kind_b, &number_b) == 2 &&
           !strcmp(kind_a, kind_b) && number_a == number_b;
}

int port_pak_clash(int a, int b, char *why, size_t size) {
    if (!paks_listed) port_paks_list();
    if (a < 0 || b < 0 || a >= pak_count || b >= pak_count || a == b) return 0;
    const Pak *first = &paks[a], *second = &paks[b];
    if (!strcmp(first->id, second->id)) {
        snprintf(why, size, "another version of it is on");
        return 1;
    }
    for (int i = 0; i < first->file_count; i++)
        for (int j = 0; j < second->file_count; j++)
            if (!strcasecmp(first->files[i], second->files[j])) {
                snprintf(why, size, "both change %s", first->files[i]);
                return 1;
            }
    for (int i = 0; i < first->number_count; i++)
        for (int j = 0; j < second->number_count; j++)
            if (same_number(first->numbers[i], second->numbers[j])) {
                snprintf(why, size, "both use %s", first->numbers[i]);
                return 1;
            }
    return 0;
}

int port_pak_enabled(int index) {
    if (!paks_listed) port_paks_list();
    const char *enabled = port_setting("mods.enabled");
    if (index < 0 || index >= pak_count || !enabled) return 0;
    size_t length = strlen(paks[index].id);
    for (const char *at = enabled; *at;) {
        const char *end = strchr(at, ',');
        size_t span = end ? (size_t)(end - at) : strlen(at);
        if (span == length && !strncmp(at, paks[index].id, length)) return 1;
        at += span + (end ? 1 : 0);
    }
    return 0;
}

static void add_override(const char *path, uint32_t size, uint32_t offset, int source) {
    for (int i = 0; i < count; i++)
        if (!strcasecmp(paths[i], path)) return;
    if (count == OVERRIDE_LIMIT) return;
    snprintf(paths[count], OVERRIDE_PATH, "%s", path);
    sizes[count] = size;
    offsets[count] = offset;
    sources[count] = source;
    count++;
}

static void scan_folder(const char *relative) {
    char directory[2048];
    snprintf(directory, sizeof directory, "%s/%s", port_overrides_folder(), relative);
    DIR *listing = opendir(directory);
    if (!listing) return;
    struct dirent *item;
    while ((item = readdir(listing)) && count < OVERRIDE_LIMIT) {
        if (item->d_name[0] == '.') continue;
        char inside[OVERRIDE_PATH];
        int length = snprintf(inside, sizeof inside, "%s%s%s", relative, *relative ? "/" : "", item->d_name);
        if (length <= 0 || length >= OVERRIDE_PATH) continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", port_overrides_folder(), inside);
        struct stat info;
        if (stat(full, &info)) continue;
        if (S_ISDIR(info.st_mode)) {
            scan_folder(inside);
            continue;
        }
        if (!S_ISREG(info.st_mode) || info.st_size > UINT32_MAX) continue;
        add_override(inside, (uint32_t)info.st_size, 0, FROM_FOLDER);
    }
    closedir(listing);
}

static void apply_settings(const Pak *pak) {
    uint32_t offset, size;
    if (sscanf(pak->settings, "%u %u", &offset, &size) != 2) return;
    int file = open(pak->file, O_RDONLY);
    char *text = calloc(1, (size_t)size + 1);
    if (file >= 0 && text && read_at(file, offset, text, size)) port_settings_beneath(text);
    free(text);
    if (file >= 0) close(file);
}

void port_overrides_scan(void) {
    count = 0;
    scanned = 1;
    scan_folder("");
    int folder_files = count, enabled = 0;
    port_paks_list();
    for (int index = 0; index < pak_count; index++) {
        if (!port_pak_enabled(index)) continue;
        enabled++;
        for (int i = 0; i < paks[index].file_count; i++)
            add_override(paks[index].files[i], paks[index].sizes[i], paks[index].offsets[i], index);
        if (!settings_applied) apply_settings(&paks[index]);
        fprintf(stderr, "mods: %s %s on\n", paks[index].id, paks[index].version);
    }
    settings_applied = 1;
    if (folder_files) fprintf(stderr, "overrides: %d files in %s\n", folder_files, port_overrides_folder());
    if (enabled) fprintf(stderr, "mods: %d on, %d files\n", enabled, count - folder_files);
}

int port_overrides_count(void) {
    if (!scanned) port_overrides_scan();
    return count;
}

const char *port_overrides_path(int index) {
    return index >= 0 && index < count ? paths[index] : "";
}

static void normalise(const char *path, char *out, size_t size) {
    if (!strncasecmp(path, "cdrom0:", 7)) path += 7;
    else if (!strncasecmp(path, "cdrom:", 6)) path += 6;
    while (*path == '/' || *path == '\\') path++;
    size_t length = 0;
    for (; *path && *path != ';' && length + 1 < size; path++) out[length++] = *path == '\\' ? '/' : *path;
    out[length] = 0;
}

int port_overrides_find(const char *path) {
    if (!path || !port_overrides_count()) return -1;
    char wanted[OVERRIDE_PATH];
    normalise(path, wanted, sizeof wanted);
    for (int i = 0; i < count; i++)
        if (!strcasecmp(paths[i], wanted)) return i;
    return -1;
}

uint32_t port_overrides_size(int index) {
    return index >= 0 && index < count ? sizes[index] : 0;
}

int port_overrides_read(int index, uint32_t offset, void *buffer, size_t size) {
    if (index < 0 || index >= count || offset > sizes[index] || size > sizes[index] - offset) return 0;
    char full[2048];
    if (sources[index] == FROM_FOLDER) snprintf(full, sizeof full, "%s/%s", port_overrides_folder(), paths[index]);
    else snprintf(full, sizeof full, "%s", paks[sources[index]].file);
    int file = open(full, O_RDONLY);
    if (file < 0) return 0;
    int read = read_at(file, (uint64_t)offsets[index] + offset, buffer, size);
    close(file);
    if (read && !offset) fprintf(stderr, "override: %s (%u bytes)\n", paths[index], sizes[index]);
    return read;
}
