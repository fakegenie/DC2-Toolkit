#include <SDL3/SDL.h>
#include "debug.h"
#include "imgui.h"
#include "character.hpp"
#include "dng_main.hpp"
#include "editloop.hpp"
#include "mainloop.hpp"
#include "memory_card.h"
#include "port_graphics.h"
#include "port_state.h"
#include "mglib.hpp"
#include "render.h"
#include "savedata.hpp"
#include "scenesnd.hpp"
#include "subgame.hpp"
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
extern "C" {
#include "audio.h"
}
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

enum { START_LAUNCH, START_CHECKPOINT };
enum { LOAD_NONE, LOAD_ARRIVE, LOAD_ARRIVING, LOAD_APPLY, LOAD_LEAVING, LOAD_SETTLING, LOAD_STEADYING };
enum { PLAY_NONE, PLAY_LOADING, PLAY_RUNNING };
enum { RECORDING_VERSION = 1, CHECKPOINT_VERSION = 1, LOAD_LIMIT = 3000, SETTLE_FRAMES = 2, STEADY_FRAMES = 4, BOOT_PRESS_EVERY = 60,
       NAME_SIZE = 160, PATH_SIZE = 512, LINE_SIZE = 1024, LIST_LIMIT = 64, LIST_REFRESH_MS = 1000 };

extern "C" unsigned int port_rand_state(void);
extern "C" void port_floor_seed_force(int seed);
extern "C" int port_floor_seed_last(void);
extern "C" char **port_arguments;
extern int LoopMode;

typedef struct {
    char magic[8];
    uint32_t version;
    int32_t loop;
    int32_t map;
    int32_t floor;
    int32_t floor_seed;
    uint32_t seed;
    float position[4];
    float rotation[4];
    unsigned char save[sizeof(CSaveData)];
} Checkpoint;

typedef struct {
    uint32_t frame;
    struct port_input_state state;
} InputStep;

typedef struct {
    uint32_t frame;
    std::string line;
} CommandStep;

typedef struct {
    DebugPoint point;
    float yaw;
    int loop;
    uint64_t field;
    float frame_ms;
    float game_ms;
    float draw_ms;
    float wait_ms;
    float present_ms;
    uint32_t replays;
    float replay_ms;
    float gpu_ms;
} TracePoint;

typedef struct {
    std::string path;
    std::vector<unsigned char> bytes;
} CardFile;

typedef struct {
    int start;
    uint32_t frames;
    Checkpoint checkpoint;
    std::vector<InputStep> inputs;
    std::vector<CommandStep> commands;
    std::vector<TracePoint> trace;
    std::vector<uint64_t> fields;
    std::vector<CardFile> card;
} Recording;

typedef struct {
    char name[NAME_SIZE];
    int start;
    uint32_t frames;
    int loop;
    long long taken;
} Entry;

static Recording live;
static uint32_t live_base;
static uint64_t live_tick_base;
static uint64_t live_field_seen;
static struct port_input_state live_last;
static char live_lost[256];

static Recording played;
static char played_name[NAME_SIZE];
static int playing;
static uint32_t play_base;
static long pause_at = -1;
static size_t play_input;
static size_t play_command;
static float worst_player;
static float worst_camera;
static long first_apart = -1;

static int load_step;
static int load_frames;
static int settled_frames;
static float steady_camera[3];
static int relaunching;
static Checkpoint loading;
static std::vector<CardFile> loading_card;

static uint32_t latched_frame = UINT32_MAX;
static struct port_input_state latched;

static char message[512];
static Entry entries[LIST_LIMIT];
static int entry_count;
static uint64_t listed_at;
static int pause_frames[LIST_LIMIT];
static char panel_name[NAME_SIZE];
static char deleting[NAME_SIZE];

static const char *const loop_names[] = {"menu", "town", "dungeon", "title", "chara viewer", "tex viewer",
                                         "map viewer", "snd viewer", "movie viewer", "save convert"};

static const char *loop_name(int loop) {
    return loop >= 0 && loop < (int)(sizeof loop_names / sizeof loop_names[0]) ? loop_names[loop] : "?";
}

static const char *place_name(const Entry *entry) {
    return entry->start == START_LAUNCH ? "boot" : loop_name(entry->loop);
}

static void note(const char *format, ...) {
    va_list list;
    va_start(list, format);
    vsnprintf(message, sizeof message, format, list);
    va_end(list);
    fprintf(stderr, "recording: %s\n", message);
}

static void folder_of(const char *name, char *path, size_t size) {
    if (strchr(name, '/')) snprintf(path, size, "%s", name);
    else snprintf(path, size, "build/port/recordings/%s", name);
}

static void file_of(const char *name, const char *file, char *path, size_t size) {
    char folder[PATH_SIZE];
    folder_of(name, folder, sizeof folder);
    snprintf(path, size, "%s/%s", folder, file);
}

static void make_folders(const char *path) { SDL_CreateDirectory(path); }

static CScene *playing_scene(void) {
    int loop = GetNowLoopNo();
    return GetSaveData() && (loop == LOOP_EDIT || loop == LOOP_DUNGEON) ? GetMainScene() : NULL;
}

static CCharacter2 *player(CScene *scene) {
    return scene ? scene->GetCharacter(scene->player_chara >= 0 ? scene->player_chara : 0) : NULL;
}

static int here_map(CScene *scene) {
    return scene->now_sub_map_no >= 0 ? scene->now_sub_map_no : scene->now_map_no;
}

