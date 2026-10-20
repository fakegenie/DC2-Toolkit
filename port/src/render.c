#include "render.h"
#include "port_stats.h"
#include "shaders.h"
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEXTURE_LIMIT = 4096, PIPELINE_LIMIT = 1024, COMMAND_LIMIT = 65536, MODEL_LIMIT = 16384, BONE_LIMIT = 2048,
       VERTEX_BYTES = 64 * 1024 * 1024 };
enum { KEY_MESH = 1 << 22, KEY_CULL_SHIFT = 23, KEY_WIRE = 1 << 25, RECORD_LIMIT = 32768 };
enum { KEY_KEEP = (3 << 15) | KEY_MESH | (3 << 23) };
enum { WINDOW_WIDTH = 960, WINDOW_HEIGHT = 720, TARGET_WIDTH = 640, TARGET_HEIGHT = 512, DEPTH_TILE = 64 };

enum {
    TEXTURED = 1, TEXA = 2, AEM = 4, FOG = 8, ALPHA_TEST = 16, FACTOR_FIX = 32,
    SCALE_SOURCE = 64, FACTOR_COLOR = 128, DECAL = 256, VERTEX_ALPHA = 512, DEPTH_READ = 1024, SOLID = 2048, OVERDRAW = 4096
};

typedef struct {
    SDL_GPUTexture *color;
    int width;
    int height;
    uint32_t base;
    int target;
    int used;
    int drawn;
} Texture;

typedef struct {
    uint32_t key;
    SDL_GPUGraphicsPipeline *pipeline;
} Pipeline;

typedef struct {
    uint32_t flags;
    uint32_t alpha;
    uint32_t fog_color;
    uint32_t unused;
} FragmentState;

typedef struct {
    uint32_t key;
    FragmentState fragment;
    float blend_constant;
    int texture;
    int sampler;
    SDL_Rect scissor;
    uint32_t offset;
    int count;
    int model;
    int bones;
} Command;

static SDL_Window *window;
static SDL_GPUDevice *device;
static SDL_GPUShader *vertex_shader;
static SDL_GPUShader *mesh_shader;
static MeshUniforms models[MODEL_LIMIT];
static int model_count;
static float bone_sets[BONE_LIMIT][384];
static int bone_count;
static SDL_GPUShader *fragment_shader;
static SDL_GPUSampler *samplers[8];
static SDL_GPUBuffer *vertex_buffer;
static SDL_GPUTransferBuffer *vertex_transfer;
static SDL_GPUCommandBuffer *commands;
static SDL_GPUTextureFormat depth_format;
static struct { SDL_GPUTexture *texture; int width; int height; int cleared; } depths[8];
static Texture textures[TEXTURE_LIMIT];
static Pipeline pipelines[PIPELINE_LIMIT];
static int pipeline_count;
static Command pending[COMMAND_LIMIT];
static int pending_count;
static unsigned char *vertex_data;
static uint32_t vertex_used;
static uint32_t vertex_flushed;
static int vertex_cycle;
static int bound;
static int blank;
static Command held;
static int held_valid;
static int frame_draws;
static int command_serial;
static int pass_serial;
static int capturing;
static RenderRecord *records;
static int record_count;
static int record_dropped;
static int command_base;
static int pass_base;

RenderToggles render_toggles;
int render_source;
int render_highlight = -1;

static void render_error(const char *what) {
    fprintf(stderr, "render: %s: %s\n", what, SDL_GetError());
    exit(1);
}

static SDL_GPUCommandBuffer *command_buffer(void) {
    if (!commands) commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) render_error("command buffer");
    return commands;
}

static SDL_GPUShader *create_shader(const unsigned char *spirv, size_t spirv_size, const unsigned char *msl,
                                    size_t msl_size, SDL_GPUShaderStage stage, int samplers_used, int uniforms) {
    SDL_GPUShaderCreateInfo info = {0};
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    info.stage = stage;
    info.num_samplers = samplers_used;
    info.num_uniform_buffers = uniforms;
    if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.code = msl;
        info.code_size = msl_size - 1;
        info.entrypoint = "main0";
    } else if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = spirv;
        info.code_size = spirv_size;
        info.entrypoint = "main";
    } else {
        fputs("render: the GPU takes neither Metal nor SPIR-V shaders\n", stderr);
        exit(1);
    }
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    if (!shader) render_error("shader");
    return shader;
}

