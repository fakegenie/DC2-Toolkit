#include "debug.h"
#include "port_mod.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "port_graphics.h"
#include "render.h"
#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

extern int DebugFlag;

typedef struct {
    const char *name;
    size_t offset;
} Counter;

static const Counter counters[] = {
    {"draw calls", offsetof(PortStats, draw_calls)},
    {"mesh calls", offsetof(PortStats, mesh_calls)},
    {"GPU commands", offsetof(PortStats, gpu_commands)},
    {"render passes", offsetof(PortStats, render_passes)},
    {"pipeline changes", offsetof(PortStats, pipeline_changes)},
    {"vertices", offsetof(PortStats, vertices)},
    {"texture uploads", offsetof(PortStats, texture_uploads)},
    {"upload bytes", offsetof(PortStats, upload_bytes)},
    {"downloads", offsetof(PortStats, downloads)},
    {"texture decodes", offsetof(PortStats, texture_decodes)},
    {"target creates", offsetof(PortStats, target_creates)},
    {"mesh builds", offsetof(PortStats, mesh_builds)},
    {"sorted draws", offsetof(PortStats, sorted_draws)},
    {"allocations", offsetof(PortStats, allocations)},
    {"frees", offsetof(PortStats, frees)},
    {"allocation bytes", offsetof(PortStats, allocation_bytes)},
    {"disc reads", offsetof(PortStats, disc_reads)},
    {"disc bytes", offsetof(PortStats, disc_bytes)},
    {"textures live", offsetof(PortStats, textures_live)},
    {"pipelines", offsetof(PortStats, pipelines)},
    {"cached textures", offsetof(PortStats, cached_textures)},
    {"cached meshes", offsetof(PortStats, cached_meshes)},
    {"arena blocks", offsetof(PortStats, arena_blocks)},
};

static SDL_Window *window;
static SDL_GPUTexture *canvas;
static Uint32 canvas_width;
static Uint32 canvas_height;
static SDL_GPUTextureFormat canvas_format;
static int shot_wanted;
static char wanted_tab[32];
static int shot_client = DEBUG_CLIENT_NONE;
static int visible = 1;
static uint64_t last_draw;
static int selected_counter;
static int selected_back = -1;
static float spike_ms = 45.0f;
static char log_filter[128];
static bool log_follow = true;
static char command_line[256];

static uint32_t counter_value(const PortStats *stats, size_t offset) {
    return *(const uint32_t *)((const char *)stats + offset);
}

void debug_window_toggle(void) {
    if (!window) return;
    visible = !visible;
    if (visible) {
        SDL_ShowWindow(window);
        if (render_background()) render_send_back(window);
    }
    else SDL_HideWindow(window);
}

void debug_window_start(void) {
#ifdef SDL_PLATFORM_IOS
    return;
#endif
    SDL_GPUDevice *device = render_device();
    window = SDL_CreateWindow("Dark Chronicle Debug", 1100, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window || !SDL_ClaimWindowForGPUDevice(device, window)) {
        fprintf(stderr, "debug: no window: %s\n", SDL_GetError());
        window = NULL;
        return;
    }
    SDL_GPUPresentMode modes[] = {SDL_GPU_PRESENTMODE_MAILBOX, SDL_GPU_PRESENTMODE_IMMEDIATE};
    for (SDL_GPUPresentMode mode : modes) {
        if (!SDL_WindowSupportsGPUPresentMode(device, window, mode)) continue;
        SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
        break;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = "build/port/debug-window.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float scale = SDL_GetWindowDisplayScale(window);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale > 0 ? scale : 1.0f);
    io.FontGlobalScale = 1.0f;
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = device;
    canvas_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.ColorTargetFormat = canvas_format;
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&info);
    if (render_background()) {
        render_send_back(window);
        render_send_back(render_window());
    } else {
        SDL_RaiseWindow(render_window());
    }
}