void debug_recording_sample(DebugPoint *point) {
    memset(point, 0, sizeof *point);
    CCharacter2 *chara = player(playing_scene());
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

static TracePoint sample_trace(void) {
    TracePoint trace;
    debug_recording_sample(&trace.point);
    float rotation[4] = {0};
    CCharacter2 *chara = player(playing_scene());
    if (chara) chara->GetRotation(rotation);
    trace.yaw = rotation[1];
    trace.loop = GetNowLoopNo();
    const DebugFrame *last = &debug_frames[(debug_frame_total - 1) % DEBUG_HISTORY];
    trace.field = last->field;
    trace.frame_ms = last->frame_ms;
    trace.game_ms = last->game_ms;
    trace.draw_ms = last->draw_ms;
    trace.wait_ms = last->wait_ms;
    trace.present_ms = last->present_ms;
    trace.replays = last->stats.replays;
    trace.replay_ms = (float)last->stats.replay_ns / 1e6f;
    trace.gpu_ms = (float)last->stats.gpu_ns / 1e6f;
    return trace;
}

static int format_trace(char *text, size_t size, uint32_t frame, const TracePoint *trace) {
    const DebugPoint *p = &trace->point;
    return snprintf(text, size,
                    "{\"frame\": %u, \"player\": [%.3f, %.3f, %.3f], \"camera\": [%.3f, %.3f, %.3f], \"look\": [%.4f, "
                    "%.4f, %.4f], \"yaw\": %.4f, \"loop\": %d, \"field\": %llu, \"frame_ms\": %.3f, \"game_ms\": %.3f, "
                    "\"draw_ms\": %.3f, \"wait_ms\": %.3f, \"present_ms\": %.3f, \"replays\": %u, \"replay_ms\": %.3f, "
                    "\"gpu_ms\": %.3f}\n",
                    frame, p->player[0], p->player[1], p->player[2], p->camera[0], p->camera[1], p->camera[2],
                    p->look[0], p->look[1], p->look[2], trace->yaw, trace->loop, (unsigned long long)trace->field,
                    trace->frame_ms, trace->game_ms, trace->draw_ms, trace->wait_ms, trace->present_ms, trace->replays,
                    trace->replay_ms, trace->gpu_ms);
}

static int read_trace(const char *line, TracePoint *trace) {
    unsigned frame;
    unsigned long long field = 0;
    DebugPoint *p = &trace->point;
    int got = sscanf(line,
                     "{\"frame\": %u, \"player\": [%f, %f, %f], \"camera\": [%f, %f, %f], \"look\": [%f, %f, %f], "
                     "\"yaw\": %f, \"loop\": %d, \"field\": %llu",
                     &frame, &p->player[0], &p->player[1], &p->player[2], &p->camera[0], &p->camera[1],
                     &p->camera[2], &p->look[0], &p->look[1], &p->look[2], &trace->yaw, &trace->loop, &field);
    trace->field = field;
    return got >= 10;
}

static float distance(const float *a, const float *b) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

static const char *checkpoint_problem(void) {
    int loop = GetNowLoopNo();
    CScene *scene = playing_scene();
    if (!scene) return "recordings start in a town or a dungeon";
    if (loop == LOOP_EDIT && LoopMode != EDIT_LOOP_WALK) return "walk about the town first: a menu or Georama is up";
    if (loop == LOOP_DUNGEON && DngStatus.mode != DNG_STATUS_FIELD) return "walk about the floor first: a menu or event is up";
    if (scene->event_run) return "wait for the event to finish";
    if (SubGameRunning()) return "a minigame is running (holding the fishing rod is one): start recording before it";
    if (scene->exit_flag || scene->fade.mode != FADE_MODE_NONE) return "wait for the map change to finish";
    if (!player(scene)) return "the player is not on the map yet";
    if (!port_state_possible()) return "wait for the game to get back to its main loop";
    return NULL;
}

static void take_checkpoint(Checkpoint *checkpoint) {
    memset(checkpoint, 0, sizeof *checkpoint);
    memcpy(checkpoint->magic, "dc2tkcp", 8);
    checkpoint->version = CHECKPOINT_VERSION;
    CScene *scene = GetMainScene();
    CSaveData *save = GetSaveData();
    checkpoint->loop = GetNowLoopNo();
    checkpoint->map = checkpoint->loop == LOOP_DUNGEON ? save->save_dungeon.stage_id : here_map(scene);
    checkpoint->floor = checkpoint->loop == LOOP_DUNGEON ? save->save_dungeon.floor_id[save->save_dungeon.stage_id] : -1;
    checkpoint->floor_seed = checkpoint->loop == LOOP_DUNGEON ? port_floor_seed_last() : -1;
    checkpoint->seed = port_rand_state();
    CCharacter2 *chara = player(scene);
    chara->GetPosition(checkpoint->position);
    chara->GetRotation(checkpoint->rotation);
    memcpy(checkpoint->save, save, sizeof checkpoint->save);
}

static int settled(const Checkpoint *checkpoint) {
    CScene *scene = playing_scene();
    if (!scene || GetNowLoopNo() != checkpoint->loop || scene->exit_flag || !player(scene)) return 0;
    if (checkpoint->loop == LOOP_EDIT && (LoopMode != EDIT_LOOP_WALK || here_map(scene) != checkpoint->map)) return 0;
    if (checkpoint->loop == LOOP_DUNGEON && DngStatus.mode != DNG_STATUS_FIELD) return 0;
    return !scene->event_run && port_state_possible();
}

static void read_card(const std::string &root, const std::string &relative, std::vector<CardFile> *card) {
    std::string folder = relative.empty() ? root : root + "/" + relative;
    DIR *directory = opendir(folder.c_str());
    if (!directory) return;
    while (struct dirent *entry = readdir(directory)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string inner = relative.empty() ? entry->d_name : relative + "/" + entry->d_name;
        std::string path = root + "/" + inner;
        struct stat info;
        if (stat(path.c_str(), &info)) continue;
        if (S_ISDIR(info.st_mode)) {
            read_card(root, inner, card);
            continue;
        }
        FILE *file = fopen(path.c_str(), "rb");
        if (!file) continue;
        CardFile copy;
        copy.path = inner;
        copy.bytes.resize((size_t)info.st_size);
        size_t got = copy.bytes.empty() ? 0 : fread(copy.bytes.data(), 1, copy.bytes.size(), file);
        fclose(file);
        if (got == copy.bytes.size()) card->push_back(copy);
    }
    closedir(directory);
}

static std::vector<CardFile> card_now(void) {
    std::vector<CardFile> card;
    read_card(port_memory_card_root(), "", &card);
    return card;
}

static void begin_live(int start, const Checkpoint *checkpoint, const std::vector<CardFile> &card) {
    live.start = start;
    live.frames = 0;
    if (checkpoint) live.checkpoint = *checkpoint;
    live.inputs.clear();
    live.commands.clear();
    live.trace.clear();
    live.fields.clear();
    live.card = card;
    live_base = debug_frame_total;
    live_tick_base = port_ticks;
    live_field_seen = port_fields;
    memset(&live_last, 0, sizeof live_last);
    latched_frame = UINT32_MAX;
    live_lost[0] = 0;
}

static void begin_play_running(void) {
    playing = PLAY_RUNNING;
    play_base = debug_frame_total;
    port_field_script = played.fields.data();
    port_field_script_count = played.fields.size();
    port_field_script_next = 0;
    port_field_script_base = port_ticks;
}

static void end_play(void) {
    playing = PLAY_NONE;
    port_field_script_count = 0;
    port_field_script_next = 0;
}

static void begin_load(const Checkpoint *checkpoint, const std::vector<CardFile> &card) {
    loading = *checkpoint;
    loading_card = card;
    load_frames = 0;
    settled_frames = 0;
    load_step = GetNowLoopNo() == checkpoint->loop && !checkpoint_problem() ? LOAD_APPLY : LOAD_ARRIVE;
}

static int enter_checkpoint_place(void) {
    if (loading.loop == LOOP_DUNGEON) port_floor_seed_force(loading.floor_seed);
    return debug_scenario_enter(loading.loop, loading.map, loading.floor, -1);
}

static void place_player(void) {
    CCharacter2 *chara = player(GetMainScene());
    chara->SetPosition(loading.position);
    chara->SetRotation(loading.rotation);
}

static void finish_load(void) {
    load_step = LOAD_NONE;
    begin_live(START_CHECKPOINT, &loading, loading_card);
    if (playing == PLAY_LOADING) {
        begin_play_running();
        note("playing %s from its checkpoint", played_name);
        return;
    }
    note("recording from a checkpoint in the %s", loop_name(loading.loop));
}

static void fail_load(const char *why) {
    load_step = LOAD_NONE;
    port_floor_seed_force(-1);
    end_play();
    note("the checkpoint did not load: %s", why);
}

static void load_frame(void) {
    if (load_step == LOAD_NONE) return;
    if (++load_frames > LOAD_LIMIT) {
        fail_load("the game did not settle on the map in time");
        return;
    }
    if (load_step == LOAD_ARRIVE) {
        memcpy(GetSaveData(), loading.save, sizeof loading.save);
        if (enter_checkpoint_place()) load_step = LOAD_ARRIVING;
        else if (load_frames % BOOT_PRESS_EVERY == 0) debug_hold(DEBUG_BUTTON_CROSS, 6);
        return;
    }
    if (load_step == LOAD_ARRIVING) {
        if (!settled(&loading)) return;
        load_step = LOAD_APPLY;
    }
    if (load_step == LOAD_APPLY) {
        memcpy(GetSaveData(), loading.save, sizeof loading.save);
        srand(loading.seed);
        if (!enter_checkpoint_place()) {
            fail_load("the game would not change map");
            return;
        }
        load_step = LOAD_LEAVING;
        return;
    }
    if (load_step == LOAD_LEAVING) {
        if (!GetMainScene()->exit_flag) load_step = LOAD_SETTLING;
        return;
    }
    if (!settled(&loading)) {
        settled_frames = 0;
        return;
    }
    if (load_step == LOAD_SETTLING) {
        if (++settled_frames < SETTLE_FRAMES) return;
        place_player();
        load_step = LOAD_STEADYING;
        settled_frames = 0;
        return;
    }
    DebugPoint point;
    debug_recording_sample(&point);
    float moved = fabsf(point.camera[0] - steady_camera[0]) + fabsf(point.camera[1] - steady_camera[1]) +
                  fabsf(point.camera[2] - steady_camera[2]);
    memcpy(steady_camera, point.camera, sizeof steady_camera);
    if (moved > 0.001f) settled_frames = 0;
    else if (++settled_frames >= STEADY_FRAMES) finish_load();
}

int debug_recording_busy(void) {
    return load_step != LOAD_NONE;
}

int debug_recording_holds_input(void) {
    return load_step != LOAD_NONE;
}

static const char *start_here(void) {
    if (load_step != LOAD_NONE) return "a checkpoint is already loading";
    const char *problem = checkpoint_problem();
    if (problem) return problem;
    Checkpoint checkpoint;
    take_checkpoint(&checkpoint);
    end_play();
    begin_load(&checkpoint, card_now());
    note("loading a checkpoint here to record from");
    return NULL;
}

void debug_recording_lost(const char *why) {
    snprintf(live_lost, sizeof live_lost, "%s", why);
    end_play();
}

static void encode(std::string *out, const char *text) {
    for (const char *at = text; *at; ++at) {
        unsigned char c = (unsigned char)*at;
        if (c <= ' ' || c == '%' || c == '&' || c == '=' || c == '?' || c >= 0x7f) {
            char escaped[4];
            snprintf(escaped, sizeof escaped, "%%%02X", c);
            *out += escaped;
        } else *out += (char)c;
    }
}

void debug_recording_command(const char *name, const DebugArgs *args) {
    if (live_lost[0]) return;
    CommandStep step;
    step.frame = debug_frame_total - live_base;
    step.line = name;
    for (int i = 0; i < args->count; ++i) {
        step.line += i ? "&" : "?";
        encode(&step.line, args->keys[i]);
        step.line += "=";
        encode(&step.line, args->values[i]);
    }
    live.commands.push_back(step);
}

static struct port_input_state recorded_input(uint32_t frame) {
    while (play_input < played.inputs.size() && played.inputs[play_input].frame <= frame) play_input++;
    struct port_input_state state;
    memset(&state, 0, sizeof state);
    if (play_input > 0) state = played.inputs[play_input - 1].state;
    return state;
}

void debug_recording_input(struct port_input_state *state) {
    if (debug_frame_total != latched_frame) {
        latched_frame = debug_frame_total;
        latched = playing == PLAY_RUNNING ? recorded_input(debug_frame_total - play_base) : *state;
        if (load_step == LOAD_NONE && !live_lost[0] && memcmp(&latched, &live_last, sizeof latched)) {
            InputStep step;
            step.frame = debug_frame_total - live_base;
            step.state = latched;
            live.inputs.push_back(step);
            live_last = latched;
        }
    }
    if (playing == PLAY_RUNNING || load_step == LOAD_NONE) *state = latched;
}

static void finish_play(void) {
    end_play();
    note("played %s, %u frames; player apart up to %.2f, camera up to %.2f, first at %ld", played_name,
         played.frames, worst_player, worst_camera, first_apart);
    char path[PATH_SIZE], line[LINE_SIZE];
    file_of(played_name, "play.jsonl", path, sizeof path);
    FILE *file = fopen(path, "wb");
    for (size_t i = 0; file && i < live.trace.size(); ++i) {
        format_trace(line, sizeof line, (uint32_t)i, &live.trace[i]);
        fputs(line, file);
    }
    if (file) fclose(file);
}

static void play_frame(void) {
    if (playing != PLAY_RUNNING) return;
    uint32_t frame = debug_frame_total - play_base;
    while (play_command < played.commands.size() && played.commands[play_command].frame <= frame)
        debug_run(played.commands[play_command++].line.c_str());
    uint32_t done = frame ? frame - 1 : 0;
    if (frame && done < played.trace.size() && done < live.trace.size()) {
        const DebugPoint *then = &played.trace[done].point, *now = &live.trace[done].point;
        float apart_player = distance(now->player, then->player), apart_camera = distance(now->camera, then->camera);
        if (apart_player > worst_player) worst_player = apart_player;
        if (apart_camera > worst_camera) worst_camera = apart_camera;
        if (first_apart < 0 && (apart_player > 0.5f || apart_camera > 0.5f)) first_apart = (long)done;
    }
    if (pause_at >= 0 && (long)frame == pause_at) debug_paused = 1;
    if (frame >= played.frames) finish_play();
}

static void collect_fields(void) {
    if (port_fields - live_field_seen > PORT_FIELD_LOG) {
        debug_recording_lost("too many fields went by in one frame");
        return;
    }
    while (live_field_seen < port_fields) {
        live_field_seen++;
        live.fields.push_back(port_field_ticks[live_field_seen % PORT_FIELD_LOG] - live_tick_base);
    }
}

static void hide_window(void *unused) {
    (void)unused;
    SDL_HideWindow(render_window());
}

// A new process with the same arguments and environment, as exec would give, which Windows has not got. This one lets
// go of the debug port first, hides its window and stops its sound, then waits for the new one and leaves with its
// status, so whoever started the game still waits on the process it started.
static void restart(void) {
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)port_arguments);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    debug_server_stop();
    SDL_Process *game = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!game) {
        debug_server_resume();
        note("the game could not restart itself: %s", SDL_GetError());
        return;
    }
    SDL_RunOnMainThread(hide_window, NULL, false);
    audio_shutdown();
    int status = 0;
    SDL_WaitProcess(game, true, &status);
    exit(status);
}

