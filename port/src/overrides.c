#define _POSIX_C_SOURCE 200809L
#include "port_overrides.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

enum { OVERRIDE_LIMIT = 4096, OVERRIDE_PATH = 256 };

static char folder[1024];
static char paths[OVERRIDE_LIMIT][OVERRIDE_PATH];
static uint32_t sizes[OVERRIDE_LIMIT];
static int count;
static int scanned;

static void binary_folder(char *out, uint32_t size) {
#ifdef __APPLE__
    if (_NSGetExecutablePath(out, &size)) out[0] = 0;
#else
    ssize_t length = readlink("/proc/self/exe", out, size - 1);
    out[length > 0 ? length : 0] = 0;
#endif
    char *slash = strrchr(out, '/');
    if (slash) slash[1] = 0;
    else out[0] = 0;
}

const char *port_overrides_folder(void) {
    if (folder[0]) return folder;
    const char *chosen = getenv("DC2TK_OVERRIDES");
    char binary[1024];
    binary_folder(binary, sizeof binary);
    if (chosen && *chosen) snprintf(folder, sizeof folder, "%s", chosen);
    else snprintf(folder, sizeof folder, "%soverrides", binary);
    return folder;
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
        memcpy(paths[count], inside, (size_t)length + 1);
        sizes[count] = (uint32_t)info.st_size;
        count++;
    }
    closedir(listing);
}

void port_overrides_scan(void) {
    count = 0;
    scanned = 1;
    scan_folder("");
    if (count) fprintf(stderr, "overrides: %d files in %s\n", count, port_overrides_folder());
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
    snprintf(full, sizeof full, "%s/%s", port_overrides_folder(), paths[index]);
    int file = open(full, O_RDONLY);
    if (file < 0) return 0;
    unsigned char *out = buffer;
    uint32_t start = offset;
    while (size) {
        ssize_t result = pread(file, out, size, (off_t)offset);
        if (result <= 0) break;
        out += result;
        offset += (uint32_t)result;
        size -= (size_t)result;
    }
    close(file);
    if (!size && !start) fprintf(stderr, "override: %s (%u bytes)\n", paths[index], sizes[index]);
    return !size;
}
