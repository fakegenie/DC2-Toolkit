#include "debug.h"
#include "dataread.hpp"
#include "dng_main.hpp"
#include "editdata.hpp"
#include "editloop.hpp"
#include "gamedata.hpp"
#include "imgui.h"
#include "mainloop.hpp"
#include "mapselect.hpp"
#include "menuaqua.hpp"
#include "menuchr.hpp"
#include "menumain.hpp"
#include "mglib.hpp"
#include "monster.hpp"
#include "port_menu.h"
#include "savedata.hpp"
#include "subgame.hpp"
#include "vlgr_info.hpp"
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef struct {
    int id;
    int type;
    char label[80];
} Entry;

typedef struct {
    const char *name;
    const char *summary;
} Preset;

typedef struct {
    int map;
    const char *label;
} Place;

enum { FLOOR_START_EVENT = 1010, RACE_MAP = 96, MAP_LIMIT = 1024, ITEM_LIMIT = ITEM_NO_MAX, LIST_LIMIT = 64, PARTS_LIMIT = 0x100, CHAPTER_LIMIT = 9 };

static const char *loop_names[LOOP_MODE_NUM] = {"menu",       "town",       "dungeon",    "title",      "chara viewer",
                                                "tex viewer", "map viewer", "snd viewer", "movie viewer", "save convert"};
static const char *character_names[USER_CHARA_NUM] = {"max", "monica", "ridepod", "monster"};
static const int item_sets[] = {0, 1, 2, 6};
static const char *item_type_names[] = {"none", "max_melee", "max_gun", "monica_melee", "monica_magic", "max_clothes", "max_hat",
                                        "max_shoes", "monica_clothes", "monica_accessory", "monica_boots", "robo_core",
                                        "robo_body", "robo_arm", "robo_leg", "robo_energy", "crystal", "spectol", "gem", "coin",
                                        "georama", "badge_box", "recovery", "throwing", "powder", "amulet", "key", "key_item",
                                        "gift_box", "aquarium", "fish", "unused", "lure", "etc", "monster_drop", "boiled"};
static const char *item_set_names[] = {"presentation", "game start", "start debug", "weapons only"};

static const Preset chapter_presets[] = {
    {"cap1", "Chapter 1 start: progress 2, Max with the starting kit (item set 7)"},
    {"cap2", "Chapter 2 start: progress 4, item set 8 (drill wrench, trumpet gun), 48 HP, defence 8"},
    {"cap3", "Chapter 3 start: progress 4, item set 9, flags 207-209, 64 HP and defence 16 for Max and Monica"},
    {"cap4", "Chapter 4 start: progress 6, item set 9, flag 360, better weapons for both, 96 HP, defence 28"},
    {"cap5", "Chapter 5 start: progress 7, item set 9, weapons 4/29 and 76/95, 128 HP, defence 34"},
    {"cap6", "Chapter 6 start: progress 8, item set 9, weapons 7/27 and 71/100, 160 HP, defence 50"},
    {"cap7", "Chapter 7 start: progress 9, item set 9, weapons 19/30 and 77/107, 200 HP, defence 70"},
    {"cap8", "Chapter 8 start: progress 10, item set 9, weapons 20/39 and 79/101, 240 HP, defence 94"},
};

static const Preset other_presets[] = {
    {"db_cap1", "Chapter 1 start: progress 2, presentation item set 0 (Max and Monica, Ridepod kit)"},
    {"db_cap2", "Chapter 2 start: progress 4, item set 0, drill wrench and trumpet gun, 48 HP"},
    {"db_cap3", "Chapter 3 start: progress 4, item set 0, weapons 18/25 and 48/92, 64 HP"},
    {"db_cap4", "Chapter 4 start: progress 46, item set 0, weapons 4/27 and 76/95, 96 HP"},
    {"db_cap5", "Chapter 5 start: progress 7, item set 0, weapons 5/30 and 50/98, 128 HP"},
    {"db_cap6", "Chapter 6 start: progress 8, item set 0, weapons 7/27 and 71/100, 160 HP"},
    {"db_cap7", "Chapter 7 start: progress 9, item set 0, weapons 20/38 and 77/108, 200 HP"},
    {"db_cap8", "Chapter 8 start: progress 10, item set 0, weapons 20/39 and 79/101, 240 HP"},
    {"dbg1", "Chapter 1 start: progress 2, item set 7"},
    {"dbg2", "Chapter 2 start: progress 4, item set 8"},
    {"dbg3", "Chapter 3 start: progress 4, item set 9"},
    {"dbg4", "Sphida fight: weapons only, item 10 equipped"},
    {"dbg5", "Fish: weapons only plus fish items 309 and 320-326"},
    {"dbg6", "Fishing: weapons only plus rods, bait 312-314 x10"},
    {"dbg7", "Chapter 1 start: progress 2, item set 0"},
    {"game0", "Chapter 2, full party: Monica, Ridepod, monster, townsperson 12, active Monica"},
    {"game1", "Chapter 1 first Underground Channel kit: progress 3, weapons only, basics, Max"},
    {"game2", "Same as game1"},
    {"game3", "Chapter 1 channel with armour 260 equipped: progress 3"},
    {"game4", "Chapter 2 with Monica active: progress 4, Monica, Ridepod, monster, townsperson 12"},
    {"game5", "Chapter 2 town menu kit: progress 4, camera, Max"},
    {"game6", "Chapter 2 with every character fully equipped: progress 4, item set 0"},
    {"game7", "Chapter 1 with full equipment and fishing gear: progress 3"},
    {"game9", "Chapter 1 channel with armour 260 equipped: progress 3"},
    {"game10", "Chapter 2 party menu: progress 4, Monica, Ridepod, monster, townsperson 13, camera"},
    {"game20", "Chapter 2 Monica rush: progress 4, Monica with 120 HP, active Monica"},
    {"game21", "Chapter 1 Max battle: progress 3, 140 HP, defence 100"},
    {"game22", "Chapter 2 Monica battle: progress 4, Monica with 48 HP, active Monica"},
    {"pb", "Chapter 2: progress 4, item set 0, fishing gear, 48 HP"},
    {"sg0", "Sphida fight: weapons only, item 10 equipped"},
    {"sg1", "Fish: weapons only plus fish items"},
    {"sg2", "Fishing: weapons only plus rods and bait"},
    {"geo", "Georama: progress 1, flags 34, 250 and 254, every georama part"},
};

static const Place chapter_places[CHAPTER_LIMIT] = {
    {-1, ""},
    {10, "Palm Brinks"},
    {0, "Sindain"},
    {1, "Balance Valley"},
    {2, "Veniccio"},
    {3, "Heim Rada"},
    {4, "Moon Flower Palace"},
    {4, "Moon Flower Palace"},
    {10, "Palm Brinks"},
};

static const int travel_flags[] = {100, 103, 207, 208, 209, 215, 300, 302, 306, 312, 332, 400, 402,
                                   404, 408, 500, 504, 600, 616, 648, 700, 702, 800};