void debug_recording_frame(void) {
    if (relaunching) {
        relaunching = 0;
        restart();
    }
    if (load_step == LOAD_NONE && !live_lost[0] && debug_frame_total > live_base) {
        live.trace.push_back(sample_trace());
        live.frames = debug_frame_total - live_base;
        collect_fields();
    }
    load_frame();
    play_frame();
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(data, 1, size, file) == size;
    return !fclose(file) && ok;
}

static std::string recording_text(const Recording *recording) {
    char line[LINE_SIZE];
    snprintf(line, sizeof line, "dc2tk recording %d\nstart %s\nframes %u\n", RECORDING_VERSION,
             recording->start == START_CHECKPOINT ? "checkpoint" : "launch", recording->frames);
    std::string text = line;
    for (const InputStep &step : recording->inputs) {
        const struct port_input_state *s = &step.state;
        snprintf(line, sizeof line, "input %u 0x%04x %d %d %d %d %d %d\n", step.frame, s->buttons, s->left_x,
                 s->left_y, s->right_x, s->right_y, s->left_trigger, s->right_trigger);
        text += line;
    }
    for (const CommandStep &step : recording->commands) {
        snprintf(line, sizeof line, "command %u ", step.frame);
        text += line;
        text += step.line;
        text += "\n";
    }
    uint64_t previous = 0;
    for (size_t i = 0; i < recording->fields.size();) {
        uint64_t gap = recording->fields[i] - previous;
        size_t repeat = 0;
        while (i < recording->fields.size() && recording->fields[i] - previous == gap) {
            previous = recording->fields[i++];
            repeat++;
        }
        snprintf(line, sizeof line, "fields %llu %zu\n", (unsigned long long)gap, repeat);
        text += line;
    }
    return text;
}

