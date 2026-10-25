#include "title.hpp"
#include "menuchr.hpp"
#include "dng_main.hpp"
#include "actionchara.hpp"
#include "monster.hpp"
#include "cameracontrol.hpp"
#include "event_func.hpp"
#include "event.hpp"
#include "nowload.hpp"
#include "editloop.hpp"
#include "character.hpp"
#include "map.hpp"
#include "menuop.hpp"
#include "menumain.hpp"
#include "menuaqua.hpp"
#include "automap.hpp"
#include "userdata.hpp"
#include "padcontrol.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>

extern "C" {
#include "input.h"
#include "port_graphics.h"
#include "port_present.h"
}

extern TITLE_INFO *replay_title asm(SYMBOL_PREFIX "_ZL9TitleInfo");
extern short replay_tree_mode asm(SYMBOL_PREFIX "_ZL11DngTreeMode");
extern short replay_title_phase asm(SYMBOL_PREFIX "_ZL10TitlePhase");
extern int replay_language_phase asm(SYMBOL_PREFIX "_ZL16title_lang_phase");
extern CMenuCostumeSel *replay_costume asm(SYMBOL_PREFIX "_ZL10MenuCosPtr");
extern int replay_pause_frames asm(SYMBOL_PREFIX "_ZL8InitFlag");
extern int replay_edit_control asm(SYMBOL_PREFIX "_ZL11ControlMode");
extern int replay_edit_loop asm(SYMBOL_PREFIX "LoopMode");
extern int replay_edit_map asm(SYMBOL_PREFIX "_ZL5MapNo");
extern CSaveMenuClass *replay_save_menu asm(SYMBOL_PREFIX "_ZL11SaveMenuPtr");
extern CMemoryCardManager *replay_card asm(SYMBOL_PREFIX "_ZL13MemoryCardPtr");

static bool save_active(void) {
    bool town_menu = GetNowLoopNo() == LOOP_EDIT && replay_edit_loop == EDIT_LOOP_WALK_MENU;
    bool dungeon_menu = GetNowLoopNo() == LOOP_DUNGEON &&
                        (DngStatus.mode == DNG_STATUS_MENU || DngStatus.mode == DNG_STATUS_EVENT_MENU);
    bool title_menu = GetNowLoopNo() == LOOP_TITLE && replay_title && replay_title->mode == TITLE_MODE_MENU;
    return (town_menu || dungeon_menu || title_menu) && MenuCommonInfo &&
           (MenuCommonInfo->now_mode == MENU_MODE_SAVE || MenuCommonInfo->now_mode == MENU_MODE_TITLE_SAVE ||
            (MenuCommonInfo->now_mode == MENU_MODE_DNG_TREE_MAP && replay_tree_mode == DNG_TREE_MODE_SAVE)) && replay_save_menu;
}

static void save_report(void) {
    if (!save_active()) return;
    CSaveMenuClass *menu = replay_save_menu;
    CDC2Mes *message = MenuDCMsg[2];
    std::fprintf(stderr, "replay: save page=%d phase=%d slot=%d select=%d mode=%d ask=%d fade=%d key=%d count=%d cursor=%d error=%d cardstep=%d transferred=%d total=%d\n",
                 menu->page, menu->phase, menu->slot, menu->select, menu->mode,
                 static_cast<CBaseMenuClass *>(menu)->mode, menu->fade_alpha, MenuCommonInfo->key_enable,
                 menu->save_count, message ? message->GetMsgCursor() : -1, replay_card ? replay_card->error.code : -1,
                 replay_card ? replay_card->step : -1, replay_card ? replay_card->transferred : 0,
                 replay_card ? replay_card->total_transferred : 0);
}

static unsigned save_buttons(void) {
    if (!save_active()) return 0;
    CSaveMenuClass *menu = replay_save_menu;
    if (!MenuCommonInfo->key_enable || menu->fade_alpha < 128 || static_cast<CBaseMenuClass *>(menu)->mode != MENU_ASK_MODE_NONE)
        return 0;
    if (menu->page == SAVE_MENU_PAGE_SLOT_SELECT) return menu->save_count ? PORT_INPUT_CIRCLE : PORT_INPUT_CROSS;
    if (menu->page != SAVE_MENU_PAGE_FILE_LIST) return 0;
    if (menu->phase == SAVE_LIST_PHASE_SELECT) return menu->save_count ? PORT_INPUT_CIRCLE : PORT_INPUT_CROSS;
    if (menu->phase == SAVE_LIST_PHASE_CONFIRM_SAVE || menu->phase == SAVE_LIST_PHASE_CONFIRM_LOAD) {
        CDC2Mes *message = MenuDCMsg[2];
        if (!message) return 0;
        return message->GetMsgCursor() == 1 ? PORT_INPUT_LEFT : PORT_INPUT_CROSS;
    }
    if (menu->phase == SAVE_LIST_PHASE_SAVE_DONE || menu->phase == SAVE_LIST_PHASE_LOAD_DONE) return PORT_INPUT_CROSS;
    return 0;
}

static void journal_event(const char *kind, const char *command) {
    static const char *path = getenv("DC2TK_REPLAY_JOURNAL");
    if (!path) return;
    FILE *file = std::fopen(path, "a");
    if (!file) return;
    CScene *scene = GetMainScene();
    const char *name = scene ? scene->GetMapName(scene->active_map) : nullptr;
    CCharacter2 *chara = scene ? scene->GetCharacter(scene->player_chara) : nullptr;
    std::fprintf(file, "%llu\t%s\tloop=%d\tmap=%.16s\tposition=%.3f,%.3f,%.3f\t%s\n",
                 (unsigned long long)port_fields, kind, GetNowLoopNo(), name ? name : "none",
                 chara ? chara->position[0] : 0, chara ? chara->position[1] : 0,
                 chara ? chara->position[2] : 0, command);
    std::fclose(file);
}

