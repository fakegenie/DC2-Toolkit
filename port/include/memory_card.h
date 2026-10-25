#pragma once

#include <stdint.h>

struct sceMcStDateTime {
    uint8_t reserved, second, minute, hour, day, month;
    uint16_t year;
};

struct sceMcTblGetDir {
    struct sceMcStDateTime create, modify;
    uint32_t file_size_byte;
    uint16_t attr_file, reserve1;
    uint32_t reserve2, pda_apl_no;
    char entry_name[32];
};

int port_memory_card_init(const char *root);
const char *port_memory_card_root(void);
int sceMcInit(void);
int sceMcEnd(void);
int sceMcSync(int mode, int *command, int *result);
int sceMcGetInfo(int port, int slot, int *type, int *free_size, int *formatted);
int sceMcOpen(int port, int slot, const char *name, int flags);
int sceMcClose(int fd);
int sceMcSeek(int fd, int offset, int origin);
int sceMcRead(int fd, void *buffer, int size);
int sceMcWrite(int fd, void *buffer, int size);
int sceMcFlush(int fd);
int sceMcMkdir(int port, int slot, const char *name);
int sceMcChdir(int port, int slot, const char *name, char *current);
int sceMcGetDir(int port, int slot, const char *name, int mode, int count, struct sceMcTblGetDir *table);
int sceMcRename(int port, int slot, char *old_name, char *new_name);
int sceMcDelete(int port, int slot, char *name);
int sceMcFormat(int port, int slot);
int sceMcUnformat(int port, int slot);