int debug_window_event(const void *event) {
    if (!window) return 0;
    const SDL_Event *e = (const SDL_Event *)event;
    ImGui_ImplSDL3_ProcessEvent(e);
    SDL_WindowID id = SDL_GetWindowID(window);
    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e->window.windowID == id) {
        visible = 0;
        SDL_HideWindow(window);
        return 1;
    }
    if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) return e->window.windowID == id;
    if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) return e->key.windowID == id;
    if (e->type == SDL_EVENT_TEXT_INPUT) return e->text.windowID == id;
    if (e->type == SDL_EVENT_MOUSE_MOTION) return e->motion.windowID == id;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) return e->button.windowID == id;
    if (e->type == SDL_EVENT_MOUSE_WHEEL) return e->wheel.windowID == id;
    return 0;
}

static void frame_graph(void) {
    const int count = DEBUG_HISTORY;
    ImVec2 size(ImGui::GetContentRegionAvail().x, 180.0f * ImGui::GetStyle().FramePadding.y / 3.0f + 120.0f);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    float top_ms = 80.0f;
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(20, 20, 24, 255));
    float lines[] = {20.0f, 40.0f, 60.0f};
    for (float line : lines) {
        float y = origin.y + size.y - line / top_ms * size.y;
        draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), IM_COL32(70, 70, 80, 255));
        char label[16];
        snprintf(label, sizeof label, "%.0f ms", line);
        draw->AddText(ImVec2(origin.x + 4, y - 14), IM_COL32(120, 120, 130, 255), label);
    }
    float spike_y = origin.y + size.y - spike_ms / top_ms * size.y;
    draw->AddLine(ImVec2(origin.x, spike_y), ImVec2(origin.x + size.x, spike_y), IM_COL32(200, 60, 60, 255));
    float step = size.x / (float)(count - 1);
    ImU32 colors[5] = {IM_COL32(230, 230, 230, 255), IM_COL32(90, 170, 250, 255), IM_COL32(250, 170, 60, 255),
                       IM_COL32(120, 220, 120, 255), IM_COL32(200, 120, 250, 255)};
    int order[5] = {3, 2, 1, 4, 0};
    const DebugFrame *newest = debug_frame_back(0);
    int first = newest && ((newest->frame - (unsigned)(count - 1)) & 1) ? count - 2 : count - 1;
    for (int drawn = 0; drawn < 5; ++drawn) {
        int series = order[drawn];
        ImVec2 previous;
        int have = 0;
        for (int i = first; i >= 0; i -= 2) {
            float value = 0.0f, longest = 0.0f;
            int found = 0;
            for (int j = i; j > i - 2 && j >= 0; --j) {
                const DebugFrame *frame = debug_frame_back(j);
                if (!frame) continue;
                float busy = frame->frame_ms - frame->wait_ms;
                float sample = series == 0 ? frame->frame_ms : series == 1 ? frame->game_ms
                               : series == 2 ? frame->draw_ms : series == 3 ? frame->present_ms : busy > 0.0f ? busy : 0.0f;
                if (sample > value) value = sample;
                if (frame->frame_ms > longest) longest = frame->frame_ms;
                found = 1;
            }
            if (!found) continue;
            if (value > top_ms) value = top_ms;
            ImVec2 point(origin.x + (float)(count - 1 - i) * step, origin.y + size.y - value / top_ms * size.y);
            if (have) draw->AddLine(previous, point, series == 0 && longest > spike_ms ? IM_COL32(255, 80, 80, 255)
                                                                                       : colors[series]);
            previous = point;
            have = 1;
        }
    }
    if (selected_back >= 0) {
        float x = origin.x + (float)(count - 1 - selected_back) * step;
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), IM_COL32(255, 255, 0, 160));
    }
    ImGui::InvisibleButton("frames", size);
    if (ImGui::IsItemHovered()) {
        int back = count - 1 - (int)((ImGui::GetIO().MousePos.x - origin.x) / step + 0.5f);
        const DebugFrame *frame = debug_frame_back(back);
        if (frame) {
            ImGui::SetTooltip("frame %u (field %llu)\nframe %.2f ms\nbusy %.2f ms\ngame %.2f ms\ndraw %.2f ms\npresent %.2f ms\n"
                              "wait %.2f ms\ndraws %u meshes %u commands %u",
                              frame->frame, (unsigned long long)frame->field, frame->frame_ms, frame->frame_ms - frame->wait_ms,
                              frame->game_ms, frame->draw_ms, frame->present_ms, frame->wait_ms, frame->stats.draw_calls, frame->stats.mesh_calls,
                              frame->stats.gpu_commands);
            if (ImGui::IsMouseClicked(0)) selected_back = back;
        }
    }
    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1), "frame");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.35f, 0.67f, 0.98f, 1), "game logic");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.98f, 0.67f, 0.24f, 1), "draw");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.47f, 0.86f, 0.47f, 1), "present");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.78f, 0.47f, 0.98f, 1), "busy (frame less wait)");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.86f, 0.24f, 0.24f, 1), "spike line");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##spike", &spike_ms, 10.0f, 80.0f, "%.0f ms");
}

