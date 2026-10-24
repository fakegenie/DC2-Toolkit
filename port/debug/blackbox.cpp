#include "debug.h"
#include "imgui.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

enum { BOX_STATES = 7, BOX_EVERY = 300, BOX_FRAMES = (BOX_STATES + 1) * BOX_EVERY, BOX_NAME = 64, BOX_PATH = 256,
       BOX_LINE = DEBUG_TRACE + 32 };

typedef struct {
    PortState state;
    uint32_t frame;
    int used;
} BoxState;

typedef struct {
    uint32_t tag;
    struct port_input_state input;
} BoxInput;

typedef struct {
    uint32_t tag;
    char text[DEBUG_TRACE];
} BoxTrace;

static BoxState states[BOX_STATES];
static BoxInput inputs[BOX_FRAMES];
static BoxTrace traces[BOX_FRAMES];
static int keeping = -1;
static int saving = -1;
static int build_kept;
static float save_ms;
static char box_message[512];

static int box_on(void) {
    if (keeping < 0) {
        const char *text = getenv("DC2TK_BLACKBOX");
        keeping = !(text && !strcmp(text, "0"));
    }
    return keeping;
}

static int newest_state(void) {
    int newest = -1;
    for (int i = 0; i < BOX_STATES; ++i)
        if (states[i].used && (newest < 0 || states[i].frame > states[newest].frame)) newest = i;
    return newest;
}

static int oldest_state(uint32_t now) {
    int oldest = -1;
    for (int i = 0; i < BOX_STATES; ++i)
        if (states[i].used && now - states[i].frame < BOX_FRAMES && (oldest < 0 || states[i].frame < states[oldest].frame))
            oldest = i;
    return oldest;
}

static int free_state(void) {
    for (int i = 0; i < BOX_STATES; ++i)
        if (!states[i].used) return i;
    int oldest = 0;
    for (int i = 1; i < BOX_STATES; ++i)
        if (states[i].frame < states[oldest].frame) oldest = i;
    return oldest;
}

static size_t kept_bytes(void) {
    size_t bytes = 0;
    for (const BoxState &box : states) bytes += box.state.room;
    return bytes;
}

static int used_states(void) {
    int used = 0;
    for (const BoxState &box : states) used += box.used;
    return used;
}

static uint32_t kept_frames(void) {
    int oldest = oldest_state(debug_frame_total);
    return oldest < 0 ? 0 : debug_frame_total - states[oldest].frame;
}

void debug_blackbox_clear(void) {
    for (BoxState &box : states) box.used = 0;
}

static void release(void) {
    for (BoxState &box : states) {
        free(box.state.data);
        memset(&box, 0, sizeof box);
    }
}

void debug_blackbox_input(const struct port_input_state *state) {
    BoxInput *entry = &inputs[debug_frame_total % BOX_FRAMES];
    entry->tag = debug_frame_total + 1;
    entry->input = *state;
}

void debug_blackbox_frame(void) {
    if (!box_on()) return;
    BoxTrace *entry = &traces[debug_frame_total % BOX_FRAMES];
    debug_clip_trace(entry->text, sizeof entry->text);
    entry->tag = debug_frame_total + 1;
    int newest = newest_state();
    if (saving >= 0 || (newest >= 0 && debug_frame_total - states[newest].frame < BOX_EVERY)) return;
    int index = free_state();
    if (debug_savestate_into(&states[index].state)) saving = index;
}

void debug_blackbox_saved(int saved) {
    if (saving < 0) return;
    if (saved) {
        states[saving].frame = debug_frame_total;
        states[saving].used = 1;
        save_ms = port_state_ms();
    }
    saving = -1;
    if (saved && !build_kept) {
        const char *problem = port_state_keep();
        if (problem) fprintf(stderr, "blackbox: %s\n", problem);
        build_kept = 1;
    }
}