static Entry maps[MAP_LIMIT];
static int map_total;
static Entry items[ITEM_LIMIT];
static int item_total;

static int pick_map;
static int pick_event = -1;
static int pick_dungeon;
static int pick_floor = -1;
static int pick_item = 1;
static int pick_count = 1;
static int pick_money = 99999;
static int pick_hour = 12;
static int pick_day;
static int pick_progress;
static bool pick_warp = true;
static bool pick_geo;
static int pick_preset_money;
static int pick_town;
static int pick_part;
static int pick_part_count = 30;
static int pick_race_class;
static int pick_race;
static int pick_race_fish = -1;
static bool pick_conditions_lifted;
static char map_filter[64];
static char item_filter[64];
static char result[1024];
static char geo_path[] = "dbg/geo.cfg";

static void clean(char *out, size_t size, const char *text) {
    size_t length = 0;
    for (const unsigned char *c = (const unsigned char *)(text ? text : ""); *c && length + 1 < size; ++c)
        out[length++] = *c >= 0x20 && *c < 0x7f && *c != '"' && *c != '\\' ? (char)*c : '?';
    out[length] = 0;
}

static void build_lists(void) {
    if (!map_total)
        while (map_total < MAP_LIMIT && GetMapType(map_total) >= 0) {
            char *title = NULL;
            char name[32];
            char text[48];
            clean(name, sizeof name, GetMapName(map_total, &title));
            clean(text, sizeof text, title);
            maps[map_total].id = map_total;
            snprintf(maps[map_total].label, sizeof maps[0].label, "%d  %s  %s", map_total, name, text);
            map_total++;
        }
    if (!item_total && map_total)
        for (int id = 1; id < ITEM_LIMIT; ++id) {
            char *name = GetItemMessage(id);
            if (!name) continue;
            char text[64];
            clean(text, sizeof text, name);
            items[item_total].id = id;
            CDataCommon *common = GetCommonItemData(id);
            items[item_total].type = common ? common->type : ITEM_DATA_NONE;
            snprintf(items[item_total].label, sizeof items[0].label, "%d  %s", id, text);
            item_total++;
        }
}

static int matches(const char *label, const char *filter) {
    if (!*filter) return 1;
    char lower[80];
    size_t length = 0;
    for (const char *c = label; *c && length + 1 < sizeof lower; ++c) lower[length++] = (char)tolower((unsigned char)*c);
    lower[length] = 0;
    char wanted[64];
    length = 0;
    for (const char *c = filter; *c && length + 1 < sizeof wanted; ++c) wanted[length++] = (char)tolower((unsigned char)*c);
    wanted[length] = 0;
    return strstr(lower, wanted) != NULL;
}

static int parse_map(const char *text) {
    if (!text || !*text) return -1;
    char *end = NULL;
    long number = strtol(text, &end, 0);
    if (*end == 0) return (int)number;
    return SearchMapNo((char *)text);
}

static CSaveData *save(void) {
    return GetSaveData();
}

static int ready(void) {
    if (!GetSaveData()) return 0;
    build_lists();
    return map_total > 0;
}

int debug_map_number(const char *text) {
    if (!ready()) return -2;
    int map = parse_map(text);
    return map >= 0 && GetMapType(map) >= 0 ? map : -1;
}

extern int LoopMode;
extern int CharaMode;
extern int UkiMode;

static const char *const fishing_steps[] = {"walk", "aim", "cast", "wait", "none", "fight", "lost", "landed"};
static const char *const float_steps[] = {"wait", "wait", "nibble", "bite", "reeled", "wait", "bite"};

static const char *fishing_step(void) {
    if (CharaMode == 3 && UkiMode >= 0 && UkiMode < 7) return float_steps[UkiMode];
    return CharaMode >= 0 && CharaMode < 8 ? fishing_steps[CharaMode] : "unknown";
}

static int menu_up(void) {
    int loop = GetNowLoopNo();
    if (loop == LOOP_EDIT) return LoopMode == EDIT_LOOP_WALK_MENU || LoopMode == EDIT_LOOP_EDIT_MENU;
    if (loop == LOOP_DUNGEON) return DngStatus.mode == DNG_STATUS_MENU || DngStatus.mode == DNG_STATUS_EVENT_MENU;
    return 0;
}

static int in_playable_loop(void) {
    int loop = GetNowLoopNo();
    return loop == LOOP_EDIT || loop == LOOP_DUNGEON;
}

static void arrive_from_town(int map) {
    CScene *scene = GetMainScene();
    scene->now_map_no = map;
    scene->old_map_no = map;
}

enum { TITLE_RETRY_FRAMES = 30, TITLE_WARP_FRAMES = 900 };

static int title_frames = -1;
static int title_loop;
static INIT_LOOP_ARG title_arg;

static void leave_title(void) {
    if (title_loop == LOOP_EDIT) arrive_from_town(title_arg.map_no);
    NextLoop(title_loop, title_arg);
    DebugFlag = 1;
    debug_run("press?buttons=start,select&frames=1");
}

static int warp(int loop, INIT_LOOP_ARG arg, DebugReply *reply) {
    int now = GetNowLoopNo();
    if (now != LOOP_TITLE && !in_playable_loop())
        return debug_fail(reply, "cannot leave the %s loop; only title, town and dungeon can be left",
                          now >= 0 && now < LOOP_MODE_NUM ? loop_names[now] : "unknown");
    if (now == LOOP_TITLE) {
        title_loop = loop;
        title_arg = arg;
        title_frames = 0;
        leave_title();
        return 0;
    }
    if (loop == LOOP_EDIT) arrive_from_town(arg.map_no);
    GetMainScene()->SetNowSubMapNo(-1);
    NextLoop(loop, arg);
    GetMainScene()->exit_flag = 1;
    return 0;
}

void debug_scenario_frame(void) {
    if (title_frames < 0) return;
    if (GetNowLoopNo() != LOOP_TITLE || ++title_frames > TITLE_WARP_FRAMES) {
        title_frames = -1;
        return;
    }
    if (title_frames % TITLE_RETRY_FRAMES == 0) leave_title();
}

static int enter(int loop, INIT_LOOP_ARG arg, DebugReply *reply) {
    int now = GetNowLoopNo();
    if (warp(loop, arg, reply)) return 1;
    debug_print(reply, "{\"ok\": 1, \"from\": \"%s\", \"to\": \"%s\", \"map\": %d, \"floor\": %d, \"event\": %d}",
                loop_names[now], loop_names[loop], arg.map_no, arg.floor_no, arg.event_no);
    return 0;
}

static int here_map(void) {
    CScene *scene = GetMainScene();
    return scene->now_sub_map_no >= 0 ? scene->now_sub_map_no : scene->now_map_no;
}

static INIT_LOOP_ARG here(int loop) {
    INIT_LOOP_ARG arg;
    arg.event_no = DefStartEventNo;
    if (loop == LOOP_DUNGEON) {
        CSaveDataDungeon *dungeon = &save()->save_dungeon;
        arg.map_no = dungeon->stage_id;
        arg.floor_no = dungeon->floor_id[dungeon->stage_id];
        arg.event_no = FLOOR_START_EVENT;
    } else arg.map_no = here_map();
    return arg;
}