static int waypoint_axis(float value) {
    if (std::fabs(value) < 1.0f / 128) return 0;
    int report = (int)std::lround((value < 0 ? 78 : 177) + 78 * value);
    return (report - 128) * 256;
}

static constexpr int combat_grid_max = 4096;
static CAutoMapParts *combat_route_grid;
static CActiveMonster *combat_route_target;
static int combat_route_floor = -1;
static int combat_route_waypoint = -1;
static int combat_route_goal = -1;

static void combat_route_reset(void) {
    combat_route_grid = nullptr;
    combat_route_target = nullptr;
    combat_route_floor = -1;
    combat_route_waypoint = -1;
    combat_route_goal = -1;
}

static int combat_grid_cell(float x, float z) {
    if (!std::isfinite(x) || !std::isfinite(z)) return -1;
    float column = std::floor(x / AutoMapGen.cell_w + 0.5f);
    float row = std::floor(z / AutoMapGen.cell_d + 0.5f);
    if (column < 0 || row < 0 || column >= AutoMapGen.grid_w || row >= AutoMapGen.grid_h) return -1;
    int cell = (int)row * AutoMapGen.grid_w + (int)column;
    return AutoMapGen.grid[cell].parts_no >= 0 ? cell : -1;
}

static bool combat_local_target(CActiveMonster *target) {
    int width = AutoMapGen.grid_w, height = AutoMapGen.grid_h;
    if (!MainChara__2 || !target || !AutoMapGen.grid || width <= 0 || height <= 0 ||
        width > combat_grid_max / height || !(AutoMapGen.cell_w > 0) || !(AutoMapGen.cell_d > 0) ||
        !std::isfinite(AutoMapGen.cell_w) || !std::isfinite(AutoMapGen.cell_d) ||
        std::fabs(target->position[1] - MainChara__2->position[1]) > 24) return false;
    int start = combat_grid_cell(MainChara__2->position[0], MainChara__2->position[2]);
    int goal = combat_grid_cell(target->position[0], target->position[2]);
    if (start < 0 || goal < 0) return false;
    if (start == goal) return true;
    int dx = goal % width - start % width, dz = goal / width - start / width;
    unsigned forward = 0, reverse = 0;
    if (dx == 0 && dz == -1) { forward = 1; reverse = 2; }
    if (dx == 0 && dz == 1) { forward = 2; reverse = 1; }
    if (dx == 1 && dz == 0) { forward = 4; reverse = 8; }
    if (dx == -1 && dz == 0) { forward = 8; reverse = 4; }
    return (AutoMapGen.grid[start].link & forward) && (AutoMapGen.grid[goal].link & reverse);
}

static void combat_motion_hex(const char *motion, char output[65]) {
    static const char digits[] = "0123456789abcdef";
    int i = 0;
    if (motion) {
        for (; i < 32 && motion[i]; ++i) {
            unsigned char value = (unsigned char)motion[i];
            output[i * 2] = digits[value >> 4];
            output[i * 2 + 1] = digits[value & 15];
        }
    }
    output[i * 2] = 0;
}

static CActiveMonster *combat_key_carrier;
static bool combat_key_drop;
static bool combat_key_collected;
static float combat_key_position[3];

static void combat_key_update(uint64_t field) {
    static CAutoMapParts *reported_grid;
    static bool monsters_reported;
    if (reported_grid != AutoMapGen.grid) {
        reported_grid = AutoMapGen.grid;
        monsters_reported = false;
        combat_key_carrier = nullptr;
        combat_key_drop = false;
        combat_key_collected = false;
    }
    if (GetUserItemHaveNum(337) > 0) {
        if (!combat_key_collected) {
            combat_key_collected = true;
            std::fprintf(stderr, "replay: key-collected item=337 field=%llu position=%.2f,%.2f,%.2f\n",
                         (unsigned long long)field, MainChara__2->position[0], MainChara__2->position[1],
                         MainChara__2->position[2]);
            journal_event("key-collected", "item=337");
        }
        return;
    }
    bool found_monster = false;
    if (!combat_key_drop) combat_key_carrier = nullptr;
    for (int i = 0; i < MONSTER_ACTIVE_MAX; ++i) {
        CActiveMonster *monster = ActiveMonster->active[i];
        if (!monster) continue;
        found_monster = true;
        if (!monsters_reported) {
            std::fprintf(stderr, "replay: monster-position slot=%d id=%d hp=%d gate_key=%d position=%.2f,%.2f,%.2f\n",
                         i, monster->monster_id, monster->life, monster->gate_key,
                         monster->position[0], monster->position[1], monster->position[2]);
        }
        if (monster->gate_key != 337 || combat_key_drop) continue;
        combat_key_carrier = monster;
        for (int axis = 0; axis < 3; ++axis) combat_key_position[axis] = monster->position[axis];
        if (monster->life <= 0) {
            combat_key_drop = true;
            std::fprintf(stderr, "replay: key-carrier-dead slot=%d field=%llu position=%.2f,%.2f,%.2f\n",
                         i, (unsigned long long)field, combat_key_position[0], combat_key_position[1], combat_key_position[2]);
            combat_route_reset();
        }
    }
    if (found_monster) monsters_reported = true;
}

