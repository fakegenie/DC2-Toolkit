#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "memory_card.h"
#include "sce/sifdev.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include "windows_posix.h"
#else
#include <sys/statvfs.h>
#endif
#include <SDL3/SDL.h>

enum {
    MC_PORTS = 2,
    MC_SLOTS = 2,
    MC_FILES = 32,
    MC_NAME_SIZE = sizeof(((struct sceMcTblGetDir *)0)->entry_name),
    MC_PATH_SIZE = 1024,
    MC_CARD_DIRECTORY_SIZE = 16,
    MC_CARD_KILOBYTES = 8192,
    KILOBYTE = 1024,
    MC_DIR_FIRST = 0,
    MC_DIR_NEXT = 1,
    MC_OPEN_MODE = SCE_RDONLY | SCE_WRONLY | SCE_RDWR,
    MC_OPEN_FLAGS = MC_OPEN_MODE | SCE_APPEND | SCE_CREAT | SCE_TRUNC,
    MC_ATTR_COMMON = sceMcFileAttrExists | sceMcFile0400 | sceMcFileAttrReadable | sceMcFileAttrWriteable | sceMcFileAttrExecutable,
    MC_ATTR_DIRECTORY = MC_ATTR_COMMON | sceMcFileAttrSubdir,
    MC_ATTR_FILE = MC_ATTR_COMMON | sceMcFileAttrClosed | sceMcFileAttrFile,
    TM_YEAR_BASE = 1900,
    TM_MONTH_BASE = 1,
    NO_FILE = -1
};

static int cards[MC_PORTS] = {-1, -1};
static char directories[MC_PORTS][MC_PATH_SIZE];
static int files[MC_FILES];
static int owners[MC_FILES];
static int initialized;
static int pending;
static int last_command;
static int last_result;
static DIR *listing;
static char pattern[MC_NAME_SIZE];
static int listing_port = -1;

static int error_result(void) {
    if (errno == ENOENT || errno == ENOTDIR || errno == EEXIST) return sceMcResNoEntry;
    if (errno == ENOSPC || errno == EDQUOT) return sceMcResFullDevice;
    return sceMcResDeniedPermit;
}

static void close_listing(void) {
    if (listing) closedir(listing);
    listing = NULL;
    listing_port = -1;
}

