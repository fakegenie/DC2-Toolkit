#include "debug.h"
#include "dng_main.hpp"
#include "gamedata.hpp"
#include "imgui.h"
#include "mainloop.hpp"
#include "mapselect.hpp"
#include "savedata.hpp"
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef struct {
    int id;
    char label[80];
} Entry;

enum { MAP_LIMIT = 1024, ITEM_LIMIT = ITEM_NO_MAX };

static const char *loop_names[LOOP_MODE_NUM] = {"menu",       "town",       "dungeon",    "title",      "chara viewer",
                                                "tex viewer", "map viewer", "snd viewer", "movie viewer", "save convert"};
static const char *character_names[USER_CHARA_NUM] = {"max", "monica", "ridepod", "monster"};
static const int item_sets[] = {0, 1, 2, 6};
static const char *item_set_names[] = {"presentation", "game start", "start debug", "weapons only"};

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
static char map_filter[64];
static char item_filter[64];
static char result[256];

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

static int in_playable_loop(void) {
    int loop = GetNowLoopNo();
    return loop == LOOP_EDIT || loop == LOOP_DUNGEON;
}

static int enter(int loop, INIT_LOOP_ARG arg, DebugReply *reply) {
    int now = GetNowLoopNo();
    if (now != LOOP_TITLE && !in_playable_loop())
        return debug_fail(reply, "cannot leave the %s loop; only title, town and dungeon can be left",
                          now >= 0 && now < LOOP_MODE_NUM ? loop_names[now] : "unknown");
    NextLoop(loop, arg);
    if (now == LOOP_TITLE) {
        DebugFlag = 1;
        debug_run("press?buttons=start,select&frames=1");
    } else GetMainScene()->exit_flag = 1;
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

static int command_character(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    const char *who = debug_arg(args, "who", "");
    int chara = -1;
    for (int i = 0; i < 2; ++i)
        if (!strcmp(who, character_names[i]) || (who[0] == '0' + i && !who[1])) chara = i;
    if (chara < 0) return debug_fail(reply, "who=max|monica");
    CUserDataManager *user = &save()->user_data;
    user->JoinPartyMember(chara);
    user->EnableCharaChange(chara);
    user->SetActiveChrNo(chara);
    int loop = GetNowLoopNo();
    if (in_playable_loop()) return enter(loop, here(loop), reply);
    debug_print(reply, "{\"ok\": 1, \"active\": \"%s\"}", character_names[chara]);
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
    debug_print(reply, "{\"game_progress\": %d}", data->game_progress);
    return 0;
}

static int command_names(const DebugArgs *args, DebugReply *reply) {
    if (!ready()) return debug_fail(reply, "the game is still starting");
    build_lists();
    const char *kind = debug_arg(args, "kind", "maps");
    const char *filter = debug_arg(args, "q", "");
    int is_items = !strcmp(kind, "items");
    const Entry *list = is_items ? items : maps;
    int total = is_items ? item_total : map_total;
    int shown = 0;
    debug_print(reply, "{\"total\": %d, \"names\": [", total);
    for (int i = 0; i < total; ++i) {
        if (!matches(list[i].label, filter)) continue;
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
    {"character", command_character, 0, "who=max|monica switches the playable character and re-enters the current place"},
    {"item", command_item, 0, "id=N [count=1] gives an item"},
    {"itemset", command_itemset, 0, "set=0|1|2|6 gives one of the engine's debug item sets"},
    {"money", command_money, 0, "[amount=N] [add=N] sets or adds money"},
    {"heal", command_heal, 0, "refills the party's health and repairs weapons"},
    {"time", command_time, 0, "[hour=H] [day=D] [freeze=0|1] sets the time of day"},
    {"progress", command_progress, 0, "[value=N] sets game_progress"},
    {"names", command_names, 1, "kind=maps|items [q=text] lists map or item names"},
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
    if (ImGui::CollapsingHeader("Town", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (in_playable_loop()) debug_print(reply, ", \"event_run\": %d, \"event\": %d", scene->event_run, scene->event_no);
    debug_print(reply, ", \"character\": \"%s\", \"money\": %d, \"hour\": %.2f, \"day\": %d, \"game_progress\": %d",
                active >= 0 && active < USER_CHARA_NUM ? character_names[active] : "unknown", data->user_data.money,
                data->now_time, data->day, data->game_progress);
}
