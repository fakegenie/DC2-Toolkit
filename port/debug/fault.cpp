#include "debug.h"
#include "imgui.h"
#include "mainloop.hpp"
#include "mglib.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

enum { FAULT_TEXT = 256, FAULT_NAME = 64 };

static const float FAULT_FAR = 1.0e6f;

static int watching = -1;
static int pausing = -1;
static int armed = 1;
static uint32_t fault_count;
static uint32_t fault_frame;
static char fault_what[FAULT_TEXT];
static char fault_clip[FAULT_NAME];

static int setting(const char *name) {
    const char *text = getenv(name);
    return !(text && !strcmp(text, "0"));
}

static int fault_on(void) {
    if (watching < 0) watching = setting("DC2TK_FAULT_WATCH");
    if (pausing < 0) pausing = setting("DC2TK_FAULT_PAUSE");
    return watching;
}

static int bad_values(const char *name, const float *values, int count, float limit, char *what, size_t size) {
    for (int i = 0; i < count; ++i) {
        if (std::isfinite(values[i]) && fabsf(values[i]) <= limit) continue;
        snprintf(what, size, "%s[%d] is %g", name, i, (double)values[i]);
        return 1;
    }
    return 0;
}

static int find_fault(char *what, size_t size) {
    DebugPoint point;
    debug_clip_sample(&point);
    const mgRENDER_INFO *info = &mgRenderInfo;
    return bad_values("player", point.player, 3, FAULT_FAR, what, size) ||
           bad_values("camera", info->camera_pos, 3, FAULT_FAR, what, size) ||
           bad_values("camera_pose", &info->camera_pose[0][0], 16, INFINITY, what, size) ||
           bad_values("view", &info->view[0][0], 16, INFINITY, what, size) ||
           bad_values("world_screen", &info->world_screen[0][0], 16, INFINITY, what, size) ||
           bad_values("world_clip", &info->world_clip[0][0], 16, INFINITY, what, size) ||
           bad_values("projection", &info->projection, 1, INFINITY, what, size);
}

void debug_fault_frame(void) {
    if (!fault_on()) return;
    char what[FAULT_TEXT];
    if (!find_fault(what, sizeof what)) {
        armed = 1;
        return;
    }
    if (!armed) return;
    armed = 0;
    fault_count++;
    fault_frame = debug_frame_total;
    snprintf(fault_what, sizeof fault_what, "%s", what);
    const char *problem = debug_blackbox_write("fault", fault_clip, sizeof fault_clip);
    if (problem) fault_clip[0] = 0;
    fprintf(stderr, "fault: frame %u, %s, loop %d; %s%s%s\n", fault_frame, fault_what, GetNowLoopNo(),
            problem ? problem : "clip ", problem ? "" : fault_clip, pausing ? "; paused" : "");
    if (!pausing) return;
    debug_paused = 1;
    debug_steps = 0;
}

void debug_fault_panel(void) {
    bool on = fault_on(), pause = pausing;
    if (ImGui::Checkbox("Fault watch", &on)) watching = on;
    ImGui::SameLine();
    if (ImGui::Checkbox("pause on a fault", &pause)) pausing = pause;
    ImGui::SameLine();
    ImGui::TextDisabled("NaN or far-off player and camera");
    if (fault_count)
        ImGui::TextWrapped("%u faults; last at frame %u: %s%s%s", fault_count, fault_frame, fault_what,
                           fault_clip[0] ? ", clip " : "", fault_clip);
}

static int command_faults(const DebugArgs *args, DebugReply *reply) {
    fault_on();
    if (debug_arg(args, "on", NULL)) watching = debug_arg_int(args, "on", 1) != 0;
    if (debug_arg(args, "pause", NULL)) pausing = debug_arg_int(args, "pause", 1) != 0;
    debug_print(reply, "{\"on\": %d, \"pause\": %d, \"count\": %u, \"frame\": %u, \"what\": ", watching, pausing,
                fault_count, fault_frame);
    debug_print_string(reply, fault_what);
    debug_print(reply, ", \"clip\": ");
    debug_print_string(reply, fault_clip);
    debug_print(reply, "}");
    return 0;
}

extern const DebugCommand debug_fault_commands[] = {
    {"faults", command_faults, 1,
     "the fault watch: every frame the player, camera and camera matrices are checked for NaN, infinity or a "
     "position past a million; the first bad frame is logged, written as a fault-<time> clip from the black box "
     "and paused; on=0|1, pause=0|1"},
    {NULL, NULL, 0, NULL},
};