static std::string trace_text(const Recording *recording) {
    std::string text;
    char line[LINE_SIZE];
    for (size_t i = 0; i < recording->trace.size(); ++i) {
        format_trace(line, sizeof line, (uint32_t)i, &recording->trace[i]);
        text += line;
    }
    return text;
}

static const char *write_recording(const char *name, const Recording *recording) {
    char folder[PATH_SIZE], path[PATH_SIZE];
    folder_of(name, folder, sizeof folder);
    make_folders(folder);
    std::string text = recording_text(recording), trace = trace_text(recording);
    file_of(name, "recording.txt", path, sizeof path);
    if (!write_file(path, text.data(), text.size())) return "cannot write recording.txt";
    file_of(name, "trace.jsonl", path, sizeof path);
    if (!write_file(path, trace.data(), trace.size())) return "cannot write trace.jsonl";
    if (recording->start == START_CHECKPOINT) {
        file_of(name, "checkpoint.bin", path, sizeof path);
        if (!write_file(path, &recording->checkpoint, sizeof recording->checkpoint)) return "cannot write checkpoint.bin";
    }
    for (const CardFile &file : recording->card) {
        std::string target = std::string(folder) + "/card/" + file.path;
        make_folders(target.substr(0, target.rfind('/')).c_str());
        if (!write_file(target.c_str(), file.bytes.data(), file.bytes.size())) return "cannot write the memory card copy";
    }
    listed_at = 0;
    return NULL;
}