static void counter_graph(void) {
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("counter", counters[selected_counter].name)) {
        for (int i = 0; i < (int)(sizeof counters / sizeof counters[0]); ++i)
            if (ImGui::Selectable(counters[i].name, i == selected_counter)) selected_counter = i;
        ImGui::EndCombo();
    }
    static float values[DEBUG_HISTORY];
    float top = 1.0f;
    for (int i = DEBUG_HISTORY - 1, n = 0; i >= 0; --i, ++n) {
        const DebugFrame *frame = debug_frame_back(i);
        values[n] = frame ? (float)counter_value(&frame->stats, counters[selected_counter].offset) : 0.0f;
        if (values[n] > top) top = values[n];
    }
    char overlay[64];
    snprintf(overlay, sizeof overlay, "peak %.0f", top);
    ImGui::PlotLines("##counter", values, DEBUG_HISTORY, 0, overlay, 0.0f, top * 1.1f,
                     ImVec2(ImGui::GetContentRegionAvail().x, 90));
}

static void counter_table(void) {
    const DebugFrame *latest = debug_frame_back(0);
    const DebugFrame *chosen = selected_back >= 0 ? debug_frame_back(selected_back) : NULL;
    if (!latest) return;
    if (ImGui::BeginTable("counters", chosen ? 5 : 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("counter");
        ImGui::TableSetupColumn("latest");
        ImGui::TableSetupColumn("average (50)");
        ImGui::TableSetupColumn("peak (600)");
        if (chosen) ImGui::TableSetupColumn("selected frame");
        ImGui::TableHeadersRow();
        for (const Counter &counter : counters) {
            double total = 0;
            uint32_t peak = 0;
            int averaged = 0;
            for (int i = 0; i < DEBUG_HISTORY; ++i) {
                const DebugFrame *frame = debug_frame_back(i);
                if (!frame) break;
                uint32_t value = counter_value(&frame->stats, counter.offset);
                if (value > peak) peak = value;
                if (i < 50) {
                    total += value;
                    averaged++;
                }
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(counter.name);
            ImGui::TableNextColumn();
            ImGui::Text("%u", counter_value(&latest->stats, counter.offset));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", averaged ? total / averaged : 0.0);
            ImGui::TableNextColumn();
            ImGui::Text("%u", peak);
            if (chosen) {
                ImGui::TableNextColumn();
                ImGui::Text("%u", counter_value(&chosen->stats, counter.offset));
            }
        }
        ImGui::EndTable();
    }
    ImGui::Text("engine arena: %.1f MB used, %.1f MB peak, %u blocks", latest->stats.arena_used / 1048576.0,
                latest->stats.arena_peak / 1048576.0, latest->stats.arena_blocks);
}

static void spikes(void) {
    if (!ImGui::CollapsingHeader("spikes")) return;
    for (int i = 0; i < DEBUG_HISTORY; ++i) {
        const DebugFrame *frame = debug_frame_back(i);
        if (!frame) break;
        if (frame->frame_ms <= spike_ms) continue;
        char label[256];
        snprintf(label, sizeof label, "frame %u: %.1f ms (game %.1f, draw %.1f, present %.1f, wait %.1f) draws %u uploads %u disc %u KB allocs %u",
                 frame->frame, frame->frame_ms, frame->game_ms, frame->draw_ms, frame->present_ms, frame->wait_ms,
                 frame->stats.draw_calls, frame->stats.texture_uploads, frame->stats.disc_bytes / 1024,
                 frame->stats.allocations);
        if (ImGui::Selectable(label, selected_back == i)) selected_back = i;
    }
}

static void performance_tab(void) {
    float total = 0, busy = 0;
    int counted = 0;
    for (int i = 0; i < 50; ++i) {
        const DebugFrame *frame = debug_frame_back(i);
        if (!frame) break;
        total += frame->frame_ms;
        busy += frame->frame_ms - frame->wait_ms;
        counted++;
    }
    float average = counted ? total / (float)counted : 0.0f;
    float busy_average = counted ? busy / (float)counted : 0.0f;
    ImGui::Text("%.1f fps (%.2f ms average over %d frames)   busy %.2f ms, %.0f fps uncapped   frame %u   field %llu",
                average > 0 ? 1000.0f / average : 0.0f, average, counted, busy_average,
                busy_average > 0 ? 1000.0f / busy_average : 0.0f, debug_frame_total, (unsigned long long)port_graphics.fields);
    if (selected_back >= 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("clear selection")) selected_back = -1;
    }
    frame_graph();
    counter_graph();
    spikes();
    counter_table();
}

static void game_tab(void) {
    if (ImGui::Button(debug_paused ? "Resume (F5)" : "Pause (F5)")) debug_run(debug_paused ? "resume" : "pause");
    ImGui::SameLine();
    if (ImGui::Button("Step 1 (F6)")) debug_run("step?frames=1");
    ImGui::SameLine();
    if (ImGui::Button("Step 10")) debug_run("step?frames=10");
    ImGui::SameLine();
    if (ImGui::Button("Screenshot")) debug_run("screenshot");
    ImGui::SameLine();
    if (ImGui::Button("Profile 3 s")) debug_run("profile?seconds=3");
    static float speed = 1.0f;
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("speed", &speed, 0.0f, 8.0f, speed == 0.0f ? "unlimited" : "%.2fx")) {
        char line[64];
        snprintf(line, sizeof line, "speed?value=%.3f", speed);
        debug_run(line);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) {
        speed = 1.0f;
        debug_run("speed?value=1");
    }
    bool flag = DebugFlag != 0;
    if (ImGui::Checkbox("engine DebugFlag (Start+Select opens the debug menu)", &flag))
        debug_run(flag ? "debugflag?value=1" : "debugflag?value=0");
    if (ImGui::CollapsingHeader("Helpers", ImGuiTreeNodeFlags_DefaultOpen)) debug_helpers_panel();
    ImGui::Separator();
    ImGui::TextUnformatted("command (same as the HTTP API, e.g. press?buttons=cross&frames=6)");
    ImGui::SetNextItemWidth(-80);
    bool entered = ImGui::InputText("##command", command_line, sizeof command_line, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run") || entered) && command_line[0]) debug_run(command_line);
    ImGui::TextWrapped("%s", debug_last_result);
}

static void log_tab(void) {
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("filter", log_filter, sizeof log_filter);
    ImGui::SameLine();
    ImGui::Checkbox("follow", &log_follow);
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    uint32_t total = debug_log_total();
    uint32_t start = total > 4000 ? total - 4000 : 0;
    char line[512];
    for (uint32_t number = start; number < total; ++number) {
        if (!debug_log_line(number, line, sizeof line)) continue;
        if (log_filter[0] && !strstr(line, log_filter)) continue;
        ImGui::TextUnformatted(line);
    }
    if (log_follow) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

int debug_window_tab(const DebugArgs *args, DebugReply *reply) {
    snprintf(wanted_tab, sizeof wanted_tab, "%s", debug_arg(args, "name", ""));
    debug_print(reply, "{\"tab\": ");
    debug_print_string(reply, wanted_tab);
    debug_print(reply, "}");
    return 0;
}

static bool tab(const char *label) {
    ImGuiTabItemFlags flags = !strcasecmp(wanted_tab, label) ? ImGuiTabItemFlags_SetSelected : 0;
    return ImGui::BeginTabItem(label, NULL, flags);
}

int debug_window_shot(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (!window || !visible) return debug_fail(reply, "the debug window is hidden; use /window");
    if (shot_wanted) return debug_fail(reply, "a window shot is already pending");
    shot_wanted = 1;
    shot_client = reply->client;
    reply->deferred = 1;
    return 0;
}

static void send_shot(SDL_GPUDevice *device) {
    Uint32 size = canvas_width * canvas_height * 4;
    SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCommandBuffer *buffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    SDL_GPUTextureRegion from = {};
    from.texture = canvas;
    from.w = canvas_width;
    from.h = canvas_height;
    from.d = 1;
    SDL_GPUTextureTransferInfo to = {transfer, 0, canvas_width, canvas_height};
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(buffer);
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    uint32_t *pixels = (uint32_t *)SDL_MapGPUTransferBuffer(device, transfer, false);
    int swap = canvas_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || canvas_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    if (swap)
        for (Uint32 i = 0; i < canvas_width * canvas_height; ++i)
            pixels[i] = (pixels[i] & 0xff00ff00u) | (pixels[i] & 0xffu) << 16 | (pixels[i] >> 16 & 0xffu);
    size_t length = 0;
    unsigned char *png = debug_png(pixels, (int)canvas_width, (int)canvas_height, &length);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    if (shot_client >= 0) debug_server_send(shot_client, 200, "image/png", png, length);
    free(png);
    shot_wanted = 0;
    shot_client = DEBUG_CLIENT_NONE;
}

void debug_window_draw(void) {
    if (!window || !visible) return;
    uint64_t now = SDL_GetTicksNS();
    if (now - last_draw < 15000000ull && !shot_wanted) return;
    last_draw = now;
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("debug", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    debug_reload_bar();
    if (ImGui::BeginTabBar("tabs")) {
        if (tab("Performance")) {
            performance_tab();
            ImGui::EndTabItem();
        }
        if (tab("Game")) {
            game_tab();
            ImGui::EndTabItem();
        }
        if (tab("Scenario")) {
            debug_scenario_panel();
            ImGui::EndTabItem();
        }
        if (tab("Capture")) {
            debug_capture_panel();
            ImGui::EndTabItem();
        }
        if (tab("Replay")) {
            debug_replay_panel();
            ImGui::EndTabItem();
        }
        if (tab("Savestates")) {
            debug_savestate_panel();
            ImGui::EndTabItem();
        }
        if (tab("Log")) {
            log_tab();
            ImGui::EndTabItem();
        }
        if (port_mods_panel_count() && tab("Mods")) {
            for (int i = 0; i < port_mods_panel_count(); ++i)
                if (ImGui::CollapsingHeader(port_mods_panel_name(i), ImGuiTreeNodeFlags_DefaultOpen)) port_mods_panel_draw(i);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    wanted_tab[0] = 0;
    ImGui::End();
    ImGui::Render();
    ImDrawData *data = ImGui::GetDrawData();
    SDL_GPUDevice *device = render_device();
    SDL_GPUCommandBuffer *buffer = SDL_AcquireGPUCommandBuffer(device);
    if (!buffer) return;
    SDL_GPUTexture *swapchain = NULL;
    Uint32 width = 0, height = 0;
    if (!SDL_AcquireGPUSwapchainTexture(buffer, window, &swapchain, &width, &height) || !swapchain ||
        data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0) {
        SDL_SubmitGPUCommandBuffer(buffer);
        return;
    }
    if (!canvas || canvas_width != width || canvas_height != height) {
        if (canvas) SDL_ReleaseGPUTexture(device, canvas);
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = canvas_format;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        canvas = SDL_CreateGPUTexture(device, &info);
        canvas_width = width;
        canvas_height = height;
    }
    ImGui_ImplSDLGPU3_PrepareDrawData(data, buffer);
    SDL_GPUColorTargetInfo target = {};
    target.texture = canvas;
    target.clear_color = SDL_FColor{0.08f, 0.08f, 0.1f, 1.0f};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(buffer, &target, 1, NULL);
    ImGui_ImplSDLGPU3_RenderDrawData(data, buffer, pass);
    SDL_EndGPURenderPass(pass);
    SDL_GPUBlitInfo blit = {};
    blit.source.texture = canvas;
    blit.source.w = width;
    blit.source.h = height;
    blit.destination.texture = swapchain;
    blit.destination.w = width;
    blit.destination.h = height;
    SDL_BlitGPUTexture(buffer, &blit);
    SDL_SubmitGPUCommandBuffer(buffer);
    if (shot_wanted) send_shot(device);
}
