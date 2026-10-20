#include <SDL3/SDL.h>
#include "debug.h"
#include "character.hpp"
#include "imgui.h"
#include "mainloop.hpp"
#include "port_overrides.h"
#include "savedata.hpp"
#include "scenesnd.hpp"
#include <cstdio>
#include <cstring>

enum { RESTORE_FRAME_LIMIT = 3000, RESTORE_SETTLE_FRAMES = 2, WATCH_SECONDS = 5 };

enum { RESTORE_NONE, RESTORE_LEAVING, RESTORE_ENTERING };

static int restore_step;
static int restore_frames;
static int settled_frames;
static int restore_map;
static float restore_position[4];
static float restore_rotation[4];
static int reload_count;
static int rerun_event;
static int from_watch;
static uint64_t reload_ns;
static uint64_t watch_ns;
static char reload_result[512];

static CCharacter2 *player(CScene *scene) {
    return scene->GetCharacter(scene->player_chara >= 0 ? scene->player_chara : 0);
}

static int here_map(CScene *scene) {
    return scene->now_sub_map_no >= 0 ? scene->now_sub_map_no : scene->now_map_no;
}

static void note_watch(const DebugArgs *args) {
    if (!strcmp(debug_arg(args, "from", ""), "watch")) watch_ns = SDL_GetTicksNS();
}

static void warp(const char *line) {
    debug_run(line);
    snprintf(reload_result, sizeof reload_result, "%s", debug_last_result);
}

static int command_reload(const DebugArgs *args, DebugReply *reply) {
    port_overrides_scan();
    reload_count++;
    reload_ns = SDL_GetTicksNS();
    from_watch = !strcmp(debug_arg(args, "from", ""), "watch");
    note_watch(args);
    reload_result[0] = 0;
    restore_step = RESTORE_NONE;
    int loop = GetNowLoopNo();
    CScene *scene = GetSaveData() ? GetMainScene() : NULL;
    long event = debug_arg_int(args, "event", -1);
    int rerun = debug_arg_int(args, "rerun", rerun_event) && scene && scene->event_run;
    if (event < 0 && rerun) event = scene->event_no;
    char line[128];
    if (scene && loop == LOOP_EDIT) {
        CCharacter2 *chara = player(scene);
        if (chara && event < 0) {
            chara->GetPosition(restore_position);
            chara->GetRotation(restore_rotation);
            restore_map = here_map(scene);
            restore_step = RESTORE_LEAVING;
            restore_frames = 0;
            settled_frames = 0;
        }
        snprintf(line, sizeof line, "town?map=%d&event=%ld", here_map(scene), event);
        warp(line);
    }
    if (scene && loop == LOOP_DUNGEON) {
        CSaveDataDungeon *dungeon = &GetSaveData()->save_dungeon;
        snprintf(line, sizeof line, "dungeon?dungeon=%d&floor=%d&event=%ld", dungeon->stage_id,
                 dungeon->floor_id[dungeon->stage_id], event);
        warp(line);
    }
    debug_print(reply, "{\"overrides\": %d, \"folder\": ", port_overrides_count());
    debug_print_string(reply, port_overrides_folder());
    debug_print(reply, ", \"files\": [");
    for (int i = 0; i < port_overrides_count(); ++i) {
        debug_print(reply, "%s", i ? ", " : "");
        debug_print_string(reply, port_overrides_path(i));
    }
    debug_print(reply, "], \"restore_position\": %d, \"warp\": %s}", restore_step != RESTORE_NONE,
                reload_result[0] ? reload_result : "null");
    return 0;
}

static int command_overrides(const DebugArgs *args, DebugReply *reply) {
    note_watch(args);
    debug_print(reply, "{\"overrides\": %d, \"folder\": ", port_overrides_count());
    debug_print_string(reply, port_overrides_folder());
    debug_print(reply, ", \"reloads\": %d, \"restoring\": %d", reload_count, restore_step);
    CScene *scene = GetSaveData() && GetNowLoopNo() == LOOP_EDIT ? GetMainScene() : NULL;
    CCharacter2 *chara = scene ? player(scene) : NULL;
    float position[4] = {0};
    if (chara) chara->GetPosition(position);
    if (chara) debug_print(reply, ", \"player\": [%.2f, %.2f, %.2f]", position[0], position[1], position[2]);
    debug_print(reply, "}");
    return 0;
}

extern const DebugCommand debug_reload_commands[] = {
    {"reload", command_reload, 0,
     "rescan the override folder and re-enter the current map at the player's position; event=N or rerun=1 "
     "re-runs an event instead; from=watch marks it automatic"},
    {"overrides", command_overrides, 1,
     "the override folder, how many files it holds and the player's position; from=watch marks dc2tk mod watch "
     "as running"},
    {NULL, NULL, 0, NULL},
};

void debug_reload_frame(void) {
    if (restore_step == RESTORE_NONE) return;
    CScene *scene = GetMainScene();
    if (++restore_frames > RESTORE_FRAME_LIMIT || GetNowLoopNo() != LOOP_EDIT) {
        restore_step = RESTORE_NONE;
        return;
    }
    if (restore_step == RESTORE_LEAVING) {
        if (!scene->exit_flag) restore_step = RESTORE_ENTERING;
        return;
    }
    CCharacter2 *chara = player(scene);
    if (!chara || !scene->GetMap(0) || here_map(scene) != restore_map || scene->fade.mode != FADE_MODE_IN) {
        settled_frames = 0;
        return;
    }
    if (++settled_frames < RESTORE_SETTLE_FRAMES) return;
    chara->SetPosition(restore_position);
    chara->SetRotation(restore_rotation);
    restore_step = RESTORE_NONE;
    fprintf(stderr, "reload: player back at %.1f %.1f %.1f\n", restore_position[0], restore_position[1],
            restore_position[2]);
}

void debug_reload_bar(void) {
    if (ImGui::Button("Reload")) debug_run(rerun_event ? "reload?rerun=1" : "reload?rerun=0");
    ImGui::SameLine();
    bool rerun = rerun_event != 0;
    if (ImGui::Checkbox("re-run event", &rerun)) rerun_event = rerun;
    ImGui::SameLine();
    uint64_t now = SDL_GetTicksNS();
    double seconds = reload_ns ? (double)(now - reload_ns) / 1e9 : 0.0;
    int watching = watch_ns && now - watch_ns < (uint64_t)WATCH_SECONDS * 1000000000ull;
    ImGui::TextColored(watching ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "auto");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(watching ? "dc2tk mod watch is running" : "dc2tk mod watch is not running");
    ImGui::SameLine();
    if (!reload_count) ImGui::Text("%d overrides, no reload yet", port_overrides_count());
    else
        ImGui::Text("%d overrides, reload #%d %.0f s ago%s", port_overrides_count(), reload_count, seconds,
                    from_watch ? " by dc2tk mod watch" : "");
}