static void time_name(const char *prefix, char *name, size_t size) {
    SDL_Time now = 0;
    SDL_DateTime local;
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &local, true)) memset(&local, 0, sizeof local);
    snprintf(name, size, "%s-%04d%02d%02d-%02d%02d%02d", prefix, local.year, local.month, local.day, local.hour,
             local.minute, local.second);
}

static const char *save_live(const char *name) {
    if (load_step != LOAD_NONE) return "a checkpoint is still loading";
    if (live_lost[0]) return live_lost;
    const char *problem = write_recording(name, &live);
    if (problem) return problem;
    note("saved %s, %u frames from %s", name, live.frames, live.start == START_CHECKPOINT ? "a checkpoint" : "launch");
    return NULL;
}

const char *debug_recording_write(const char *prefix, char *name, size_t size) {
    time_name(prefix, name, size);
    return save_live(name);
}

void debug_recording_crash(void) {
    char name[NAME_SIZE];
    time_name("crash", name, sizeof name);
    const char *problem = save_live(name);
    char line[NAME_SIZE + 64];
    int length = problem ? snprintf(line, sizeof line, "recording: %s\n", problem)
                         : snprintf(line, sizeof line, "recording: saved %s up to the crash\n", name);
    ssize_t written = write(2, line, (size_t)length);
    (void)written;
}

static int read_file(const char *path, std::vector<unsigned char> *bytes) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes->resize(size > 0 ? (size_t)size : 0);
    size_t got = bytes->empty() ? 0 : fread(bytes->data(), 1, bytes->size(), file);
    fclose(file);
    return got == bytes->size();
}

static const char *read_recording(const char *name, Recording *recording) {
    char path[PATH_SIZE], line[LINE_SIZE], start[32] = "";
    file_of(name, "recording.txt", path, sizeof path);
    FILE *file = fopen(path, "r");
    if (!file) return "no such recording";
    int version = 0;
    recording->frames = 0;
    recording->inputs.clear();
    recording->commands.clear();
    recording->trace.clear();
    recording->fields.clear();
    recording->card.clear();
    uint64_t field_tick = 0;
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\n")] = 0;
        InputStep input;
        unsigned frame = 0, buttons = 0;
        int consumed = 0;
        unsigned long long gap = 0;
        size_t repeat = 0;
        struct port_input_state *s = &input.state;
        if (sscanf(line, "dc2tk recording %d", &version) == 1) continue;
        if (sscanf(line, "start %31s", start) == 1) continue;
        if (sscanf(line, "frames %u", &recording->frames) == 1) continue;
        if (sscanf(line, "fields %llu %zu", &gap, &repeat) == 2) {
            for (size_t i = 0; i < repeat; ++i) recording->fields.push_back(field_tick += gap);
            continue;
        }
        if (sscanf(line, "input %u %x %d %d %d %d %d %d", &frame, &buttons, &s->left_x, &s->left_y, &s->right_x,
                   &s->right_y, &s->left_trigger, &s->right_trigger) == 8) {
            input.frame = frame;
            s->buttons = (uint16_t)buttons;
            recording->inputs.push_back(input);
            continue;
        }
        if (sscanf(line, "command %u %n", &frame, &consumed) == 1 && consumed) {
            CommandStep step;
            step.frame = frame;
            step.line = line + consumed;
            recording->commands.push_back(step);
        }
    }
    fclose(file);
    if (version != RECORDING_VERSION) return "the recording is from another version of the recorder";
    recording->start = !strcmp(start, "checkpoint") ? START_CHECKPOINT : START_LAUNCH;
    file_of(name, "trace.jsonl", path, sizeof path);
    file = fopen(path, "r");
    while (file && fgets(line, sizeof line, file)) {
        TracePoint trace;
        if (read_trace(line, &trace)) recording->trace.push_back(trace);
    }
    if (file) fclose(file);
    if (recording->start == START_CHECKPOINT) {
        std::vector<unsigned char> bytes;
        file_of(name, "checkpoint.bin", path, sizeof path);
        if (!read_file(path, &bytes) || bytes.size() != sizeof(Checkpoint)) return "checkpoint.bin is missing or the wrong size";
        memcpy(&recording->checkpoint, bytes.data(), sizeof(Checkpoint));
        if (memcmp(recording->checkpoint.magic, "dc2tkcp", 8) || recording->checkpoint.version != CHECKPOINT_VERSION)
            return "checkpoint.bin is from another version";
    }
    char folder[PATH_SIZE];
    folder_of(name, folder, sizeof folder);
    read_card(std::string(folder) + "/card", "", &recording->card);
    return NULL;
}