static int event_arg(const DebugArgs *args) {
    long event = debug_arg_int(args, "event", -1);
    return event < 0 ? DefStartEventNo : (int)event;
}

static void prepare_save(const DebugArgs *args) {
    long config = debug_arg_int(args, "init", -1);
    if (config < 0) return;
    char path[64];
    snprintf(path, sizeof path, "dbg/game%ld.cfg", config);
    InitSaveData();
    LoadGameConfig(path);
}

static int command_town(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    int map = parse_map(debug_arg(args, "map", NULL));
    if (map < 0 || GetMapType(map) < 0) return debug_fail(reply, "map=<number or name> names no map");
    prepare_save(args);
    INIT_LOOP_ARG arg;
    arg.map_no = map;
    arg.event_no = event_arg(args);
    return enter(LOOP_EDIT, arg, reply);
}

static int command_dungeon(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    long dungeon = debug_arg_int(args, "dungeon", -1);
    if (dungeon < 0 || dungeon >= SAVE_DUNGEON_NUM) return debug_fail(reply, "dungeon=0..%d", SAVE_DUNGEON_NUM - 1);
    prepare_save(args);
    INIT_LOOP_ARG arg;
    arg.map_no = (int)dungeon;
    arg.floor_no = (int)debug_arg_int(args, "floor", -1);
    arg.event_no = event_arg(args);
    return enter(LOOP_DUNGEON, arg, reply);
}

static int command_event(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    long event = debug_arg_int(args, "no", -1);
    if (event < 0) return debug_fail(reply, "no=<event number> [map=<number or name>]");
    const char *map_text = debug_arg(args, "map", NULL);
    int loop = GetNowLoopNo();
    INIT_LOOP_ARG arg;
    if (map_text) {
        int map = parse_map(map_text);
        if (map < 0 || GetMapType(map) < 0) return debug_fail(reply, "map=%s names no map", map_text);
        arg.map_no = map;
        loop = LOOP_EDIT;
    } else if (in_playable_loop()) arg = here(loop);
    else return debug_fail(reply, "not in a town or dungeon; pass map=");
    arg.event_no = (int)event;
    return enter(loop, arg, reply);
}

static int command_defeat(const DebugArgs *args, DebugReply *reply) {
    if (GetNowLoopNo() != LOOP_DUNGEON || ActiveMonster == NULL) return debug_fail(reply, "not in a dungeon");
    int listing = debug_arg_int(args, "list", 0) != 0;
    int defeated = 0;
    debug_print(reply, "{\"monsters\": [");
    for (int slot = 0; slot < MONSTER_ACTIVE_MAX; ++slot) {
        CActiveMonster *monster = ActiveMonster->active[slot];
        if (!monster || monster->state != ACTIVE_MONSTER_LIVE || monster->life <= 0) continue;
        debug_print(reply, "%s{\"slot\": %d, \"life\": %d, \"max_life\": %d}", defeated ? ", " : "", slot, monster->life,
                    monster->max_life);
        if (!listing) {
            monster->life = 0;
            monster->damage_req = ACTION_DAMAGE_REQ_DEAD;
        }
        ++defeated;
    }
    debug_print(reply, "], \"defeated\": %d}", listing ? 0 : defeated);
    return 0;
}

static int menu_wanted = -1;
static int menu_params[16];

static int monster_form(int id, int *row, int *level) {
    for (int r = 0; r < MONSTER_PROGRESS_NUM; ++r)
        for (int l = 0; l < MONSTER_PROGRESS_LEVEL_NUM; ++l)
            if (monster_progress_tbl[r][1 + l] == id) {
                if (row) *row = r;
                if (level) *level = l;
                return 1;
            }
    return 0;
}

static int command_character(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    const char *who = debug_arg(args, "who", "");
    int chara = -1;
    for (int i = 0; i < USER_CHARA_NUM; ++i)
        if (!strcmp(who, character_names[i]) || (who[0] == '0' + i && !who[1])) chara = i;
    if (chara < 0) return debug_fail(reply, "who=max|monica|ridepod|monster [monster=id]");
    CUserDataManager *user = &save()->user_data;
    if (chara == USER_CHARA_MONSTER) {
        int monster = (int)debug_arg_int(args, "monster", user->monster_id);
        int row = 0, level = 0;
        if (!monster_form(monster, &row, &level)) return debug_fail(reply, "monster=%d is no monster form", monster);
        int no = monster_progress_tbl[row][0] + 1;
        user->monster_box.EnableChange(no);
        MOS_CHANGE_PARAM *badge = user->monster_box.GetMonsterBajjiData(no);
        badge->progress = (s16)row;
        badge->class_level = (s16)level;
        badge->monster_id = (s16)monster;
        user->monster_id = (s16)monster;
        user->JoinPartyMember(chara);
        user->EnableCharaChange(chara);
        if (GetNowLoopNo() != LOOP_DUNGEON || user->active_chr_no != USER_CHARA_MONICA)
            return debug_fail(reply, "monsters transform from Monica in a dungeon: character who=monica first");
        memset(menu_params, 0, sizeof menu_params);
        menu_params[0] = USER_CHARA_MONSTER;
        menu_wanted = MENU_OPEN_MAIN_CHARA_BG_DUNGEON;
        debug_hold(1u << 4, 3);
        debug_print(reply, "{\"ok\": 1, \"active\": \"monster\", \"monster\": %d}", monster);
        return 0;
    }
    user->JoinPartyMember(chara);
    user->EnableCharaChange(chara);
    user->SetActiveChrNo(chara);
    int loop = GetNowLoopNo();
    if (in_playable_loop()) return enter(loop, here(loop), reply);
    debug_print(reply, "{\"ok\": 1, \"active\": \"%s\"}", character_names[chara]);
    return 0;
}

static int fill_aquarium(CUserDataManager *user) {
    int moved = 0;
    for (CGameDataUsed &item : user->used_data)
        if (item.item_no > 0 && item.item_type == ITEM_DATA_FISH && user->FishInAquarium(&item, AQUARIUM_TANK_FIRST)) moved++;
    return moved;
}

static void print_tank(DebugReply *reply) {
    CGameDataUsed *fish = GetAquariumData()->GetAquariumFishTop(AQUARIUM_TANK_FIRST);
    debug_print(reply, "\"tank\": [");
    for (int slot = 0; slot < 6; ++slot) {
        if (slot) debug_print(reply, ", ");
        if (fish[slot].item_no <= 0) {
            debug_print(reply, "null");
            continue;
        }
        char name[64];
        clean(name, sizeof name, fish[slot].GetName(ITEM_NAME_TYPE_LEVEL));
        debug_print(reply, "{\"slot\": %d, \"item\": %d, \"name\": ", slot, fish[slot].item_no);
        debug_print_string(reply, name);
        debug_print(reply, ", \"electric\": %d}", (fish[slot].data.fish.flags & BREEDFISH_FLAG_ELECTRIC) != 0);
    }
    debug_print(reply, "]");
}