static uint64_t combat_hit_samples;
static uint64_t combat_prim_frames;
static uint64_t combat_window_frames;
static unsigned combat_peak_prims;

static void combat_hit_sample(int floor, uint64_t field) {
    static CActionChara *reported_chara;
    static CAutoMapParts *reported_grid;
    static int reported_floor = -1;
    static uint64_t sampled_field = UINT64_MAX;
    static bool entries_reported;
    if (!MainChara__2 || GetPauseFlag()) return;
    if (reported_chara != MainChara__2 || reported_grid != AutoMapGen.grid || reported_floor != floor) {
        reported_chara = MainChara__2;
        reported_grid = AutoMapGen.grid;
        reported_floor = floor;
        sampled_field = UINT64_MAX;
        entries_reported = false;
        combat_hit_samples = 0;
        combat_prim_frames = 0;
        combat_window_frames = 0;
        combat_peak_prims = 0;
    }
    if (sampled_field == field) return;
    sampled_field = field;
    unsigned prims = 0, windows = 0, registered = 0;
    for (int i = 0; i < ACTION_DAMAGE_MAX; ++i) {
        ACTION_DAMAGE *entry = &MainChara__2->damage[i];
        if (entry->prim) ++prims;
        if (!entry->use) continue;
        ++registered;
        float frame = MainChara__2->GetNowFrame(entry->chara);
        if (frame >= entry->start_frame && frame < entry->end_frame) ++windows;
    }
    ++combat_hit_samples;
    if (prims) ++combat_prim_frames;
    if (windows) ++combat_window_frames;
    if (prims > combat_peak_prims) combat_peak_prims = prims;
    if (!registered || entries_reported) return;
    entries_reported = true;
    std::fprintf(stderr, "replay: attack-entries floor=%d count=%u field=%llu\n",
                 floor, registered, (unsigned long long)field);
    for (int i = 0; i < ACTION_DAMAGE_MAX; ++i) {
        ACTION_DAMAGE *entry = &MainChara__2->damage[i];
        if (!entry->use) continue;
        std::fprintf(stderr, "replay: attack-entry index=%d start=%.6f end=%.6f radius=%.3f frame0=%p frame1=%p chara=%p frame=%.6f prim=%p\n",
                     i, entry->start_frame, entry->end_frame, entry->radius,
                     (void *)entry->frame0, (void *)entry->frame1, (void *)entry->chara,
                     MainChara__2->GetNowFrame(entry->chara), (void *)entry->prim);
    }
}

static int combat_route_point(CActiveMonster *target, int floor, float goal_x, float goal_z, float *x, float *z) {
    int width = AutoMapGen.grid_w, height = AutoMapGen.grid_h;
    if (!AutoMapGen.grid || width <= 0 || height <= 0 || width > combat_grid_max / height ||
        !(AutoMapGen.cell_w > 0) || !(AutoMapGen.cell_d > 0) ||
        !std::isfinite(AutoMapGen.cell_w) || !std::isfinite(AutoMapGen.cell_d)) {
        combat_route_reset();
        return -1;
    }
    if (combat_route_grid != AutoMapGen.grid || combat_route_target != target || combat_route_floor != floor) {
        combat_route_reset();
        combat_route_grid = AutoMapGen.grid;
        combat_route_target = target;
        combat_route_floor = floor;
    }
    int start = combat_grid_cell(MainChara__2->position[0], MainChara__2->position[2]);
    int goal = combat_grid_cell(goal_x, goal_z);
    combat_route_goal = goal;
    if (start < 0 || goal < 0) {
        combat_route_waypoint = -1;
        return -1;
    }
    if (combat_route_waypoint >= 0) {
        *x = (combat_route_waypoint % width) * AutoMapGen.cell_w;
        *z = (combat_route_waypoint / width) * AutoMapGen.cell_d;
        float dx = *x - MainChara__2->position[0], dz = *z - MainChara__2->position[2];
        if (dx * dx + dz * dz > 12 * 12) return 1;
        combat_route_waypoint = -1;
    }
    if (start == goal) return 0;
    static int previous[combat_grid_max];
    static int queue[combat_grid_max];
    int count = width * height;
    for (int i = 0; i < count; ++i) previous[i] = -1;
    int head = 0, tail = 1;
    queue[0] = start;
    previous[start] = start;
    const int dx[4] = {0, 0, 1, -1};
    const int dz[4] = {-1, 1, 0, 0};
    const unsigned forward[4] = {1, 2, 4, 8};
    const unsigned reverse[4] = {2, 1, 8, 4};
    while (head < tail && previous[goal] < 0) {
        int cell = queue[head++];
        for (int direction = 0; direction < 4; ++direction) {
            int column = cell % width + dx[direction], row = cell / width + dz[direction];
            if (column < 0 || row < 0 || column >= width || row >= height) continue;
            int next = row * width + column;
            if (previous[next] >= 0 || AutoMapGen.grid[next].parts_no < 0 ||
                !(AutoMapGen.grid[cell].link & forward[direction]) ||
                !(AutoMapGen.grid[next].link & reverse[direction])) continue;
            previous[next] = cell;
            queue[tail++] = next;
        }
    }
    if (previous[goal] < 0) return -1;
    int next = goal;
    while (previous[next] != start) next = previous[next];
    combat_route_waypoint = next;
    *x = (next % width) * AutoMapGen.cell_w;
    *z = (next / width) * AutoMapGen.cell_d;
    return 1;
}