static SDL_GPUTexture *create_texture(int width, int height, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = (Uint32)width;
    info.height = (Uint32)height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);
    if (!texture) render_error("texture");
    return texture;
}

static int free_slot(void) {
    for (int i = 1; i < TEXTURE_LIMIT; ++i)
        if (!textures[i].used) return i;
    fputs("render: out of texture slots\n", stderr);
    exit(1);
}

int render_init(void) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) render_error("video");
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, 0, NULL);
    if (!device) render_error("device");
    window = SDL_CreateWindow("Dark Chronicle", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!window || !SDL_ClaimWindowForGPUDevice(device, window)) render_error("window");
    SDL_GPUPresentMode modes[] = {SDL_GPU_PRESENTMODE_MAILBOX, SDL_GPU_PRESENTMODE_IMMEDIATE};
    for (int i = 0; i < 2; ++i) {
        if (!SDL_WindowSupportsGPUPresentMode(device, window, modes[i])) continue;
        SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, modes[i]);
        break;
    }
    depth_format = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                       ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
                       : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    vertex_shader = create_shader(draw_vert_spirv, sizeof draw_vert_spirv, draw_vert_msl, sizeof draw_vert_msl,
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    mesh_shader = create_shader(mesh_vert_spirv, sizeof mesh_vert_spirv, mesh_vert_msl, sizeof mesh_vert_msl,
                                SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    fragment_shader = create_shader(draw_frag_spirv, sizeof draw_frag_spirv, draw_frag_msl, sizeof draw_frag_msl,
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    for (int i = 0; i < 8; ++i) {
        SDL_GPUSamplerCreateInfo info = {0};
        info.min_filter = info.mag_filter = (i & 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        info.address_mode_u = (i & 2) ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v = (i & 4) ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplers[i] = SDL_CreateGPUSampler(device, &info);
        if (!samplers[i]) render_error("sampler");
    }
    SDL_GPUBufferCreateInfo buffer = {SDL_GPU_BUFFERUSAGE_VERTEX, VERTEX_BYTES, 0};
    vertex_buffer = SDL_CreateGPUBuffer(device, &buffer);
    SDL_GPUTransferBufferCreateInfo transfer = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, VERTEX_BYTES, 0};
    vertex_transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
    vertex_data = malloc(VERTEX_BYTES);
    if (!vertex_buffer || !vertex_transfer || !vertex_data) render_error("vertex buffer");
    vertex_cycle = 1;
    bone_count = 1;
    blank = render_texture(1, 1);
    uint32_t white = 0xffffffff;
    render_upload(blank, 0, 0, 1, 1, &white);
    return 1;
}

void render_shutdown(void) {
    if (!device) return;
    if (commands) SDL_SubmitGPUCommandBuffer(commands);
    commands = NULL;
    SDL_WaitForGPUIdle(device);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);
    device = NULL;
}

int render_poll(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_EVENT_QUIT) return 0;
    return 1;
}

static uint32_t blend_factor(int c, int complement) {
    static const SDL_GPUBlendFactor factors[3][2] = {
        {SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA},
        {SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA},
        {SDL_GPU_BLENDFACTOR_CONSTANT_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR},
    };
    return factors[c > 2 ? 2 : c][complement];
}

