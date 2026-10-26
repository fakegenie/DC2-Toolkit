#include <SDL3/SDL.h>
#include "debug.h"
#include "draw.hpp"
#include "imgui.h"
#include "port_graphics.h"
#include "render.h"
#include "port_mod.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Draw {
    RenderRecord record;
    char texture_name[32];
    int texture_width;
    int texture_height;
    int texture_bpp;
};

struct Described {
    char blend[64];
    char alpha_test[64];
    char depth[48];
    char texa[48];
};

struct Toggle {
    const char *name;
    int *flag;
    int inverted;
};

static const Toggle toggles[] = {
    {"models", &render_toggles.no_models, 1},
    {"2d", &render_toggles.no_2d, 1},
    {"shadows", &render_toggles.no_shadows, 1},
    {"water", &render_toggles.no_water, 1},
    {"sprites", &render_toggles.no_sprites, 1},
    {"fog", &render_toggles.no_fog, 1},
    {"textures", &render_toggles.no_textures, 1},
    {"wireframe", &render_toggles.wireframe, 0},
    {"overdraw", &render_toggles.overdraw, 0},
    {"unpaced", &render_toggles.unpaced, 0},
};

static const char *const sources[] = {"2d", "mesh", "shadow", "sprite", "water", "depth"};
static const char *const topologies[] = {"points", "lines", "triangles"};
static const char *const culls[] = {"none", "front", "back"};
static const char *const depth_tests[] = {"never", "always", "gequal", "greater"};
static const char *const alpha_tests[] = {"never", "always", "less", "lequal", "equal", "gequal", "greater", "notequal"};
static const char *const alpha_fails[] = {"keep", "fb_only", "zb_only", "rgb_only"};
static const char *const blend_colors[] = {"Cs", "Cd", "0", "0"};
static const char *const blend_alphas[] = {"As", "Ad", "FIX", "FIX"};

static std::vector<Draw> draws;
static int capture_commands;
static int capture_passes;
static int capture_dropped;
static uint32_t capture_frame;
static int waiting;
static int waiting_client = DEBUG_CLIENT_NONE;
static int selected = -1;
static int selected_slot;
static float zoom = 1.0f;
static int only_source = -1;
static bool highlight_on;
static int shown_target;
static int shown_width;
static int shown_height;
static const int scales[] = {1, 2, 3, 4};
static const int sample_counts[] = {1, 2, 4, 8};

static void describe(const RenderState &state, Described *text) {
    const RenderBlending &blending = state.blending;
    const RenderTests &tests = state.tests;
    if (!state.blend) snprintf(text->blend, sizeof text->blend, "off");
    else if (blending.c == RENDER_BLEND_FIX)
        snprintf(text->blend, sizeof text->blend, "(%s - %s) * %u/128 + %s", blend_colors[blending.a],
                 blend_colors[blending.b], (unsigned)blending.fix, blend_colors[blending.d]);
    else
        snprintf(text->blend, sizeof text->blend, "(%s - %s) * %s + %s", blend_colors[blending.a],
                 blend_colors[blending.b], blend_alphas[blending.c], blend_colors[blending.d]);
    if (!tests.alpha_test) snprintf(text->alpha_test, sizeof text->alpha_test, "off");
    else
        snprintf(text->alpha_test, sizeof text->alpha_test, "%s ref %u fail %s", alpha_tests[tests.alpha_method],
                 (unsigned)tests.alpha_reference, alpha_fails[tests.alpha_fail]);
    if (tests.destination_test) {
        size_t used = strlen(text->alpha_test);
        snprintf(text->alpha_test + used, sizeof text->alpha_test - used, ", destination alpha %d",
                 (int)tests.destination_mode);
    }
    if (!tests.depth_test) snprintf(text->depth, sizeof text->depth, "off, write %d", state.depth_write);
    else snprintf(text->depth, sizeof text->depth, "%s, write %d", depth_tests[tests.depth_method], state.depth_write);
    snprintf(text->texa, sizeof text->texa, "%d aem %d ta0 %u ta1 %u", state.texa, (int)state.expansion.aem,
             (unsigned)state.expansion.ta0, (unsigned)state.expansion.ta1);
}

