#include <SDL3/SDL.h>
#include "debug.h"
#include "imgui.h"
#include "character.hpp"
#include "mainloop.hpp"
#include "mglib.hpp"
#include "savedata.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

enum { CLIP_IDLE, CLIP_SAVING, CLIP_RECORDING, CLIP_LOADING, CLIP_PLAYING, CLIP_STARTING };
enum { CLIP_NAME = 160, CLIP_PATH = 320, TRACE_LINE = 512, CLIP_SLOT = 7, CLIP_LIMIT = 64, CLIP_REFRESH_MS = 1000 };

typedef struct {
    char name[CLIP_NAME];
    uint32_t frames;
    int loop;
    long long taken;
} ClipEntry;

static int clip_state;
static char clip_name[CLIP_NAME];
static FILE *trace;
static FILE *recorded;
static uint32_t clip_base;
static uint32_t clip_frames;
static long pause_at = -1;
static float worst_player;
static float worst_camera;
static long first_apart = -1;
static char clip_message[256];
static ClipEntry entries[CLIP_LIMIT];
static int entry_count;
static uint64_t listed_at;
static int pause_frames[CLIP_LIMIT];
static char panel_name[CLIP_NAME];
static char deleting[CLIP_NAME];

static const char *const loop_names[] = {"menu", "town", "dungeon", "title", "chara viewer", "tex viewer",
                                         "map viewer", "snd viewer", "movie viewer", "save convert"};

static void clip_folder(const char *name, char *path, size_t size) {
    if (strchr(name, '/')) snprintf(path, size, "%s", name);
    else snprintf(path, size, "build/port/clips/%s", name);
}

static void clip_file(const char *name, const char *file, char *path, size_t size) {
    char folder[CLIP_PATH];
    clip_folder(name, folder, sizeof folder);
    snprintf(path, size, "%s/%s", folder, file);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    FILE *out = in ? fopen(to, "wb") : NULL;
    char buffer[1 << 16];
    size_t length = 0;
    int ok = in && out;
    while (ok && (length = fread(buffer, 1, sizeof buffer, in)) > 0) ok = fwrite(buffer, 1, length, out) == length;
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    return ok;
}

static int read_point(FILE *file, DebugPoint *point) {
    char line[TRACE_LINE];
    while (fgets(line, sizeof line, file)) {
        unsigned frame;
        if (sscanf(line, "{\"frame\": %u, \"player\": [%f, %f, %f], \"camera\": [%f, %f, %f], \"look\": [%f, %f, %f]",
                   &frame, &point->player[0], &point->player[1], &point->player[2], &point->camera[0],
                   &point->camera[1], &point->camera[2], &point->look[0], &point->look[1], &point->look[2]) == 10)
            return 1;
    }
    return 0;
}

static float distance(const float *a, const float *b) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void debug_clip_sample(DebugPoint *point) {
    memset(point, 0, sizeof *point);
    CScene *scene = GetSaveData() && (GetNowLoopNo() == LOOP_EDIT || GetNowLoopNo() == LOOP_DUNGEON) ? GetMainScene() : NULL;
    CCharacter2 *chara = scene ? scene->GetCharacter(scene->player_chara >= 0 ? scene->player_chara : 0) : NULL;
    alignas(16) float position[4] = {0};
    mgCFrame *model = chara ? chara->CObjectFrame::frame : NULL;
    if (model) model->GetWorldPosition0(position);
    else if (chara) chara->GetPosition(position);
    memcpy(point->player, position, sizeof point->player);
    float eye[4] = {0};
    float pose[4][4];
    mgGetCameraPos(eye);
    mgGetCameraPose(pose);
    memcpy(point->camera, eye, sizeof point->camera);
    memcpy(point->look, pose[2], sizeof point->look);
}