static void reset_compare(void) {
    worst_player = worst_camera = 0;
    first_apart = -1;
    play_input = play_command = 0;
    latched_frame = UINT32_MAX;
}

static const char *relaunch(const char *name, long pause) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    (void)name;
    (void)pause;
    return "a recording from launch plays in a new process, which iOS cannot start";
#else
    char folder[PATH_SIZE], frame[32];
    folder_of(name, folder, sizeof folder);
    snprintf(frame, sizeof frame, "%ld", pause >= 0 ? pause : (long)played.frames);
    SDL_setenv_unsafe("DC2TK_RECORDING", folder, 1);
    SDL_setenv_unsafe("DC2TK_RECORDING_PAUSE", frame, 1);
    SDL_setenv_unsafe("DC2TK_DEBUG_SPEED", "0", 1);
    relaunching = 1;
    if (debug_paused && debug_steps == 0) debug_steps = 1;
    note("restarting the game to play %s from launch", name);
    return NULL;
#endif
}

static const char *play(const char *name, long pause) {
    if (load_step != LOAD_NONE) return "a checkpoint is loading";
    const char *problem = read_recording(name, &played);
    if (problem) return problem;
    snprintf(played_name, sizeof played_name, "%s", name);
    if (played.start == START_LAUNCH) return relaunch(name, pause);
    pause_at = pause >= 0 ? pause : (long)played.frames;
    reset_compare();
    playing = PLAY_LOADING;
    begin_load(&played.checkpoint, played.card);
    note("loading the checkpoint of %s", name);
    return NULL;
}

static void stop_play(void) {
    if (!playing) return;
    load_step = LOAD_NONE;
    end_play();
    note("playback of %s stopped", played_name);
}

// A new folder in the system's temporary folder (TMPDIR, or TEMP on Windows)
static std::string temporary_folder(const char *prefix) {
    const char *temporary = SDL_getenv("TMPDIR");
    if (!temporary || !*temporary) temporary = SDL_getenv("TEMP");
    std::string parent = temporary && *temporary ? temporary : "/tmp";
    for (int attempt = 0; attempt < 100; ++attempt) {
        char name[64];
        snprintf(name, sizeof name, "/%s-%llx-%d", prefix, (unsigned long long)SDL_GetTicksNS(), attempt);
        std::string path = parent + name;
        if (!SDL_GetPathInfo(path.c_str(), NULL) && SDL_CreateDirectory(path.c_str())) return path;
    }
    return "";
}

static void restore_card(const std::vector<CardFile> &card) {
    std::string path = temporary_folder("dc2tk-card");
    if (path.empty()) return;
    for (const CardFile &file : card) {
        std::string target = path + "/" + file.path;
        make_folders(target.substr(0, target.rfind('/')).c_str());
        write_file(target.c_str(), file.bytes.data(), file.bytes.size());
    }
    SDL_setenv_unsafe("DC2_SAVE_ROOT", path.c_str(), 1);
}

static void apply_launch_environment(void) {
    const char *speed = getenv("DC2TK_DEBUG_SPEED");
    if (speed && *speed) {
        debug_speed = (float)atof(speed);
        port_field_period = debug_speed > 0.0f ? (uint64_t)(20000000.0 / debug_speed) : 0;
    }
    const char *paused = getenv("DC2TK_DEBUG_PAUSED");
    if (paused && *paused) debug_paused = 1;
}

void debug_recording_start(void) {
    const char *name = getenv("DC2TK_RECORDING");
    const char *pause = getenv("DC2TK_RECORDING_PAUSE");
    if (name && *name) {
        const char *problem = read_recording(name, &played);
        if (problem) note("cannot play %s: %s", name, problem);
        else {
            snprintf(played_name, sizeof played_name, "%s", name);
            pause_at = pause && *pause ? atol(pause) : (long)played.frames;
            reset_compare();
            restore_card(played.card);
            if (played.start == START_LAUNCH) {
                begin_play_running();
            } else {
                playing = PLAY_LOADING;
                begin_load(&played.checkpoint, played.card);
            }
        }
    }
    apply_launch_environment();
    begin_live(START_LAUNCH, NULL, card_now());
}

void debug_recording_hotkey_start(void) {
    const char *problem = start_here();
    if (problem) note("%s", problem);
}