static void print_draw(DebugReply *reply, int index) {
    const Draw &draw = draws[(size_t)index];
    const RenderRecord &r = draw.record;
    const RenderState &s = r.state;
    Described text;
    describe(s, &text);
    debug_print(reply, "{\"index\": %d, \"kind\": \"%s\", \"target\": {\"slot\": %d, \"width\": %d, \"height\": %d, \"base\": %u}",
                index, sources[r.source], r.target, r.target_width, r.target_height, r.target_base);
    if (s.texture) {
        debug_print(reply, ", \"texture\": {\"slot\": %d, \"width\": %d, \"height\": %d, \"bpp\": %d, \"name\": ", s.texture,
                    draw.texture_width, draw.texture_height, draw.texture_bpp);
        debug_print_string(reply, draw.texture_name);
        debug_print(reply, "}");
    } else {
        debug_print(reply, ", \"texture\": null");
    }
    debug_print(reply, ", \"topology\": \"%s\", \"vertices\": %d, \"cull\": \"%s\"", topologies[r.topology], r.count,
                r.source == RENDER_SOURCE_MESH ? culls[r.cull % 3] : "none");
    debug_print(reply, ", \"blend\": \"%s\", \"alpha_test\": \"%s\", \"depth\": \"%s\", \"texa\": \"%s\"", text.blend,
                text.alpha_test, text.depth, text.texa);
    debug_print(reply, ", \"fog\": %d, \"fog_color\": %u, \"decal\": %d, \"vertex_alpha\": %d, \"linear\": %d, \"repeat\": [%d, %d]",
                s.fog, s.fog_color, s.decal, s.vertex_alpha, s.linear, s.repeat_u, s.repeat_v);
    debug_print(reply, ", \"color_mask\": %u, \"scissor\": [%d, %d, %d, %d]", s.color_mask, s.scissor.left, s.scissor.top,
                s.scissor.right, s.scissor.bottom);
    if (r.source == RENDER_SOURCE_MESH) {
        const MeshUniforms &u = r.uniforms;
        debug_print(reply, ", \"flags\": [%u, %u, %u, %u], \"ambient\": [%.1f, %.1f, %.1f, %.1f], \"object_color\": [%.1f, %.1f, %.1f, %.1f]",
                    u.flags.program, u.flags.face_type, u.flags.specular_passes, u.flags.bone_set, u.ambient[0], u.ambient[1], u.ambient[2], u.ambient[3],
                    u.object_color[0], u.object_color[1], u.object_color[2], u.object_color[3]);
        debug_print(reply, ", \"diffuse\": [%.3f, %.3f, %.3f, %.3f], \"light_color\": [", u.diffuse[0], u.diffuse[1],
                    u.diffuse[2], u.diffuse[3]);
        for (int k = 0; k < 4; ++k)
            debug_print(reply, "%s[%.1f, %.1f, %.1f]", k ? ", " : "", u.light_color[k][0], u.light_color[k][1], u.light_color[k][2]);
        debug_print(reply, "], \"point_color\": [");
        for (int k = 0; k < 4; ++k)
            debug_print(reply, "%s[%.1f, %.1f, %.1f, %.1f]", k ? ", " : "", u.point_color[k][0], u.point_color[k][1],
                        u.point_color[k][2], u.point_light[k][3]);
        debug_print(reply, "], \"fog_params\": [%.3f, %.3f, %.3f, %.3f]", u.fog.offset, u.fog.near_value, u.fog.far_value, u.fog.scale);
    }
    if (r.source != RENDER_SOURCE_MESH)
        debug_print(reply, ", \"bounds\": [%.1f, %.1f, %.1f, %.1f]", r.bounds[0], r.bounds[1], r.bounds[2], r.bounds[3]);
        debug_print(reply, ", \"uv\": [%.4f, %.4f, %.4f, %.4f]", r.uv[0], r.uv[1], r.uv[2], r.uv[3]);
    debug_print(reply, ", \"command\": %d, \"pass\": %d, \"merged\": %d}", r.command, r.pass, r.merged);
}

static void print_capture(DebugReply *reply, int from, int limit) {
    int total = (int)draws.size();
    int end = limit < 0 || from + limit > total ? total : from + limit;
    int mesh = 0;
    for (const Draw &draw : draws) mesh += draw.record.source == RENDER_SOURCE_MESH;
    debug_print(reply, "{\"frame\": %u, \"draws\": %d, \"mesh_calls\": %d, \"draw_calls\": %d, \"dropped\": %d, "
                       "\"commands\": %d, \"passes\": %d, \"from\": %d, \"list\": [",
                capture_frame, total, mesh, total - mesh, capture_dropped, capture_commands, capture_passes, from);
    for (int i = from; i < end; ++i) {
        if (i > from) debug_print(reply, ", ");
        print_draw(reply, i);
    }
    debug_print(reply, "]}");
}