static void blend_equation(const RenderState *state, uint32_t *source, uint32_t *destination, uint32_t *op,
                           uint32_t *flags) {
    int a = state->alpha & 3, b = (state->alpha >> 2) & 3, c = (state->alpha >> 4) & 3, d = (state->alpha >> 6) & 3;
    uint32_t f = blend_factor(c, 0), g = blend_factor(c, 1);
    uint32_t one = SDL_GPU_BLENDFACTOR_ONE, zero = SDL_GPU_BLENDFACTOR_ZERO;
    uint32_t add = SDL_GPU_BLENDOP_ADD, sub = SDL_GPU_BLENDOP_SUBTRACT, reverse = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    if (a > 2) a = 2;
    if (b > 2) b = 2;
    if (d > 2) d = 2;
    *op = add;
    if (!state->blend || (a == b && d == 0)) {
        *source = one, *destination = zero;
        return;
    }
    if (a == b) {
        *source = zero, *destination = d == 1 ? one : zero;
        return;
    }
    int scalable = c != 1;
    if (a == 0 && b == 1) {
        if (d == 1) *source = f, *destination = g;
        else if (d == 2) *source = f, *destination = f, *op = sub;
        else {
            *source = one, *destination = f, *op = sub;
            if (scalable) *flags |= SCALE_SOURCE;
        }
    } else if (a == 1 && b == 0) {
        if (d == 0) *source = g, *destination = f;
        else if (d == 2) *source = f, *destination = f, *op = reverse;
        else *source = f, *destination = one, *op = reverse;
    } else if (a == 0 && b == 2) {
        if (d == 1) *source = f, *destination = one;
        else if (d == 2) *source = f, *destination = zero;
        else {
            *source = one, *destination = zero;
            if (scalable) *flags |= SCALE_SOURCE;
        }
    } else if (a == 1 && b == 2) {
        if (d == 0) *source = one, *destination = f;
        else if (d == 2) *source = zero, *destination = f;
        else if (scalable) *source = SDL_GPU_BLENDFACTOR_DST_COLOR, *destination = one, *flags |= FACTOR_COLOR;
        else *source = zero, *destination = one;
    } else if (a == 2 && b == 0) {
        if (d == 1) *source = f, *destination = one, *op = reverse;
        else if (d == 0) *source = g, *destination = zero;
        else *source = zero, *destination = zero;
    } else {
        if (d == 0) *source = one, *destination = f, *op = sub;
        else if (d == 1) *source = zero, *destination = g;
        else *source = zero, *destination = zero;
    }
}

static SDL_GPUGraphicsPipeline *pipeline_for(uint32_t key) {
    for (int i = 0; i < pipeline_count; ++i)
        if (pipelines[i].key == key) return pipelines[i].pipeline;
    if (pipeline_count == PIPELINE_LIMIT) {
        fputs("render: out of pipelines\n", stderr);
        exit(1);
    }
    int mesh = (key & KEY_MESH) != 0;
    SDL_GPUVertexBufferDescription buffer = {0, mesh ? sizeof(MeshVertex) : sizeof(RenderVertex),
                                             SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute flat[4] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(RenderVertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(RenderVertex, u)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(RenderVertex, r)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(RenderVertex, fog)},
    };
    SDL_GPUVertexAttribute model[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(MeshVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(MeshVertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(MeshVertex, uv)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(MeshVertex, color)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(MeshVertex, bone)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(MeshVertex, weight)},
    };
    SDL_GPUColorTargetDescription target = {0};
    target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    target.blend_state.enable_blend = true;
    target.blend_state.src_color_blendfactor = (SDL_GPUBlendFactor)(key & 15);
    target.blend_state.dst_color_blendfactor = (SDL_GPUBlendFactor)((key >> 4) & 15);
    target.blend_state.color_blend_op = (SDL_GPUBlendOp)((key >> 8) & 7);
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.enable_color_write_mask = true;
    target.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)((key >> 11) & 15);
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    info.vertex_shader = mesh ? mesh_shader : vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = mesh ? model : flat;
    info.vertex_input_state.num_vertex_attributes = mesh ? 6 : 4;
    static const SDL_GPUPrimitiveType topologies[3] = {
        SDL_GPU_PRIMITIVETYPE_POINTLIST, SDL_GPU_PRIMITIVETYPE_LINELIST, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST};
    info.primitive_type = topologies[(key >> 15) & 3];
    info.rasterizer_state.fill_mode = (key & KEY_WIRE) ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    static const SDL_GPUCullMode culls[3] = {SDL_GPU_CULLMODE_NONE, SDL_GPU_CULLMODE_FRONT, SDL_GPU_CULLMODE_BACK};
    info.rasterizer_state.cull_mode = culls[((key >> KEY_CULL_SHIFT) & 3) % 3];
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.compare_op = (SDL_GPUCompareOp)((key >> 17) & 15);
    info.depth_stencil_state.enable_depth_write = (key >> 21) & 1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = depth_format;
    info.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline) render_error("pipeline");
    pipelines[pipeline_count].key = key;
    pipelines[pipeline_count].pipeline = pipeline;
    port_stats.pipelines = (uint32_t)pipeline_count + 1;
    pipeline_count++;
    return pipeline;
}