void debug_recording_hotkey_save(void) {
    char name[NAME_SIZE];
    const char *problem = debug_recording_write("recording", name, sizeof name);
    if (problem) note("%s", problem);
}

static void describe(Entry *entry) {
    char path[PATH_SIZE], line[LINE_SIZE], start[32] = "";
    file_of(entry->name, "recording.txt", path, sizeof path);
    FILE *file = fopen(path, "r");
    entry->frames = 0;
    entry->start = START_LAUNCH;
    while (file && fgets(line, sizeof line, file)) {
        if (sscanf(line, "start %31s", start) == 1) entry->start = !strcmp(start, "checkpoint") ? START_CHECKPOINT : START_LAUNCH;
        if (sscanf(line, "frames %u", &entry->frames) == 1) break;
    }
    if (file) fclose(file);
    entry->loop = -1;
    file_of(entry->name, "trace.jsonl", path, sizeof path);
    file = fopen(path, "r");
    TracePoint trace;
    if (file && fgets(line, sizeof line, file) && read_trace(line, &trace)) entry->loop = trace.loop;
    if (file) fclose(file);
    file_of(entry->name, "recording.txt", path, sizeof path);
    SDL_PathInfo info;
    entry->taken = SDL_GetPathInfo(path, &info) ? (long long)(info.modify_time / 1000000000) : 0;
}