static int command_aquarium(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    int moved = debug_arg_int(args, "fill", 0) ? fill_aquarium(&save()->user_data) : 0;
    debug_print(reply, "{\"moved\": %d, ", moved);
    print_tank(reply);
    debug_print(reply, "}");
    return 0;
}

static int command_fishrace(const DebugArgs *args, DebugReply *reply) {
    if (!ready() || GetNowLoopNo() != LOOP_EDIT || here_map() != RACE_MAP)
        return debug_fail(reply, "the race runs in the Finny Frenzy race course; town map=%d first", RACE_MAP);
    CScene *scene = GetMainScene();
    if (scene->event_run || SubGameRunning()) return debug_fail(reply, "an event or a sub game is running");
    long race_class = debug_arg_int(args, "class", 0);
    long race = debug_arg_int(args, "race", 0);
    long slot = debug_arg_int(args, "fish", -1);
    if (race_class < 0 || race_class > 3 || race < 0 || race > 2) return debug_fail(reply, "class=0..3 race=0..2");
    CGameDataUsed *fish = GetAquariumData()->GetAquariumFishTop(AQUARIUM_TANK_FIRST);
    for (int i = 0; slot < 0 && i < 6; ++i)
        if (fish[i].item_no > 0 && !(fish[i].data.fish.flags & BREEDFISH_FLAG_ELECTRIC)) slot = i;
    if (slot < 0 || slot > 5 || fish[slot].item_no <= 0) return debug_fail(reply, "no fish to race in the first tank; aquarium fill=1 moves the inventory's fish there");
    SetGyoRaceClass((int)race_class);
    SetGyoRaceNo((int)race);
    SetGyoRaceAquariumNo((int)slot);
    GyoraceFish = &fish[slot];
    SubGameInfo info;
    memset(&info, 0, sizeof info);
    info.scene = scene;
    if (!sgInitSubGame(SUBGAME_GYORACE, &info)) return debug_fail(reply, "the race did not start");
    debug_print(reply, "{\"ok\": 1, \"class\": %ld, \"race\": %ld, \"fish\": %ld}", race_class, race, slot);
    return 0;
}

static int command_item(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    long id = debug_arg_int(args, "id", -1);
    long count = debug_arg_int(args, "count", 1);
    if (id <= 0 || id >= ITEM_NO_MAX || !GetItemMessage((int)id)) return debug_fail(reply, "id=%ld is not an item", id);
    int left = save()->user_data.GetItem((int)id, (int)count);
    debug_print(reply, "{\"ok\": 1, \"id\": %ld, \"count\": %ld, \"owned\": %d, \"result\": %d}", id, count,
                save()->user_data.GetNumSameItem((int)id), left);
    return 0;
}

static int command_itemset(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    long set = debug_arg_int(args, "set", -1);
    if (set != 0 && set != 1 && set != 2 && set != 6) return debug_fail(reply, "set=0|1|2|6");
    DebugGetItem(&save()->user_data, (int)set);
    debug_print(reply, "{\"ok\": 1, \"set\": %ld}", set);
    return 0;
}

static int command_seed(const DebugArgs *args, DebugReply *reply) {
    long value = debug_arg_int(args, "value", 1);
    srand((unsigned)value);
    debug_print(reply, "{\"ok\": 1, \"seed\": %ld}", value);
    return 0;
}

static int command_money(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    CUserDataManager *user = &save()->user_data;
    if (debug_arg(args, "amount", NULL)) user->AddMoney((int)debug_arg_int(args, "amount", 0) - user->money);
    if (debug_arg(args, "add", NULL)) user->AddMoney((int)debug_arg_int(args, "add", 0));
    debug_print(reply, "{\"money\": %d}", user->money);
    return 0;
}

static int command_heal(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    (void)args;
    CUserDataManager *user = &save()->user_data;
    PlayerPartyCure();
    user->AllWeaponRepair();
    debug_print(reply, "{\"ok\": 1, \"max_hp\": %.0f, \"monica_hp\": %.0f}", user->GetHp(0), user->GetHp(1));
    return 0;
}

static int command_time(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    CSaveData *data = save();
    if (debug_arg(args, "hour", NULL)) data->now_time = (float)debug_arg_number(args, "hour", data->now_time);
    if (debug_arg(args, "day", NULL)) data->day = (int)debug_arg_int(args, "day", data->day);
    if (debug_arg(args, "freeze", NULL)) data->time_stop = (int)debug_arg_int(args, "freeze", 0);
    debug_print(reply, "{\"hour\": %.2f, \"day\": %d, \"time_stop\": %d}", data->now_time, data->day, data->time_stop);
    return 0;
}

static int command_progress(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    CSaveData *data = save();
    if (debug_arg(args, "value", NULL)) data->game_progress = (int)debug_arg_int(args, "value", data->game_progress);
    debug_print(reply, "{\"game_progress\": %d, \"chapter\": %d", data->game_progress, GetGameChapter(data->game_progress));
    if (debug_arg_int(args, "list", 0)) {
        debug_print(reply, ", \"points\": [");
        int shown = 0;
        for (int i = 0; i < GetGameProgressNum(); ++i) {
            GAME_PROGRESS_INFO *info = GetGameProgressInfo(i);
            if (!info->chapter) continue;
            debug_print(reply, "%s[%d, %d, %d, %d]", shown++ ? ", " : "", i, info->chapter, info->section, info->order);
        }
        debug_print(reply, "]");
    }
    debug_print(reply, "}");
    return 0;
}

static int parse_list(const char *text, int *out) {
    int count = 0;
    while (*text && count < LIST_LIMIT) {
        char *end;
        long number = strtol(text, &end, 0);
        if (end == text) break;
        out[count++] = (int)number;
        text = *end == ',' ? end + 1 : end;
    }
    return count;
}

static void all_geo_parts(void) {
    int progress = save()->game_progress;
    LoadGameConfig(geo_path);
    save()->game_progress = progress;
}

static void open_travel(int chapter) {
    if (chapter < 2) return;
    save()->SetBitFlag(SAVE_FLAG_EARLY_GAME_DONE, 1);
    for (int flag : travel_flags)
        if (flag / 100 <= chapter) save()->SetBitFlag(flag, 1);
}

static int preset_path(const DebugArgs *args, char *path, size_t size) {
    const char *given = debug_arg(args, "path", NULL);
    const char *name = debug_arg(args, "name", NULL);
    if (given) snprintf(path, size, "%s", given);
    else if (name) snprintf(path, size, "dbg/%s.cfg", name);
    else return 0;
    return 1;
}