static bool waypoint_input(float x, float z, float radius, unsigned buttons, port_input_state *input, float *distance) {
    int loop = GetNowLoopNo();
    if (GetPauseFlag() || (loop != LOOP_EDIT && loop != LOOP_DUNGEON)) return false;
    if (loop == LOOP_EDIT && (replay_edit_control != EDIT_CONTROL_PLAYER || replay_edit_loop != EDIT_LOOP_WALK)) return false;
    if (loop == LOOP_DUNGEON && DngStatus.mode != DNG_STATUS_FIELD) return false;
    CScene *scene = loop == LOOP_DUNGEON ? DngMainScene : GetMainScene();
    if (!scene || scene->event_run) return false;
    CCharacter2 *chara = scene->GetCharacter(scene->player_chara);
    mgCCamera *base_camera = scene->GetCamera(scene->active_camera);
    if (!chara || !base_camera || base_camera->Iam() != CAMERA_KIND_CONTROL) return false;
    float angle = static_cast<CCameraControl *>(base_camera)->GetAngle();
    float dx = x - chara->position[0];
    float dz = z - chara->position[2];
    *distance = std::sqrt(dx * dx + dz * dz);
    *input = {};
    if (*distance <= radius) input->buttons = (uint16_t)buttons;
    else {
        float scale = 1 / *distance;
        input->left_x = waypoint_axis((dx * std::cos(angle) - dz * std::sin(angle)) * scale);
        input->left_y = waypoint_axis((dx * std::sin(angle) + dz * std::cos(angle)) * scale);
    }
    return true;
}

static void screenshot_request(void) {
    static const char *request = getenv("DC2TK_REPLAY_SCREENSHOTS");
    static unsigned long long previous;
    static uint64_t next_poll;
    if (!request || port_fields < next_poll) return;
    next_poll = port_fields + 6;
    FILE *file = std::fopen(request, "r");
    if (!file) return;
    unsigned long long id;
    char path[1024];
    int count = std::fscanf(file, "%llu %1023s", &id, path);
    std::fclose(file);
    if (count != 2 || id == previous) return;
    previous = id;
    port_present_save(path);
    std::fprintf(stderr, "replay: screenshot id=%llu field=%llu path=%s\n", id,
                 (unsigned long long)port_fields, path);
}

static void town_report(void) {
    CScene *scene = GetMainScene();
    if (!scene) return;
    const char *name = scene->GetMapName(scene->active_map);
    CCharacter2 *chara = scene->GetCharacter(scene->player_chara);
    ClsMes *message = GetEventMessage(0);
    std::fprintf(stderr, "replay: town map=%.16s map_no=%d control=%d mode=%d menu=%d event=%d running=%d skip=%d pause=%d message=%d select=%d position=%.2f,%.2f,%.2f\n",
                 name ? name : "none", replay_edit_map, replay_edit_control, replay_edit_loop,
                 replay_edit_loop == EDIT_LOOP_WALK_MENU && MenuCommonInfo ? MenuCommonInfo->now_mode : -1,
                 scene->event_no, scene->event_run, EdEventInfo.skip_state, GetPauseFlag(),
                 message ? message->State() : -1, message ? message->select : -1,
                 chara ? chara->position[0] : 0, chara ? chara->position[1] : 0, chara ? chara->position[2] : 0);
    CMap *map = scene->GetMap(scene->active_map);
    if (!map || !name || std::strcmp(name, "s42")) return;
    const char *points[] = {"item1", "item2", "event", "s_talk", "door01"};
    for (int i = 0; i < 5; ++i) {
        CFuncPoint *point = map->func_point.Search(const_cast<char *>(points[i]));
        if (point) std::fprintf(stderr, "replay: point=%s enabled=%d event=%d position=%.2f,%.2f,%.2f\n",
                                points[i], point->enable, point->event.event_no,
                                point->position[0], point->position[1], point->position[2]);
    }
}

static void dungeon_points_report(CFuncPointMngr *manager, CMapParts *part) {
    float matrix[4][4];
    if (part) part->GetLWMatrix(matrix);
    for (CList<CFuncPoint> *node = manager->list[FUNC_POINT_EVENT]; node; node = node->next) {
        CFuncPoint *point = &node->data;
        float position[4] = {point->position[0], point->position[1], point->position[2], 1};
        if (part) {
            for (int axis = 0; axis < 3; ++axis)
                position[axis] = matrix[0][axis] * point->position[0] + matrix[1][axis] * point->position[1] +
                                 matrix[2][axis] * point->position[2] + matrix[3][axis];
        }
        std::fprintf(stderr, "replay: dungeon-point part=%.32s name=%.32s enabled=%d event=%d point=%d flag=%u position=%.2f,%.2f,%.2f\n",
                     part ? part->name : "map", point->name ? point->name : "none", point->enable,
                     point->event.event_no, point->event.point_no, point->event.flag,
                     position[0], position[1], position[2]);
    }
}