static void finish_capture(void) {
    render_capture_end();
    waiting = 0;
    draws.clear();
    const RenderRecord *records = render_capture_records();
    for (int i = 0; i < render_capture_count(); ++i) {
        Draw draw;
        memset(&draw, 0, sizeof draw);
        draw.record = records[i];
        RenderSlot slot;
        if (draw.record.state.texture && render_slot(draw.record.state.texture, &slot)) {
            draw.texture_width = slot.width;
            draw.texture_height = slot.height;
            CacheInfo info;
            if (gs_cache_info(draw.record.state.texture, &info)) {
                snprintf(draw.texture_name, sizeof draw.texture_name, "%s", info.name);
                draw.texture_bpp = info.psm;
            } else {
                snprintf(draw.texture_name, sizeof draw.texture_name, "%s", slot.target ? "target" : "");
            }
        }
        draws.push_back(draw);
    }
    capture_commands = render_capture_commands();
    capture_passes = 0;
    for (const Draw &draw : draws)
        if (draw.record.pass >= capture_passes) capture_passes = draw.record.pass + 1;
    capture_dropped = render_capture_dropped();
    capture_frame = debug_frame_total;
    selected = -1;
    if (waiting_client < 0) return;
    DebugReply reply;
    memset(&reply, 0, sizeof reply);
    reply.client = waiting_client;
    reply.status = 200;
    print_capture(&reply, 0, -1);
    debug_finish(&reply);
    waiting_client = DEBUG_CLIENT_NONE;
}

static int command_capture(const DebugArgs *args, DebugReply *reply) {
    if (debug_arg_int(args, "last", 0)) {
        if (draws.empty()) return debug_fail(reply, "no capture yet; run /capture");
        print_capture(reply, (int)debug_arg_int(args, "from", 0), (int)debug_arg_int(args, "limit", -1));
        return 0;
    }
    if (waiting) return debug_fail(reply, "a capture is already pending");
    waiting = 1;
    render_capture_begin();
    if (!render_capturing()) {
        waiting = 0;
        return debug_fail(reply, "no memory for the capture");
    }
    if (reply->client >= 0) {
        waiting_client = reply->client;
        reply->deferred = 1;
    }
    return 0;
}

static int command_draw(const DebugArgs *args, DebugReply *reply) {
    long index = debug_arg_int(args, "index", -1);
    if (index < 0 || index >= (long)draws.size()) return debug_fail(reply, "index 0..%d", (int)draws.size() - 1);
    print_draw(reply, (int)index);
    return 0;
}

static int command_highlight(const DebugArgs *args, DebugReply *reply) {
    render_highlight = (int)debug_arg_int(args, "index", -1);
    selected = render_highlight;
    highlight_on = render_highlight >= 0;
    debug_print(reply, "{\"highlight\": %d}", render_highlight);
    return 0;
}

static int command_textures(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_print(reply, "{\"list\": [");
    int first = 1;
    for (int slot = 1; slot < render_slot_limit(); ++slot) {
        RenderSlot info;
        if (!render_slot(slot, &info)) continue;
        debug_print(reply, "%s{\"slot\": %d, \"kind\": \"%s\", \"width\": %d, \"height\": %d, \"base\": %u, \"drawn\": %d",
                    first ? "" : ", ", slot, info.target ? "target" : "texture", info.width, info.height, info.base,
                    info.drawn);
        first = 0;
        CacheInfo cache;
        if (gs_cache_info(slot, &cache)) {
            debug_print(reply, ", \"name\": ");
            debug_print_string(reply, cache.name);
            debug_print(reply, ", \"bpp\": %d, \"clut\": %d, \"idle_frames\": %u", cache.psm, cache.clut != NULL, cache.age);
        }
        debug_print(reply, "}");
    }
    debug_print(reply, "]}");
    return 0;
}