static int command_preset(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    char path[64];
    if (!preset_path(args, path, sizeof path))
        return debug_fail(reply, "name=cap1..cap8|db_cap1..8|game0..22|dbg1..7|geo|pb|sg0..2 or path=dbg/x.cfg "
                                 "[warp=0] [geo=1] [money=N] [event=N]");
    int size = 0;
    if (!LoadFile2(path, NULL, &size, LOAD_FILE_SIZE)) return debug_fail(reply, "%s is not on the disc", path);
    int now = GetNowLoopNo();
    int warping = debug_arg_int(args, "warp", 1);
    if (warping && now != LOOP_TITLE && !in_playable_loop())
        return debug_fail(reply, "cannot warp from the %s loop; pass warp=0", now >= 0 && now < LOOP_MODE_NUM ? loop_names[now] : "unknown");
    InitSaveData();
    LoadGameConfig(path);
    if (debug_arg_int(args, "geo", 0)) all_geo_parts();
    if (debug_arg(args, "money", NULL)) save()->user_data.AddMoney((int)debug_arg_int(args, "money", 0));
    int chapter = GetGameChapter(save()->game_progress);
    open_travel(chapter);
    const Place *place = chapter > 0 && chapter < CHAPTER_LIMIT ? &chapter_places[chapter] : NULL;
    debug_print(reply, "{\"ok\": 1, \"path\": ");
    debug_print_string(reply, path);
    debug_print(reply, ", \"game_progress\": %d, \"chapter\": %d", save()->game_progress, chapter);
    if (!warping || !place || place->map < 0) {
        debug_print(reply, ", \"warped\": 0}");
        return 0;
    }
    INIT_LOOP_ARG arg;
    arg.map_no = place->map;
    arg.event_no = event_arg(args);
    if (warp(LOOP_EDIT, arg, reply)) return 1;
    debug_print(reply, ", \"warped\": 1, \"map\": %d, \"place\": \"%s\"}", place->map, place->label);
    return 0;
}

static int command_flag(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    int on[LIST_LIMIT];
    int off[LIST_LIMIT];
    int get[LIST_LIMIT];
    int on_count = parse_list(debug_arg(args, "on", ""), on);
    int off_count = parse_list(debug_arg(args, "off", ""), off);
    int get_count = parse_list(debug_arg(args, "get", ""), get);
    if (!on_count && !off_count && !get_count) return debug_fail(reply, "[on=N,N] [off=N,N] [get=N,N] sets or reads bit flags");
    for (int i = 0; i < on_count; ++i) save()->SetBitFlag(on[i], 1);
    for (int i = 0; i < off_count; ++i) save()->SetBitFlag(off[i], 0);
    debug_print(reply, "{\"flags\": [");
    for (int i = 0; i < on_count; ++i) debug_print(reply, "%s[%d, %d]", i ? ", " : "", on[i], save()->GetBitFlag(on[i]));
    for (int i = 0; i < off_count; ++i)
        debug_print(reply, "%s[%d, %d]", on_count || i ? ", " : "", off[i], save()->GetBitFlag(off[i]));
    for (int i = 0; i < get_count; ++i)
        debug_print(reply, "%s[%d, %d]", on_count || off_count || i ? ", " : "", get[i], save()->GetBitFlag(get[i]));
    debug_print(reply, "]}");
    return 0;
}

static int command_party(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    const char *who = debug_arg(args, "who", "");
    CUserDataManager *user = &save()->user_data;
    if (!strcmp(who, "monica")) {
        user->JoinPartyMember(USER_CHARA_MONICA);
        user->EnableCharaChange(USER_CHARA_MONICA);
    } else if (!strcmp(who, "ridepod")) {
        user->JoinPartyMember(USER_CHARA_ROBO);
        user->EnableCharaChange(USER_CHARA_ROBO);
        user->GetItemNotOver(ITEM_ID_CORE, 1);
    } else if (!strcmp(who, "monster")) {
        user->JoinPartyMember(USER_CHARA_MONSTER);
        user->EnableCharaChange(USER_CHARA_MONSTER);
        user->GetItemNotOver(ITEM_ID_BADGE_BOX, 1);
    } else if (!strcmp(who, "townsperson")) {
        long id = debug_arg_int(args, "id", 0);
        if (id <= 0 || id > 0x1A) return debug_fail(reply, "id=1..26");
        user->JoinPartyChara((int)id, PARTY_CHARA_STATUS_JOIN, 1);
        user->SetPartyCharaStatus((int)id, PARTY_CHARA_STATUS_IN_PARTY);
    } else return debug_fail(reply, "who=monica|ridepod|monster|townsperson [id=1..26]");
    debug_print(reply, "{\"ok\": 1, \"who\": \"%s\", \"party_member\": %d}", who, user->party_member);
    return 0;
}

static int command_unlock(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (!ready()) return debug_fail(reply, "the game is still starting");
    CUserDataManager *user = &save()->user_data;
    for (int chara = USER_CHARA_MONICA; chara <= USER_CHARA_MONSTER; ++chara) {
        user->JoinPartyMember(chara);
        user->EnableCharaChange(chara);
    }
    user->GetItemNotOver(ITEM_ID_CORE, 1);
    user->GetItemNotOver(ITEM_ID_BADGE_BOX, 1);
    int medals = 0;
    for (int row = 0; row < MONSTER_PROGRESS_NUM; ++row) {
        int no = monster_progress_tbl[row][0] + 1;
        if (user->monster_box.IsChange(no)) continue;
        user->monster_box.EnableChange(no);
        medals++;
    }
    debug_print(reply, "{\"ok\": 1, \"party_member\": %d, \"medals\": %d, \"monsters\": [", user->party_member, medals);
    int shown = 0;
    for (int id = 0; id < 0x200; ++id)
        if (monster_form(id, NULL, NULL)) debug_print(reply, "%s%d", shown++ ? ", " : "", id);
    debug_print(reply, "]}");
    return 0;
}

static int command_equip(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    long chara = debug_arg_int(args, "chara", -1);
    long item = debug_arg_int(args, "item", -1);
    if (chara < 0 || chara >= USER_CHARA_NUM) return debug_fail(reply, "chara=0..%d", USER_CHARA_NUM - 1);
    if (item <= 0 || item >= ITEM_NO_MAX || !GetItemMessage((int)item)) return debug_fail(reply, "item=%ld is not an item", item);
    CUserDataManager *user = &save()->user_data;
    if (!user->GetNumSameItem((int)item)) user->GetItem((int)item, 1);
    int result = user->SetChrEquip((int)chara, (int)item);
    long bait = debug_arg_int(args, "bait", 0);
    if (bait > 0) {
        CGameDataUsed *slot = user->GetActiveEsa((int)item);
        if (!slot) return debug_fail(reply, "bait= needs item= to be a fishing rod");
        user->CopyGameData(slot, (int)bait);
        slot->data.item.num = (s16)debug_arg_int(args, "count", 10);
    }
    debug_print(reply, "{\"ok\": 1, \"chara\": %ld, \"item\": %ld, \"result\": %d}", chara, item, result);
    return 0;
}