static int write_all(int fd, const void *data, size_t size) {
    const unsigned char *at = (const unsigned char *)data;
    while (size) {
        ssize_t written = write(fd, at, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        at += written;
        size -= (size_t)written;
    }
    return 1;
}

static int open_file(const char *name, const char *file) {
    char path[BOX_PATH];
    snprintf(path, sizeof path, "build/port/clips/%s/%s", name, file);
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

static int write_state(const char *name, const BoxState *box) {
    int fd = open_file(name, "start.state");
    int ok = fd >= 0 && write_all(fd, box->state.data, box->state.size);
    return (fd < 0 || !close(fd)) && ok;
}

static int write_trace(const char *name, uint32_t from, uint32_t to) {
    int fd = open_file(name, "trace.jsonl");
    int ok = fd >= 0;
    char line[BOX_LINE];
    for (uint32_t frame = from; ok && frame <= to; ++frame) {
        const BoxTrace *entry = &traces[frame % BOX_FRAMES];
        if (entry->tag != frame + 1) continue;
        int length = snprintf(line, sizeof line, "{\"frame\": %u, %s\n", frame - from, entry->text);
        ok = write_all(fd, line, (size_t)length);
    }
    return (fd < 0 || !close(fd)) && ok;
}

static int write_input(const char *name, uint32_t from, uint32_t to) {
    int fd = open_file(name, "input.txt");
    int ok = fd >= 0;
    char line[BOX_LINE];
    struct port_input_state last;
    memset(&last, 0, sizeof last);
    for (uint32_t frame = from; ok && frame <= to; ++frame) {
        const BoxInput *entry = &inputs[frame % BOX_FRAMES];
        if (entry->tag != frame + 1 || !memcmp(&entry->input, &last, sizeof last)) continue;
        const struct port_input_state *s = &entry->input;
        int length = snprintf(line, sizeof line, "%u 0x%04x %d %d %d %d %d %d\n", frame - from, s->buttons, s->left_x,
                              s->left_y, s->right_x, s->right_y, s->left_trigger, s->right_trigger);
        ok = write_all(fd, line, (size_t)length);
        last = *s;
    }
    int length = snprintf(line, sizeof line, "end %u\n", to - from + 1);
    ok = ok && write_all(fd, line, (size_t)length);
    return (fd < 0 || !close(fd)) && ok;
}

static const char *write_box(const char *name, uint32_t *frames) {
    uint32_t to = debug_frame_total;
    int oldest = oldest_state(to);
    if (oldest < 0) return "the black box has no state yet";
    char path[BOX_PATH];
    mkdir("build/port/clips", 0755);
    snprintf(path, sizeof path, "build/port/clips/%s", name);
    mkdir(path, 0755);
    uint32_t from = states[oldest].frame;
    if (!write_state(name, &states[oldest]) || !write_input(name, from, to) || !write_trace(name, from, to))
        return "the black box could not write its clip";
    *frames = to - from;
    return NULL;
}

static void box_name(const char *prefix, char *name, size_t size) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char when[32];
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", &local);
    snprintf(name, size, "%s-%s", prefix, when);
}

void debug_blackbox_crash(void) {
    if (!box_on() || saving >= 0) return;
    char name[BOX_NAME], line[BOX_PATH];
    box_name("crash", name, sizeof name);
    uint32_t frames = 0;
    const char *problem = write_box(name, &frames);
    int length = problem ? snprintf(line, sizeof line, "blackbox: %s\n", problem)
                         : snprintf(line, sizeof line, "blackbox: wrote clip %s, %u frames up to the crash\n", name, frames);
    write_all(2, line, (size_t)length);
}

static const char *write_named(const char *name) {
    uint32_t frames = 0;
    const char *problem = write_box(name, &frames);
    if (problem) snprintf(box_message, sizeof box_message, "%s", problem);
    else snprintf(box_message, sizeof box_message, "wrote clip %s, the last %.1f s", name, (double)frames / 60.0);
    fprintf(stderr, "blackbox: %s\n", box_message);
    return problem;
}

void debug_blackbox_hotkey(void) {
    char name[BOX_NAME];
    box_name("box", name, sizeof name);
    write_named(name);
}

static void set_on(int on) {
    keeping = on;
    if (!on) release();
}

void debug_blackbox_panel(void) {
    bool on = box_on();
    if (ImGui::Checkbox("Black box", &on)) set_on(on);
    ImGui::SameLine();
    if (!on) {
        ImGui::TextDisabled("off");
        return;
    }
    ImGui::Text("%.1f s kept in %.0f MB, last state %.1f ms", (double)kept_frames() / 60.0,
                (double)kept_bytes() / 1048576.0, save_ms);
    ImGui::SameLine();
    if (ImGui::Button("Write (F9)")) debug_blackbox_hotkey();
    if (box_message[0]) ImGui::TextWrapped("%s", box_message);
}

static void print_status(DebugReply *reply) {
    debug_print(reply, "{\"on\": %d, \"frames\": %u, \"seconds\": %.1f, \"states\": %d, \"bytes\": %zu, \"save_ms\": %.2f, "
                       "\"message\": ",
                box_on(), kept_frames(), (double)kept_frames() / 60.0, used_states(), kept_bytes(),
                save_ms);
    debug_print_string(reply, box_message);
    debug_print(reply, "}");
}

static int command_blackbox(const DebugArgs *args, DebugReply *reply) {
    if (debug_arg(args, "on", NULL)) {
        set_on(debug_arg_int(args, "on", 1) != 0);
        print_status(reply);
        return 0;
    }
    char name[BOX_NAME];
    box_name("box", name, sizeof name);
    const char *problem = write_named(debug_arg(args, "name", name));
    if (problem) return debug_fail(reply, "%s", problem);
    print_status(reply);
    return 0;
}

static int command_blackbox_status(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    print_status(reply);
    return 0;
}

extern const DebugCommand debug_blackbox_commands[] = {
    {"blackbox", command_blackbox, 0,
     "write the black box (a savestate every 5 s and the input since) as a clip of the last 30 s or more; name=N; "
     "on=0 stops keeping it and frees its memory, on=1 starts again; a crash writes one by itself, F9 writes one"},
    {"blackbox_status", command_blackbox_status, 1, "how much the black box holds"},
    {NULL, NULL, 0, NULL},
};
