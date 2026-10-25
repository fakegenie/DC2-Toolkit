#include "cdvd.h"
#include "executable.h"
#include "split_data.h"
#include "port_elf.h"
#include "port_game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MY_DRAW_ENV_START 0x50
#define SYMBOL_OFFSET_TEXT(offset) #offset
#define SYMBOL_OFFSET(offset) SYMBOL_OFFSET_TEXT(offset)

enum {
    VU_PROG_3DSP_SIZE = 0x6d0,
    VU_PROG_SDW_SIZE = 0x970,
    VU_PROGMAIN_SIZE = 0x2d0,
    VU_PROG0_SIZE = 0x3040,
    VU_PROG_WTR_SIZE = 0x350,
    MY_DRAW_ENV_SIZE = 0x90,
    MY_DMA_START0_SIZE = MY_DRAW_ENV_START + MY_DRAW_ENV_SIZE,
    GP_SIZE = 16,
    QUADWORD = 16,
    EXECUTABLE_SIZE_LIMIT = 64 * 1024 * 1024
};

PORT_GAME_BEGIN
_Alignas(QUADWORD) unsigned char Vu_prog_3dsp[VU_PROG_3DSP_SIZE];
_Alignas(QUADWORD) unsigned char Vu_prog_sdw[VU_PROG_SDW_SIZE];
_Alignas(QUADWORD) unsigned char Vu_progmain[VU_PROGMAIN_SIZE];
_Alignas(QUADWORD) unsigned char Vu_prog0[VU_PROG0_SIZE];
_Alignas(QUADWORD) unsigned char Vu_prog_wtr[VU_PROG_WTR_SIZE];
_Alignas(QUADWORD) unsigned char My_dma_start0[MY_DMA_START0_SIZE];
__asm__(".globl My_DrawEnv\n.set My_DrawEnv, My_dma_start0 + " SYMBOL_OFFSET(MY_DRAW_ENV_START));
_Alignas(QUADWORD) unsigned char _gp[GP_SIZE];
PORT_GAME_END

static const struct {
    const char *name;
    const char *end;
    unsigned char *data;
    size_t capacity;
    size_t trailing;
} targets[] = {
    {"Vu_prog_3dsp", "Vu_prog_sdw", Vu_prog_3dsp, sizeof(Vu_prog_3dsp), 0},
    {"Vu_prog_sdw", "Vu_progmain", Vu_prog_sdw, sizeof(Vu_prog_sdw), 0},
    {"Vu_progmain", "Vu_prog0", Vu_progmain, sizeof(Vu_progmain), 0},
    {"Vu_prog0", "Vu_prog_wtr", Vu_prog0, sizeof(Vu_prog0), 0},
    {"Vu_prog_wtr", "__data_start", Vu_prog_wtr, sizeof(Vu_prog_wtr), 0},
    {"My_dma_start0", "My_DrawEnv", My_dma_start0, sizeof(My_dma_start0), MY_DRAW_ENV_SIZE}
};

static int span(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

static int symbol(const unsigned char *data, size_t size, const Elf32_Ehdr *header, const char *name, Elf32_Sym *result) {
    for (size_t i = 0; i < header->e_shnum; i++) {
        Elf32_Shdr section;
        memcpy(&section, data + header->e_shoff + i * sizeof(section), sizeof(section));
        if (section.sh_type != SHT_SYMTAB) continue;
        if (section.sh_link >= header->e_shnum || section.sh_entsize != sizeof(Elf32_Sym) || section.sh_size % sizeof(Elf32_Sym) || !span(size, section.sh_offset, section.sh_size)) return 0;
        Elf32_Shdr strings;
        memcpy(&strings, data + header->e_shoff + section.sh_link * sizeof(strings), sizeof(strings));
        if (strings.sh_type != SHT_STRTAB || !span(size, strings.sh_offset, strings.sh_size)) return 0;
        for (size_t position = 0; position < section.sh_size; position += sizeof(Elf32_Sym)) {
            Elf32_Sym entry;
            memcpy(&entry, data + section.sh_offset + position, sizeof(entry));
            if (entry.st_name >= strings.sh_size || entry.st_shndx == SHN_UNDEF) continue;
            const char *text = (const char *)data + strings.sh_offset + entry.st_name;
            if (!memchr(text, 0, strings.sh_size - entry.st_name)) return 0;
            if (!strcmp(text, name)) { *result = entry; return 1; }
        }
    }
    return 0;
}

int port_split_data_init(void) {
    struct sceCdlFILE file;
    if (!port_cdvd_find(PORT_EXECUTABLE_PATH, &file) || file.size < sizeof(Elf32_Ehdr) || file.size > EXECUTABLE_SIZE_LIMIT) {
        fputs("disc executable missing or invalid\n", stderr);
        return 0;
    }
    unsigned char *data = malloc(file.size);
    if (!data) return 0;
    int success = 0;
    if (!port_cdvd_read_file(&file, 0, data, file.size)) goto done;
    Elf32_Ehdr header;
    memcpy(&header, data, sizeof(header));
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS32 || header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_MIPS || header.e_shentsize != sizeof(Elf32_Shdr) || !header.e_shnum || !span(file.size, header.e_shoff, (uint64_t)header.e_shnum * sizeof(Elf32_Shdr))) goto done;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        Elf32_Sym start, end;
        if (!symbol(data, file.size, &header, targets[i].name, &start) || !symbol(data, file.size, &header, targets[i].end, &end) || start.st_shndx >= header.e_shnum || start.st_shndx != end.st_shndx || end.st_value < start.st_value) goto done;
        Elf32_Shdr section;
        memcpy(&section, data + header.e_shoff + start.st_shndx * sizeof(section), sizeof(section));
        uint64_t length = targets[i].trailing ? (uint64_t)end.st_value - start.st_value + targets[i].trailing :
                          start.st_size ? start.st_size : (uint64_t)end.st_value - start.st_value;
        if (!length || length > targets[i].capacity || section.sh_type == SHT_NOBITS || start.st_value < section.sh_addr || !span(section.sh_size, (uint64_t)start.st_value - section.sh_addr, length)) goto done;
        uint64_t offset = (uint64_t)section.sh_offset + start.st_value - section.sh_addr;
        if (!span(file.size, offset, length)) goto done;
        memcpy(targets[i].data, data + offset, length);
        fprintf(stderr, "executable: %s (%zu bytes)\n", targets[i].name, (size_t)length);
    }
    success = 1;
done:
    if (!success) fputs("cannot load split data from disc executable\n", stderr);
    free(data);
    return success;
}