static int command_texture(const DebugArgs *args, DebugReply *reply) {
    long slot = debug_arg_int(args, "slot", 0);
    RenderSlot info;
    if (!render_slot((int)slot, &info)) return debug_fail(reply, "no live slot %ld; try /textures", slot);
    uint32_t *pixels = (uint32_t *)malloc((size_t)info.width * (size_t)info.height * 4);
    render_download((int)slot, 0, 0, info.width, info.height, pixels);
    if (debug_arg_int(args, "alpha", 0))
        for (int i = 0; i < info.width * info.height; ++i) {
            uint32_t a = pixels[i] >> 24;
            pixels[i] = 0xff000000u | a << 16 | a << 8 | a;
        }
    reply->body = debug_png(pixels, info.width, info.height, &reply->body_length);
    reply->content_type = "image/png";
    free(pixels);
    return 0;
}

static int command_toggle(const DebugArgs *args, DebugReply *reply) {
    const char *name = debug_arg(args, "name", "");
    const char *value = debug_arg(args, "value", "");
    for (const Toggle &toggle : toggles) {
        if (strcmp(toggle.name, name)) continue;
        if (value[0]) *toggle.flag = toggle.inverted ? !atoi(value) : atoi(value) != 0;
        break;
    }
    debug_print(reply, "{");
    for (const Toggle &toggle : toggles)
        debug_print(reply, "%s\"%s\": %d", &toggle == toggles ? "" : ", ", toggle.name,
                    toggle.inverted ? !*toggle.flag : *toggle.flag);
    debug_print(reply, "}");
    return 0;
}

static int command_skip(const DebugArgs *args, DebugReply *reply) {
    if (debug_arg_int(args, "prisms", 0)) {
        render_toggles.prism_from = (int)debug_arg_int(args, "from", 0);
        render_toggles.prism_to = (int)debug_arg_int(args, "to", 0);
        debug_print(reply, "{\"prism_from\": %d, \"prism_to\": %d}", render_toggles.prism_from, render_toggles.prism_to);
        return 0;
    }
    render_toggles.skip_from = (int)debug_arg_int(args, "from", 0);
    render_toggles.skip_to = (int)debug_arg_int(args, "to", 0);
    debug_print(reply, "{\"from\": %d, \"to\": %d}", render_toggles.skip_from, render_toggles.skip_to);
    return 0;
}

static void print_quality(DebugReply *reply) {
    int scale, samples;
    render_quality(&scale, &samples);
    debug_print(reply, "{\"scale\": %d, \"msaa\": %d}", scale, samples);
}

static int command_scale(const DebugArgs *args, DebugReply *reply) {
    render_set_quality((int)debug_arg_int(args, "value", 0), 0);
    debug_print(reply, "{\"requested\": %ld, \"now\": ", debug_arg_int(args, "value", 0));
    print_quality(reply);
    debug_print(reply, "}");
    return 0;
}

static int command_msaa(const DebugArgs *args, DebugReply *reply) {
    render_set_quality(0, (int)debug_arg_int(args, "value", 0));
    debug_print(reply, "{\"requested\": %ld, \"now\": ", debug_arg_int(args, "value", 0));
    print_quality(reply);
    debug_print(reply, "}");
    return 0;
}

static int command_grab(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (!shown_target || shown_width <= 0 || shown_height <= 0) return debug_fail(reply, "nothing shown yet");
    int scale = render_scale(shown_target);
    std::vector<uint32_t> pixels((size_t)(shown_width * scale) * (size_t)(shown_height * scale));
    render_download_full(shown_target, shown_width, shown_height, pixels.data());
    reply->body = debug_png(pixels.data(), shown_width * scale, shown_height * scale, &reply->body_length);
    reply->content_type = "image/png";
    return 0;
}

static int command_shown(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    int width = 0, height = 0;
    uint32_t *pixels = render_download_screen(&width, &height);
    if (!pixels) return debug_fail(reply, "nothing shown yet");
    reply->body = debug_png(pixels, width, height, &reply->body_length);
    reply->content_type = "image/png";
    free(pixels);
    return 0;
}

static int command_between(const DebugArgs *args, DebugReply *reply) {
    if (!render_smoothing()) return debug_fail(reply, "smoothing is off: start with --smooth HZ or the dc2tk.smooth mod");
    if (!render_replay((float)debug_arg_number(args, "alpha", 0.5))) return debug_fail(reply, "no recorded frame yet");
    return command_grab(args, reply);
}