static int command_stats(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    long chara = debug_arg_int(args, "chara", -1);
    CHARA_DATA *data = chara >= 0 && chara < USER_CHARA_NUM ? save()->user_data.GetCharaDataPtr((int)chara) : NULL;
    if (!data) return debug_fail(reply, "chara=0..%d [hp=N] [defence=N]", USER_CHARA_NUM - 1);
    if (debug_arg(args, "hp", NULL)) {
        data->hp.max = (float)debug_arg_number(args, "hp", data->hp.max);
        data->hp.now = data->hp.max;
    }
    if (debug_arg(args, "defence", NULL)) data->defence = (int)debug_arg_int(args, "defence", data->defence);
    debug_print(reply, "{\"chara\": %ld, \"hp\": %.0f, \"defence\": %d}", chara, data->hp.max, (int)data->defence);
    return 0;
}

static int command_geo(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    CSaveData *data = save();
    if (debug_arg_int(args, "all", 0)) all_geo_parts();
    if (debug_arg(args, "debug", NULL)) DebugInfo.georama_debug = debug_arg_int(args, "debug", 0) != 0;
    if (debug_arg(args, "free", NULL)) DebugInfo.chara_move = (int)debug_arg_int(args, "free", 0);
    long town = debug_arg_int(args, "town", -1);
    if (town >= 0) {
        CEditData *edit = data->GetEditData((int)town);
        if (!edit) return debug_fail(reply, "town=0..%d", SAVE_EDIT_DATA_MAX - 1);
        edit->dbgSetAllContintionFlag((int)town, debug_arg_int(args, "conditions", 1) != 0);
    }
    long part = debug_arg_int(args, "part", -1);
    if (part >= 0) {
        if (part >= PARTS_LIMIT) return debug_fail(reply, "part=0..%d", PARTS_LIMIT - 1);
        data->SetBuildPartsNum((int)part, (int)debug_arg_int(args, "count", 30));
    }
    int owned = 0;
    for (int i = 0; i < PARTS_LIMIT; ++i) owned += data->GetBuildPartsNum(i) > 0;
    debug_print(reply, "{\"georama_debug\": %d, \"free_edit\": %d, \"flags\": {\"start\": %d, \"geostone\": %d, \"migration\": %d}, "
                       "\"parts_owned\": %d, \"towns\": [",
                DebugInfo.georama_debug, DebugInfo.chara_move, data->GetBitFlag(250), data->GetBitFlag(254), data->GetBitFlag(34), owned);
    for (int i = 0; i < SAVE_EDIT_DATA_MAX; ++i) {
        CEditData *edit = data->GetEditData(i);
        int told = 0;
        int met = 0;
        for (int c = 0; c < EDIT_ANALYZE_CONDITION_MAX; ++c) {
            told += edit->analyze.condition_open[c] != 0;
            met += edit->analyze.condition[c] != 0;
        }
        debug_print(reply, "%s{\"town\": %d, \"saves\": %d, \"culture\": %d, \"parts_placed\": %d, \"houses\": %d, "
                           "\"conditions_known\": %d, \"conditions_met\": %d}",
                    i ? ", " : "", i, edit->save_count, edit->culture_point, edit->parts_max, edit->house_max, told, met);
    }
    debug_print(reply, "]");
    if (debug_arg_int(args, "list", 0)) {
        debug_print(reply, ", \"parts\": [");
        for (int i = 0, shown = 0; i < PARTS_LIMIT; ++i)
            if (data->GetBuildPartsNum(i) > 0) debug_print(reply, "%s[%d, %d]", shown++ ? ", " : "", i, data->GetBuildPartsNum(i));
        debug_print(reply, "]");
    }
    debug_print(reply, "}");
    return 0;
}

typedef struct {
    const char *name;
    int open_type;
} MenuName;

static const MenuName menu_names[] = {
    {"main_town", MENU_OPEN_MAIN_TOWN},
    {"main_dungeon", MENU_OPEN_MAIN_DUNGEON},
    {"georama", MENU_OPEN_GEORAMA},
    {"floor_map", MENU_OPEN_DNG_TREE_MAP},
    {"chara_change", MENU_OPEN_CHARA_CHANGE},
    {"chara_change_dungeon", MENU_OPEN_CHARA_CHANGE_DUNGEON},
    {"name", MENU_OPEN_NAME_REGIST},
    {"shop", MENU_OPEN_SHOP},
    {"save", MENU_OPEN_SAVE},
    {"use_item", MENU_OPEN_USE_ITEM},
    {"invent", MENU_OPEN_INVENT},
    {"chapter", MENU_OPEN_CHAPTER},
    {"removal", MENU_OPEN_REMOVAL},
    {"world_move", MENU_OPEN_WORLD_MOVE},
    {"fish_select", MENU_OPEN_GYORACE_FISH_SEL},
    {"item_over", MENU_OPEN_MAIN_TOWN_ITEM_OVER},
    {"option", MENU_OPEN_OPTION},
    {"costume", MENU_OPEN_COSTUME},
    {"chara_show", MENU_OPEN_MAIN_CHARA_BG},
    {"chara_show_dungeon", MENU_OPEN_MAIN_CHARA_BG_DUNGEON},
    {"fish_race", MENU_OPEN_GYORACE},
    {"spheda", MENU_OPEN_SPHIDA},
    {"monster_book", MENU_OPEN_MONSTER_BOOK},
    {"subgame_save", MENU_OPEN_SUBGAME_SAVE},
    {"spheda_score", MENU_OPEN_SPHIDA_SCORE_VIEW},
};

extern "C" int port_menu_open(int open_type, int *param) {
    if (menu_wanted < 0) return open_type;
    int wanted = menu_wanted;
    menu_wanted = -1;
    memcpy(param, menu_params, sizeof menu_params);
    return wanted;
}

static int command_menu(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    const char *name = debug_arg(args, "open", "");
    int open_type = -1;
    for (const MenuName &menu : menu_names)
        if (!strcmp(menu.name, name)) open_type = menu.open_type;
    if (open_type < 0) {
        char known[1024] = "";
        for (const MenuName &menu : menu_names) snprintf(known + strlen(known), sizeof known - strlen(known), " %s", menu.name);
        return debug_fail(reply, "open= is one of%s", known);
    }
    memset(menu_params, 0, sizeof menu_params);
    char list[256];
    snprintf(list, sizeof list, "%s", debug_arg(args, "param", ""));
    int count = 0;
    for (char *value = strtok(list, ","); value && count < 16; value = strtok(NULL, ",")) menu_params[count++] = atoi(value);
    menu_wanted = open_type;
    debug_hold(1u << 4, 3);
    debug_print(reply, "{\"ok\": 1, \"open\": \"%s\", \"open_type\": %d, \"params\": %d}", name, open_type, count);
    return 0;
}