static void format_point(char *text, size_t size, const DebugPoint *point) {
    CScene *scene = GetSaveData() && (GetNowLoopNo() == LOOP_EDIT || GetNowLoopNo() == LOOP_DUNGEON) ? GetMainScene() : NULL;
    float rotation[4] = {0};
    CCharacter2 *chara = scene ? scene->GetCharacter(scene->player_chara >= 0 ? scene->player_chara : 0) : NULL;
    if (chara) chara->GetRotation(rotation);
    snprintf(text, size,
             "\"player\": [%.3f, %.3f, %.3f], \"camera\": [%.3f, %.3f, %.3f], \"look\": [%.4f, %.4f, %.4f], "
             "\"yaw\": %.4f, \"loop\": %d, \"field\": %llu}",
             point->player[0], point->player[1], point->player[2], point->camera[0], point->camera[1], point->camera[2],
             point->look[0], point->look[1], point->look[2], rotation[1], GetNowLoopNo(),
             (unsigned long long)debug_frames[(debug_frame_total - 1) % DEBUG_HISTORY].field);
}

static void write_point(uint32_t frame, const DebugPoint *point) {
    char text[DEBUG_TRACE];
    format_point(text, sizeof text, point);
    fprintf(trace, "{\"frame\": %u, %s\n", frame, text);
}

void debug_clip_trace(char *text, size_t size) {
    DebugPoint point;
    debug_clip_sample(&point);
    format_point(text, size, &point);
}

static void finish(const char *message) {
    if (trace) fclose(trace);
    if (recorded) fclose(recorded);
    trace = NULL;
    recorded = NULL;
    clip_state = CLIP_IDLE;
    snprintf(clip_message, sizeof clip_message, "%s", message);
    fprintf(stderr, "clip: %s\n", message);
}

static int open_trace(const char *file) {
    char path[CLIP_PATH];
    clip_file(clip_name, file, path, sizeof path);
    trace = fopen(path, "w");
    return trace != NULL;
}

static const char *state_name(void);

static int clip_start(const char *name, const char *from) {
    if (clip_state != CLIP_IDLE) {
        snprintf(clip_message, sizeof clip_message, "a clip is already %s", state_name());
        return 0;
    }
    char path[CLIP_PATH], line[CLIP_PATH + 32], state[CLIP_PATH];
    mkdir("build/port/clips", 0755);
    clip_folder(name, path, sizeof path);
    mkdir(path, 0755);
    snprintf(clip_name, sizeof clip_name, "%s", name);
    clip_file(name, "start.state", path, sizeof path);
    clip_message[0] = 0;
    if (!from) {
        snprintf(line, sizeof line, "save?slot=%d&path=%s", CLIP_SLOT, path);
        debug_run(line);
        clip_state = CLIP_SAVING;
        return 1;
    }
    if (strchr(from, '/')) snprintf(state, sizeof state, "%s", from);
    else snprintf(state, sizeof state, "build/port/savestates/%s", from);
    if (!copy_file(state, path)) {
        snprintf(clip_message, sizeof clip_message, "cannot copy %s into %s", state, path);
        return 0;
    }
    snprintf(line, sizeof line, "load?slot=%d&path=%s", CLIP_SLOT, path);
    debug_run(line);
    clip_state = CLIP_STARTING;
    debug_paused = 0;
    return 1;
}

static void clip_stop(void) {
    if (clip_state == CLIP_RECORDING) {
        debug_run("record_stop");
        char message[128];
        snprintf(message, sizeof message, "recorded %s, %u frames", clip_name, debug_frame_total - clip_base);
        finish(message);
    }
    if (clip_state == CLIP_PLAYING || clip_state == CLIP_LOADING || clip_state == CLIP_STARTING) {
        debug_run("replay_stop");
        finish("playback stopped");
    }
}

static int clip_play(const char *name, long pause) {
    if (clip_state != CLIP_IDLE) return 0;
    char path[CLIP_PATH], line[CLIP_PATH + 32];
    snprintf(clip_name, sizeof clip_name, "%s", name);
    clip_file(name, "trace.jsonl", path, sizeof path);
    recorded = fopen(path, "r");
    if (!recorded) return 0;
    clip_frames = 0;
    char text[TRACE_LINE];
    while (fgets(text, sizeof text, recorded)) clip_frames++;
    rewind(recorded);
    clip_file(name, "start.state", path, sizeof path);
    snprintf(line, sizeof line, "load?slot=%d&path=%s", CLIP_SLOT, path);
    debug_run(line);
    clip_state = CLIP_LOADING;
    debug_paused = 0;
    pause_at = pause;
    worst_player = worst_camera = 0;
    first_apart = -1;
    clip_message[0] = 0;
    return clip_state == CLIP_LOADING;
}