static std::vector<std::vector<uint32_t>> presents;
static std::vector<float> present_alphas;
static int presents_wanted;
static int presents_full;
static int present_width;
static int present_height;

static void write_presents() {
    SDL_CreateDirectory("build/port/presents");
    for (size_t i = 0; i < presents.size(); ++i) {
        char path[96];
        snprintf(path, sizeof path, "build/port/presents/%03zu_%.3f.png", i, present_alphas[i]);
        size_t length = 0;
        unsigned char *png = debug_png(presents[i].data(), present_width, present_height, &length);
        FILE *file = fopen(path, "wb");
        if (file) {
            fwrite(png, 1, length, file);
            fclose(file);
        }
        free(png);
    }
}

extern "C" void port_debug_replayed(int slot, float alpha) {
    if ((int)presents.size() >= presents_wanted || !slot) return;
    int scale = render_scale(slot);
    std::vector<uint32_t> full((size_t)(shown_width * scale) * (size_t)(shown_height * scale));
    render_download_full(slot, shown_width, shown_height, full.data());
    int step = presents_full ? 1 : scale;
    present_width = shown_width * scale / step;
    present_height = shown_height * scale / step;
    std::vector<uint32_t> kept((size_t)present_width * (size_t)present_height);
    for (int y = 0; y < present_height; ++y)
        for (int x = 0; x < present_width; ++x)
            kept[(size_t)y * present_width + x] = full[(size_t)(y * step) * (size_t)(shown_width * scale) + (size_t)(x * step)];
    presents.push_back(kept);
    present_alphas.push_back(alpha);
    if ((int)presents.size() == presents_wanted) write_presents();
}

static int command_presents(const DebugArgs *args, DebugReply *reply) {
    int count = (int)debug_arg_int(args, "count", 60);
    if (count < 1 || count > 240) return debug_fail(reply, "count 1..240");
    presents.clear();
    present_alphas.clear();
    presents_wanted = count;
    presents_full = (int)debug_arg_int(args, "full", 0);
    debug_print(reply, "{\"armed\": %d, \"folder\": \"build/port/presents\"}", count);
    return 0;
}

static int command_depth(const DebugArgs *args, DebugReply *reply) {
    int x = (int)debug_arg_int(args, "x", 252), y = (int)debug_arg_int(args, "y", 204);
    int size = (int)debug_arg_int(args, "size", 8);
    if (size < 1 || size > 64) return debug_fail(reply, "size 1..64");
    std::vector<unsigned> pixels((size_t)(size * size));
    port_depth_download(x, y, size, size, pixels.data());
    unsigned nearest = 0, farthest = 0xffffff;
    for (unsigned value : pixels) {
        if (value > nearest) nearest = value;
        if (value < farthest) farthest = value;
    }
    debug_print(reply, "{\"x\": %d, \"y\": %d, \"size\": %d, \"nearest\": %u, \"farthest\": %u}", x, y, size, nearest,
                farthest);
    return 0;
}

extern const DebugCommand debug_capture_commands[] = {
    {"capture", command_capture, 0, "record every draw of the next frame; last=1 returns the previous capture (from=N limit=N)"},
    {"draw", command_draw, 1, "one draw of the last capture; index=N"},
    {"highlight", command_highlight, 1, "redraw draw index=N of every frame in magenta on top; -1 clears"},
    {"textures", command_textures, 1, "every live texture and render target slot"},
    {"texture", command_texture, 0, "a slot as PNG; slot=N, alpha=1 for its alpha channel as grey"},
    {"skip", command_skip, 1, "skip draws numbered from=A to=B (exclusive) in every frame, to find which draws make a picture; to=0 clears; prisms=1 hides shadow prisms from=A to=B in in-between pictures"},
    {"toggle", command_toggle, 1, "name=models|2d|shadows|water|sprites|fog|textures|wireframe|overdraw|unpaced value=0|1; no arguments lists them"},
    {"scale", command_scale, 0, "internal resolution value=1|2|3|4 times the PS2 size; applied at the next frame"},
    {"msaa", command_msaa, 0, "multisampling value=1|2|4|8, lowered to what the GPU supports; applied at the next frame"},
    {"depth", command_depth, 0, "reads the depth buffer like the game's photo and pick-z; x=N y=N size=N (8)"},
    {"grab", command_grab, 0, "the shown picture as PNG at the full internal resolution"},
    {"shown", command_shown, 0, "the last picture handed to the window, as PNG at the full internal resolution"},
    {"presents", command_presents, 0,
     "save the next count=N (60) pictures put on screen, in-between frames included, to build/port/presents; full=1 keeps the internal resolution"},
    {"between", command_between, 0,
     "an in-between frame as PNG at the full internal resolution: alpha=0..1 from the game frame before the last to "
     "the last (needs --smooth)"},
    {NULL, NULL, 0, NULL},
};

