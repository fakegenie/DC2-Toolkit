#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "memory_card.h"
#include "sce/sifdev.h"
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

enum {
    MC_DIR_FIRST = 0,
    MC_DIR_NEXT = 1,
    MC_ATTR_FILE = sceMcFileAttrExists | sceMcFile0400 | sceMcFileAttrClosed | sceMcFileAttrFile | sceMcFileAttrReadable | sceMcFileAttrWriteable | sceMcFileAttrExecutable
};

static int settle(int submitted, int expected) {
    int command = -1, result = -999;
    assert(submitted == 0);
    assert(sceMcSync(MC_NOWAIT, &command, &result) == sceMcExecFinish);
    assert(command == expected);
    assert(sceMcSync(MC_WAIT, NULL, NULL) == sceMcExecIdle);
    return result;
}

static char *temporary_folder(char *root, size_t size) {
#ifdef _WIN32
    snprintf(root, size, "%s/dc2-saves-%d", getenv("TEMP"), _getpid());
    return _mkdir(root) ? NULL : root;
#else
    snprintf(root, size, "/tmp/dc2-saves-XXXXXX");
    return mkdtemp(root);
#endif
}

int main(void) {
    assert(settle(sceMcClose(0), sceMcFuncNoClose) == sceMcResDeniedPermit);
    char root[1024];
    assert(temporary_folder(root, sizeof root));
    assert(port_memory_card_init(root) == 0);
    int type, free_size, formatted;
    assert(sceMcGetInfo(0, 1, &type, &free_size, &formatted) == 0);
    assert(sceMcMkdir(0, 1, "busy") == PORT_MC_BUSY);
    assert(settle(0, sceMcFuncNoCardInfo) == sceMcResSucceed);
    assert(type == sceMcTypePS2 && free_size > 400 && formatted == 1);
    assert(settle(sceMcGetInfo(2, 1, &type, &free_size, &formatted), sceMcFuncNoCardInfo) == sceMcResFailDetect);
    assert(type == sceMcTypeNoCard && free_size == 0 && formatted == 0);
    assert(settle(sceMcMkdir(0, 1, "SAVE"), sceMcFuncNoMkdir) == 0);
    assert(settle(sceMcMkdir(0, 1, "SAVE"), sceMcFuncNoMkdir) == sceMcResNoEntry);
    assert(settle(sceMcChdir(0, 1, "/SAVE", NULL), sceMcFuncNoChDir) == 0);
    int fd = settle(sceMcOpen(0, 1, "data", SCE_CREAT | SCE_WRONLY), sceMcFuncNoOpen);
    assert(fd >= 0);
    unsigned char payload[65537], restored[65537];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (unsigned char)(i * 37);
    assert(settle(sceMcWrite(fd, payload, sizeof(payload)), sceMcFuncNoWrite) == sizeof(payload));
    assert(settle(sceMcFlush(fd), sceMcFuncNoFlush) == 0);
    assert(settle(sceMcClose(fd), sceMcFuncNoClose) == 0);
    assert(sceMcEnd() == 0);
    assert(setenv("DC2_SAVE_ROOT", root, 1) == 0);
    assert(sceMcInit() == 0);
    fd = settle(sceMcOpen(0, 0, "/SAVE/data", SCE_RDONLY), sceMcFuncNoOpen);
    assert(fd >= 0);
    assert(settle(sceMcRead(fd, restored, sizeof(restored)), sceMcFuncNoRead) == sizeof(restored));
    assert(!memcmp(payload, restored, sizeof(payload)));
    assert(settle(sceMcRead(fd, restored, 1), sceMcFuncNoRead) == 0);
    assert(settle(sceMcSeek(fd, 123, SCE_SEEK_SET), sceMcFuncNoSeek) == 123);
    assert(settle(sceMcRead(fd, restored, 19), sceMcFuncNoRead) == 19);
    assert(!memcmp(payload + 123, restored, 19));
    assert(settle(sceMcWrite(fd, payload, 1), sceMcFuncNoWrite) == sceMcResDeniedPermit);
    assert(settle(sceMcClose(fd), sceMcFuncNoClose) == 0);
    assert(settle(sceMcRead(fd, restored, 1), sceMcFuncNoRead) == sceMcResDeniedPermit);
    assert(settle(sceMcOpen(1, 1, "/SAVE/data", SCE_RDONLY), sceMcFuncNoOpen) == sceMcResNoEntry);
    fd = settle(sceMcOpen(0, 1, "/SAVE/second", SCE_CREAT | SCE_WRONLY), sceMcFuncNoOpen);
    assert(fd >= 0 && settle(sceMcClose(fd), sceMcFuncNoClose) == 0);
    struct sceMcTblGetDir rows[3];
    int seen = 0;
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", MC_DIR_FIRST, 1, rows), sceMcFuncNoGetDir) == 1);
    seen += !strcmp(rows[0].entry_name, "data") ? 1 : 2;
    assert(rows[0].attr_file == MC_ATTR_FILE);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", MC_DIR_NEXT, 1, rows), sceMcFuncNoGetDir) == 1);
    seen += !strcmp(rows[0].entry_name, "data") ? 1 : 2;
    assert(seen == 3);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", MC_DIR_NEXT, 1, rows), sceMcFuncNoGetDir) == 0);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/data", MC_DIR_FIRST, 3, rows), sceMcFuncNoGetDir) == 1);
    assert(rows[0].file_size_byte == sizeof(payload) && rows[0].modify.year >= 2026);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", MC_DIR_FIRST, 3, rows), sceMcFuncNoGetDir) == 2);
    assert(settle(sceMcRename(0, 1, "/SAVE/second", "/SAVE/data"), sceMcFuncNoFileInfo) == sceMcResFailReplace);
    assert(settle(sceMcRename(0, 1, "/SAVE/second", "/SAVE/renamed"), sceMcFuncNoFileInfo) == 0);
    assert(settle(sceMcOpen(0, 1, "../escape", SCE_CREAT | SCE_WRONLY), sceMcFuncNoOpen) == sceMcResDeniedPermit);
    char link[1024];
