#include "debug.h"
#include "imgui.h"
#include "port_state.h"
#include "render.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

enum { SLOT_LIMIT = 8, THUMB_WIDTH = STATE_THUMB_WIDTH, THUMB_HEIGHT = STATE_THUMB_HEIGHT, SAVE = STATE_SAVE,
       LOAD = STATE_LOAD };

typedef PortStateHeader Header;

typedef struct {
    PortState state;
    float save_ms;
    float load_ms;
    int loads;
    int texture;
    int shown;
} Slot;

typedef struct {
    int kind;
    int slot;
    int client;
    char path[512];
} Request;

static const char *const loop_names[] = {"menu", "town", "dungeon", "title", "character viewer", "texture viewer",
                                         "map viewer", "sound viewer", "movie viewer", "save conversion"};

static Slot slots[SLOT_LIMIT];
static Request request;
static char message[512];
static uint32_t frame_seen = UINT32_MAX;
static int pause_hook;
static char path_text[256] = "slot0.dc2state";

static const char *loop_name(int loop) {
    return loop >= 0 && loop < (int)(sizeof loop_names / sizeof loop_names[0]) ? loop_names[loop] : "unknown";
}

static const Header *slot_header(const Slot *slot) { return port_state_header(&slot->state); }

extern "C" void debug_host(void) {}

static void show_thumbnail(Slot *slot) {
    if (!slot->state.data || slot->shown) return;
    if (!slot->texture) slot->texture = render_texture(THUMB_WIDTH, THUMB_HEIGHT);
    render_upload(slot->texture, 0, 0, THUMB_WIDTH, THUMB_HEIGHT, port_state_thumbnail(&slot->state));
    slot->shown = 1;
}

static void state_path(const char *name, char *path, size_t size) {
    if (strchr(name, '/')) snprintf(path, size, "%s", name);
    else snprintf(path, size, "build/port/savestates/%s", name);
}

static const char *write_state(const Slot *slot, const char *name) {
    char path[512];
    state_path(name, path, sizeof path);
    mkdir("build/port/savestates", 0755);
    return port_state_write(&slot->state, path);
}

static const char *read_state(Slot *slot, const char *name) {
    char path[512];
    state_path(name, path, sizeof path);
    const char *problem = port_state_read(&slot->state, path);
    if (problem) return problem;
    slot->save_ms = 0;
    slot->load_ms = 0;
    slot->loads = 0;
    slot->shown = 0;
    return NULL;
}

static void print_slot(DebugReply *reply, int index) {
    const Slot *slot = &slots[index];
    const Header *header = slot_header(slot);
    debug_print(reply, "{\"slot\": %d, \"used\": %d", index, header != NULL);
    if (header)
        debug_print(reply, ", \"frame\": %u, \"field\": %llu, \"loop\": %d, \"scene\": \"%s\", \"map\": %d, \"taken\": %lld, "
                           "\"bytes\": %zu, \"save_ms\": %.2f, \"load_ms\": %.2f, \"loads\": %d",
                    header->frame, (unsigned long long)header->field, header->loop, loop_name(header->loop), header->map,
                    (long long)header->taken, slot->state.size, slot->save_ms, slot->load_ms, slot->loads);
    debug_print(reply, "}");
}

static void finish_box(void) {
    request.kind = 0;
    debug_blackbox_saved(!port_state_problem());
}

static void finish_request(void) {
    if (request.slot < 0) {
        finish_box();
        return;
    }
    Slot *slot = &slots[request.slot];
    float ms = port_state_ms();
    DebugReply reply;
    memset(&reply, 0, sizeof reply);
    reply.client = request.client;
    reply.status = 200;
    const char *problem = port_state_problem();
    if (!problem && request.kind == SAVE) {
        ((Header *)slot->state.data)->frame = debug_frame_total;
        slot->save_ms = ms;
        slot->loads = 0;
        slot->load_ms = 0;
        slot->shown = 0;
        show_thumbnail(slot);
        if (request.path[0]) problem = write_state(slot, request.path);
    }
    if (!problem && request.kind == LOAD) {
        slot->load_ms = ms;
        slot->loads++;
        show_thumbnail(slot);
    }
    if (problem) {
        debug_fail(&reply, "%s", problem);
        snprintf(message, sizeof message, "%s slot %d failed: %s", request.kind == SAVE ? "save" : "load", request.slot,
                 problem);
    } else {
        debug_print(&reply, "{\"%s\": ", request.kind == SAVE ? "saved" : "loaded");
        print_slot(&reply, request.slot);
        debug_print(&reply, ", \"ms\": %.2f, \"at_frame\": %u", ms, debug_frame_total);
        if (request.kind == LOAD && port_state_note()) {
            debug_print(&reply, ", \"moved\": ");
            debug_print_string(&reply, port_state_note());
        }
        debug_print(&reply, "}");
        snprintf(message, sizeof message, "%s slot %d in %.1f ms (%.1f MB)", request.kind == SAVE ? "saved" : "loaded",
                 request.slot, ms, (double)slot->state.size / 1048576.0);
    }
    request.kind = 0;
    debug_finish(&reply);
}

STATE_RESUMABLE void debug_savestate_frame(void) {
    if (debug_frame_total == frame_seen) pause_hook = 1;
    frame_seen = debug_frame_total;
    if (!request.kind) return;
    if (port_state_pending()) port_state_frame();
    if (!port_state_pending()) finish_request();
}

