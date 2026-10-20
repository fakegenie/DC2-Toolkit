#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "memory_card.h"

static int cards[2] = {-1, -1};
static char directories[2][1024];
static int files[32];
static int owners[32];
static int initialized;
static int pending;
static int last_command;
static int last_result;
static DIR *listing;
static char pattern[32];
static int listing_port = -1;

static int error_result(void) {
    if (errno == ENOENT || errno == ENOTDIR || errno == EEXIST) return -4;
    if (errno == ENOSPC || errno == EDQUOT) return -3;
    return -5;
}

static void close_listing(void) {
    if (listing) closedir(listing);
    listing = NULL;
    listing_port = -1;
}

int sceMcEnd(void) {
    close_listing();
    for (int i = 0; i < 32; ++i) {
        if (initialized && files[i] >= 0) close(files[i]);
        files[i] = -1;
    }
    for (int i = 0; i < 2; ++i) {
        if (cards[i] >= 0) close(cards[i]);
        cards[i] = -1;
        directories[i][0] = 0;
    }
    initialized = 0;
    pending = 0;
    return 0;
}

int port_memory_card_init(const char *root) {
    sceMcEnd();
    if (!root || !root[0]) return -101;
    if (mkdir(root, 0700) && errno != EEXIST) return -101;
    int base = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (base < 0) return -101;
    for (int i = 0; i < 2; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "card%d", i);
        if (mkdirat(base, name, 0700) && errno != EEXIST) break;
        cards[i] = openat(base, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (cards[i] < 0) break;
    }
    close(base);
    initialized = 1;
    return cards[0] >= 0 ? 0 : -101;
}

static const char *default_root(void) {
    static char path[1024];
    const char *home = getenv("HOME");
    struct stat info;
    if (!home) return "saves";
    snprintf(path, sizeof path, "%s/Library/Mobile Documents/com~apple~CloudDocs/Dark Chronicle", home);
    if (stat(path, &info) || !S_ISDIR(info.st_mode)) return "saves";
    snprintf(path, sizeof path, "%s/Library/Mobile Documents/com~apple~CloudDocs/Dark Chronicle/saves", home);
    return path;
}

int sceMcInit(void) {
    if (initialized) return cards[0] >= 0 ? 0 : -101;
    const char *root = getenv("DC2_SAVE_ROOT");
    return port_memory_card_init(root && root[0] ? root : default_root());
}

static int request(int command) {
    if (pending) return -200;
    pending = 1;
    last_command = command;
    last_result = -5;
    return 0;
}

int sceMcSync(int mode, int *command, int *result) {
    (void)mode;
    if (!pending) return -1;
    if (command) *command = last_command;
    if (result) *result = last_result;
    pending = 0;
    return 1;
}

static int card_status(int port, int slot) {
    if (port < 0 || port > 1 || slot < 0 || slot > 1 || cards[port] < 0) return -12;
    struct stat status;
    if (!fstatat(cards[port], ".unformatted", &status, AT_SYMLINK_NOFOLLOW)) return -2;
    return 0;
}

static int path_name(int port, const char *name, char *path) {
    if (!name || !name[0]) return -5;
    size_t used = 0;
    if (name[0] != '/' && directories[port][0]) {
        used = strlen(directories[port]);
        memcpy(path, directories[port], used);
    }
    while (*name) {
        while (*name == '/') ++name;
        const char *start = name;
        while (*name && *name != '/') ++name;
        size_t length = (size_t)(name - start);
        if (!length) break;
        if (length == 1 && start[0] == '.') continue;
        if ((length == 2 && !memcmp(start, "..", 2)) || length > 31 || start[0] == '.') return -5;
        if (used + length + 1 >= 1024) return -5;
        if (used) path[used++] = '/';
        memcpy(path + used, start, length);
        used += length;
    }
    path[used] = 0;
    return 0;
}