#ifndef _WIN32
    // Windows makes symlinks only with elevated rights, and the port does not refuse them there
    char outside[] = "/tmp/dc2-outside-XXXXXX";
    int outside_fd = mkstemp(outside);
    assert(outside_fd >= 0);
    assert(write(outside_fd, "keep", 4) == 4);
    snprintf(link, sizeof(link), "%s/card0/link", root);
    assert(symlink(outside, link) == 0);
    assert(settle(sceMcOpen(0, 1, "/link", SCE_TRUNC | SCE_CREAT | SCE_WRONLY), sceMcFuncNoOpen) == sceMcResDeniedPermit);
    assert(settle(sceMcGetDir(0, 1, "/link", MC_DIR_FIRST, 3, rows), sceMcFuncNoGetDir) == 0);
    assert(unlink(link) == 0);
    snprintf(link, sizeof(link), "%s/card0/link", root);
    assert(symlink("/tmp", link) == 0);
    assert(settle(sceMcOpen(0, 1, "/link/escape", SCE_CREAT | SCE_WRONLY), sceMcFuncNoOpen) == sceMcResNoEntry);
#endif
    assert(settle(sceMcDelete(0, 1, "/SAVE"), sceMcFuncNoDelete) == sceMcResDeniedPermit);
    assert(settle(sceMcDelete(0, 1, "/SAVE/renamed"), sceMcFuncNoDelete) == 0);
    const char *names[] = {"mc", "mc0", "mc01", "mc012", "wa", "wab", "wacb", "waccb", "waxb", "wax"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(settle(sceMcMkdir(0, 1, names[i]), sceMcFuncNoMkdir) == 0);
    }
    struct sceMcTblGetDir matches[16];
    assert(settle(sceMcGetDir(0, 1, "/mc??", MC_DIR_FIRST, 16, matches), sceMcFuncNoGetDir) == 3);
    assert(settle(sceMcGetDir(0, 1, "/wa?b", MC_DIR_FIRST, 16, matches), sceMcFuncNoGetDir) == 3);
    assert(settle(sceMcGetDir(0, 1, "/wa??b", MC_DIR_FIRST, 16, matches), sceMcFuncNoGetDir) == 4);
    assert(settle(sceMcGetDir(0, 1, "/wa*?b", MC_DIR_FIRST, 16, matches), sceMcFuncNoGetDir) == 4);
    assert(settle(sceMcGetDir(0, 1, "/wa*??*", MC_DIR_FIRST, 16, matches), sceMcFuncNoGetDir) == 6);
    assert(settle(sceMcGetDir(0, 1, "/wa*z", MC_DIR_FIRST, 16, matches), sceMcFuncNoGetDir) == 0);
    assert(settle(sceMcUnformat(0, 1), sceMcFuncNoUnformat) == 0);
    assert(settle(sceMcGetInfo(0, 1, &type, &free_size, &formatted), sceMcFuncNoCardInfo) == sceMcResNoFormat);
    assert(type == sceMcTypePS2 && formatted == 0);
    assert(sceMcEnd() == 0 && sceMcInit() == 0);
    assert(settle(sceMcOpen(0, 1, "/anything", SCE_CREAT | SCE_WRONLY), sceMcFuncNoOpen) == sceMcResNoFormat);
    assert(settle(sceMcFormat(0, 1), sceMcFuncNoFormat) == 0);
    assert(settle(sceMcGetDir(0, 1, "/*", MC_DIR_FIRST, 3, rows), sceMcFuncNoGetDir) == 0);
    assert(settle(sceMcFormat(1, 1), sceMcFuncNoFormat) == 0);
    assert(sceMcEnd() == 0);
#ifndef _WIN32
    char kept[4];
    assert(pread(outside_fd, kept, 4, 0) == 4 && !memcmp(kept, "keep", 4));
    close(outside_fd);
    assert(unlink(outside) == 0);
#endif
    snprintf(link, sizeof(link), "%s/card0", root);
    assert(rmdir(link) == 0);
    snprintf(link, sizeof(link), "%s/card1", root);
    assert(rmdir(link) == 0);
    assert(rmdir(root) == 0);
    return 0;
}