static int command_names(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    build_lists();
    const char *kind = debug_arg(args, "kind", "maps");
    const char *filter = debug_arg(args, "q", "");
    int is_items = !strcmp(kind, "items");
    int type = -1;
    for (int i = 0; i < (int)(sizeof item_type_names / sizeof item_type_names[0]); ++i)
        if (!strcmp(debug_arg(args, "type", ""), item_type_names[i])) type = i;
    const Entry *list = is_items ? items : maps;
    int total = is_items ? item_total : map_total;
    int shown = 0;
    debug_print(reply, "{\"total\": %d, \"names\": [", total);
    for (int i = 0; i < total; ++i) {
        if (!matches(list[i].label, filter) || (is_items && type >= 0 && list[i].type != type)) continue;
        debug_print(reply, "%s", shown++ ? ", " : "");
        debug_print_string(reply, list[i].label);
    }
    debug_print(reply, "]}");
    return 0;
}

extern const DebugCommand debug_scenario_commands[] = {
    {"town", command_town, 0, "map=<number|name> [event=N] [init=N] warps to a town map; works from title, town and dungeon"},
    {"dungeon", command_dungeon, 0, "dungeon=0..6 [floor=N, -1 saved] [event=N] [init=N] warps to a dungeon floor"},
    {"event", command_event, 0, "no=N [map=<number|name>] starts an event here, or on the given map"},
    {"defeat", command_defeat, 0, "[list=1] defeats every living monster on the dungeon floor through its own death program, or lists them"},
    {"character", command_character, 0, "who=max|monica|ridepod switches the playable character and re-enters the current place; who=monster monster=id transforms Monica in a dungeon"},
    {"item", command_item, 0, "id=N [count=1] gives an item"},
    {"aquarium", command_aquarium, 0, "[fill=1] lists the aquarium's first tank; fill=1 first moves every fish in the inventory into it"},
    {"fishrace", command_fishrace, 0, "[class=0..3] [race=0..2] [fish=slot] starts a fish race with a fish from the aquarium's first tank, as the engine's debug menu does; only on the race course, map 96 (i03h05)"},
    {"itemset", command_itemset, 0, "set=0|1|2|6 gives one of the engine's debug item sets"},
    {"seed", command_seed, 0, "value=N seeds the game's random numbers, so dungeon floors and drops repeat"},
    {"money", command_money, 0, "[amount=N] [add=N] sets or adds money"},
    {"heal", command_heal, 0, "refills the party's health and repairs weapons"},
    {"time", command_time, 0, "[hour=H] [day=D] [freeze=0|1] sets the time of day"},
    {"progress", command_progress, 0, "[value=N] [list=1] sets game_progress, or lists the story points"},
    {"preset", command_preset, 0, "name=cap1..8|db_cap1..8|game0..22|dbg1..7|geo|pb|sg0..2 or path=dbg/x.cfg [warp=0] [geo=1] [money=N] applies a disc config and warps to that point of the story"},
    {"flag", command_flag, 0, "[on=N,N] [off=N,N] [get=N,N] sets or reads bit flags"},
    {"party", command_party, 0, "who=monica|ridepod|monster|townsperson [id=1..26] adds a party member"},
    {"unlock", command_unlock, 0, "Monica, the ridepod and monster transform with every monster medal"},
    {"equip", command_equip, 0, "chara=0..2 item=N equips an item, giving it first if missing; with a fishing rod, bait=N [count=10] fits that bait"},
    {"charstats", command_stats, 0, "chara=0..2 [hp=N] [defence=N] sets a character's health and defence"},
    {"geo", command_geo, 0, "[all=1] [debug=0|1] [free=0|1] [town=N [conditions=0|1]] [part=N [count=30]] [list=1] georama parts, conditions and state"},
    {"names", command_names, 1, "kind=maps|items [q=text] [type=max_melee|max_clothes|robo_arm|fish|...] lists map or item names"},
    {"menu", command_menu, 0, "open=shop|save|georama|... [param=a,b] opens that menu the next time the town or dungeon would open its main menu; no open lists them"},
    {NULL, NULL, 0, NULL},
};