static void begin_after_state(const char *input_command, const char *file, const char *done) {
    if (!strstr(debug_last_result, done)) {
        char message[256];
        snprintf(message, sizeof message, "savestate failed: %s", debug_last_result);
        finish(message);
        return;
    }
    char path[CLIP_PATH], line[CLIP_PATH + 32];
    clip_file(clip_name, "input.txt", path, sizeof path);
    snprintf(line, sizeof line, "%s?path=%s", input_command, path);
    debug_run(line);
    if (!open_trace(file)) {
        finish("cannot write the trace");
        return;
    }
    clip_base = debug_frame_total;
    clip_state = clip_state == CLIP_LOADING ? CLIP_PLAYING : CLIP_RECORDING;
}

static void compare(uint32_t frame, const DebugPoint *now) {
    DebugPoint then;
    if (!recorded || !read_point(recorded, &then)) return;
    float apart_player = distance(now->player, then.player), apart_camera = distance(now->camera, then.camera);
    if (apart_player > worst_player) worst_player = apart_player;
    if (apart_camera > worst_camera) worst_camera = apart_camera;
    if (first_apart < 0 && (apart_player > 0.5f || apart_camera > 0.5f)) first_apart = (long)frame;
}

void debug_clip_frame(void) {
    if (clip_state == CLIP_SAVING) begin_after_state("record", "trace.jsonl", "\"saved\"");
    else if (clip_state == CLIP_LOADING) begin_after_state("replay", "play.jsonl", "\"loaded\"");
    else if (clip_state == CLIP_STARTING) begin_after_state("record", "trace.jsonl", "\"loaded\"");
    if (clip_state != CLIP_RECORDING && clip_state != CLIP_PLAYING) return;
    uint32_t frame = debug_frame_total - clip_base;
    DebugPoint point;
    debug_clip_sample(&point);
    write_point(frame, &point);
    if (clip_state != CLIP_PLAYING) return;
    compare(frame, &point);
    if (pause_at >= 0 && (long)frame == pause_at) debug_paused = 1;
    if (frame + 1 < clip_frames) return;
    char message[256];
    snprintf(message, sizeof message, "played %s, %u frames; player apart up to %.2f, camera up to %.2f, first at %ld",
             clip_name, clip_frames, worst_player, worst_camera, first_apart);
    finish(message);
}

void debug_clip_hotkey(void) {
    if (clip_state == CLIP_RECORDING || clip_state == CLIP_PLAYING) {
        clip_stop();
        return;
    }
    char name[CLIP_NAME];
    time_t now = time(NULL);
    strftime(name, sizeof name, "clip-%Y%m%d-%H%M%S", localtime(&now));
    clip_start(name, NULL);
}

static const char *state_name(void) {
    static const char *const names[] = {"idle", "saving", "recording", "loading", "playing", "starting"};
    return names[clip_state];
}

static void describe(ClipEntry *entry) {
    char path[CLIP_PATH], line[TRACE_LINE];
    clip_file(entry->name, "trace.jsonl", path, sizeof path);
    FILE *file = fopen(path, "r");
    entry->frames = 0;
    entry->loop = -1;
    while (file && fgets(line, sizeof line, file)) {
        const char *loop = strstr(line, "\"loop\": ");
        if (!entry->frames && loop) entry->loop = atoi(loop + 8);
        entry->frames++;
    }
    if (file) fclose(file);
    clip_file(entry->name, "start.state", path, sizeof path);
    SDL_PathInfo info;
    entry->taken = SDL_GetPathInfo(path, &info) ? (long long)(info.modify_time / 1000000000) : 0;
}