static void dungeon_report(void) {
    CScene *scene = DngMainScene;
    if (!scene) return;
    ClsMes *message = GetEventMessage(0);
    int floor = DngSaveDataDungeon ? DngSaveDataDungeon->floor_id[DngStatus.dungeon_no] : -1;
    bool menu = DngStatus.mode == DNG_STATUS_MENU || DngStatus.mode == DNG_STATUS_EVENT_MENU;
    std::fprintf(stderr, "replay: dungeon-ui floor=%d mode=%d menu=%d event=%d running=%d skip=%d pause=%d message=%d select=%d key=%d gate_open=%d\n",
                 floor, DngStatus.mode, menu && MenuCommonInfo ? MenuCommonInfo->now_mode : -1,
                 scene->event_no, scene->event_run, EdEventInfo.skip_state, GetPauseFlag(),
                 message ? message->State() : -1, message ? message->select : -1,
                 GetUserItemHaveNum(337), GetLocalFlag(0));
    static CAutoMapParts *reported_grid;
    static int reported_floor = -1;
    if (floor != 1 || !DngMainMap || !AutoMapGen.grid || AutoMapGen.grid_w <= 0 || AutoMapGen.grid_h <= 0) return;
    if (reported_grid == AutoMapGen.grid && reported_floor == floor) return;
    reported_grid = AutoMapGen.grid;
    reported_floor = floor;
    std::fprintf(stderr, "replay: dungeon-grid floor=%d width=%d height=%d cell=%.2f,%.2f\n",
                 floor, AutoMapGen.grid_w, AutoMapGen.grid_h, AutoMapGen.cell_w, AutoMapGen.cell_d);
    for (int z = 0; z < AutoMapGen.grid_h; ++z) {
        for (int x = 0; x < AutoMapGen.grid_w; ++x) {
            CAutoMapParts *cell = &AutoMapGen.grid[z * AutoMapGen.grid_w + x];
            if (cell->parts_no < 0) continue;
            float position[4] = {0, 0, 0, 1};
            if (cell->parts) cell->parts->GetPosition(position);
            std::fprintf(stderr, "replay: dungeon-cell x=%d z=%d kind=%u link=%u road=%u wall=%u room=%d parts=%d name=%.32s position=%.2f,%.2f,%.2f\n",
                         x, z, cell->kind, cell->link, cell->road_link, cell->wall, cell->room_no, cell->parts_no,
                         cell->parts ? cell->parts->name : "none", position[0], position[1], position[2]);
        }
    }
    dungeon_points_report(&DngMainMap->func_point, nullptr);
    for (int i = 0; i < DngMainMap->place_parts_num; ++i) {
        CMapParts *part = &DngMainMap->place_parts[i];
        dungeon_points_report(&part->func_point_mngr, part);
    }
}