int sceMcEnd(void) {
    close_listing();
    for (int i = 0; i < MC_FILES; ++i) {
        if (initialized && files[i] >= 0) close(files[i]);
        files[i] = NO_FILE;
    }
    for (int i = 0; i < MC_PORTS; ++i) {
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
    if (!root || !root[0]) return sceMcIniErrKernel;
    if (mkdir(root, S_IRWXU) && errno != EEXIST) return sceMcIniErrKernel;
    int base = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (base < 0) return sceMcIniErrKernel;
    for (int i = 0; i < MC_PORTS; ++i) {
        char name[MC_CARD_DIRECTORY_SIZE];
        snprintf(name, sizeof(name), "card%d", i);
        if (mkdirat(base, name, S_IRWXU) && errno != EEXIST) break;
        cards[i] = openat(base, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (cards[i] < 0) break;
    }
    close(base);
    initialized = 1;
    return cards[0] >= 0 ? sceMcIniSucceed : sceMcIniErrKernel;
}

static const char *default_root(void) {
    static char path[MC_PATH_SIZE];
    if (path[0]) return path;
    const char *home = getenv("HOME");
    struct stat info;
    snprintf(path, sizeof path, "%s/Library/Mobile Documents/com~apple~CloudDocs/Dark Chronicle", home ? home : "");
    if (home && !stat(path, &info) && S_ISDIR(info.st_mode)) {
        snprintf(path, sizeof path, "%s/Library/Mobile Documents/com~apple~CloudDocs/Dark Chronicle/saves", home);
        return path;
    }
    char *preferred = SDL_GetPrefPath("DC2 Toolkit", "Dark Chronicle");
    snprintf(path, sizeof path, "%ssaves", preferred ? preferred : "");
    SDL_free(preferred);
    return path;
}

const char *port_memory_card_root(void) {
    const char *root = getenv("DC2_SAVE_ROOT");
    return root && root[0] ? root : default_root();
}

int sceMcInit(void) {
    if (initialized) return cards[0] >= 0 ? sceMcIniSucceed : sceMcIniErrKernel;
    return port_memory_card_init(port_memory_card_root());
}

static int request(int command) {
    if (pending) return PORT_MC_BUSY;
    pending = 1;
    last_command = command;
    last_result = sceMcResDeniedPermit;
    return 0;
}

int sceMcSync(int mode, int *command, int *result) {
    (void)mode;
    if (!pending) return sceMcExecIdle;
    if (command) *command = last_command;
    if (result) *result = last_result;
    pending = 0;
    return sceMcExecFinish;
}

static int card_status(int port, int slot) {
    if (port < 0 || port >= MC_PORTS || slot < 0 || slot >= MC_SLOTS || cards[port] < 0) return sceMcResFailDetect;
    struct stat status;
    if (!fstatat(cards[port], ".unformatted", &status, AT_SYMLINK_NOFOLLOW)) return sceMcResNoFormat;
    return sceMcResSucceed;
}

static int path_name(int port, const char *name, char *path) {
    if (!name || !name[0]) return sceMcResDeniedPermit;
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
        if ((length == 2 && !memcmp(start, "..", 2)) || length >= MC_NAME_SIZE || start[0] == '.') return sceMcResDeniedPermit;
        if (used + length + 1 >= MC_PATH_SIZE) return sceMcResDeniedPermit;
        if (used) path[used++] = '/';
        memcpy(path + used, start, length);
        used += length;
    }
    path[used] = 0;
    return sceMcResSucceed;
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
    char path[MC_PATH_SIZE];
    status = path_name(port, name, path);
    if (status) { last_result = status; return -1; }
    int parent = parent_path(port, path, leaf);
    if (parent < 0) last_result = error_result();
    return parent;
}

int sceMcGetInfo(int port, int slot, int *type, int *free_size, int *formatted) {
    if (request(sceMcFuncNoCardInfo)) return PORT_MC_BUSY;
    last_result = card_status(port, slot);
    if (type) *type = last_result == sceMcResFailDetect ? sceMcTypeNoCard : sceMcTypePS2;
    if (formatted) *formatted = last_result == sceMcResSucceed;
    if (free_size) {
        *free_size = 0;
        struct statvfs space;
        if (last_result == sceMcResSucceed && !fstatvfs(cards[port], &space)) {
            unsigned long long available = (unsigned long long)space.f_bavail * space.f_frsize / KILOBYTE;
            *free_size = available > MC_CARD_KILOBYTES ? MC_CARD_KILOBYTES : (int)available;
        }
    }
    return 0;
}

int sceMcOpen(int port, int slot, const char *name, int flags) {
    if (request(sceMcFuncNoOpen)) return PORT_MC_BUSY;
    char leaf[MC_NAME_SIZE];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    int mode = flags & MC_OPEN_MODE;
    if (!mode || (flags & ~MC_OPEN_FLAGS)) { close(parent); return 0; }
    int host_flags = mode == SCE_RDONLY ? O_RDONLY : mode == SCE_WRONLY ? O_WRONLY : O_RDWR;
    if (flags & SCE_APPEND) host_flags |= O_APPEND;
    if (flags & SCE_CREAT) host_flags |= O_CREAT;
    if (flags & SCE_TRUNC) host_flags |= O_TRUNC;
    for (int i = 0; i < MC_FILES; ++i) {
        if (files[i] >= 0) continue;
        int fd = openat(parent, leaf, host_flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, S_IRUSR | S_IWUSR);
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
    if (request(command)) return PORT_MC_BUSY;
    if (!initialized || fd < 0 || fd >= MC_FILES || files[fd] < 0) return NO_FILE;
    return 0;
}

int sceMcClose(int fd) {
    int status = file_request(sceMcFuncNoClose, fd);
    if (status) return status == PORT_MC_BUSY ? PORT_MC_BUSY : 0;
    last_result = close(files[fd]) ? error_result() : sceMcResSucceed;
    files[fd] = NO_FILE;
    return 0;
}

int sceMcSeek(int fd, int offset, int origin) {
    static const int whence[] = {[SCE_SEEK_SET] = SEEK_SET, [SCE_SEEK_CUR] = SEEK_CUR, [SCE_SEEK_END] = SEEK_END};
    int status = file_request(sceMcFuncNoSeek, fd);
    if (status) return status == PORT_MC_BUSY ? PORT_MC_BUSY : 0;
    if (origin < SCE_SEEK_SET || origin > SCE_SEEK_END) return 0;
    off_t result = lseek(files[fd], offset, whence[origin]);
    last_result = result < 0 ? error_result() : result > INT_MAX ? sceMcResDeniedPermit : (int)result;
    return 0;
}

int sceMcRead(int fd, void *buffer, int size) {
    int status = file_request(sceMcFuncNoRead, fd);
    if (status) return status == PORT_MC_BUSY ? PORT_MC_BUSY : 0;
    if (size < 0 || (!buffer && size)) return 0;
    ssize_t result = read(files[fd], buffer, (size_t)size);
    last_result = result < 0 ? error_result() : (int)result;
    return 0;
}

int sceMcWrite(int fd, void *buffer, int size) {
    int status = file_request(sceMcFuncNoWrite, fd);
    if (status) return status == PORT_MC_BUSY ? PORT_MC_BUSY : 0;
    if (size < 0 || (!buffer && size)) return 0;
    ssize_t result = write(files[fd], buffer, (size_t)size);
    last_result = result < 0 ? error_result() : (int)result;
    return 0;
}

int sceMcFlush(int fd) {
    int status = file_request(sceMcFuncNoFlush, fd);
    if (status) return status == PORT_MC_BUSY ? PORT_MC_BUSY : 0;
    last_result = fsync(files[fd]) ? error_result() : sceMcResSucceed;
    return 0;
}

int sceMcMkdir(int port, int slot, const char *name) {
    if (request(sceMcFuncNoMkdir)) return PORT_MC_BUSY;
    char leaf[MC_NAME_SIZE];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    last_result = mkdirat(parent, leaf, S_IRWXU) ? error_result() : sceMcResSucceed;
    close(parent);
    return 0;
}

int sceMcChdir(int port, int slot, const char *name, char *current) {
    if (request(sceMcFuncNoChDir)) return PORT_MC_BUSY;
    char leaf[MC_NAME_SIZE], path[MC_PATH_SIZE];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    int fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (fd < 0) { last_result = error_result(); return 0; }
    close(fd);
    if (current) { current[0] = '/'; strcpy(current + 1, directories[port]); }
    path_name(port, name, path);
    strcpy(directories[port], path);
    last_result = sceMcResSucceed;
    return 0;
}

static void entry_time(struct sceMcStDateTime *date, time_t timestamp) {
    struct tm value;
    if (!localtime_r(&timestamp, &value)) return;
    date->sec = (u_char)value.tm_sec;
    date->min = (u_char)value.tm_min;
    date->hour = (u_char)value.tm_hour;
    date->day = (u_char)value.tm_mday;
    date->month = (u_char)(value.tm_mon + TM_MONTH_BASE);
    date->year = (u_short)(value.tm_year + TM_YEAR_BASE);
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
    if (request(sceMcFuncNoGetDir)) return PORT_MC_BUSY;
    last_result = card_status(port, slot);
    if (last_result) return 0;
    last_result = sceMcResDeniedPermit;
    if (count < 0 || (!table && count) || (mode != MC_DIR_FIRST && mode != MC_DIR_NEXT)) return 0;
    if (mode == MC_DIR_FIRST) {
        close_listing();
        char leaf[MC_NAME_SIZE];
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
        if (entry->d_name[0] == '.' || strlen(entry->d_name) >= MC_NAME_SIZE || !name_matches(pattern, entry->d_name)) continue;
        struct stat info;
        if (fstatat(dirfd(listing), entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) continue;
        if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) continue;
        struct sceMcTblGetDir *row = &table[last_result++];
        memset(row, 0, sizeof(*row));
        entry_time(&row->create, info.st_ctime);
        entry_time(&row->modify, info.st_mtime);
        row->file_size_byte = info.st_size > UINT32_MAX ? UINT32_MAX : (uint32_t)info.st_size;
        row->attr_file = S_ISDIR(info.st_mode) ? MC_ATTR_DIRECTORY : MC_ATTR_FILE;
        strcpy(row->entry_name, entry->d_name);
    }
    return 0;
}

int sceMcRename(int port, int slot, char *old_name, char *new_name) {
    if (request(sceMcFuncNoFileInfo)) return PORT_MC_BUSY;
    char old_leaf[MC_NAME_SIZE], new_leaf[MC_NAME_SIZE];
    int old_parent = resolve(port, slot, old_name, old_leaf);
    if (old_parent < 0) return 0;
    int new_parent = resolve(port, slot, new_name, new_leaf);
    if (new_parent < 0) { close(old_parent); return 0; }
    struct stat info;
    if (!fstatat(new_parent, new_leaf, &info, AT_SYMLINK_NOFOLLOW)) last_result = sceMcResFailReplace;
    else last_result = renameat(old_parent, old_leaf, new_parent, new_leaf) ? error_result() : sceMcResSucceed;
    close(old_parent);
    close(new_parent);
    return 0;
}

int sceMcDelete(int port, int slot, char *name) {
    if (request(sceMcFuncNoDelete)) return PORT_MC_BUSY;
    char leaf[MC_NAME_SIZE];
    int parent = resolve(port, slot, name, leaf);
    if (parent < 0) return 0;
    struct stat info;
    if (fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW)) last_result = error_result();
    else last_result = unlinkat(parent, leaf, S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0) ? error_result() : sceMcResSucceed;
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
    if (request(command)) return PORT_MC_BUSY;
    last_result = card_status(port, slot);
    if (last_result == sceMcResFailDetect) return 0;
    close_listing();
    for (int i = 0; i < MC_FILES; ++i) {
        if (files[i] >= 0 && owners[i] == port) { close(files[i]); files[i] = NO_FILE; }
    }
    directories[port][0] = 0;
    last_result = clear_directory(cards[port]) ? error_result() : sceMcResSucceed;
    if (!last_result && command == sceMcFuncNoUnformat) {
        int marker = openat(cards[port], ".unformatted", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (marker < 0) last_result = error_result();
        else close(marker);
    }
    return 0;
}

int sceMcFormat(int port, int slot) { return format_card(sceMcFuncNoFormat, port, slot); }
int sceMcUnformat(int port, int slot) { return format_card(sceMcFuncNoUnformat, port, slot); }
