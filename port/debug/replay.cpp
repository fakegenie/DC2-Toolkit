#include "debug.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef struct {
    uint32_t frame;
    struct port_input_state state;
} Step;

static FILE *recording;
static char record_path[256] = "build/port/replay.txt";
static uint32_t record_base;
static struct port_input_state record_last;

static Step *steps;
static int step_count;
static int step_at;
static int playing;
static char play_path[256];
static uint32_t play_base;
static uint32_t play_end;
static int play_client = DEBUG_CLIENT_NONE;
static int play_waiting;

static uint32_t latched_frame = UINT32_MAX;
static struct port_input_state latched;
static int environment_read;

static char panel_path[256] = "build/port/replay.txt";
static int panel_action;

enum { ACTION_NONE, ACTION_RECORD, ACTION_RECORD_STOP, ACTION_PLAY, ACTION_PLAY_STOP };

extern "C" uint64_t port_field_period;

static int same_state(const struct port_input_state *a, const struct port_input_state *b) {
    return !memcmp(a, b, sizeof *a);
}

static int start_record(const char *path) {
    if (recording) fclose(recording);
    recording = fopen(path, "w");
    if (!recording) return 0;
    snprintf(record_path, sizeof record_path, "%s", path);
    record_base = debug_frame_total;
    memset(&record_last, 0, sizeof record_last);
    latched_frame = UINT32_MAX;
    return 1;
}

static uint32_t stop_record(void) {
    if (!recording) return 0;
    uint32_t frames = debug_frame_total - record_base;
    fprintf(recording, "end %u\n", frames);
    fclose(recording);
    recording = NULL;
    return frames;
}

static void write_step(uint32_t frame, const struct port_input_state *s) {
    fprintf(recording, "%u 0x%04x %d %d %d %d %d %d\n", frame, s->buttons, s->left_x, s->left_y, s->right_x,
            s->right_y, s->left_trigger, s->right_trigger);
    fflush(recording);
}

static int load_steps(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    free(steps);
    steps = NULL;
    step_count = 0;
    play_end = 0;
    int capacity = 0;
    char line[256];
    while (fgets(line, sizeof line, file)) {
        unsigned end = 0;
        if (sscanf(line, "end %u", &end) == 1) {
            play_end = end;
            continue;
        }
        Step step;
        memset(&step, 0, sizeof step);
        unsigned buttons = 0;
        if (sscanf(line, "%u %x %d %d %d %d %d %d", &step.frame, &buttons, &step.state.left_x, &step.state.left_y,
                   &step.state.right_x, &step.state.right_y, &step.state.left_trigger,
                   &step.state.right_trigger) < 2)
            continue;
        step.state.buttons = (uint16_t)buttons;
        if (step_count == capacity) {
            capacity = capacity ? capacity * 2 : 256;
            steps = (Step *)realloc(steps, (size_t)capacity * sizeof *steps);
        }
        steps[step_count++] = step;
        if (!play_end || step.frame >= play_end) play_end = step.frame + 1;
    }
    fclose(file);
    return 1;
}

static int start_play(const char *path) {
    if (!load_steps(path)) return 0;
    snprintf(play_path, sizeof play_path, "%s", path);
    step_at = 0;
    play_base = debug_frame_total;
    playing = 1;
    latched_frame = UINT32_MAX;
    return 1;
}

static void report_finished(void) {
    if (!play_waiting) return;
    DebugReply reply;
    memset(&reply, 0, sizeof reply);
    reply.client = play_client;
    reply.status = 200;
    debug_print(&reply, "{\"finished\": 1, \"frame\": %u}", debug_frame_total);
    debug_finish(&reply);
    play_waiting = 0;
    play_client = DEBUG_CLIENT_NONE;
}

static void stop_play(void) {
    playing = 0;
    latched_frame = UINT32_MAX;
    report_finished();
}

static void start_from_environment(void) {
    if (environment_read) return;
    environment_read = 1;
    const char *record = getenv("DC2TK_RECORD_INPUT");
    const char *replay = getenv("DC2TK_REPLAY_INPUT");
    if (record && *record && !start_record(record)) fprintf(stderr, "replay: cannot write %s\n", record);
    if (replay && *replay && !start_play(replay)) fprintf(stderr, "replay: cannot read %s\n", replay);
}

static int apply_launch_environment(void) {
    const char *speed = getenv("DC2TK_DEBUG_SPEED");
    const char *paused = getenv("DC2TK_DEBUG_PAUSED");
    if (speed && *speed) {
        double value = atof(speed);
        debug_speed = (float)value;
        port_field_period = value > 0.0 ? (uint64_t)(20000000.0 / value) : 0;
    }
    if (paused && *paused) debug_paused = 1;
    return 1;
}