static ImTextureRef slot_image(int slot) {
    return ImTextureRef((ImTextureID)(intptr_t)render_gpu_texture(slot));
}

static void thumbnail(int slot, float limit) {
    RenderSlot info;
    if (!render_slot(slot, &info)) {
        ImGui::TextUnformatted("(slot gone)");
        return;
    }
    float scale = limit / (float)(info.width > info.height ? info.width : info.height);
    ImGui::Image(slot_image(slot), ImVec2((float)info.width * scale, (float)info.height * scale));
}

static void draw_details(const Draw &draw) {
    const RenderRecord &r = draw.record;
    const RenderState &s = r.state;
    Described text;
    describe(s, &text);
    ImGui::Text("%s  %s  %d vertices", sources[r.source], topologies[r.topology], r.count);
    ImGui::Text("target slot %d  %dx%d  base %u", r.target, r.target_width, r.target_height, r.target_base);
    if (s.texture) ImGui::Text("texture slot %d  %dx%d  bpp %d  %s", s.texture, draw.texture_width, draw.texture_height,
                               draw.texture_bpp, draw.texture_name);
    else ImGui::TextUnformatted("untextured");
    ImGui::Text("blend %s", text.blend);
    ImGui::Text("alpha test %s", text.alpha_test);
    ImGui::Text("depth %s", text.depth);
    ImGui::Text("texa %s  decal %d  vertex alpha %d  linear %d  repeat %d %d", text.texa, s.decal, s.vertex_alpha, s.linear,
                s.repeat_u, s.repeat_v);
    ImGui::Text("fog %d  color %06x  color mask %08x  cull %s", s.fog, s.fog_color, s.color_mask,
                r.source == RENDER_SOURCE_MESH ? culls[r.cull % 3] : "none");
    ImGui::Text("scissor %d %d %d %d", s.scissor.left, s.scissor.top, s.scissor.right, s.scissor.bottom);
    if (r.source != RENDER_SOURCE_MESH)
        ImGui::Text("bounds %.1f %.1f - %.1f %.1f", r.bounds[0], r.bounds[1], r.bounds[2], r.bounds[3]);
    ImGui::Text("GPU command %d%s  render pass %d", r.command, r.merged ? " (merged)" : "", r.pass);
}

