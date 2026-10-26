#pragma once
// The POSIX calls the port's file code uses that MinGW lacks (port/windows/posix.c). Directory descriptors are slots that
// keep a directory's path, which the *at calls join to the name they get; files open in binary mode. Paths are UTF-8, the
// C runtime's code page under the port's manifest. Windows has no symlinks to refuse here, so O_NOFOLLOW is nothing.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define O_DIRECTORY 0x01000000
#define O_NOFOLLOW 0
#define O_CLOEXEC O_NOINHERIT
#define O_NONBLOCK 0
#define AT_SYMLINK_NOFOLLOW 0
#define AT_REMOVEDIR 1
#ifndef EDQUOT
#define EDQUOT ENOSPC
#endif

struct statvfs {
    unsigned long long f_bavail;
    unsigned long long f_frsize;
};

ssize_t pread(int fd, void *buffer, size_t size, off_t offset);
int port_open(const char *path, int flags, ...);
int port_openat(int directory, const char *name, int flags, ...);
int port_close(int fd);
int port_mkdir(const char *path, int mode);
int port_mkdirat(int directory, const char *name, int mode);
int port_fstatat(int directory, const char *name, struct stat *info, int flags);
int port_unlinkat(int directory, const char *name, int flags);
int port_renameat(int old_directory, const char *old_name, int new_directory, const char *new_name);
DIR *port_fdopendir(int directory);
int port_closedir(DIR *listing);
int port_dirfd(DIR *listing);
int port_fstatvfs(int directory, struct statvfs *space);
int port_fsync(int fd);

#define open port_open
#define openat port_openat
#define close port_close
#define mkdir port_mkdir
#define mkdirat port_mkdirat
#define fstatat port_fstatat
#define unlinkat port_unlinkat
#define renameat port_renameat
#define fdopendir port_fdopendir
#define closedir port_closedir
#define dirfd port_dirfd
#define fstatvfs port_fstatvfs
#define fsync port_fsync