static int parent_path(int port, const char *path, char *leaf) {
    int fd = openat(cards[port], ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (*path) {
        const char *end = strchr(path, '/');
        size_t size = end ? (size_t)(end - path) : strlen(path);
        memcpy(leaf, path, size);
        leaf[size] = 0;
        if (!end) return fd;
        int next = openat(fd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0) return -1;
        fd = next;
        path = end + 1;
    }
    strcpy(leaf, ".");
    return fd;
}

static int resolve(int port, int slot, const char *name, char *leaf) {
    int status = card_status(port, slot);
    if (status) { last_result = status; return -1; }
    char path[1024];
    status = path_name(port, name, path);
    if (status) { last_result = status; return -1; }
    int parent = parent_path(port, path, leaf);
    if (parent < 0) last_result = error_result();
    return parent;
}

int sceMcGetInfo(int port, int slot, int *type, int *free_size, int *formatted) {
    if (request(1)) return -200;
    last_result = card_status(port, slot);
    if (type) *type = last_result == -12 ? 0 : 2;
    if (formatted) *formatted = last_result == 0;
    if (free_size) {
        *free_size = 0;
        struct statvfs space;
        if (last_result == 0 && !fstatvfs(cards[port], &space)) {
            unsigned long long available = (unsigned long long)space.f_bavail * space.f_frsize / 1024;
            *free_size = available > 8192 ? 8192 : (int)available;
        }
    }
    return 0;
}

int sceMcOpen(int port, int slot, const char *name, int flags) {
    if (request(2)) return -200;
    char leaf[32];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    int mode = flags & 3;
    if (!mode || (flags & ~(3 | 0x100 | 0x200 | 0x400))) { close(parent); return 0; }
    int host_flags = mode == 1 ? O_RDONLY : mode == 2 ? O_WRONLY : O_RDWR;
    if (flags & 0x100) host_flags |= O_APPEND;
    if (flags & 0x200) host_flags |= O_CREAT;
    if (flags & 0x400) host_flags |= O_TRUNC;
    for (int i = 0; i < 32; ++i) {
        if (files[i] >= 0) continue;
        int fd = openat(parent, leaf, host_flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        close(parent);
        if (fd < 0) { last_result = error_result(); return 0; }
        struct stat status;
        if (fstat(fd, &status) || !S_ISREG(status.st_mode)) { close(fd); return 0; }
        files[i] = fd;
        owners[i] = port;
        last_result = i;
        return 0;
    }
    close(parent);
    return 0;
}

static int file_request(int command, int fd) {
    if (request(command)) return -200;
    if (!initialized || fd < 0 || fd >= 32 || files[fd] < 0) return -1;
    return 0;
}

int sceMcClose(int fd) {
    int status = file_request(3, fd);
    if (status) return status == -200 ? -200 : 0;
    last_result = close(files[fd]) ? error_result() : 0;
    files[fd] = -1;
    return 0;
}

int sceMcSeek(int fd, int offset, int origin) {
    int status = file_request(4, fd);
    if (status) return status == -200 ? -200 : 0;
    if (origin < 0 || origin > 2) return 0;
    off_t result = lseek(files[fd], offset, origin);
    last_result = result < 0 ? error_result() : result > INT_MAX ? -5 : (int)result;
    return 0;
}

int sceMcRead(int fd, void *buffer, int size) {
    int status = file_request(5, fd);
    if (status) return status == -200 ? -200 : 0;
    if (size < 0 || (!buffer && size)) return 0;
    ssize_t result = read(files[fd], buffer, (size_t)size);
    last_result = result < 0 ? error_result() : (int)result;
    return 0;
}

int sceMcWrite(int fd, void *buffer, int size) {
    int status = file_request(6, fd);
    if (status) return status == -200 ? -200 : 0;
    if (size < 0 || (!buffer && size)) return 0;
    ssize_t result = write(files[fd], buffer, (size_t)size);
    last_result = result < 0 ? error_result() : (int)result;
    return 0;
}

int sceMcFlush(int fd) {
    int status = file_request(10, fd);
    if (status) return status == -200 ? -200 : 0;
    last_result = fsync(files[fd]) ? error_result() : 0;
    return 0;
}

int sceMcMkdir(int port, int slot, const char *name) {
    if (request(11)) return -200;
    char leaf[32];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    last_result = mkdirat(parent, leaf, 0700) ? error_result() : 0;
    close(parent);
    return 0;
}

int sceMcChdir(int port, int slot, const char *name, char *current) {
    if (request(12)) return -200;
    char leaf[32], path[1024];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    int fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (fd < 0) { last_result = error_result(); return 0; }
    close(fd);
    if (current) { current[0] = '/'; strcpy(current + 1, directories[port]); }
    path_name(port, name, path);
    strcpy(directories[port], path);
    last_result = 0;
    return 0;
}

static void entry_time(struct sceMcStDateTime *date, time_t timestamp) {
    struct tm value;
    if (!localtime_r(&timestamp, &value)) return;
    date->second = (uint8_t)value.tm_sec;
    date->minute = (uint8_t)value.tm_min;
    date->hour = (uint8_t)value.tm_hour;
    date->day = (uint8_t)value.tm_mday;
    date->month = (uint8_t)(value.tm_mon + 1);
    date->year = (uint16_t)(value.tm_year + 1900);
}

static int name_matches(const char *filter, const char *name) {
    size_t part = 0, position = 0, retry_part = SIZE_MAX, retry_position = 0;
    for (;;) {
        if (filter[part] == '*') {
            while (filter[part] == '*' || filter[part] == '?') ++part;
            if (!filter[part]) return 1;
            retry_part = part;
            retry_position = position;
            continue;
        }
        if (filter[part] == '?' && !name[position]) return 1;
        if (name[position] && (filter[part] == '?' || filter[part] == name[position])) {
            ++part;
            ++position;
            continue;
        }
        if (!filter[part] && !name[position]) return 1;
        if (retry_part == SIZE_MAX || !name[retry_position]) return 0;
        position = ++retry_position;
        part = retry_part;
    }
}

int sceMcGetDir(int port, int slot, const char *name, int mode, int count, struct sceMcTblGetDir *table) {
    if (request(13)) return -200;
    last_result = card_status(port, slot);
    if (last_result) return 0;
    last_result = -5;
    if (count < 0 || (!table && count) || (mode != 0 && mode != 1)) return 0;
    if (!mode) {
        close_listing();
        char leaf[32];
        int parent = resolve(port, slot, name, leaf);
        if (parent < 0) return 0;
        listing = fdopendir(parent);
        if (!listing) { close(parent); last_result = error_result(); return 0; }
        strcpy(pattern, leaf);
        listing_port = port;
    }
    if (!listing || listing_port != port) return 0;
    last_result = 0;
    while (last_result < count) {
        struct dirent *entry = readdir(listing);
        if (!entry) break;
        if (entry->d_name[0] == '.' || strlen(entry->d_name) > 31 || !name_matches(pattern, entry->d_name)) continue;
        struct stat info;
        if (fstatat(dirfd(listing), entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) continue;
        if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) continue;
        struct sceMcTblGetDir *row = &table[last_result++];
        memset(row, 0, sizeof(*row));
        entry_time(&row->create, info.st_ctime);
        entry_time(&row->modify, info.st_mtime);
        row->file_size_byte = info.st_size > UINT32_MAX ? UINT32_MAX : (uint32_t)info.st_size;
        row->attr_file = S_ISDIR(info.st_mode) ? 0x8427 : 0x8497;
        strcpy(row->entry_name, entry->d_name);
    }
    return 0;
}

int sceMcRename(int port, int slot, char *old_name, char *new_name) {
    if (request(14)) return -200;
    char old_leaf[32], new_leaf[32];
    int old_parent = resolve(port, slot, old_name, old_leaf);
    if (old_parent < 0) return 0;
    int new_parent = resolve(port, slot, new_name, new_leaf);
    if (new_parent < 0) { close(old_parent); return 0; }
    struct stat info;
    if (!fstatat(new_parent, new_leaf, &info, AT_SYMLINK_NOFOLLOW)) last_result = -8;
    else last_result = renameat(old_parent, old_leaf, new_parent, new_leaf) ? error_result() : 0;
    close(old_parent);
    close(new_parent);
    return 0;
}

int sceMcDelete(int port, int slot, char *name) {
    if (request(15)) return -200;
    char leaf[32];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    struct stat info;
    if (fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW)) last_result = error_result();
    else last_result = unlinkat(parent, leaf, S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0) ? error_result() : 0;
    close(parent);
    return 0;
}

static int clear_directory(int fd) {
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) return -1;
    DIR *entries = fdopendir(copy);
    if (!entries) { close(copy); return -1; }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat info;
        if (fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) { result = -1; break; }
        if (S_ISDIR(info.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { result = -1; break; }
            result = clear_directory(child);
            close(child);
            if (result) break;
        }
        if (unlinkat(fd, entry->d_name, S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0)) { result = -1; break; }
    }
    closedir(entries);
    return result;
}

static int format_card(int command, int port, int slot) {
    if (request(command)) return -200;
    last_result = card_status(port, slot);
    if (last_result == -12) return 0;
    close_listing();
    for (int i = 0; i < 32; ++i) {
        if (files[i] >= 0 && owners[i] == port) { close(files[i]); files[i] = -1; }
    }
    directories[port][0] = 0;
    last_result = clear_directory(cards[port]) ? error_result() : 0;
    if (!last_result && command == 17) {
        int marker = openat(cards[port], ".unformatted", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (marker < 0) last_result = error_result();
        else close(marker);
    }
    return 0;
}

int sceMcFormat(int port, int slot) { return format_card(16, port, slot); }
int sceMcUnformat(int port, int slot) { return format_card(17, port, slot); }
