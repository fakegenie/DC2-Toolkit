#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

struct statvfs {
    unsigned long long f_bavail;
    unsigned long long f_frsize;
};

enum { DIRECTORY_LIMIT = 64, DIRECTORY_BASE = 1 << 24, PATH_SIZE = 1024, O_DIRECTORY = 0x01000000, AT_REMOVEDIR = 1 };

static struct {
    int used;
    char path[PATH_SIZE];
    DIR *listing;
} directories[DIRECTORY_LIMIT];

static int slot(int fd) {
    int index = fd - DIRECTORY_BASE;
    return index >= 0 && index < DIRECTORY_LIMIT && directories[index].used ? index : -1;
}

static int join(int directory, const char *name, char *path) {
    int index = slot(directory);
    if (index < 0) {
        errno = EBADF;
        return 0;
    }
    if (!strcmp(name, ".")) return snprintf(path, PATH_SIZE, "%s", directories[index].path) < PATH_SIZE;
    if (snprintf(path, PATH_SIZE, "%s/%s", directories[index].path, name) < PATH_SIZE) return 1;
    errno = ENAMETOOLONG;
    return 0;
}

static int open_directory(const char *path) {
    struct stat info;
    if (stat(path, &info)) return -1;
    if (!S_ISDIR(info.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    for (int index = 0; index < DIRECTORY_LIMIT; ++index) {
        if (directories[index].used) continue;
        directories[index].used = 1;
        directories[index].listing = NULL;
        snprintf(directories[index].path, PATH_SIZE, "%s", path);
        return DIRECTORY_BASE + index;
    }
    errno = EMFILE;
    return -1;
}

static int open_file(const char *path, int flags, int mode) {
    if (flags & O_DIRECTORY) return open_directory(path);
    return _open(path, flags | O_BINARY, mode ? _S_IREAD | _S_IWRITE : 0);
}

ssize_t pread(int fd, void *buffer, size_t size, off_t offset) {
    HANDLE file = (HANDLE)_get_osfhandle(fd);
    if (file == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED at;
    memset(&at, 0, sizeof at);
    at.Offset = (DWORD)(uint64_t)offset;
    at.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD read = 0;
    if (ReadFile(file, buffer, size > 0x40000000 ? 0x40000000 : (DWORD)size, &read, &at)) return (ssize_t)read;
    if (GetLastError() == ERROR_HANDLE_EOF) return 0;
    errno = EIO;
    return -1;
}

int port_open(const char *path, int flags, ...) {
    va_list arguments;
    va_start(arguments, flags);
    int mode = flags & O_CREAT ? va_arg(arguments, int) : 0;
    va_end(arguments);
    return open_file(path, flags, mode);
}

int port_openat(int directory, const char *name, int flags, ...) {
    va_list arguments;
    va_start(arguments, flags);
    int mode = flags & O_CREAT ? va_arg(arguments, int) : 0;
    va_end(arguments);
    char path[PATH_SIZE];
    return join(directory, name, path) ? open_file(path, flags, mode) : -1;
}

int port_close(int fd) {
    int index = slot(fd);
    if (index < 0) return _close(fd);
    directories[index].used = 0;
    return 0;
}

int port_mkdir(const char *path, int mode) {
    (void)mode;
    return _mkdir(path);
}

int port_mkdirat(int directory, const char *name, int mode) {
    char path[PATH_SIZE];
    return join(directory, name, path) ? port_mkdir(path, mode) : -1;
}

int port_fstatat(int directory, const char *name, struct stat *info, int flags) {
    (void)flags;
    char path[PATH_SIZE];
    return join(directory, name, path) ? stat(path, info) : -1;
}

int port_unlinkat(int directory, const char *name, int flags) {
    char path[PATH_SIZE];
    if (!join(directory, name, path)) return -1;
    return flags & AT_REMOVEDIR ? _rmdir(path) : _unlink(path);
}

int port_renameat(int old_directory, const char *old_name, int new_directory, const char *new_name) {
    char old_path[PATH_SIZE], new_path[PATH_SIZE];
    if (!join(old_directory, old_name, old_path) || !join(new_directory, new_name, new_path)) return -1;
    return rename(old_path, new_path);
}

DIR *port_fdopendir(int directory) {
    int index = slot(directory);
    if (index < 0) {
        errno = EBADF;
        return NULL;
    }
    directories[index].listing = opendir(directories[index].path);
    return directories[index].listing;
}

int port_closedir(DIR *listing) {
    for (int index = 0; index < DIRECTORY_LIMIT; ++index) {
        if (directories[index].used && directories[index].listing == listing) directories[index].used = 0;
    }
    return closedir(listing);
}

int port_dirfd(DIR *listing) {
    for (int index = 0; index < DIRECTORY_LIMIT; ++index) {
        if (directories[index].used && directories[index].listing == listing) return DIRECTORY_BASE + index;
    }
    errno = EBADF;
    return -1;
}

int port_fstatvfs(int directory, struct statvfs *space) {
    int index = slot(directory);
    wchar_t path[PATH_SIZE];
    ULARGE_INTEGER available;
    if (index < 0 || !MultiByteToWideChar(CP_UTF8, 0, directories[index].path, -1, path, PATH_SIZE) ||
        !GetDiskFreeSpaceExW(path, &available, NULL, NULL)) {
        errno = EIO;
        return -1;
    }
    space->f_bavail = available.QuadPart;
    space->f_frsize = 1;
    return 0;
}

int port_fsync(int fd) { return _commit(fd); }