static void list_recordings(void) {
    int count = 0;
    char **names = SDL_GlobDirectory("build/port/recordings", "*", 0, &count);
    entry_count = 0;
    for (int i = 0; names && i < count && entry_count < LIST_LIMIT; ++i) {
        Entry entry;
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

static void delete_tree(const char *path) {
    DIR *directory = opendir(path);
    if (!directory) {
        unlink(path);
        return;
    }
    while (struct dirent *entry = readdir(directory)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char inner[PATH_SIZE];
        snprintf(inner, sizeof inner, "%s/%s", path, entry->d_name);
        delete_tree(inner);
    }
    closedir(directory);
    rmdir(path);
}

static void delete_recording(const char *name) {
    char folder[PATH_SIZE];
    folder_of(name, folder, sizeof folder);
    delete_tree(folder);
    listed_at = 0;
}

static const char *state_name(void) {
    if (load_step != LOAD_NONE) return playing ? "loading playback" : "loading checkpoint";
    if (playing == PLAY_RUNNING) return "playing";
    return live_lost[0] ? "lost" : "recording";
}

static void live_values(void) {
    DebugPoint point;
    debug_recording_sample(&point);
    ImGui::Text("player %.1f %.1f %.1f   camera %.1f %.1f %.1f   looking %.2f %.2f %.2f", point.player[0],
                point.player[1], point.player[2], point.camera[0], point.camera[1], point.camera[2], point.look[0],
                point.look[1], point.look[2]);
}

static void entry_row(int index) {
    Entry *entry = &entries[index];
    ImGui::PushID(index);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(entry->name);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(entry->start == START_CHECKPOINT ? "checkpoint" : "launch");
    ImGui::TableNextColumn();
    ImGui::Text("%u", entry->frames);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(place_name(entry));
    ImGui::TableNextColumn();
    char when[32] = "";
    time_t taken = (time_t)entry->taken;
    if (taken) strftime(when, sizeof when, "%d %b %H:%M", localtime(&taken));
    ImGui::TextUnformatted(when);
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(load_step != LOAD_NONE);
    const char *problem = NULL;
    if (ImGui::SmallButton("Play")) problem = play(entry->name, -1);
    if (entry->start == START_LAUNCH && ImGui::IsItemHovered()) ImGui::SetTooltip("restarts the game and plays from launch");
    ImGui::SameLine();
    if (ImGui::SmallButton("Play to")) problem = play(entry->name, pause_frames[index]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    int last = entry->frames ? (int)entry->frames - 1 : 0;
    ImGui::SliderInt("##frame", &pause_frames[index], 0, last, "frame %d");
    ImGui::SameLine();
    int confirming = !strcmp(deleting, entry->name);
    if (ImGui::SmallButton(confirming ? "Really delete?" : "Delete")) {
        if (confirming) {
            delete_recording(entry->name);
            deleting[0] = 0;
        } else snprintf(deleting, sizeof deleting, "%s", entry->name);
    }
    ImGui::EndDisabled();
    if (problem) note("%s", problem);
    ImGui::PopID();
}

void debug_recording_panel(void) {
    if (!listed_at || SDL_GetTicks() - listed_at > LIST_REFRESH_MS) list_recordings();
    ImGui::BeginDisabled(load_step != LOAD_NONE);
    if (ImGui::Button("Record from here (F8)")) debug_recording_hotkey_start();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##name", "name (blank for the time)", panel_name, sizeof panel_name);
    ImGui::SameLine();
    if (ImGui::Button("Save (F9)")) {
        if (panel_name[0]) {
            const char *problem = save_live(panel_name);
            if (problem) note("%s", problem);
        } else debug_recording_hotkey_save();
        panel_name[0] = 0;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(debug_paused ? "Resume (F5)" : "Pause (F5)")) debug_paused = !debug_paused;
    ImGui::SameLine();
    if (ImGui::Button("Step (F6)")) {
        debug_paused = 1;
        debug_steps = 1;
    }
    if (playing == PLAY_RUNNING) {
        uint32_t frame = debug_frame_total - play_base;
        ImGui::Text("playing %s: frame %u of %u   player apart %.2f, camera apart %.2f", played_name, frame,
                    played.frames, worst_player, worst_camera);
        ImGui::ProgressBar(played.frames ? (float)frame / (float)played.frames : 0.0f);
        if (ImGui::Button("Stop playback")) stop_play();
    } else if (load_step != LOAD_NONE) ImGui::Text("%s...", state_name());
    else if (live_lost[0]) ImGui::TextWrapped("not recording: %s; press F8 to record from here", live_lost);
    else
        ImGui::Text("recording from %s: %u frames (%.1f s)", live.start == START_CHECKPOINT ? "a checkpoint" : "launch",
                    live.frames, (double)live.frames / 60.0);
    if (message[0]) ImGui::TextWrapped("%s", message);
    live_values();
    debug_fault_panel();
    ImGui::Separator();
    if (!entry_count) {
        ImGui::TextUnformatted("No recordings yet. F8 records from here, F9 saves what has been recorded.");
        return;
    }
    if (!ImGui::BeginTable("recordings", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) return;
    ImGui::TableSetupColumn("recording", ImGuiTableColumnFlags_WidthFixed, 220);
    ImGui::TableSetupColumn("start", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("place", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("saved", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < entry_count; ++i) entry_row(i);
    ImGui::EndTable();
}

static void print_status(DebugReply *reply) {
    uint32_t frame = playing == PLAY_RUNNING ? debug_frame_total - play_base : 0;
    debug_print(reply, "{\"state\": \"%s\", \"start\": \"%s\", \"frames\": %u, \"playing\": ", state_name(),
                live.start == START_CHECKPOINT ? "checkpoint" : "launch", live.frames);
    debug_print_string(reply, playing ? played_name : "");
    debug_print(reply, ", \"play_frame\": %u, \"play_frames\": %u, \"player_apart\": %.3f, \"camera_apart\": %.3f, "
                       "\"first_apart\": %ld, \"lost\": ",
                frame, playing ? played.frames : 0, worst_player, worst_camera, first_apart);
    debug_print_string(reply, live_lost);
    debug_print(reply, ", \"message\": ");
    debug_print_string(reply, message);
    debug_print(reply, "}");
}

static int command_recording_start(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    const char *problem = start_here();
    if (problem) return debug_fail(reply, "%s", problem);
    print_status(reply);
    return 0;
}

static int command_recording_save(const DebugArgs *args, DebugReply *reply) {
    char name[NAME_SIZE];
    time_name("recording", name, sizeof name);
    const char *chosen = debug_arg(args, "name", name);
    const char *problem = save_live(chosen);
    if (problem) return debug_fail(reply, "%s", problem);
    debug_print(reply, "{\"saved\": ");
    debug_print_string(reply, chosen);
    debug_print(reply, ", \"frames\": %u, \"start\": \"%s\"}", live.frames,
                live.start == START_CHECKPOINT ? "checkpoint" : "launch");
    return 0;
}

static int command_recording_play(const DebugArgs *args, DebugReply *reply) {
    const char *name = debug_arg(args, "name", "");
    if (!*name) return debug_fail(reply, "name=recording is needed");
    const char *problem = play(name, debug_arg_int(args, "pause", -1));
    if (problem) return debug_fail(reply, "cannot play %s: %s", name, problem);
    print_status(reply);
    return 0;
}

static int command_recording_stop(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (!playing) return debug_fail(reply, "nothing is playing");
    stop_play();
    print_status(reply);
    return 0;
}

static int command_recording_status(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    print_status(reply);
    return 0;
}

static int command_recordings(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    list_recordings();
    debug_print(reply, "{\"recordings\": [");
    for (int i = 0; i < entry_count; ++i) {
        debug_print(reply, "%s{\"name\": ", i ? ", " : "");
        debug_print_string(reply, entries[i].name);
        debug_print(reply, ", \"start\": \"%s\", \"frames\": %u, \"place\": \"%s\", \"saved\": %lld}",
                    entries[i].start == START_CHECKPOINT ? "checkpoint" : "launch", entries[i].frames,
                    place_name(&entries[i]), entries[i].taken);
    }
    debug_print(reply, "]}");
    return 0;
}

static int command_recording_delete(const DebugArgs *args, DebugReply *reply) {
    const char *name = debug_arg(args, "name", "");
    if (!*name) return debug_fail(reply, "name=recording is needed");
    char path[PATH_SIZE];
    file_of(name, "recording.txt", path, sizeof path);
    if (access(path, F_OK)) return debug_fail(reply, "no recording %s", name);
    delete_recording(name);
    debug_print(reply, "{\"deleted\": ");
    debug_print_string(reply, name);
    debug_print(reply, "}");
    return 0;
}

extern const DebugCommand debug_recording_commands[] = {
    {"recording_start", command_recording_start, 0,
     "record from here: take a checkpoint (save data, place, player position, random and floor seeds), load it straight away "
     "and record from there (F8); refused in menus, events, minigames and map changes"},
    {"recording_save", command_recording_save, 0,
     "save what has been recorded since launch or the last recording_start to build/port/recordings/NAME, or a "
     "folder path (F9); name=N, default the time"},
    {"recording_play", command_recording_play, 0,
     "play a recording: a checkpoint one loads its checkpoint here, a launch one restarts the game; reports how far "
     "player and camera strayed from the recorded trace; name=N or a folder path [pause=frame, default the last]"},
    {"recording_stop", command_recording_stop, 0, "stop playback and give input back"},
    {"recording_status", command_recording_status, 1, "what is being recorded or played, and how far playback strayed"},
    {"recordings", command_recordings, 1, "the saved recordings in build/port/recordings"},
    {"recording_delete", command_recording_delete, 0, "delete a saved recording; name=N or a folder path"},
    {NULL, NULL, 0, NULL},
};