static int request_state(const DebugArgs *args, DebugReply *reply, int kind) {
    long slot = debug_arg_int(args, "slot", 0);
    const char *path = debug_arg(args, "path", "");
    if (slot < 0 || slot >= SLOT_LIMIT) return debug_fail(reply, "slot must be 0 to %d", SLOT_LIMIT - 1);
    if (request.kind) return debug_fail(reply, "a savestate is already waiting for the end of the frame");
    if (port_state_ready()) return debug_fail(reply, "%s", port_state_ready());
    if (strlen(path) >= sizeof request.path) return debug_fail(reply, "path is too long");
    if (kind == LOAD && *path) {
        const char *problem = read_state(&slots[slot], path);
        if (problem) return debug_fail(reply, "%s", problem);
    }
    if (kind == LOAD && !slots[slot].state.data) return debug_fail(reply, "slot %ld is empty", slot);
    port_state_force(kind == LOAD && debug_arg_int(args, "force", 0));
    if (!port_state_request(kind, &slots[slot].state)) return debug_fail(reply, "%s", port_state_problem());
    if (kind == LOAD) debug_blackbox_clear();
    request.kind = kind;
    request.slot = (int)slot;
    request.client = reply->client;
    snprintf(request.path, sizeof request.path, "%s", kind == SAVE ? path : "");
    if (debug_paused && debug_steps == 0 && !pause_hook) debug_steps = 1;
    reply->deferred = 1;
    return 0;
}

int debug_savestate_into(PortState *state) {
    if (request.kind || port_state_ready() || !port_state_request(SAVE, state)) return 0;
    request.kind = SAVE;
    request.slot = -1;
    request.client = DEBUG_CLIENT_NONE;
    request.path[0] = 0;
    return 1;
}

static int command_save(const DebugArgs *args, DebugReply *reply) {
    return request_state(args, reply, SAVE);
}

static int command_load(const DebugArgs *args, DebugReply *reply) {
    return request_state(args, reply, LOAD);
}

static int command_savestates(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_print(reply, "{\"pending\": %d, \"slots\": [", request.kind);
    for (int i = 0; i < SLOT_LIMIT; ++i) {
        if (i) debug_print(reply, ", ");
        print_slot(reply, i);
    }
    debug_print(reply, "], \"message\": ");
    debug_print_string(reply, message);
    debug_print(reply, "}");
    return 0;
}

extern const DebugCommand debug_savestate_commands[] = {
    {"save", command_save, 1, "save the whole game into slot=N (0-7) at the end of the frame; path=name also writes "
                              "build/port/savestates/name"},
    {"load", command_load, 1, "load slot=N at the end of the frame; path=name first reads build/port/savestates/name; a state "
                              "from another kept build is moved into this one, and force=1 loads it even when code "
                              "or data it uses changed size"},
    {"savestates", command_savestates, 1, "list the savestate slots"},
    {NULL, NULL, 0, NULL},
};

static void describe_age(char *text, size_t size, int64_t taken) {
    long seconds = (long)(time(NULL) - taken);
    if (seconds < 60) snprintf(text, size, "%lds ago", seconds);
    else if (seconds < 3600) snprintf(text, size, "%ldm ago", seconds / 60);
    else snprintf(text, size, "%ldh ago", seconds / 3600);
}

void debug_savestate_panel(void) {
    if (port_state_ready()) {
        ImGui::TextWrapped("savestates are unavailable: %s", port_state_ready());
        return;
    }
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("file", path_text, sizeof path_text);
    ImGui::SameLine();
    ImGui::TextDisabled("under build/port/savestates");
    if (request.kind) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s slot %d at the end of the frame",
                                         request.kind == SAVE ? "saving" : "loading", request.slot);
    else ImGui::TextUnformatted(message[0] ? message : "no savestate taken yet");
    if (!ImGui::BeginTable("savestates", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) return;
    ImGui::TableSetupColumn("frame", ImGuiTableColumnFlags_WidthFixed, (float)THUMB_WIDTH + 8);
    ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableHeadersRow();
    for (int i = 0; i < SLOT_LIMIT; ++i) {
        Slot *slot = &slots[i];
        const Header *header = slot_header(slot);
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        SDL_GPUTexture *texture = slot->shown ? render_gpu_texture(slot->texture) : NULL;
        if (texture) ImGui::Image((ImTextureID)(uintptr_t)texture, ImVec2(THUMB_WIDTH, THUMB_HEIGHT));
        else ImGui::Dummy(ImVec2(THUMB_WIDTH, header ? THUMB_HEIGHT : 24));
        ImGui::TableNextColumn();
        ImGui::Text("slot %d", i);
        if (header) {
            char age[32];
            describe_age(age, sizeof age, header->taken);
            ImGui::Text("frame %u, field %llu", header->frame, (unsigned long long)header->field);
            ImGui::Text("%s, map %d", loop_name(header->loop), header->map);
            ImGui::Text("%s, %.1f MB", age, (double)slot->state.size / 1048576.0);
            ImGui::Text("save %.1f ms, load %.1f ms, %d loads", slot->save_ms, slot->load_ms, slot->loads);
        } else {
            ImGui::TextDisabled("empty");
        }
        ImGui::TableNextColumn();
        char line[320];
        if (ImGui::Button("Save", ImVec2(70, 0))) {
            snprintf(line, sizeof line, "save?slot=%d", i);
            debug_run(line);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!header);
        if (ImGui::Button("Load", ImVec2(70, 0))) {
            snprintf(line, sizeof line, "load?slot=%d", i);
            debug_run(line);
        }
        if (ImGui::Button("Write", ImVec2(70, 0))) {
            const char *problem = write_state(slot, path_text);
            snprintf(message, sizeof message, "%s", problem ? problem : "written");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Read", ImVec2(70, 0))) {
            snprintf(line, sizeof line, "load?slot=%d&path=%s", i, path_text);
            debug_run(line);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}