static void quality_panel(void) {
    int scale, samples;
    render_quality(&scale, &samples);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("scale");
    for (int value : scales) {
        ImGui::SameLine();
        char label[16];
        snprintf(label, sizeof label, "%dx##scale", value);
        if (ImGui::RadioButton(label, scale == value)) render_set_quality(value, 0);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("  msaa");
    for (int value : sample_counts) {
        ImGui::SameLine();
        char label[16];
        snprintf(label, sizeof label, "%d##msaa", value);
        if (ImGui::RadioButton(label, samples == value)) render_set_quality(0, value);
    }
}

static void toggle_panel(void) {
    for (const Toggle &toggle : toggles) {
        bool on = toggle.inverted ? !*toggle.flag : *toggle.flag;
        if (ImGui::Checkbox(toggle.name, &on)) *toggle.flag = toggle.inverted ? !on : on;
        ImGui::SameLine();
    }
    ImGui::NewLine();
    quality_panel();
}

static void draws_panel(void) {
    if (ImGui::Button(waiting ? "capturing..." : "Capture next frame")) debug_run("capture");
    ImGui::SameLine();
    ImGui::Text("%d draws, %d GPU commands, %d passes, frame %u", (int)draws.size(), capture_commands, capture_passes,
                capture_frame);
    ImGui::SameLine();
    if (ImGui::Checkbox("highlight selected", &highlight_on) && !highlight_on) render_highlight = -1;
    if (highlight_on && selected >= 0) render_highlight = selected;
    ImGui::SetNextItemWidth(120);
    const char *filter = only_source < 0 ? "all" : sources[only_source];
    if (ImGui::BeginCombo("kind", filter)) {
        if (ImGui::Selectable("all", only_source < 0)) only_source = -1;
        for (int i = 0; i < 6; ++i)
            if (ImGui::Selectable(sources[i], only_source == i)) only_source = i;
        ImGui::EndCombo();
    }
    float list_width = ImGui::GetContentRegionAvail().x * 0.5f;
    ImGui::BeginChild("draws", ImVec2(list_width, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("list", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("kind");
        ImGui::TableSetupColumn("texture");
        ImGui::TableSetupColumn("verts");
        ImGui::TableSetupColumn("cmd");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)draws.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Draw &draw = draws[(size_t)i];
                if (only_source >= 0 && draw.record.source != only_source) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char label[16];
                snprintf(label, sizeof label, "%d", i);
                if (ImGui::Selectable(label, selected == i, ImGuiSelectableFlags_SpanAllColumns)) selected = i;
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sources[draw.record.source]);
                ImGui::TableNextColumn();
                if (draw.record.state.texture) ImGui::Text("%d %s", draw.record.state.texture, draw.texture_name);
                else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                ImGui::Text("%d", draw.record.count);
                ImGui::TableNextColumn();
                ImGui::Text("%d%s", draw.record.command, draw.record.merged ? "m" : "");
            }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (selected >= 0 && selected < (int)draws.size()) {
        const Draw &draw = draws[(size_t)selected];
        draw_details(draw);
        if (draw.record.state.texture) {
            ImGui::Separator();
            ImGui::TextUnformatted("texture");
            thumbnail(draw.record.state.texture, 256.0f);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("render target (current contents)");
        thumbnail(draw.record.target, 256.0f);
    } else {
        ImGui::TextUnformatted("select a draw");
    }
    ImGui::EndChild();
}

static void textures_panel(void) {
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("zoom", &zoom, 0.25f, 4.0f);
    float list_width = ImGui::GetContentRegionAvail().x * 0.45f;
    ImGui::BeginChild("slots", ImVec2(list_width, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("slot list", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("slot");
        ImGui::TableSetupColumn("image");
        ImGui::TableSetupColumn("kind");
        ImGui::TableSetupColumn("size");
        ImGui::TableSetupColumn("base");
        ImGui::TableSetupColumn("name");
        ImGui::TableHeadersRow();
        for (int slot = 1; slot < render_slot_limit(); ++slot) {
            RenderSlot info;
            if (!render_slot(slot, &info)) continue;
            ImGui::TableNextRow(0, 40.0f);
            ImGui::TableNextColumn();
            char label[16];
            snprintf(label, sizeof label, "%d", slot);
            if (ImGui::Selectable(label, selected_slot == slot, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 40.0f)))
                selected_slot = slot;
            ImGui::TableNextColumn();
            thumbnail(slot, 40.0f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(info.target ? "target" : "texture");
            ImGui::TableNextColumn();
            if (info.scale > 1 || info.samples > 1) ImGui::Text("%dx%d x%d msaa %d", info.width, info.height, info.scale, info.samples);
            else ImGui::Text("%dx%d", info.width, info.height);
            ImGui::TableNextColumn();
            ImGui::Text("%u", info.base);
            ImGui::TableNextColumn();
            CacheInfo cache;
            if (gs_cache_info(slot, &cache)) ImGui::Text("%s bpp %d idle %u", cache.name, cache.psm, cache.age);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("zoomed", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    RenderSlot info;
    if (render_slot(selected_slot, &info))
        ImGui::Image(slot_image(selected_slot), ImVec2((float)info.width * zoom, (float)info.height * zoom));
    else ImGui::TextUnformatted("select a slot");
    ImGui::EndChild();
}

void debug_capture_panel(void) {
    toggle_panel();
    if (ImGui::BeginTabBar("capture tabs")) {
        if (ImGui::BeginTabItem("Draws")) {
            draws_panel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            textures_panel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void debug_capture_frame(void) {}

void debug_capture_present(int target, int width, int height) {
    shown_target = target;
    shown_width = width;
    shown_height = height;
    if (!waiting || (debug_paused && debug_steps == 0)) return;
    finish_capture();
}
