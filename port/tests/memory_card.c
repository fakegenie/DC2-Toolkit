#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "memory_card.h"

static int settle(int submitted, int expected) {
    int command = -1, result = -999;
    assert(submitted == 0);
    assert(sceMcSync(1, &command, &result) == 1);
    assert(command == expected);
    assert(sceMcSync(0, NULL, NULL) == -1);
    return result;
}

int main(void) {
    _Static_assert(sizeof(struct sceMcTblGetDir) == 64, "directory size");
    _Static_assert(offsetof(struct sceMcTblGetDir, entry_name) == 32, "directory name");
    assert(settle(sceMcClose(0), 3) == -5);
    char root[] = "/tmp/dc2-saves-XXXXXX";
    assert(mkdtemp(root));
    assert(port_memory_card_init(root) == 0);
    int type, free_size, formatted;
    assert(sceMcGetInfo(0, 1, &type, &free_size, &formatted) == 0);
    assert(sceMcMkdir(0, 1, "busy") == -200);
    assert(settle(0, 1) == 0);
    assert(type == 2 && free_size > 400 && formatted == 1);
    assert(settle(sceMcGetInfo(2, 1, &type, &free_size, &formatted), 1) == -12);
    assert(type == 0 && free_size == 0 && formatted == 0);
    assert(settle(sceMcMkdir(0, 1, "SAVE"), 11) == 0);
    assert(settle(sceMcMkdir(0, 1, "SAVE"), 11) == -4);
    assert(settle(sceMcChdir(0, 1, "/SAVE", NULL), 12) == 0);
    int fd = settle(sceMcOpen(0, 1, "data", 0x202), 2);
    assert(fd >= 0);
    unsigned char payload[65537], restored[65537];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (unsigned char)(i * 37);
    assert(settle(sceMcWrite(fd, payload, sizeof(payload)), 6) == sizeof(payload));
    assert(settle(sceMcFlush(fd), 10) == 0);
    assert(settle(sceMcClose(fd), 3) == 0);
    assert(sceMcEnd() == 0);
    assert(setenv("DC2_SAVE_ROOT", root, 1) == 0);
    assert(sceMcInit() == 0);
    fd = settle(sceMcOpen(0, 0, "/SAVE/data", 1), 2);
    assert(fd >= 0);
    assert(settle(sceMcRead(fd, restored, sizeof(restored)), 5) == sizeof(restored));
    assert(!memcmp(payload, restored, sizeof(payload)));
    assert(settle(sceMcRead(fd, restored, 1), 5) == 0);
    assert(settle(sceMcSeek(fd, 123, 0), 4) == 123);
    assert(settle(sceMcRead(fd, restored, 19), 5) == 19);
    assert(!memcmp(payload + 123, restored, 19));
    assert(settle(sceMcWrite(fd, payload, 1), 6) == -5);
    assert(settle(sceMcClose(fd), 3) == 0);
    assert(settle(sceMcRead(fd, restored, 1), 5) == -5);
    assert(settle(sceMcOpen(1, 1, "/SAVE/data", 1), 2) == -4);
    fd = settle(sceMcOpen(0, 1, "/SAVE/second", 0x202), 2);
    assert(fd >= 0 && settle(sceMcClose(fd), 3) == 0);
    struct sceMcTblGetDir rows[3];
    int seen = 0;
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", 0, 1, rows), 13) == 1);
    seen += !strcmp(rows[0].entry_name, "data") ? 1 : 2;
    assert(rows[0].attr_file == 0x8497);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", 1, 1, rows), 13) == 1);
    seen += !strcmp(rows[0].entry_name, "data") ? 1 : 2;
    assert(seen == 3);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", 1, 1, rows), 13) == 0);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/data", 0, 3, rows), 13) == 1);
    assert(rows[0].file_size_byte == sizeof(payload) && rows[0].modify.year >= 2026);
    assert(settle(sceMcGetDir(0, 1, "/SAVE/*", 0, 3, rows), 13) == 2);
    assert(settle(sceMcRename(0, 1, "/SAVE/second", "/SAVE/data"), 14) == -8);
    assert(settle(sceMcRename(0, 1, "/SAVE/second", "/SAVE/renamed"), 14) == 0);
    assert(settle(sceMcOpen(0, 1, "../escape", 0x202), 2) == -5);
    char outside[] = "/tmp/dc2-outside-XXXXXX";
    int outside_fd = mkstemp(outside);
    assert(outside_fd >= 0);
    assert(write(outside_fd, "keep", 4) == 4);
    char link[1024];
    snprintf(link, sizeof(link), "%s/card0/link", root);
    assert(symlink(outside, link) == 0);
    assert(settle(sceMcOpen(0, 1, "/link", 0x602), 2) == -5);
    assert(settle(sceMcGetDir(0, 1, "/link", 0, 3, rows), 13) == 0);
    assert(unlink(link) == 0);
    snprintf(link, sizeof(link), "%s/card0/link", root);
    assert(symlink("/tmp", link) == 0);
    assert(settle(sceMcOpen(0, 1, "/link/escape", 0x202), 2) == -4);
    assert(settle(sceMcDelete(0, 1, "/SAVE"), 15) == -5);
    assert(settle(sceMcDelete(0, 1, "/SAVE/renamed"), 15) == 0);
    const char *names[] = {"mc", "mc0", "mc01", "mc012", "wa", "wab", "wacb", "waccb", "waxb", "wax"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(settle(sceMcMkdir(0, 1, names[i]), 11) == 0);
    }
    struct sceMcTblGetDir matches[16];
    assert(settle(sceMcGetDir(0, 1, "/mc??", 0, 16, matches), 13) == 3);
    assert(settle(sceMcGetDir(0, 1, "/wa?b", 0, 16, matches), 13) == 3);
    assert(settle(sceMcGetDir(0, 1, "/wa??b", 0, 16, matches), 13) == 4);
    assert(settle(sceMcGetDir(0, 1, "/wa*?b", 0, 16, matches), 13) == 4);
    assert(settle(sceMcGetDir(0, 1, "/wa*??*", 0, 16, matches), 13) == 6);
    assert(settle(sceMcGetDir(0, 1, "/wa*z", 0, 16, matches), 13) == 0);
    assert(settle(sceMcUnformat(0, 1), 17) == 0);
    assert(settle(sceMcGetInfo(0, 1, &type, &free_size, &formatted), 1) == -2);
    assert(type == 2 && formatted == 0);
    assert(sceMcEnd() == 0 && sceMcInit() == 0);
    assert(settle(sceMcOpen(0, 1, "/anything", 0x202), 2) == -2);
    assert(settle(sceMcFormat(0, 1), 16) == 0);
    assert(settle(sceMcGetDir(0, 1, "/*", 0, 3, rows), 13) == 0);
    assert(settle(sceMcFormat(1, 1), 16) == 0);
    assert(sceMcEnd() == 0);
    char kept[4];
    assert(pread(outside_fd, kept, 4, 0) == 4 && !memcmp(kept, "keep", 4));
    close(outside_fd);
    assert(unlink(outside) == 0);
    snprintf(link, sizeof(link), "%s/card0", root);
    assert(rmdir(link) == 0);
    snprintf(link, sizeof(link), "%s/card1", root);
    assert(rmdir(link) == 0);
    assert(rmdir(root) == 0);
    return 0;
}