static double seconds(clockid_t clock) {
    timespec value;
    clock_gettime(clock, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}

extern "C" void port_input_read(pad_dualshock2_report *report) {
    static int stage;
    static uint64_t last_press;
    static uint64_t next_report;
    static uint64_t next_skip;
    static bool skip = getenv("DC2TK_REPLAY_SKIP") != nullptr;
    static bool fight = getenv("DC2TK_REPLAY_FIGHT") != nullptr;
    static bool gun = getenv("DC2TK_REPLAY_GUN") != nullptr;
    static bool key = getenv("DC2TK_REPLAY_KEY") != nullptr;
    static uint64_t last_shot;
    static bool save = getenv("DC2TK_REPLAY_SAVE") != nullptr;
    static bool resume = getenv("DC2TK_REPLAY_CONTINUE") != nullptr;
    static bool title = getenv("DC2TK_REPLAY_TITLE") != nullptr;
    static const char *control_file = getenv("DC2TK_REPLAY_CONTROL");
    static double start = seconds(CLOCK_MONOTONIC);
    static uint64_t limit = getenv("DC2TK_REPLAY_FIELDS") ? strtoull(getenv("DC2TK_REPLAY_FIELDS"), nullptr, 10) : 3000;
    port_input_state input = {};
    uint64_t field = port_fields;
    screenshot_request();
    if (resume && stage == 4 && GetNowLoopNo() != LOOP_TITLE) {
        stage = 5;
        std::fprintf(stderr, "replay: resumed loop=%d field=%llu\n", GetNowLoopNo(), (unsigned long long)field);
    }
    static uint64_t control_limit;
    static port_input_state controls;
    static int control_mode;
    static float walk_x, walk_z, walk_radius;
    static unsigned walk_buttons;
    static char control_text[256];
    static bool arrived;
    static bool walk_pressed;
    static bool walk_finished;
    static uint64_t next_control;
    if (control_file && field >= next_control) {
        int previous_mode = control_mode;
        FILE *file = std::fopen(control_file, "r");
        unsigned long long requested_limit;
        unsigned buttons;
        port_input_state requested = {};
        control_mode = 0;
        if (file) {
            char line[256] = {};
            if (std::fgets(line, sizeof(line), file)) line[std::strcspn(line, "\r\n")] = 0;
            float x, z, radius;
            if (std::sscanf(line, "walk %llu %f %f %f %x", &requested_limit, &x, &z, &radius, &buttons) == 5 &&
                std::isfinite(x) && std::isfinite(z) && std::isfinite(radius) && radius >= 0) {
                walk_x = x;
                walk_z = z;
                walk_radius = radius;
                walk_buttons = buttons;
                control_limit = requested_limit;
                control_mode = 2;
            } else if (std::sscanf(line, "%llu %d %d %d %d %x", &requested_limit, &requested.left_x,
                                   &requested.left_y, &requested.right_x, &requested.right_y, &buttons) == 6) {
                requested.buttons = (uint16_t)buttons;
                controls = requested;
                control_limit = requested_limit;
                control_mode = 1;
            }
            if (control_mode && std::strcmp(control_text, line)) {
                std::strcpy(control_text, line);
                arrived = false;
                walk_pressed = false;
                walk_finished = false;
                std::fprintf(stderr, "replay: control field=%llu command=%s\n", (unsigned long long)field, line);
                journal_event("command", line);
            }
            std::fclose(file);
        }
        if (!control_mode && previous_mode) {
            control_text[0] = 0;
            arrived = false;
            walk_pressed = false;
            walk_finished = false;
            journal_event("automatic", "");
            std::fprintf(stderr, "replay: automatic field=%llu\n", (unsigned long long)field);
        }
        next_control = field + 6;
    }
    if (control_limit) limit = control_limit;
    bool controlled = stage == 5 && control_mode == 1;
    float walk_distance = -1;
    if (stage == 5 && control_mode == 2 && !walk_finished) {
        controlled = waypoint_input(walk_x, walk_z, walk_radius, walk_buttons, &controls, &walk_distance);
        if (!controlled && walk_pressed) walk_finished = true;
        if (controlled && walk_distance <= walk_radius && !arrived) {
            arrived = true;
            std::fprintf(stderr, "replay: arrived field=%llu target=%.3f,%.3f distance=%.3f\n",
                         (unsigned long long)field, walk_x, walk_z, walk_distance);
            journal_event("arrived", control_text);
        }
    }
    bool fighting = stage == 5 && fight && GetNowLoopNo() == LOOP_DUNGEON && DngStatus.dungeon_no == 0 && DngStatus.mode == DNG_STATUS_FIELD;
    bool approaching = false;
    bool local_target = false;
    bool gun_aim = false;
    CActiveMonster *fight_target = nullptr;
    float target_distance = -1;
    float fight_angle = 0;
    int floor = fighting && DngSaveDataDungeon ? DngSaveDataDungeon->floor_id[0] : -1;
    bool key_mode = key && gun && fighting && floor == 1 && MainChara__2 && ActiveMonster;
    if (key_mode) combat_key_update(field);
    bool key_stopped = key_mode && combat_key_collected;
    bool key_pickup = key_mode && combat_key_drop && !key_stopped;
    bool navigate = fighting && !controlled && floor == 1 && MainChara__2 && ActiveMonster;
    if (!navigate) combat_route_reset();
    int route_state = 0;
    float route_x = 0, route_z = 0;
    if (fighting && !key_stopped && !key_pickup) {
        if (MainChara__2 && ActiveMonster) {
            CActiveMonster *target = nullptr;
            if (MainChara__2->lock_on && MainChara__2->target_no >= 24 && MainChara__2->target_no < 24 + MONSTER_ACTIVE_MAX)
                target = ActiveMonster->active[MainChara__2->target_no - 24];
            if ((!target || target->life <= 0) && key_mode && combat_key_carrier && combat_key_carrier->life > 0)
                target = combat_key_carrier;
            if (!target || target->life <= 0) {
                float nearest = 1e30f;
                for (int i = 0; i < MONSTER_ACTIVE_MAX; ++i) {
                    CActiveMonster *candidate = ActiveMonster->active[i];
                    if (!candidate || candidate->life <= 0) continue;
                    float dx = candidate->position[0] - MainChara__2->position[0];
                    float dz = candidate->position[2] - MainChara__2->position[2];
                    float distance = dx * dx + dz * dz;
                    if (distance < nearest) {
                        target = candidate;
                        nearest = distance;
                    }
                }
            }
            if (target) {
                float dx = target->position[0] - MainChara__2->position[0];
                float dz = target->position[2] - MainChara__2->position[2];
                fight_target = target;
                target_distance = std::sqrt(dx * dx + dz * dz);
                fight_angle = MainCamera.GetAngle();
                local_target = floor == 1 && combat_local_target(target);
                gun_aim = gun && local_target && MainChara__2->lock_on && target_distance <= 250;
                if (navigate && local_target && target_distance <= (gun ? 400.0f : 110.0f)) {
                    combat_route_reset();
                } else if (navigate) {
                    route_state = combat_route_point(target, floor, target->position[0], target->position[2], &route_x, &route_z);
                    if (route_state > 0) {
                        dx = route_x - MainChara__2->position[0];
                        dz = route_z - MainChara__2->position[2];
                    }
                }
                float approach_distance = floor == 1 ? (gun && local_target ? 250.0f : 80.0f) : 50.0f;
                if (route_state >= 0 && (route_state > 0 || dx * dx + dz * dz > approach_distance * approach_distance)) {
                    float angle = MainCamera.GetAngle();
                    float scale = 1 / std::sqrt(dx * dx + dz * dz);
                    input.left_x = waypoint_axis((dx * std::cos(angle) - dz * std::sin(angle)) * scale);
                    input.left_y = waypoint_axis((dx * std::sin(angle) + dz * std::cos(angle)) * scale);
                    approaching = true;
                }
            } else combat_route_reset();
        }
    }
    if (key_pickup && !controlled) {
        route_x = combat_key_position[0];
        route_z = combat_key_position[2];
        route_state = combat_route_point(nullptr, floor, route_x, route_z, &route_x, &route_z);
        if (route_state >= 0) {
            float dx = route_x - MainChara__2->position[0], dz = route_z - MainChara__2->position[2];
            float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > 12) {
                float angle = MainCamera.GetAngle();
                input.left_x = waypoint_axis((dx * std::cos(angle) - dz * std::sin(angle)) / distance);
                input.left_y = waypoint_axis((dx * std::sin(angle) + dz * std::cos(angle)) / distance);
                approaching = true;
            }
        }
    }
    if (key_stopped && !controlled) combat_route_reset();
    if (fighting) combat_hit_sample(floor, field);
    if (field >= next_report) {
        std::fprintf(stderr, "replay: field=%llu stage=%d loop=%d dungeon=%d mode=%d wall=%.3f cpu=%.3f\n",
                     (unsigned long long)field, stage, GetNowLoopNo(), DngStatus.dungeon_no, DngStatus.mode,
                     seconds(CLOCK_MONOTONIC) - start, seconds(CLOCK_PROCESS_CPUTIME_ID));
        if (stage == 5 && fight && GetNowLoopNo() == LOOP_DUNGEON && DngStatus.dungeon_no == 0 &&
            DngStatus.mode == DNG_STATUS_FIELD && ActiveMonster) {
            int alive = 0;
            for (int i = 0; i < MONSTER_ACTIVE_MAX; ++i)
                if (ActiveMonster->active[i] && ActiveMonster->active[i]->life > 0) ++alive;
            std::fprintf(stderr, "replay: monsters floor=%d alive=%d defeated=%u life=",
                         DngSaveDataDungeon ? DngSaveDataDungeon->floor_id[DngStatus.dungeon_no] : -1,
                         alive, NowFloorInfoPtr ? (unsigned)NowFloorInfoPtr->kill_count : 0);
            for (int i = 0; i < 4; i++)
                std::fprintf(stderr, "%s%d", i ? "," : "", ActiveMonster->active[i] ? ActiveMonster->active[i]->life : 0);
            std::fputc('\n', stderr);
            CBattleCharaInfo *battle = GetBattleCharaInfo();
            CGameDataUsed *melee = battle ? battle->GetEquipTablePtr(0) : nullptr;
            CGameDataUsed *ranged = battle ? battle->GetEquipTablePtr(1) : nullptr;
            if (melee && ranged && melee->used_type == USED_ITEM_TYPE_WEAPON && ranged->used_type == USED_ITEM_TYPE_WEAPON)
                std::fprintf(stderr, "replay: equipment melee=%.1f/%.1f ranged=%.1f/%.1f bread=%d melee_powder=%d gun_powder=%d\n",
                             melee->data.weapon.whp.now, melee->data.weapon.whp.max,
                             ranged->data.weapon.whp.now, ranged->data.weapon.whp.max,
                             GetUserItemHaveNum(268), GetUserItemHaveNum(294), GetUserItemHaveNum(298));
            std::fprintf(stderr, "replay: combat player=%.2f,%.2f,%.2f lock=%d target=%d nearest=%.2f,%.2f,%.2f distance=%.2f camera=%.4f input=%d,%d approaching=%d hp=%.1f/%.1f target_hp=%d gate_key=%d gun=%d local=%d key_pickup=%d key_collected=%d route=%d cell=%d goal=%d waypoint=%.2f,%.2f\n",
                         MainChara__2 ? MainChara__2->position[0] : 0,
                         MainChara__2 ? MainChara__2->position[1] : 0,
                         MainChara__2 ? MainChara__2->position[2] : 0,
                         MainChara__2 ? MainChara__2->lock_on : -1,
                         MainChara__2 ? MainChara__2->target_no : -1,
                         fight_target ? fight_target->position[0] : 0,
                         fight_target ? fight_target->position[1] : 0,
                         fight_target ? fight_target->position[2] : 0,
                         target_distance, fight_angle, input.left_x, input.left_y, approaching,
                         battle && battle->hp ? battle->hp->now : -1.0f,
                         battle && battle->hp ? battle->hp->max : -1.0f,
                         fight_target ? fight_target->life : -1, fight_target ? fight_target->gate_key : -1,
                         gun, local_target, key_pickup, key_stopped, route_state, combat_route_waypoint, combat_route_goal, route_x, route_z);
            if (MainChara__2) {
                int attacks = 0, active_hits = 0;
                for (int i = 0; i < ACTION_DAMAGE_MAX; ++i) {
                    if (MainChara__2->damage[i].use) ++attacks;
                    if (MainChara__2->damage[i].use && MainChara__2->damage[i].prim) ++active_hits;
                }
                CRunScript *script = &MainChara__2->script;
                int weapon_model = script->version == RS_VERSION_2 && script->prog && script->global &&
                                   script->prog->global_num > 0 ? script->global[0].val.i : -1;
                char motion[65];
                combat_motion_hex(MainChara__2->GetNowMotionName(), motion);
                std::fprintf(stderr, "replay: action program=%d state=%d pc=%p end=%d weapon_model=%d pause=%08x motion_hex=%s frame=%.3f ratio=%.3f attacks=%d active_hits=%d hit_samples=%llu prim_frames=%llu window_frames=%llu peak_prims=%u pending_damage=%d history=%08x confirm=%d held=%d cancel=%d debug=%d\n",
                             MainChara__2->prog_no, MainChara__2->prog, (void *)script->pc, script->end, weapon_model,
                             BattleAreaScene ? BattleAreaScene->pause_flag : 0, motion,
                             MainChara__2->frame, MainChara__2->frame_ratio, attacks, active_hits,
                             (unsigned long long)combat_hit_samples, (unsigned long long)combat_prim_frames,
                             (unsigned long long)combat_window_frames, combat_peak_prims, MainChara__2->damage_req, MainChara__2->pad_history,
                             PadCtrl.Btn(PAD_BTN_ACTION_CONFIRM), PadCtrl.Btn(PAD_BTN_ACTION_HELD),
                             PadCtrl.Btn(PAD_BTN_ACTION_CANCEL), DebugFlag);
            }
        }
        next_report = field + 100;
        if (GetNowLoopNo() == LOOP_EDIT) town_report();
        if (stage == 5 && GetNowLoopNo() == LOOP_DUNGEON && DngStatus.dungeon_no == 0) dungeon_report();
        save_report();
        if (control_mode == 2) std::fprintf(stderr, "replay: waypoint target=%.3f,%.3f radius=%.3f distance=%.3f controlled=%d\n",
                                           walk_x, walk_z, walk_radius, walk_distance, controlled);
    }
    if (field >= limit) {
        bool title_ready = title && stage == 2 && GetNowLoopNo() == LOOP_TITLE && replay_title &&
                           replay_title->mode == TITLE_MODE_TITLE && replay_title_phase == TITLE_PHASE_PUSH_START;
        std::exit(stage == 5 || title_ready ? 0 : 2);
    }
    if (controlled) {
        input = controls;
        input.buttons = 0;
    }
    if (field >= last_press + (fighting ? 6 : 13)) {
        if (controlled) {
            input.buttons = controls.buttons;
            if (control_mode == 2 && input.buttons) walk_pressed = true;
        } else if ((save || resume) && save_active()) {
            input.buttons = (uint16_t)save_buttons();
        } else
        if (stage == 0 && replay_language_phase == TITLE_LANG_SELECT) {
            input.buttons = PORT_INPUT_CROSS;
            stage = 1;
        } else if (stage == 1 && replay_title && replay_title->mode == TITLE_MODE_TITLE &&
                   replay_title_phase == TITLE_PHASE_WAIT) {
            input.buttons = PORT_INPUT_START;
            stage = 2;
        } else if (stage == 2 && !title && replay_title_phase == TITLE_PHASE_PUSH_START) {
            input.buttons = PORT_INPUT_START;
            stage = 3;
        } else if (stage == 3 && replay_title_phase == TITLE_PHASE_MENU && replay_title->menu_alpha >= 128) {
            if (resume && replay_title->select != TITLE_MENU_CONTINUE) input.buttons = PORT_INPUT_DOWN;
            else {
                input.buttons = PORT_INPUT_CROSS;
                stage = 4;
            }
        } else if (stage == 4 && !resume && replay_costume) {
            if (replay_costume->mode == 2) stage = 5;
            else if (replay_costume->mode == 0) {
                if (replay_costume->step == 0 && replay_costume->select < 3) input.buttons = PORT_INPUT_DOWN;
                else if (replay_costume->step == 1) input.buttons = PORT_INPUT_LEFT | PORT_INPUT_CROSS;
                else input.buttons = PORT_INPUT_CROSS;
            }
        } else if (stage == 5 && skip &&
                   ((GetNowLoopNo() == LOOP_DUNGEON && DngStatus.mode == DNG_STATUS_EVENT) ||
                    (GetNowLoopNo() == LOOP_EDIT && replay_edit_control == EDIT_CONTROL_EVENT)) && EdEventInfo.skip_state == 1) {
            if (GetPauseFlag() && replay_pause_frames > 17) input.buttons = PORT_INPUT_TRIANGLE;
            else if (!GetPauseFlag() && field >= next_skip) {
                input.buttons = PORT_INPUT_START;
                next_skip = field + 120;
            }
        } else if (stage == 5 && skip &&
                   ((GetNowLoopNo() == LOOP_DUNGEON && DngStatus.mode == DNG_STATUS_EVENT) ||
                    GetNowLoopNo() == LOOP_EDIT)) {
            ClsMes *message = GetEventMessage(0);
            if (message && (message->State() == CLSMES_REVEALING || message->State() == CLSMES_SHOWN ||
                            message->State() == CLSMES_PAGE_WAIT)) input.buttons = PORT_INPUT_CROSS;
        } else if (fighting && !key_stopped && !key_pickup) {
            if (floor == 1) {
                bool can_attack = target_distance >= 0 && target_distance <= 110 && local_target;
                if (gun) {
                    if (!MainChara__2 || !MainChara__2->lock_on) {
                        if (!key || (local_target && target_distance >= 0 && target_distance <= 250))
                            input.buttons = PORT_INPUT_CIRCLE;
                    } else if (gun_aim && local_target && field >= last_shot + 20) {
                        input.buttons = PORT_INPUT_CROSS;
                        last_shot = field;
                    }
                } else if (!MainChara__2 || !MainChara__2->lock_on)
                    input.buttons = can_attack ? PORT_INPUT_CROSS : PORT_INPUT_CIRCLE;
                else if (can_attack) input.buttons = PORT_INPUT_CROSS;
            } else {
                if (!MainChara__2 || !MainChara__2->lock_on)
                    input.buttons = target_distance >= 0 && target_distance <= 80 ? PORT_INPUT_CROSS : PORT_INPUT_CIRCLE;
                else if (!approaching) input.buttons = PORT_INPUT_CROSS;
            }
        }
        if (input.buttons) {
            last_press = field;
            std::fprintf(stderr, "replay: press=%04x field=%llu stage=%d\n", input.buttons,
                         (unsigned long long)field, stage);
        }
    }
    if (!controlled && fighting && gun_aim) input.buttons |= PORT_INPUT_L1;
    port_input_report(&input, report);
}