static void list_clips(void) {
    int count = 0;
    char **names = SDL_GlobDirectory("build/port/clips", "*", 0, &count);
    entry_count = 0;
    for (int i = 0; names && i < count && entry_count < CLIP_LIMIT; ++i) {
        ClipEntry entry;
        snprintf(entry.name, sizeof entry.name, "%s", names[i]);
        describe(&entry);
        int at = entry_count++;
        while (at > 0 && entries[at - 1].taken < entry.taken) {
            entries[at] = entries[at - 1];
            at--;
        }
        entries[at] = entry;
    }
    SDL_free(names);
    listed_at = SDL_GetTicks();
}

static void delete_clip(const char *name) {
    static const char *const files[] = {"start.state", "input.txt", "trace.jsonl", "play.jsonl"};
    char path[CLIP_PATH];
    for (const char *file : files) {
        clip_file(name, file, path, sizeof path);
        unlink(path);
    }
    clip_folder(name, path, sizeof path);
    rmdir(path);
    listed_at = 0;
}

static const char *loop_name(int loop) {
    return loop >= 0 && loop < (int)(sizeof loop_names / sizeof loop_names[0]) ? loop_names[loop] : "?";
}

static void live_values(void) {
    DebugPoint point;
    debug_clip_sample(&point);
    ImGui::Text("player %.1f %.1f %.1f   camera %.1f %.1f %.1f   looking %.2f %.2f %.2f", point.player[0],
                point.player[1], point.player[2], point.camera[0], point.camera[1], point.camera[2], point.look[0],
                point.look[1], point.look[2]);
}