static int launch_environment_applied = apply_launch_environment();

static struct port_input_state state_at(uint32_t frame) {
    while (step_at < step_count && steps[step_at].frame <= frame) step_at++;
    struct port_input_state state;
    memset(&state, 0, sizeof state);
    if (step_at > 0) state = steps[step_at - 1].state;
    return state;
}

void debug_replay_input(struct port_input_state *state) {
    start_from_environment();
    if (!playing && !recording) return;
    if (debug_frame_total != latched_frame) {
        latched_frame = debug_frame_total;
        if (playing) latched = state_at(debug_frame_total - play_base);
        else latched = *state;
        if (recording && !same_state(&latched, &record_last)) {
            write_step(debug_frame_total - record_base, &latched);
            record_last = latched;
        }
    }
    *state = latched;
}

void debug_replay_frame(void) {
    start_from_environment();
    if (panel_action == ACTION_RECORD) start_record(panel_path);
    if (panel_action == ACTION_RECORD_STOP) stop_record();
    if (panel_action == ACTION_PLAY) start_play(panel_path);
    if (panel_action == ACTION_PLAY_STOP) stop_play();
    panel_action = ACTION_NONE;
    if (playing && debug_frame_total - play_base >= play_end) stop_play();
}

static int command_record(const DebugArgs *args, DebugReply *reply) {
    const char *path = debug_arg(args, "path", record_path);
    if (!start_record(path)) return debug_fail(reply, "cannot write %s", path);
    debug_print(reply, "{\"recording\": 1, \"base\": %u}", record_base);
    return 0;
}

static int command_record_stop(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (!recording) return debug_fail(reply, "not recording");
    uint32_t frames = stop_record();
    debug_print(reply, "{\"recording\": 0, \"frames\": %u}", frames);
    return 0;
}

static int command_replay(const DebugArgs *args, DebugReply *reply) {
    const char *path = debug_arg(args, "path", panel_path);
    if (!start_play(path)) return debug_fail(reply, "cannot read %s", path);
    if (debug_arg_int(args, "wait", 0) && reply->client >= 0) {
        play_client = reply->client;
        play_waiting = 1;
        reply->deferred = 1;
        return 0;
    }
    debug_print(reply, "{\"playing\": 1, \"frames\": %u, \"base\": %u}", play_end, play_base);
    return 0;
}

static int command_replay_stop(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (!playing) return debug_fail(reply, "not playing");
    stop_play();
    debug_print(reply, "{\"playing\": 0}");
    return 0;
}

static int command_replay_status(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_print(reply, "{\"recording\": %d, \"record_frames\": %u, \"playing\": %d, \"play_frame\": %u, "
                       "\"play_frames\": %u, \"frame\": %u}",
                recording ? 1 : 0, recording ? debug_frame_total - record_base : 0, playing,
                playing ? debug_frame_total - play_base : 0, playing ? play_end : 0, debug_frame_total);
    return 0;
}

extern const DebugCommand debug_replay_commands[] = {
    {"record", command_record, 0, "start recording input to path=file; frames count from this frame"},
    {"record_stop", command_record_stop, 0, "stop recording and write the end frame"},
    {"replay", command_replay, 0, "play path=file from this frame, live input ignored; wait=1 replies when it ends"},
    {"replay_stop", command_replay_stop, 0, "stop playback and give input back"},
    {"replay_status", command_replay_status, 1, "recording and playback state and progress"},
    {NULL, NULL, 0, NULL},
};

void debug_replay_panel(void) {
    ImGui::InputText("file", panel_path, sizeof panel_path);
    if (ImGui::Button(recording ? "Stop recording" : "Record")) panel_action = recording ? ACTION_RECORD_STOP : ACTION_RECORD;
    ImGui::SameLine();
    if (ImGui::Button(playing ? "Stop playback" : "Play")) panel_action = playing ? ACTION_PLAY_STOP : ACTION_PLAY;
    if (recording) ImGui::Text("recording %s: %u frames", record_path, debug_frame_total - record_base);
    if (playing) {
        uint32_t at = debug_frame_total - play_base;
        ImGui::Text("playing %s: %u of %u frames", play_path, at, play_end);
        ImGui::ProgressBar(play_end ? (float)at / (float)play_end : 0.0f);
        ImGui::TextUnformatted("live input is ignored");
    }
    if (!recording && !playing) ImGui::TextUnformatted("idle");
}