static void run(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void run(const char *format, ...) {
    char line[256];
    va_list list;
    va_start(list, format);
    vsnprintf(line, sizeof line, format, list);
    va_end(list);
    debug_run(line);
    snprintf(result, sizeof result, "%s", debug_last_result);
}

static int pick_from(const char *id, Entry *list, int total, const char *filter, int *picked) {
    int changed = 0;
    if (ImGui::BeginChild(id, ImVec2(0, 140), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < total; ++i) {
            if (!matches(list[i].label, filter)) continue;
            if (ImGui::Selectable(list[i].label, *picked == list[i].id)) {
                *picked = list[i].id;
                changed = 1;
            }
        }
    }
    ImGui::EndChild();
    return changed;
}

static void preset_rows(const Preset *list, int total) {
    for (int i = 0; i < total; ++i) {
        ImGui::PushID(list[i].name);
        if (ImGui::Button("Apply"))
            run("preset?name=%s&warp=%d&geo=%d&money=%d", list[i].name, (int)pick_warp, (int)pick_geo, pick_preset_money);
        ImGui::SameLine();
        ImGui::TextWrapped("%s  %s", list[i].name, list[i].summary);
        ImGui::PopID();
    }
}

static void panel_status(void) {
    int loop = GetNowLoopNo();
    CSaveData *data = save();
    ImGui::Text("loop %s%s  debug flag %d", loop >= 0 && loop < LOOP_MODE_NUM ? loop_names[loop] : "?",
                OmakeFlag ? " (omake)" : "", DebugFlag);
    if (loop == LOOP_EDIT) {
        char *title = NULL;
        char *name = GetMapName(here_map(), &title);
        ImGui::Text("map %d  %s  %s", here_map(), name ? name : "", title ? title : "");
    }
    if (loop == LOOP_DUNGEON) {
        ImGui::Text("dungeon %d  floor %d", data->save_dungeon.stage_id, data->save_dungeon.floor_id[data->save_dungeon.stage_id]);
    }
    CScene *scene = GetMainScene();
    if (in_playable_loop() && scene->event_run) ImGui::Text("event %d running", scene->event_no);
    int active = data->user_data.GetActiveChrNo();
    ImGui::Text("playing %s  money %d  hour %.2f  day %d  progress %d", active >= 0 && active < USER_CHARA_NUM ? character_names[active] : "?",
                data->user_data.money, data->now_time, data->day, data->game_progress);
}

void debug_scenario_panel(void) {
    if (!ready()) {
        ImGui::TextUnformatted("the game is still starting");
        return;
    }
    panel_status();
    if (result[0]) ImGui::TextWrapped("%s", result);
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Story presets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Applies the developers' own config from the disc to a fresh save, then warps to the town of that chapter.");
        ImGui::Checkbox("warp to the chapter's town", &pick_warp);
        ImGui::Checkbox("also all georama parts", &pick_geo);
        ImGui::InputInt("starting money (0 keeps none)", &pick_preset_money);
        preset_rows(chapter_presets, (int)(sizeof chapter_presets / sizeof chapter_presets[0]));
        if (ImGui::TreeNode("Other disc configs")) {
            preset_rows(other_presets, (int)(sizeof other_presets / sizeof other_presets[0]));
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Chapter towns")) {
            for (int i = 1; i < CHAPTER_LIMIT; ++i) ImGui::Text("chapter %d  %s", i, chapter_places[i].label);
            ImGui::TreePop();
        }
    }
    if (ImGui::CollapsingHeader("Georama")) {
        if (ImGui::Button("All parts")) run("geo?all=1");
        ImGui::SameLine();
        if (ImGui::Button("Show state")) run("geo?list=1");
        ImGui::SameLine();
        if (ImGui::Button("Select edits anywhere")) run("geo?free=1");
        pick_conditions_lifted = DebugInfo.georama_debug != 0;
        if (ImGui::Checkbox("lift placement conditions", &pick_conditions_lifted)) run("geo?debug=%d", (int)pick_conditions_lifted);
        ImGui::InputInt("town (0 Sindain, 1 Balance Valley, 2 Veniccio, 3 Heim Rada, 4 Moon Flower Palace)", &pick_town);
        if (ImGui::Button("Open all conditions")) run("geo?town=%d&conditions=1", pick_town);
        ImGui::SameLine();
        if (ImGui::Button("Close all conditions")) run("geo?town=%d&conditions=0", pick_town);
        ImGui::InputInt("part", &pick_part);
        ImGui::InputInt("count##part", &pick_part_count);
        if (ImGui::Button("Set part count")) run("geo?part=%d&count=%d", pick_part, pick_part_count);
    }
    if (ImGui::CollapsingHeader("Fish race")) {
        if (ImGui::Button("Fish preset")) run("preset?name=sg1&warp=0");
        ImGui::SameLine();
        if (ImGui::Button("Go to the race course")) run("town?map=%d", RACE_MAP);
        ImGui::SameLine();
        if (ImGui::Button("Fish into the aquarium")) run("aquarium?fill=1");
        ImGui::InputInt("class (0-3)", &pick_race_class);
        ImGui::InputInt("race (0-2)", &pick_race);
        ImGui::InputInt("tank slot (-1 the first fish)", &pick_race_fish);
        if (ImGui::Button("Start race")) run("fishrace?class=%d&race=%d&fish=%d", pick_race_class, pick_race, pick_race_fish);
    }
    if (ImGui::CollapsingHeader("Town")) {
        ImGui::InputText("filter##map", map_filter, sizeof map_filter);
        pick_from("maps", maps, map_total, map_filter, &pick_map);
        ImGui::InputInt("event##town", &pick_event);
        if (ImGui::Button("Warp to town")) run("town?map=%d&event=%d", pick_map, pick_event);
    }
    if (ImGui::CollapsingHeader("Dungeon")) {
        ImGui::InputInt("dungeon", &pick_dungeon);
        ImGui::InputInt("floor (-1 saved)", &pick_floor);
        ImGui::InputInt("event##dungeon", &pick_event);
        if (ImGui::Button("Warp to dungeon")) run("dungeon?dungeon=%d&floor=%d&event=%d", pick_dungeon, pick_floor, pick_event);
    }
    if (ImGui::CollapsingHeader("Event")) {
        ImGui::InputInt("event number", &pick_event);
        if (ImGui::Button("Start event here")) run("event?no=%d", pick_event);
    }
    if (ImGui::CollapsingHeader("Character")) {
        if (ImGui::Button("Max")) run("character?who=max");
        ImGui::SameLine();
        if (ImGui::Button("Monica")) run("character?who=monica");
    }
    if (ImGui::CollapsingHeader("Items")) {
        ImGui::InputText("filter##item", item_filter, sizeof item_filter);
        pick_from("items", items, item_total, item_filter, &pick_item);
        ImGui::InputInt("count", &pick_count);
        if (ImGui::Button("Give item")) run("item?id=%d&count=%d", pick_item, pick_count);
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Button(item_set_names[i])) run("itemset?set=%d", item_sets[i]);
            if (i < 3) ImGui::SameLine();
        }
    }
    if (ImGui::CollapsingHeader("Money, health, time")) {
        ImGui::InputInt("money", &pick_money);
        if (ImGui::Button("Set money")) run("money?amount=%d", pick_money);
        ImGui::SameLine();
        if (ImGui::Button("Heal and repair")) run("heal");
        ImGui::SliderInt("hour", &pick_hour, 0, 23);
        ImGui::InputInt("day", &pick_day);
        if (ImGui::Button("Set time")) run("time?hour=%d&day=%d", pick_hour, pick_day);
        ImGui::InputInt("game progress", &pick_progress);
        if (ImGui::Button("Set progress")) run("progress?value=%d", pick_progress);
    }
}

void debug_scenario_status(DebugReply *reply) {
    if (!ready()) return;
    int loop = GetNowLoopNo();
    CSaveData *data = save();
    int active = data->user_data.GetActiveChrNo();
    debug_print(reply, ", \"loop_name\": \"%s\"", loop >= 0 && loop < LOOP_MODE_NUM ? loop_names[loop] : "unknown");
    if (loop == LOOP_EDIT) {
        char *title = NULL;
        char name[32];
        char text[48];
        clean(name, sizeof name, GetMapName(here_map(), &title));
        clean(text, sizeof text, title);
        debug_print(reply, ", \"map\": %d, \"map_name\": \"%s\", \"map_title\": \"%s\"", here_map(), name, text);
    }
    if (loop == LOOP_DUNGEON) {
        debug_print(reply, ", \"dungeon\": %d, \"floor\": %d", data->save_dungeon.stage_id,
                    data->save_dungeon.floor_id[data->save_dungeon.stage_id]);
    }
    CScene *scene = GetMainScene();
    if (in_playable_loop()) debug_print(reply, ", \"event_run\": %d, \"event\": %d, \"menu\": %d", scene->event_run, scene->event_no, menu_up());
    if (in_playable_loop() && GetSubGameNo() == SUBGAME_FISHING) debug_print(reply, ", \"fishing\": \"%s\"", fishing_step());
    if (in_playable_loop()) debug_print(reply, ", \"subgame\": %d", SubGameRunning() ? GetSubGameNo() : 0);
    CCharacter2 *chara = in_playable_loop() ? scene->GetCharacter(scene->player_chara >= 0 ? scene->player_chara : 0) : NULL;
    alignas(16) float position[4] = {0};
    if (chara && chara->CObjectFrame::frame) chara->CObjectFrame::frame->GetWorldPosition0(position);
    else if (chara) chara->GetPosition(position);
    float eye[4] = {0};
    mgGetCameraPos(eye);
    if (chara)
        debug_print(reply, ", \"player\": [%.2f, %.2f, %.2f], \"camera\": [%.2f, %.2f, %.2f]", position[0], position[1],
                    position[2], eye[0], eye[1], eye[2]);
    debug_print(reply, ", \"character\": \"%s\", \"money\": %d, \"hour\": %.2f, \"day\": %d, \"game_progress\": %d",
                active >= 0 && active < USER_CHARA_NUM ? character_names[active] : "unknown", data->user_data.money,
                data->now_time, data->day, data->game_progress);
}