static void clip_row(int index) {
    ClipEntry *entry = &entries[index];
    ImGui::PushID(index);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(entry->name);
    ImGui::TableNextColumn();
    ImGui::Text("%u", entry->frames);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(loop_name(entry->loop));
    ImGui::TableNextColumn();
    char when[32] = "";
    time_t taken = (time_t)entry->taken;
    if (taken) strftime(when, sizeof when, "%d %b %H:%M", localtime(&taken));
    ImGui::TextUnformatted(when);
    ImGui::TableNextColumn();
    int idle = clip_state == CLIP_IDLE;
    ImGui::BeginDisabled(!idle);
    if (ImGui::SmallButton("Play")) clip_play(entry->name, -1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Play to")) clip_play(entry->name, pause_frames[index]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    int last = entry->frames ? (int)entry->frames - 1 : 0;
    ImGui::SliderInt("##frame", &pause_frames[index], 0, last, "frame %d");
    ImGui::SameLine();
    int confirming = !strcmp(deleting, entry->name);
    if (ImGui::SmallButton(confirming ? "Really delete?" : "Delete")) {
        if (confirming) {
            delete_clip(entry->name);
            deleting[0] = 0;
        } else {
            snprintf(deleting, sizeof deleting, "%s", entry->name);
        }
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void debug_clip_panel(void) {
    if (!listed_at || SDL_GetTicks() - listed_at > CLIP_REFRESH_MS) list_clips();
    uint32_t frame = clip_state == CLIP_RECORDING || clip_state == CLIP_PLAYING ? debug_frame_total - clip_base : 0;
    if (clip_state == CLIP_IDLE) {
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##name", "name (blank for the time)", panel_name, sizeof panel_name);
        ImGui::SameLine();
        if (ImGui::Button("Record clip (F8)")) {
            if (panel_name[0]) clip_start(panel_name, NULL);
            else debug_clip_hotkey();
            panel_name[0] = 0;
        }
    } else {
        if (ImGui::Button(clip_state == CLIP_RECORDING ? "Stop recording (F8)" : "Stop playback")) clip_stop();
    }
    ImGui::SameLine();
    if (ImGui::Button(debug_paused ? "Resume (F5)" : "Pause (F5)")) debug_paused = !debug_paused;
    ImGui::SameLine();
    if (ImGui::Button("Step (F6)")) {
        debug_paused = 1;
        debug_steps = 1;
    }
    if (clip_state == CLIP_RECORDING) ImGui::Text("recording %s: frame %u", clip_name, frame);
    if (clip_state == CLIP_SAVING || clip_state == CLIP_LOADING || clip_state == CLIP_STARTING) ImGui::Text("%s %s...", state_name(), clip_name);
    if (clip_state == CLIP_PLAYING) {
        ImGui::Text("playing %s: frame %u of %u   player apart %.2f, camera apart %.2f", clip_name, frame, clip_frames,
                    worst_player, worst_camera);
        ImGui::ProgressBar(clip_frames ? (float)frame / (float)clip_frames : 0.0f);
    }
    if (clip_state == CLIP_IDLE && clip_message[0]) ImGui::TextWrapped("%s", clip_message);
    live_values();
    debug_blackbox_panel();
    debug_fault_panel();
    ImGui::Separator();
    if (!entry_count) {
        ImGui::TextUnformatted("No clips yet. Press F8 in the game window to record one.");
        return;
    }
    if (!ImGui::BeginTable("clips", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) return;
    ImGui::TableSetupColumn("clip", ImGuiTableColumnFlags_WidthFixed, 230);
    ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("place", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("recorded", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < entry_count; ++i) clip_row(i);
    ImGui::EndTable();
}

static void print_status(DebugReply *reply) {
    debug_print(reply, "{\"state\": \"%s\", \"name\": ", state_name());
    debug_print_string(reply, clip_name);
    uint32_t frame = clip_state == CLIP_RECORDING || clip_state == CLIP_PLAYING ? debug_frame_total - clip_base : 0;
    debug_print(reply, ", \"frame\": %u, \"frames\": %u, \"player_apart\": %.3f, \"camera_apart\": %.3f, "
                       "\"first_apart\": %ld, \"message\": ",
                frame, clip_frames, worst_player, worst_camera, first_apart);
    debug_print_string(reply, clip_message);
    debug_print(reply, "}");
}

static int command_clip(const DebugArgs *args, DebugReply *reply) {
    char name[CLIP_NAME];
    time_t now = time(NULL);
    strftime(name, sizeof name, "clip-%Y%m%d-%H%M%S", localtime(&now));
    if (!clip_start(debug_arg(args, "name", name), debug_arg(args, "from", NULL)))
        return debug_fail(reply, "%s", clip_message);
    print_status(reply);
    return 0;
}

static int command_clip_stop(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (clip_state == CLIP_IDLE) return debug_fail(reply, "no clip is recording or playing");
    clip_stop();
    print_status(reply);
    return 0;
}

static int command_clip_play(const DebugArgs *args, DebugReply *reply) {
    const char *name = debug_arg(args, "name", clip_name);
    if (!*name) return debug_fail(reply, "name=clip is needed");
    if (!clip_play(name, debug_arg_int(args, "pause", -1)))
        return debug_fail(reply, "cannot play %s: %s", name, clip_message[0] ? clip_message : "no such clip");
    print_status(reply);
    return 0;
}

static int command_clip_status(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    print_status(reply);
    return 0;
}

static int command_clips(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    int count = 0;
    char **names = SDL_GlobDirectory("build/port/clips", "*", 0, &count);
    debug_print(reply, "{\"clips\": [");
    for (int i = 0; names && i < count; ++i) {
        if (i) debug_print(reply, ", ");
        debug_print_string(reply, names[i]);
    }
    debug_print(reply, "]}");
    SDL_free(names);
    return 0;
}

extern const DebugCommand debug_clip_commands[] = {
    {"clip", command_clip, 0,
     "record a clip: a savestate now, then the input and each frame's player position and camera until clip_stop "
     "(F8 in the game window starts and stops one); name=N, or a folder path; from=STATE starts it from that "
     "savestate (a name under build/port/savestates or a path) instead of now"},
    {"clip_stop", command_clip_stop, 0, "stop recording or playing a clip"},
    {"clip_play", command_clip_play, 0,
     "load a clip's savestate and replay its input, tracing the same values to play.jsonl and comparing them; "
     "name=N or a folder path [pause=frame]"},
    {"clip_status", command_clip_status, 1, "the clip being recorded or played, and how far playback strayed"},
    {"clips", command_clips, 1, "the recorded clips in build/port/clips"},
    {NULL, NULL, 0, NULL},
};
