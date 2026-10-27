#include "memcard.hpp"
#include "inventmn.hpp"
#include "mainloop.hpp"
#include <cassert>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern CSaveData *ActiveSaveData asm(SYMBOL_PREFIX "_ZL14ActiveSaveData");
extern "C" int port_memory_card_init(const char *);
extern "C" char **port_arguments;
char **port_arguments;
void MakeMemoryCardFileName(int, char *);

static_assert(sizeof(CDC2AlbumData) == 0x64cb0);
static_assert(sizeof(USER_PICTURE_INFO) == 0x18);
static_assert(offsetof(USER_PICTURE_INFO, image) == 0x14);
static_assert(sizeof(CInventUserData) == 0x3ce60);
static_assert(offsetof(CInventUserData, photo) == 0x408);
static_assert(offsetof(CInventUserData, photo_work) == 0xd60);
static_assert(sizeof(CUserDataManager) == 0x457a0);
static_assert(offsetof(CUserDataManager, invent_data) == 0x7f30);
static_assert(offsetof(CSaveData, user_data) == 0x1d2a0);
static_assert(sizeof(CSaveData) == 0x65930);
static_assert(sizeof(SAVEDATA_FORMAT) == 0x659c0);
static_assert(offsetof(SAVEDATA_FORMAT, incomplete) == 0x45);
static_assert(offsetof(SAVEDATA_FORMAT, save_data) == 0x80);

static CSaveData data;
static unsigned char icon_bytes[3][4096];

static void run_operation(CMemoryCardManager *manager, int operation) {
    manager->SetFuncNo(operation);
    for (int i = 0; i < 1000; ++i) {
        manager->Step();
        if (manager->GetFuncNo() == MC_FUNC_IDLE) return;
    }
    assert(false);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    port_arguments = argv;
    assert(port_memory_card_init(argv[2]) == 0);
    ActiveSaveData = &data;
    CMemoryCardManager manager;
    manager.save_buffer = (SAVEDATA_FORMAT *)calloc(1, sizeof(SAVEDATA_FORMAT));
    assert(manager.save_buffer);
    run_operation(&manager, MC_FUNC_SEARCH_TYPE);
    assert(manager.error.code == 0 && manager.card[0].present && manager.card[0].type == 2);
    if (!strcmp(argv[1], "save")) {
        MC_ICON_DATA icons[3] = {};
        for (int i = 0; i < 3; ++i) {
            snprintf(icons[i].name, sizeof(icons[i].name), "test%d.icn", i);
            icons[i].data = icon_bytes[i];
            icons[i].size = sizeof(icon_bytes[i]);
        }
        manager.SetIconData(icons, 0);
        run_operation(&manager, MC_FUNC_MAKE_DIR);
        assert(manager.error.code == 0);
        data.game_progress = 17;
        data.map_no = 3;
        data.play_time = 123456;
        data.save_dungeon.stage_id = 1;
        data.save_dungeon.floor_id[1] = 5;
        data.user_data.invent_data.photo[0].used = 1;
        data.user_data.invent_data.photo[0].neta_id = 314;
        data.user_data.invent_data.photo[0].image = (char *)data.user_data.invent_data.photo_work[1];
        data.user_data.invent_data.photo_work[0][0] = 0x1234;
        data.user_data.invent_data.photo_work[0][4095] = 0xabcd;
        run_operation(&manager, MC_FUNC_SAVE);
        assert(manager.error.code == 0);
        assert(manager.transferred == sizeof(SAVEDATA_FORMAT));
        assert(manager.save_buffer->incomplete == 0);
        printf("engine save: %d bytes, checksum %d\n", manager.transferred, manager.save_buffer->check_digit);
    } else {
        if (!strcmp(argv[1], "corrupt")) {
            char name[128], path[2048];
            MakeMemoryCardFileName(0, name);
            snprintf(path, sizeof(path), "%s/card0%s", argv[2], name);
            FILE *file = fopen(path, "r+b");
            assert(file);
            assert(fseek(file, offsetof(SAVEDATA_FORMAT, save_data), SEEK_SET) == 0);
            int byte = fgetc(file);
            assert(byte >= 0);
            assert(fseek(file, -1, SEEK_CUR) == 0);
            assert(fputc(byte ^ 1, file) >= 0);
            assert(fclose(file) == 0);
        } else {
            run_operation(&manager, MC_FUNC_GET_ALL_FILE_INFO);
            assert(manager.error.code == 0);
            assert(manager.file_info[0].state == 1);
            assert(manager.file_info[0].map_no == 3 && manager.file_info[0].progress == 17);
        }
        run_operation(&manager, MC_FUNC_LOAD);
        if (!strcmp(argv[1], "corrupt")) {
            assert(manager.error.code == MC_ERROR_LOAD);
            assert(data.game_progress == 0 && data.map_no == 0 && data.play_time == 0);
            printf("engine load: rejected checksum corruption\n");
        } else {
            assert(!strcmp(argv[1], "load"));
            assert(manager.error.code == 0);
            assert(data.game_progress == 17 && data.map_no == 3 && data.play_time == 123456);
            assert(data.save_dungeon.stage_id == 1 && data.save_dungeon.floor_id[1] == 5);
            assert(data.user_data.invent_data.photo[0].used == 1);
            assert(data.user_data.invent_data.photo[0].neta_id == 314);
            assert(data.user_data.invent_data.photo[0].image == (char *)data.user_data.invent_data.photo_work[1]);
            data.user_data.invent_data.ResetAddress();
            assert(data.user_data.invent_data.photo[0].image == (char *)data.user_data.invent_data.photo_work[0]);
            assert(data.user_data.invent_data.photo_work[0][0] == 0x1234);
            assert(data.user_data.invent_data.photo_work[0][4095] == 0xabcd);
            printf("engine load: restored progress, map, play time, dungeon and photo in a new process\n");
        }
    }
    assert(sceMcEnd() == 0);
    free(manager.save_buffer);
}