static void flush(void) {
    if (!pending_count) return;
    if (held_valid && pending_count < COMMAND_LIMIT) pending[pending_count++] = held;
    held_valid = 0;
    SDL_GPUCommandBuffer *buffer = command_buffer();
    uint32_t size = vertex_used - vertex_flushed;
    unsigned char *mapped = SDL_MapGPUTransferBuffer(device, vertex_transfer, vertex_cycle);
    if (!mapped) render_error("map vertices");
    memcpy(mapped + vertex_flushed, vertex_data + vertex_flushed, size);
    SDL_UnmapGPUTransferBuffer(device, vertex_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    SDL_GPUTransferBufferLocation from = {vertex_transfer, vertex_flushed};
    SDL_GPUBufferRegion to = {vertex_buffer, vertex_flushed, size};
    SDL_UploadToGPUBuffer(copy, &from, &to, vertex_cycle);
    SDL_EndGPUCopyPass(copy);
    vertex_cycle = 0;
    Texture *target = &textures[bound];
    int shared = 0;
    while (shared < 8 && depths[shared].texture &&
           (depths[shared].width != target->width || depths[shared].height != target->height))
        shared++;
    if (shared == 8) render_error("depth buffers");
    if (!depths[shared].texture) {
        depths[shared].texture = create_texture(target->width, target->height, depth_format,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        depths[shared].width = target->width;
        depths[shared].height = target->height;
    }
    SDL_GPUColorTargetInfo color = {0};
    color.texture = target->color;
    color.load_op = target->drawn ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    color.clear_color.a = 1.0f;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {0};
    depth.texture = depths[shared].texture;
    depth.load_op = depths[shared].cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    depths[shared].cleared = 1;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = depth.load_op;
    depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
    target->drawn = 1;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(buffer, &color, 1, &depth);
    pass_serial++;
    port_stats.render_passes++;
    port_stats.gpu_commands += (uint32_t)pending_count;
    float scale[4] = {2.0f / (float)target->width, 2.0f / (float)target->height, 0.0f, 0.0f};
    int scale_pushed = 0;
    for (int i = 0; i < pending_count; ++i) {
        Command *command = &pending[i];
        SDL_GPUBufferBinding binding = {vertex_buffer, command->offset};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        if (command->model >= 0) {
            SDL_PushGPUVertexUniformData(buffer, 0, &models[command->model], sizeof models[0]);
            SDL_PushGPUVertexUniformData(buffer, 1, bone_sets[command->bones], sizeof bone_sets[0]);
            scale_pushed = 0;
        } else if (!scale_pushed) {
            SDL_PushGPUVertexUniformData(buffer, 0, scale, sizeof scale);
            scale_pushed = 1;
        }
        if (i == 0 || command->key != pending[i - 1].key) port_stats.pipeline_changes++;
        SDL_BindGPUGraphicsPipeline(pass, pipeline_for(command->key));
        SDL_SetGPUScissor(pass, &command->scissor);
        SDL_FColor constant = {command->blend_constant, command->blend_constant, command->blend_constant,
                               command->blend_constant};
        SDL_SetGPUBlendConstants(pass, constant);
        SDL_GPUTextureSamplerBinding sampled = {textures[command->texture].color, samplers[command->sampler]};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampled, 1);
        SDL_PushGPUFragmentUniformData(buffer, 0, &command->fragment, sizeof command->fragment);
        SDL_DrawGPUPrimitives(pass, (Uint32)command->count, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    pending_count = 0;
    model_count = 0;
    bone_count = 1;
    vertex_flushed = vertex_used;
}

static void flush_if_used(int texture) {
    for (int i = 0; i < pending_count; ++i)
        if (pending[i].texture == texture) {
            flush();
            return;
        }
}

int render_target(uint32_t base, int width, int height) {
    if (width < TARGET_WIDTH) width = TARGET_WIDTH;
    if (height < TARGET_HEIGHT) height = TARGET_HEIGHT;
    for (int i = 1; i < TEXTURE_LIMIT; ++i) {
        Texture *texture = &textures[i];
        if (!texture->used || !texture->target || texture->base != base) continue;
        if (texture->width >= width && texture->height >= height) return i;
        if (bound == i) flush();
        render_release(i);
        break;
    }
    int slot = free_slot();
    Texture *texture = &textures[slot];
    texture->color = create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    texture->width = width;
    texture->height = height;
    texture->base = base;
    texture->target = 1;
    texture->used = 1;
    texture->drawn = 0;
    port_stats.target_creates++;
    port_stats.textures_live++;
    return slot;
}

int render_texture(int width, int height) {
    int slot = free_slot();
    Texture *texture = &textures[slot];
    texture->color = create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    texture->width = width;
    texture->height = height;
    texture->base = 0;
    texture->target = 0;
    texture->used = 1;
    texture->drawn = 1;
    port_stats.textures_live++;
    return slot;
}

void render_release(int slot) {
    if (slot <= 0 || slot >= TEXTURE_LIMIT || !textures[slot].used) return;
    flush_if_used(slot);
    if (bound == slot) {
        flush();
        bound = 0;
    }
    SDL_ReleaseGPUTexture(device, textures[slot].color);
    memset(&textures[slot], 0, sizeof textures[slot]);
    port_stats.textures_live--;
}

int render_width(int slot) { return textures[slot].width; }

struct SDL_GPUDevice *render_device(void) { return device; }

struct SDL_Window *render_window(void) { return window; }

struct SDL_GPUTexture *render_gpu_texture(int slot) {
    return slot > 0 && slot < TEXTURE_LIMIT && textures[slot].used ? textures[slot].color : NULL;
}

int render_height(int slot) { return textures[slot].height; }

void render_upload(int slot, int x, int y, int width, int height, const uint32_t *pixels) {
    if (width <= 0 || height <= 0) return;
    flush_if_used(slot);
    if (bound == slot) flush();
    uint32_t size = (uint32_t)(width * height * 4);
    port_stats.texture_uploads++;
    port_stats.upload_bytes += size;
    SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    memcpy(mapped, pixels, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command_buffer());
    SDL_GPUTextureTransferInfo from = {transfer, 0, (Uint32)width, (Uint32)height};
    SDL_GPUTextureRegion to = {textures[slot].color, 0, 0, (Uint32)x, (Uint32)y, 0, (Uint32)width, (Uint32)height, 1};
    SDL_UploadToGPUTexture(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    textures[slot].drawn = 1;
}

void render_download(int slot, int x, int y, int width, int height, uint32_t *pixels) {
    if (width <= 0 || height <= 0) return;
    flush();
    uint32_t size = (uint32_t)(width * height * 4);
    port_stats.downloads++;
    SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command_buffer());
    SDL_GPUTextureRegion from = {textures[slot].color, 0, 0, (Uint32)x, (Uint32)y, 0, (Uint32)width, (Uint32)height, 1};
    SDL_GPUTextureTransferInfo to = {transfer, 0, (Uint32)width, (Uint32)height};
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    commands = NULL;
    vertex_cycle = 1;
    vertex_used = vertex_flushed = 0;
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    memcpy(pixels, mapped, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
}

void render_copy(int source, int source_x, int source_y, int destination, int destination_x, int destination_y,
                 int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (bound == source || bound == destination) flush();
    flush_if_used(destination);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command_buffer());
    SDL_GPUTextureLocation from = {textures[source].color, 0, 0, (Uint32)source_x, (Uint32)source_y, 0};
    SDL_GPUTextureLocation to = {textures[destination].color, 0, 0, (Uint32)destination_x, (Uint32)destination_y, 0};
    SDL_CopyGPUTextureToTexture(copy, &from, &to, (Uint32)width, (Uint32)height, 1, false);
    SDL_EndGPUCopyPass(copy);
    textures[destination].drawn = 1;
}

void render_download_depth(int target, int x, int y, int width, int height, uint32_t *pixels) {
    if (width <= 0 || height <= 0) return;
    memset(pixels, 0, (size_t)width * (size_t)height * sizeof *pixels);
    int found = -1;
    for (int i = 0; i < 8 && target; ++i)
        if (depths[i].texture && depths[i].width == textures[target].width &&
            depths[i].height == textures[target].height)
            found = i;
    if (found < 0) return;
    flush();
    int previous = bound;
    int source = free_slot();
    memset(&textures[source], 0, sizeof textures[source]);
    textures[source].color = depths[found].texture;
    textures[source].width = depths[found].width;
    textures[source].height = depths[found].height;
    textures[source].used = 1;
    textures[source].drawn = 1;
    int tile = render_texture(DEPTH_TILE, DEPTH_TILE);
    uint32_t tile_pixels[DEPTH_TILE * DEPTH_TILE];
    for (int tile_y = 0; tile_y < height; tile_y += DEPTH_TILE)
        for (int tile_x = 0; tile_x < width; tile_x += DEPTH_TILE) {
            int w = width - tile_x < DEPTH_TILE ? width - tile_x : DEPTH_TILE;
            int h = height - tile_y < DEPTH_TILE ? height - tile_y : DEPTH_TILE;
            float u0 = (float)(x + tile_x) / (float)depths[found].width;
            float u1 = (float)(x + tile_x + w) / (float)depths[found].width;
            float v0 = (float)(y + tile_y) / (float)depths[found].height;
            float v1 = (float)(y + tile_y + h) / (float)depths[found].height;
            RenderVertex a = {0, 0, 0, u0, v0}, b = {(float)w, 0, 0, u1, v0};
            RenderVertex c = {0, (float)h, 0, u0, v1}, d = {(float)w, (float)h, 0, u1, v1};
            RenderVertex quad[6] = {a, b, c, b, d, c};
            RenderState state;
            memset(&state, 0, sizeof state);
            state.texture = source;
            state.decal = 1;
            state.depth_read = 1;
            state.scissor[2] = w - 1;
            state.scissor[3] = h - 1;
            render_bind(tile);
            render_source = RENDER_SOURCE_DEPTH;
            render_draw(&state, RENDER_TRIANGLES, quad, 6);
            render_download(tile, 0, 0, w, h, tile_pixels);
            for (int row = 0; row < h; ++row)
                for (int column = 0; column < w; ++column)
                    pixels[(tile_y + row) * width + tile_x + column] = tile_pixels[row * w + column] & 0xffffff;
        }
    render_release(tile);
    memset(&textures[source], 0, sizeof textures[source]);
    render_bind(previous);
}

void render_bind(int target) {
    if (target == bound) return;
    flush();
    bound = target;
}

int render_bound(void) { return bound; }

static SDL_GPUCompareOp depth_compare(uint64_t test) {
    if (!((test >> 16) & 1)) return SDL_GPU_COMPAREOP_ALWAYS;
    static const SDL_GPUCompareOp ops[4] = {SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_ALWAYS,
                                            SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_GREATER};
    return ops[(test >> 17) & 3];
}

static int prepare(Command *command, const RenderState *state, uint32_t key_extra) {
    if (state->texture && state->texture == bound) {
        fputs("render: a draw samples its own target\n", stderr);
        return 0;
    }
    uint32_t source, destination, op, flags = 0;
    blend_equation(state, &source, &destination, &op, &flags);
    uint32_t mask = 0;
    if ((state->color_mask & 0x000000ff) != 0x000000ff) mask |= SDL_GPU_COLORCOMPONENT_R;
    if ((state->color_mask & 0x0000ff00) != 0x0000ff00) mask |= SDL_GPU_COLORCOMPONENT_G;
    if ((state->color_mask & 0x00ff0000) != 0x00ff0000) mask |= SDL_GPU_COLORCOMPONENT_B;
    if ((state->color_mask & 0xff000000) != 0xff000000) mask |= SDL_GPU_COLORCOMPONENT_A;
    memset(command, 0, sizeof *command);
    command->key = source | destination << 4 | op << 8 | mask << 11 | (uint32_t)depth_compare(state->test) << 17 |
                   (uint32_t)(state->depth_write != 0) << 21 | key_extra;
    int textured = state->texture && !(render_toggles.no_textures && !state->depth_read);
    if (textured) flags |= TEXTURED;
    if (state->texa) flags |= TEXA;
    if ((state->texa_register >> 15) & 1) flags |= AEM;
    if (state->fog && !render_toggles.no_fog) flags |= FOG;
    if (state->test & 1) flags |= ALPHA_TEST;
    if (((state->alpha >> 4) & 3) == 2) flags |= FACTOR_FIX;
    if (state->decal) flags |= DECAL;
    if (state->vertex_alpha) flags |= VERTEX_ALPHA;
    if (state->depth_read) flags |= DEPTH_READ;
    uint32_t fix = (uint32_t)((state->alpha >> 32) & 255);
    command->fragment.flags = flags;
    command->fragment.alpha = (uint32_t)((state->test >> 4) & 255) | (uint32_t)((state->test >> 1) & 7) << 8 |
                              fix << 16 | (uint32_t)(state->texa_register & 255) << 24;
    command->fragment.fog_color = state->fog_color;
    command->blend_constant = fix >= 128 ? 1.0f : (float)fix / 128.0f;
    command->texture = textured ? state->texture : blank;
    if (!state->depth_read) {
        if (render_toggles.overdraw) {
            command->key = (command->key & ~0x7fffu) | SDL_GPU_BLENDFACTOR_ONE | SDL_GPU_BLENDFACTOR_ONE << 4 |
                           SDL_GPU_BLENDOP_ADD << 8 | 7 << 11;
            command->fragment.flags |= OVERDRAW;
        }
        if (render_toggles.wireframe && ((key_extra >> 15) & 3) == RENDER_TRIANGLES) command->key |= KEY_WIRE;
    }
    command->sampler = (state->linear ? 1 : 0) | (state->repeat_u ? 2 : 0) | (state->repeat_v ? 4 : 0);
    command->model = -1;
    Texture *target = &textures[bound];
    int x0 = state->scissor[0] < 0 ? 0 : state->scissor[0];
    int y0 = state->scissor[1] < 0 ? 0 : state->scissor[1];
    int x1 = state->scissor[2] >= target->width ? target->width - 1 : state->scissor[2];
    int y1 = state->scissor[3] >= target->height ? target->height - 1 : state->scissor[3];
    command->scissor.x = x0;
    command->scissor.y = y0;
    command->scissor.w = x1 >= x0 ? x1 - x0 + 1 : 0;
    command->scissor.h = y1 >= y0 ? y1 - y0 + 1 : 0;
    return 1;
}

static int store_vertices(Command *command, const void *vertices, uint32_t bytes, int count) {
    if (vertex_used + bytes > VERTEX_BYTES || pending_count == COMMAND_LIMIT) {
        fputs("render: too many vertices in one frame\n", stderr);
        return 0;
    }
    command->offset = vertex_used;
    command->count = count;
    memcpy(vertex_data + vertex_used, vertices, bytes);
    vertex_used += bytes;
    return 1;
}

static void append(const Command *command) {
    pending[pending_count++] = *command;
    command_serial++;
}

static void highlight(const Command *command) {
    Command solid = *command;
    solid.key = (command->key & KEY_KEEP) | SDL_GPU_BLENDFACTOR_ONE | SDL_GPU_BLENDFACTOR_ZERO << 4 |
                SDL_GPU_BLENDOP_ADD << 8 | 15 << 11 | (uint32_t)SDL_GPU_COMPAREOP_ALWAYS << 17;
    solid.fragment.flags = SOLID;
    solid.texture = blank;
    held = solid;
    held_valid = 1;
}

static void record(int source, const RenderState *state, int topology, int cull, int count,
                   const RenderVertex *vertices, int command, int merged, const MeshUniforms *uniforms) {
    if (record_count == RECORD_LIMIT) {
        record_dropped++;
        return;
    }
    RenderRecord *entry = &records[record_count++];
    memset(entry, 0, sizeof *entry);
    entry->source = source;
    entry->topology = topology;
    entry->count = count;
    entry->cull = cull;
    entry->target = bound;
    entry->target_width = textures[bound].width;
    entry->target_height = textures[bound].height;
    entry->target_base = textures[bound].base;
    entry->state = *state;
    entry->command = command < 0 ? command : command - command_base;
    entry->pass = pass_serial - pass_base;
    entry->merged = merged;
    if (uniforms) entry->uniforms = *uniforms;
    if (!vertices) return;
    entry->bounds[0] = entry->bounds[2] = vertices[0].x;
    entry->bounds[1] = entry->bounds[3] = vertices[0].y;
    for (int i = 1; i < count; ++i) {
        if (vertices[i].x < entry->bounds[0]) entry->bounds[0] = vertices[i].x;
        if (vertices[i].y < entry->bounds[1]) entry->bounds[1] = vertices[i].y;
        if (vertices[i].x > entry->bounds[2]) entry->bounds[2] = vertices[i].x;
        if (vertices[i].y > entry->bounds[3]) entry->bounds[3] = vertices[i].y;
    }
}

void render_draw(const RenderState *state, int topology, const RenderVertex *vertices, int count) {
    if (!bound || count <= 0) return;
    port_stats.draw_calls++;
    port_stats.vertices += (uint32_t)count;
    int index = frame_draws++;
    if (index >= render_toggles.skip_from && index < render_toggles.skip_to) return;
    int source = render_source;
    Command command;
    if (!prepare(&command, state, (uint32_t)topology << 15) ||
        !store_vertices(&command, vertices, (uint32_t)(count * (int)sizeof(RenderVertex)), count)) {
        if (capturing) record(source, state, topology, 0, count, vertices, -1, 0, NULL);
        return;
    }
    Command *last = pending_count ? &pending[pending_count - 1] : NULL;
    int merged = last && last->model < 0 && last->offset + (uint32_t)last->count * sizeof(RenderVertex) == command.offset &&
                 last->key == command.key && last->texture == command.texture && last->sampler == command.sampler &&
                 last->blend_constant == command.blend_constant &&
                 !memcmp(&last->fragment, &command.fragment, sizeof last->fragment) &&
                 !memcmp(&last->scissor, &command.scissor, sizeof last->scissor);
    if (merged) last->count += count;
    else append(&command);
    if (capturing) record(source, state, topology, 0, count, vertices, command_serial - 1, merged, NULL);
    if (index == render_highlight) highlight(&command);
}

void render_mesh(const RenderState *state, int cull, const MeshUniforms *uniforms, const float *bones,
                 const MeshVertex *vertices, int count) {
    if (!bound || count <= 0 || render_toggles.no_models) return;
    port_stats.mesh_calls++;
    port_stats.vertices += (uint32_t)count;
    int index = frame_draws++;
    if (index >= render_toggles.skip_from && index < render_toggles.skip_to) return;
    if (model_count == MODEL_LIMIT || (bones && bone_count == BONE_LIMIT)) flush();
    Command command;
    uint32_t key = KEY_MESH | (uint32_t)RENDER_TRIANGLES << 15 | (uint32_t)cull << KEY_CULL_SHIFT;
    if (!prepare(&command, state, key) ||
        !store_vertices(&command, vertices, (uint32_t)(count * (int)sizeof(MeshVertex)), count)) {
        if (capturing) record(RENDER_SOURCE_MESH, state, RENDER_TRIANGLES, cull, count, NULL, -1, 0, uniforms);
        return;
    }
    command.model = model_count;
    models[model_count++] = *uniforms;
    command.bones = 0;
    if (bones) {
        command.bones = bone_count;
        memcpy(bone_sets[bone_count++], bones, sizeof bone_sets[0]);
    }
    append(&command);
    if (capturing) record(RENDER_SOURCE_MESH, state, RENDER_TRIANGLES, cull, count, NULL, command_serial - 1, 0, uniforms);
    if (index == render_highlight) highlight(&command);
}

void render_capture_begin(void) {
    if (!records) records = malloc(RECORD_LIMIT * sizeof *records);
    if (!records) return;
    record_count = 0;
    record_dropped = 0;
    command_base = command_serial;
    pass_base = pass_serial;
    capturing = 1;
}

void render_capture_end(void) { capturing = 0; }

int render_capturing(void) { return capturing; }

int render_capture_count(void) { return record_count; }

int render_capture_dropped(void) { return record_dropped; }

int render_capture_commands(void) { return command_serial - command_base; }

int render_capture_passes(void) { return pass_serial - pass_base; }

const RenderRecord *render_capture_records(void) { return records; }

int render_slot_limit(void) { return TEXTURE_LIMIT; }

int render_slot(int slot, RenderSlot *info) {
    if (slot <= 0 || slot >= TEXTURE_LIMIT || !textures[slot].used) return 0;
    info->used = 1;
    info->target = textures[slot].target;
    info->width = textures[slot].width;
    info->height = textures[slot].height;
    info->base = textures[slot].base;
    info->drawn = textures[slot].drawn;
    return 1;
}

void render_present(int target, int width, int height) {
    flush();
    frame_draws = 0;
    SDL_GPUCommandBuffer *buffer = command_buffer();
    SDL_GPUTexture *swapchain = NULL;
    Uint32 window_width = 0, window_height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(buffer, window, &swapchain, &window_width, &window_height))
        render_error("swapchain");
    if (swapchain && target) {
        float fit = (float)window_width / 4.0f < (float)window_height / 3.0f ? (float)window_width / 4.0f
                                                                             : (float)window_height / 3.0f;
        SDL_GPUBlitInfo blit = {0};
        blit.source.texture = textures[target].color;
        blit.source.w = (Uint32)width;
        blit.source.h = (Uint32)height;
        blit.destination.texture = swapchain;
        blit.destination.w = (Uint32)(fit * 4.0f);
        blit.destination.h = (Uint32)(fit * 3.0f);
        blit.destination.x = (window_width - blit.destination.w) / 2;
        blit.destination.y = (window_height - blit.destination.h) / 2;
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(buffer, &blit);
    }
    SDL_SubmitGPUCommandBuffer(buffer);
    commands = NULL;
    vertex_cycle = 1;
    vertex_used = vertex_flushed = 0;
}
