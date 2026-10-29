#include "memcard.hpp"
#include "menumain.hpp"
#include "port_mod.h"
#include <cstdlib>
#include <cstring>

static int setting_number(const char *name) {
    const char *value = port_setting(name);
    return value ? atoi(value) : 0;
}

int port_boot_language(void) { return setting_number("boot.language"); }

int port_skip_intro(void) {
    const char *value = port_setting("boot.skip_intro");
    return value && !strcmp(value, "true");
}

int port_boot_save(CMemoryCardManager *card) {
    int slot = setting_number("boot.save");
    if (slot <= 0) return 0;
    static SAVEDATA_FORMAT save;
    card->save_buffer = &save;
    card->port = 0;
    card->file_no = slot - 1;
    card->load_program_loop_no = -1;
    card->InitError();
    card->SetFuncNo(MC_FUNC_LOAD);
    while (card->func_no == MC_FUNC_LOAD) card->Step();
    card->save_buffer = NULL;
    if (card->error.code != MC_ERROR_NONE || card->load_program_loop_no < 0) return 0;
    MenuArg.result[0] = card->load_program_loop_no;
    MenuArg.result[1] = card->load_map_no;
    MenuArg.result[2] = card->load_dungeon_no;
    MenuArg.result[3] = card->load_floor_id;
    MenuArg.result[4] = card->load_dng_tree_flag;
    return 1;
}
