#include "render.h"
#include "port_mod.h"
#include "port_stats.h"
#include "smooth.h"
#include "shaders.h"
#include "icon.h"
#include "touch.h"
#include <SDL3/SDL.h>
#if defined(__APPLE__) && !defined(SDL_PLATFORM_IOS)
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEXTURE_LIMIT = 4096, PIPELINE_LIMIT = 1024, COMMAND_LIMIT = 65536, MODEL_LIMIT = 16384, BONE_LIMIT = 2048,
       VERTEX_BYTES = 64 * 1024 * 1024 };
enum { RECORD_LIMIT = 32768 };
enum { SCALE_LIMIT = 4, SAMPLE_LIMIT = 8, WIDEN_LIMIT = 1024 };
#define MODEL_BYTES ((Uint32)(MODEL_LIMIT * sizeof(MeshUniforms)))
#define BONE_BYTES ((Uint32)(BONE_LIMIT * RENDER_BONE_SLOTS * sizeof(RenderBone)))
#ifdef SDL_PLATFORM_IOS
#define WINDOW_FLAGS (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY)
#else
#define WINDOW_FLAGS (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)
#endif

#define PIXEL_ASPECT 1.25f

enum { LAYOUT_WIDTH = 512, LAYOUT_HEIGHT = 480, FRAME_WIDTH_LIMIT = 1024 };

enum { WINDOW_WIDTH = 960, WINDOW_HEIGHT = 720, TARGET_WIDTH = 640, TARGET_HEIGHT = 512, DEPTH_TILE = 64 };

enum {
    TEXTURED = 1, TEXA = 2, AEM = 4, FOG = 8, ALPHA_TEST = 16, FACTOR_FIX = 32,
    SCALE_SOURCE = 64, FACTOR_COLOR = 128, DECAL = 256, VERTEX_ALPHA = 512, DEPTH_READ = 1024, SOLID = 2048, OVERDRAW = 4096,
    DEST_TEST = 8192, DEST_SET = 16384, TINT = 32768
};

typedef struct {
    SDL_GPUTexture *color;
    SDL_GPUTexture *multi;
    int width;
    int height;
    int scale;
    int samples;
    uint32_t base;
    int target;
    int scaled;
    int stale;
    int used;
    int drawn;
    int queued;
    int sampled;
} Texture;

typedef union {
    uint32_t value;
    struct {
        uint32_t source : 4;
        uint32_t destination : 4;
        uint32_t op : 3;
        uint32_t mask : 4;
        uint32_t topology : 2;
        uint32_t depth_compare : 4;
        uint32_t depth_write : 1;
        uint32_t mesh : 1;
        uint32_t cull : 2;
        uint32_t wire : 1;
        uint32_t samples : 2;
        uint32_t mirror : 1;
    };
} PipelineKey;

typedef struct {
    PipelineKey key;
    SDL_GPUGraphicsPipeline *pipeline;
} Pipeline;

typedef struct {
    uint32_t flags;
    uint32_t alpha_reference;
    uint32_t alpha_method;
    uint32_t blend_fix;
    uint32_t texture_alpha;
    uint32_t fog_color;
    uint32_t tint;
    uint32_t unused;
} FragmentState;

typedef struct {
    PipelineKey key;
    FragmentState fragment;
    float blend_constant;
    int texture;
    int resident;
    SDL_GPUTexture *color;
    SDL_GPUTexture *destination;
    int sampler;
    SDL_Rect scissor;
    uint32_t offset;
    int count;
    int model;
    int bones;
    uint64_t signature;
    int pair;
    int shadow;
    int follow;
    uint32_t copy;
    int moving;
    int target;
} Command;

enum { STEP_PASS, STEP_COPY, STEP_BLIT };

#define SNAP_FRACTION 0.25f
#define CUT_DISTANCE 40.0f
#define CUT_TURN 0.3f
#define BONE_FLIP 1.6f
#define DRAW_JUMP 64.0f
#define CLAIM_NEAR 0x40000000
#define ATTACH_GAP 0.25f
#define ATTACH_REACH 400.0f
#define EXACT_FRACTION 1e-6f
#define SCREEN_WEIGHT 1e-7f

static void forget_recordings(void);
static void multiply(float *out, const float *a, const float *b);
static int invert(float *out, const float *matrix);
static int grow(void **items, int *capacity, int needed, size_t size);

typedef struct {
    int kind;
    SDL_GPUTexture *color;
    SDL_GPUTexture *multi;
    int depth;
    int width;
    int height;
    SDL_GPULoadOp color_load;
    SDL_GPULoadOp depth_load;
    int mirror;
    int first;
    int count;
    SDL_GPUTextureLocation from;
    SDL_GPUTextureLocation to;
    Uint32 copy_width;
    Uint32 copy_height;
    SDL_GPUBlitInfo blit;
} Step;

typedef struct {
    RenderScreenMapping mapping;
    float depth;
} RenderFollow;

typedef struct {
    const void *key;
    float place[3];
    float world[16];
    float camera[16];
    float light[16];
    float screen[16];
    RenderScreenMapping mapping;
    float depth;
    int first;
    int count;
    int prism_first;
    int match;
    int model;
    int bone;
    float offset[16];
} ShadowRecord;

typedef struct {
    Step *steps;
    int step_count;
    int step_capacity;
    Command *commands;
    int command_count;
    int command_capacity;
    MeshUniforms *models;
    uint64_t *keys;
    int *model_bones;
    int *matches;
    int model_count;
    int model_capacity;
    RenderBone (*bones)[RENDER_BONE_SLOTS];
    int bone_count;
    int bone_capacity;
    unsigned char *vertices;
    uint32_t vertex_used;
    uint32_t vertex_capacity;
    int paired;
    uint32_t moving_bytes;
    ShadowRecord *shadows;
    int shadow_count;
    int shadow_capacity;
    int prism_count;
    float (*shapes)[4];
    int shape_count;
    int shape_capacity;
    int shadows_matched;
    ShadowPoint *points;
    uint32_t point_capacity;
    RenderFollow *follows;
    int follow_count;
    int follow_capacity;
    float eye[3];
    float pose[9];
    float view[16];
    int has_camera;
    SDL_GPUTexture *shown;
    int shown_target;
    int shown_width;
    int shown_height;
    int shown_scale;
    int valid;
} Recording;

static Recording recordings[3];
static int kept[2];
static int kept_now;
static struct {
    SDL_GPUTexture *texture;
    uint64_t frame;
} *graveyard;
static int graveyard_count;
static int graveyard_capacity;
static struct {
    int first;
    int count;
    uint64_t frame;
} *mesh_graveyard;
static int mesh_graveyard_count;
static int mesh_graveyard_capacity;
static int recording_slot;
static int shown_slot = 1;
static int previous_slot = 2;
static int smoothing;
static SDL_GPUBuffer *replay_buffer;
static SDL_GPUTransferBuffer *replay_transfer;
static SDL_GPUBuffer *moving_buffer;
static SDL_GPUTransferBuffer *moving_transfer;
static uint32_t moving_capacity;
static int replay_uploaded;
static MeshUniforms *replay_models;
static int replay_model_capacity;
static RenderBone (*replay_bones)[RENDER_BONE_SLOTS];
static int replay_bone_capacity;

enum { TOUCH_SHAPE_LIMIT = 256, TOUCH_MARGIN = 2 };

enum { SCREEN_GAME, SCREEN_DEBUG, SCREEN_LIMIT, SCREEN_POOL = 3 };

typedef struct {
    SDL_Window *window;
    SDL_GPUTexture *pool[SCREEN_POOL];
    Uint32 pool_width[SCREEN_POOL];
    Uint32 pool_height[SCREEN_POOL];
    SDL_GPUTextureFormat pool_format[SCREEN_POOL];
    int published;
    int reading;
    uint64_t serial;
    uint64_t shown;
    int scale;
    int width;
    int height;
    int pixel_width;
    int pixel_height;
    TouchShape shapes[TOUCH_SHAPE_LIMIT];
    int shape_count;
} Screen;

static Screen screens[SCREEN_LIMIT] = {{.published = -1, .reading = -1}, {.published = -1, .reading = -1}};

static SDL_Window *window;
static SDL_GPUDevice *device;
static SDL_GPUShader *vertex_shader;
static SDL_GPUShader *mesh_shader;
static MeshUniforms models[MODEL_LIMIT];
static int model_count;
static RenderBone bone_sets[BONE_LIMIT][RENDER_BONE_SLOTS];
static int bone_count;
static SDL_GPUShader *fragment_shader;
static SDL_GPUSampler *samplers[8];
static SDL_GPUBuffer *vertex_buffer;
static SDL_GPUTransferBuffer *vertex_transfer;
static SDL_GPUBuffer *model_buffer;
static SDL_GPUBuffer *bone_buffer;
static SDL_GPUTransferBuffer *model_transfer;
static SDL_GPUBuffer *replay_model_buffer;
static SDL_GPUBuffer *replay_bone_buffer;
static SDL_GPUTransferBuffer *replay_model_transfer;
static uint32_t replay_storage_capacity;
static int model_flushed;
static int bone_flushed;
static int model_cycle;
static SDL_GPUCommandBuffer *commands;
static SDL_Mutex *screen_lock;
static Uint32 wake_event;
static SDL_GPUTextureFormat depth_format;
static struct {
    SDL_GPUTexture *texture;
    SDL_GPUTexture *color;
    SDL_GPUTexture *resolved;
    int width;
    int height;
    int samples;
    int cleared;
} depths[8];
static Texture textures[TEXTURE_LIMIT];
static Pipeline pipelines[PIPELINE_LIMIT];
static int pipeline_count;
static Command pending[COMMAND_LIMIT];
static Command ordered[COMMAND_LIMIT];
static int pending_count;
enum { SPARE_LIMIT = 64, SPARE_FRAMES = 3 };
static struct {
    SDL_GPUTexture *texture;
    int width;
    int height;
    uint64_t frame;
} spares[SPARE_LIMIT];
static uint64_t presented;
enum { MESH_VERTEX_LIMIT = 1 << 20, MESH_FREE_LIMIT = 8192 };
static SDL_GPUBuffer *mesh_buffer;
static struct {
    int first;
    int count;
} mesh_free[MESH_FREE_LIMIT];
static int mesh_free_count;
static struct {
    int first;
    int count;
    MeshVertex *data;
} *mesh_uploads;
static int mesh_upload_count;
static int mesh_upload_capacity;
static struct {
    int first;
    int count;
} *mesh_retired;
static int mesh_retired_count;
static int mesh_retired_capacity;
static unsigned char *vertex_data;
static ShadowPoint *point_data;
static const ShadowPoint *shadow_points;
static int drawing_shadow = -1;
static int drawing_follow = -1;
static uint32_t vertex_used;
static uint32_t vertex_flushed;
static int vertex_cycle;
static int bound;
static int drawing;
static int blank;
static int fill_screen;
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
static int quality_scale = 1;
static int quality_samples = 1;
static int wanted_scale = 1;
static int wanted_samples = 1;
static int auto_scale;
static int frame_width = LAYOUT_WIDTH;
static int window_width = WINDOW_WIDTH;
static int window_height = WINDOW_HEIGHT;
static int fullscreen;
static int vsync;
static int nearest;
static int background;
static int restore_slot;

RenderToggles render_toggles;
int render_source;
int render_highlight = -1;

static void render_error(const char *what) {
    fprintf(stderr, "render: %s: %s\n", what, SDL_GetError());
    exit(1);
}

enum { TIMED_LIMIT = 64 };

static struct {
    SDL_GPUFence *fence;
    uint64_t submitted;
    SDL_AtomicInt done;
} timed[TIMED_LIMIT];
static int timed_pushed;
static int timed_released;
static int timed_waited;
static SDL_Mutex *timed_lock;
static SDL_Condition *timed_ready;
static SDL_Thread *timer;
static int timer_stop;
static SDL_AtomicInt gpu_busy_ns;

static int time_fences(void *unused) {
    (void)unused;
    uint64_t finished = 0;
    for (;;) {
        SDL_LockMutex(timed_lock);
        while (timed_waited == timed_pushed && !timer_stop) SDL_WaitCondition(timed_ready, timed_lock);
        if (timed_waited == timed_pushed) {
            SDL_UnlockMutex(timed_lock);
            return 0;
        }
        int slot = timed_waited % TIMED_LIMIT;
        SDL_UnlockMutex(timed_lock);
        SDL_WaitForGPUFences(device, true, &timed[slot].fence, 1);
        uint64_t now = SDL_GetTicksNS();
        uint64_t began = timed[slot].submitted > finished ? timed[slot].submitted : finished;
        SDL_AddAtomicInt(&gpu_busy_ns, (int)(now - began));
        finished = now;
        SDL_SetAtomicInt(&timed[slot].done, 1);
        SDL_LockMutex(timed_lock);
        timed_waited++;
        SDL_UnlockMutex(timed_lock);
    }
}

static void release_timed(void) {
    while (timed_released < timed_pushed && SDL_GetAtomicInt(&timed[timed_released % TIMED_LIMIT].done)) {
        SDL_ReleaseGPUFence(device, timed[timed_released % TIMED_LIMIT].fence);
        timed_released++;
    }
}

static SDL_GPUFence *submit_timed(SDL_GPUCommandBuffer *buffer) {
    release_timed();
    if (!timer || timed_pushed - timed_released == TIMED_LIMIT) {
        SDL_SubmitGPUCommandBuffer(buffer);
        return NULL;
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(buffer);
    if (!fence) return NULL;
    int slot = timed_pushed % TIMED_LIMIT;
    timed[slot].fence = fence;
    timed[slot].submitted = SDL_GetTicksNS();
    SDL_SetAtomicInt(&timed[slot].done, 0);
    SDL_LockMutex(timed_lock);
    timed_pushed++;
    SDL_SignalCondition(timed_ready);
    SDL_UnlockMutex(timed_lock);
    return fence;
}

static void stop_timer(void) {
    if (!timer) return;
    SDL_LockMutex(timed_lock);
    timer_stop = 1;
    SDL_SignalCondition(timed_ready);
    SDL_UnlockMutex(timed_lock);
    SDL_WaitThread(timer, NULL);
    timer = NULL;
    release_timed();
}

static SDL_GPUCommandBuffer *command_buffer(void) {
    if (!commands) commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) render_error("command buffer");
    return commands;
}

static SDL_GPUShader *create_shader(const unsigned char *spirv, size_t spirv_size, const unsigned char *msl,
                                    size_t msl_size, SDL_GPUShaderStage stage, int samplers_used, int uniforms,
                                    int storage) {
    SDL_GPUShaderCreateInfo info = {0};
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    info.stage = stage;
    info.num_samplers = samplers_used;
    info.num_uniform_buffers = uniforms;
    info.num_storage_buffers = storage;
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

static SDL_GPUSampleCount sample_count(int samples) {
    return samples >= 8 ? SDL_GPU_SAMPLECOUNT_8
           : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4
           : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2
                          : SDL_GPU_SAMPLECOUNT_1;
}

static int supported_samples(int samples) {
    while (samples > 1 &&
           !(SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, sample_count(samples)) &&
             SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, sample_count(samples)) &&
             SDL_GPUTextureSupportsSampleCount(device, depth_format, sample_count(samples))))
        samples /= 2;
    return samples;
}

static SDL_GPUTexture *create_texture(int width, int height, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                                      int samples) {
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = (Uint32)width;
    info.height = (Uint32)height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = sample_count(samples);
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);
    if (!texture) render_error("texture");
    return texture;
}

static void apply_present_mode(void) {
    SDL_GPUPresentMode modes[] = {SDL_GPU_PRESENTMODE_MAILBOX, SDL_GPU_PRESENTMODE_IMMEDIATE,
                                  SDL_GPU_PRESENTMODE_VSYNC};
    for (int i = vsync ? 2 : 0; i < 3; ++i) {
        if (!SDL_WindowSupportsGPUPresentMode(device, window, modes[i])) continue;
        SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, modes[i]);
        return;
    }
}

static void release_later(SDL_GPUTexture *texture) {
    if (!texture) return;
    if (!smoothing || !grow((void **)&graveyard, &graveyard_capacity, graveyard_count + 1, sizeof *graveyard)) {
        SDL_ReleaseGPUTexture(device, texture);
        return;
    }
    graveyard[graveyard_count].texture = texture;
    graveyard[graveyard_count].frame = presented;
    graveyard_count++;
}

static void release_mesh_range(int first, int count);

static void bury(uint64_t before) {
    int kept = 0;
    for (int i = 0; i < graveyard_count; ++i) {
        if (graveyard[i].frame + 2 <= before) SDL_ReleaseGPUTexture(device, graveyard[i].texture);
        else graveyard[kept++] = graveyard[i];
    }
    graveyard_count = kept;
    kept = 0;
    for (int i = 0; i < mesh_graveyard_count; ++i) {
        if (mesh_graveyard[i].frame + 2 <= before) release_mesh_range(mesh_graveyard[i].first, mesh_graveyard[i].count);
        else mesh_graveyard[kept++] = mesh_graveyard[i];
    }
    mesh_graveyard_count = kept;
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
    timed_lock = SDL_CreateMutex();
    screen_lock = SDL_CreateMutex();
    wake_event = SDL_RegisterEvents(1);
    timed_ready = SDL_CreateCondition();
    timer = SDL_CreateThread(time_fences, "gpu timer", NULL);
    window = SDL_CreateWindow("Dark Chronicle", window_width, window_height,
                              WINDOW_FLAGS | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window || !SDL_ClaimWindowForGPUDevice(device, window)) render_error("window");
    screens[SCREEN_GAME].window = window;
    port_window_icon(window);
    int pixel_width = 0, pixel_height = 0;
    if (fill_screen && SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) && pixel_width > 0 && pixel_height > 0)
        render_set_aspect(SDL_max(pixel_width, pixel_height), SDL_min(pixel_width, pixel_height));
    if (background) render_send_back(window);
    apply_present_mode();
    depth_format = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
                       ? SDL_GPU_TEXTUREFORMAT_D24_UNORM
                       : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    quality_scale = wanted_scale;
    quality_samples = wanted_samples = supported_samples(wanted_samples);
    vertex_shader = create_shader(draw_vert_spirv, sizeof draw_vert_spirv, draw_vert_msl, sizeof draw_vert_msl,
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0);
    mesh_shader = create_shader(mesh_vert_spirv, sizeof mesh_vert_spirv, mesh_vert_msl, sizeof mesh_vert_msl,
                                SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 2);
    fragment_shader = create_shader(draw_frag_spirv, sizeof draw_frag_spirv, draw_frag_msl, sizeof draw_frag_msl,
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, 0);
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
    SDL_GPUBufferCreateInfo meshes = {SDL_GPU_BUFFERUSAGE_VERTEX, MESH_VERTEX_LIMIT * (Uint32)sizeof(MeshVertex), 0};
    mesh_buffer = SDL_CreateGPUBuffer(device, &meshes);
    mesh_free[0].first = 0;
    mesh_free[0].count = MESH_VERTEX_LIMIT;
    mesh_free_count = mesh_buffer ? 1 : 0;
    SDL_GPUTransferBufferCreateInfo transfer = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, VERTEX_BYTES, 0};
    vertex_transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
    vertex_data = malloc(VERTEX_BYTES);
    if (!vertex_buffer || !vertex_transfer || !vertex_data) render_error("vertex buffer");
    SDL_GPUBufferCreateInfo model_info = {SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, MODEL_BYTES, 0};
    model_buffer = SDL_CreateGPUBuffer(device, &model_info);
    SDL_GPUBufferCreateInfo bone_info = {SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BONE_BYTES, 0};
    bone_buffer = SDL_CreateGPUBuffer(device, &bone_info);
    SDL_GPUTransferBufferCreateInfo model_upload = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, MODEL_BYTES + BONE_BYTES, 0};
    model_transfer = SDL_CreateGPUTransferBuffer(device, &model_upload);
    if (!model_buffer || !bone_buffer || !model_transfer) render_error("model buffer");
    vertex_cycle = 1;
    model_cycle = 1;
    bone_count = 1;
    blank = render_texture(1, 1);
    uint32_t white = 0xffffffff;
    render_upload(blank, 0, 0, 1, 1, &white);
    render_finish();
    return 1;
}

void render_shutdown(void) {
    if (!device) return;
    SDL_WaitForGPUIdle(device);
    stop_timer();
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);
    device = NULL;
}

static uint32_t blend_factor(int c, int complement) {
    static const SDL_GPUBlendFactor factors[3][2] = {
        {SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA},
        {SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA},
        {SDL_GPU_BLENDFACTOR_CONSTANT_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR},
    };
    return factors[c > RENDER_BLEND_FIX ? RENDER_BLEND_FIX : c][complement];
}

static void blend_equation(const RenderState *state, uint32_t *source, uint32_t *destination, uint32_t *op,
                           uint32_t *flags) {
    enum { S = RENDER_BLEND_SOURCE, D = RENDER_BLEND_DESTINATION, Z = RENDER_BLEND_ZERO };
    int a = state->blending.a, b = state->blending.b, c = state->blending.c, d = state->blending.d;
    uint32_t f = blend_factor(c, 0), g = blend_factor(c, 1);
    uint32_t one = SDL_GPU_BLENDFACTOR_ONE, zero = SDL_GPU_BLENDFACTOR_ZERO;
    uint32_t add = SDL_GPU_BLENDOP_ADD, sub = SDL_GPU_BLENDOP_SUBTRACT, reverse = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    if (a > Z) a = Z;
    if (b > Z) b = Z;
    if (d > Z) d = Z;
    *op = add;
    if (!state->blend || (a == b && d == S)) {
        *source = one, *destination = zero;
        return;
    }
    if (a == b) {
        *source = zero, *destination = d == D ? one : zero;
        return;
    }
    int scalable = c != RENDER_BLEND_DESTINATION;
    if (a == S && b == D) {
        if (d == D) *source = f, *destination = g;
        else if (d == Z) *source = f, *destination = f, *op = sub;
        else {
            *source = one, *destination = f, *op = sub;
            if (scalable) *flags |= SCALE_SOURCE;
        }
    } else if (a == D && b == S) {
        if (d == S) *source = g, *destination = f;
        else if (d == Z) *source = f, *destination = f, *op = reverse;
        else *source = f, *destination = one, *op = reverse;
    } else if (a == S && b == Z) {
        if (d == D) *source = f, *destination = one;
        else if (d == Z) *source = f, *destination = zero;
        else {
            *source = one, *destination = zero;
            if (scalable) *flags |= SCALE_SOURCE;
        }
    } else if (a == D && b == Z) {
        if (d == S) *source = one, *destination = f;
        else if (d == Z) *source = zero, *destination = f;
        else if (scalable) *source = SDL_GPU_BLENDFACTOR_DST_COLOR, *destination = one, *flags |= FACTOR_COLOR;
        else *source = zero, *destination = one;
    } else if (a == Z && b == S) {
        if (d == D) *source = f, *destination = one, *op = reverse;
        else if (d == S) *source = g, *destination = zero;
        else *source = zero, *destination = zero;
    } else {
        if (d == S) *source = one, *destination = f, *op = sub;
        else if (d == D) *source = zero, *destination = g;
        else *source = zero, *destination = zero;
    }
}

static SDL_GPUGraphicsPipeline *pipeline_for(PipelineKey key) {
    for (int i = 0; i < pipeline_count; ++i)
        if (pipelines[i].key.value == key.value) return pipelines[i].pipeline;
    if (pipeline_count == PIPELINE_LIMIT) {
        fputs("render: out of pipelines\n", stderr);
        exit(1);
    }
    int mesh = key.mesh;
    SDL_GPUVertexBufferDescription buffer = {0, mesh ? sizeof(MeshVertex) : sizeof(RenderVertex),
                                             SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute flat[5] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(RenderVertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(RenderVertex, u)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(RenderVertex, r)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(RenderVertex, fog)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(RenderVertex, limits)},
    };
    SDL_GPUVertexAttribute model[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(MeshVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(MeshVertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(MeshVertex, uv)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(MeshVertex, color)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(MeshVertex, bone)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(MeshVertex, weight)},
    };
    SDL_GPUColorTargetDescription targets[2];
    memset(targets, 0, sizeof targets);
    SDL_GPUColorTargetDescription *target = &targets[0];
    target->format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    target->blend_state.enable_blend = true;
    target->blend_state.src_color_blendfactor = (SDL_GPUBlendFactor)key.source;
    target->blend_state.dst_color_blendfactor = (SDL_GPUBlendFactor)key.destination;
    target->blend_state.color_blend_op = (SDL_GPUBlendOp)key.op;
    target->blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target->blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    target->blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    target->blend_state.enable_color_write_mask = true;
    target->blend_state.color_write_mask = (SDL_GPUColorComponentFlags)key.mask;
    SDL_GPUSampleCount samples = (SDL_GPUSampleCount)key.samples;
    targets[1].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    targets[1].blend_state.enable_color_write_mask = true;
    targets[1].blend_state.color_write_mask = key.depth_write ? SDL_GPU_COLORCOMPONENT_R : 0;
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    info.vertex_shader = mesh ? mesh_shader : vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = mesh ? model : flat;
    info.vertex_input_state.num_vertex_attributes = mesh ? 6 : 5;
    static const SDL_GPUPrimitiveType topologies[3] = {
        SDL_GPU_PRIMITIVETYPE_POINTLIST, SDL_GPU_PRIMITIVETYPE_LINELIST, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST};
    info.primitive_type = topologies[key.topology];
    info.rasterizer_state.fill_mode = key.wire ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    static const SDL_GPUCullMode culls[3] = {SDL_GPU_CULLMODE_NONE, SDL_GPU_CULLMODE_FRONT, SDL_GPU_CULLMODE_BACK};
    info.rasterizer_state.cull_mode = culls[key.cull % 3];
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.compare_op = (SDL_GPUCompareOp)key.depth_compare;
    info.depth_stencil_state.enable_depth_write = key.depth_write;
    info.multisample_state.sample_count = samples;
    info.target_info.color_target_descriptions = targets;
    info.target_info.num_color_targets = key.mirror ? 2 : 1;
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

static int depth_for(int slot) {
    Texture *target = &textures[slot];
    for (int i = 0; i < 8; ++i)
        if (depths[i].texture && depths[i].width == target->width * target->scale &&
            depths[i].height == target->height * target->scale && depths[i].samples == target->samples)
            return i;
    return -1;
}

static void create_depth(int index, int width, int height, int samples) {
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (samples == 1) usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    depths[index].texture = create_texture(width, height, depth_format, usage, samples);
    if (samples > 1) {
        depths[index].color = create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
                                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, samples);
        depths[index].resolved = create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
                                                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, 1);
    }
    depths[index].width = width;
    depths[index].height = height;
    depths[index].samples = samples;
    depths[index].cleared = 0;
}

static void release_depths(void) {
    for (int i = 0; i < 8; ++i) {
        if (depths[i].texture) SDL_ReleaseGPUTexture(device, depths[i].texture);
        if (depths[i].color) SDL_ReleaseGPUTexture(device, depths[i].color);
        if (depths[i].resolved) SDL_ReleaseGPUTexture(device, depths[i].resolved);
    }
    memset(depths, 0, sizeof depths);
}

static int writes_depth(const Command *list, int count) {
    for (int i = 0; i < count; ++i)
        if (list[i].key.depth_write) return 1;
    return 0;
}

static void upload_meshes(SDL_GPUCommandBuffer *buffer) {
    if (!mesh_upload_count) return;
    uint32_t total = 0;
    for (int i = 0; i < mesh_upload_count; ++i) total += (uint32_t)mesh_uploads[i].count * sizeof(MeshVertex);
    SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, total, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    unsigned char *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    uint32_t offset = 0;
    for (int i = 0; i < mesh_upload_count; ++i) {
        uint32_t bytes = (uint32_t)mesh_uploads[i].count * sizeof(MeshVertex);
        memcpy(mapped + offset, mesh_uploads[i].data, bytes);
        offset += bytes;
    }
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    offset = 0;
    for (int i = 0; i < mesh_upload_count; ++i) {
        uint32_t bytes = (uint32_t)mesh_uploads[i].count * sizeof(MeshVertex);
        SDL_GPUTransferBufferLocation from = {transfer, offset};
        SDL_GPUBufferRegion to = {mesh_buffer, (uint32_t)mesh_uploads[i].first * sizeof(MeshVertex), bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
        offset += bytes;
        free(mesh_uploads[i].data);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    port_stats.upload_bytes += total;
    mesh_upload_count = 0;
}

int render_mesh_store(const MeshVertex *vertices, int count) {
    if (count <= 0) return -1;
    if (mesh_upload_count == mesh_upload_capacity) {
        int capacity = mesh_upload_capacity ? mesh_upload_capacity * 2 : 256;
        void *grown = realloc(mesh_uploads, (size_t)capacity * sizeof *mesh_uploads);
        if (!grown) return -1;
        mesh_uploads = grown;
        mesh_upload_capacity = capacity;
    }
    MeshVertex *data = malloc((size_t)count * sizeof *data);
    if (!data) return -1;
    for (int i = 0; i < mesh_free_count; ++i) {
        if (mesh_free[i].count < count) continue;
        int first = mesh_free[i].first;
        mesh_free[i].first += count;
        mesh_free[i].count -= count;
        if (!mesh_free[i].count) {
            memmove(&mesh_free[i], &mesh_free[i + 1], (size_t)(mesh_free_count - i - 1) * sizeof mesh_free[0]);
            mesh_free_count--;
        }
        memcpy(data, vertices, (size_t)count * sizeof *data);
        mesh_uploads[mesh_upload_count].first = first;
        mesh_uploads[mesh_upload_count].count = count;
        mesh_uploads[mesh_upload_count].data = data;
        mesh_upload_count++;
        return first;
    }
    free(data);
    return -1;
}

static void release_mesh_range(int first, int count) {
    int at = 0;
    while (at < mesh_free_count && mesh_free[at].first < first) at++;
    int joins_previous = at > 0 && mesh_free[at - 1].first + mesh_free[at - 1].count == first;
    int joins_next = at < mesh_free_count && first + count == mesh_free[at].first;
    if (joins_previous && joins_next) {
        mesh_free[at - 1].count += count + mesh_free[at].count;
        memmove(&mesh_free[at], &mesh_free[at + 1], (size_t)(mesh_free_count - at - 1) * sizeof mesh_free[0]);
        mesh_free_count--;
    } else if (joins_previous) {
        mesh_free[at - 1].count += count;
    } else if (joins_next) {
        mesh_free[at].first = first;
        mesh_free[at].count += count;
    } else if (mesh_free_count < MESH_FREE_LIMIT) {
        memmove(&mesh_free[at + 1], &mesh_free[at], (size_t)(mesh_free_count - at) * sizeof mesh_free[0]);
        mesh_free[at].first = first;
        mesh_free[at].count = count;
        mesh_free_count++;
    }
}

static void release_retired_meshes(void) {
    for (int i = 0; i < mesh_retired_count; ++i) release_mesh_range(mesh_retired[i].first, mesh_retired[i].count);
    mesh_retired_count = 0;
}

void render_mesh_forget(int first, int count) {
    if (first < 0 || count <= 0) return;
    if (smoothing &&
        grow((void **)&mesh_graveyard, &mesh_graveyard_capacity, mesh_graveyard_count + 1, sizeof *mesh_graveyard)) {
        mesh_graveyard[mesh_graveyard_count].first = first;
        mesh_graveyard[mesh_graveyard_count].count = count;
        mesh_graveyard[mesh_graveyard_count].frame = presented;
        mesh_graveyard_count++;
        return;
    }
    if (!pending_count) {
        release_mesh_range(first, count);
        return;
    }
    if (mesh_retired_count == mesh_retired_capacity) {
        int capacity = mesh_retired_capacity ? mesh_retired_capacity * 2 : 256;
        void *grown = realloc(mesh_retired, (size_t)capacity * sizeof *mesh_retired);
        if (!grown) return;
        mesh_retired = grown;
        mesh_retired_capacity = capacity;
    }
    mesh_retired[mesh_retired_count].first = first;
    mesh_retired[mesh_retired_count].count = count;
    mesh_retired_count++;
}

static void encode(SDL_GPUCommandBuffer *buffer, SDL_GPURenderPass *pass, const Command *list, int count,
                   SDL_GPUBuffer *model_list, SDL_GPUBuffer *bone_list, SDL_GPUBuffer *drawn, int width, int height,
                   int mirror) {
    float scale[4] = {2.0f / (float)width, 2.0f / (float)height, 0.0f, 0.0f};
    SDL_GPUBuffer *bound_buffer = NULL;
    SDL_GPUGraphicsPipeline *last_pipeline = NULL;
    const Command *last = NULL;
    int vertex_uniforms = -2, storage_bound = 0;
    for (int i = 0; i < count; ++i) {
        const Command *command = &list[i];
        SDL_GPUBuffer *source = command->resident ? mesh_buffer : command->moving ? moving_buffer : drawn;
        if (source != bound_buffer) {
            SDL_GPUBufferBinding binding = {source, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
            bound_buffer = source;
        }
        if (command->model >= 0) {
            if (!storage_bound) {
                SDL_GPUBuffer *storage[2] = {model_list, bone_list};
                SDL_BindGPUVertexStorageBuffers(pass, 0, storage, 2);
                storage_bound = 1;
            }
        } else if (vertex_uniforms != -1) {
            SDL_PushGPUVertexUniformData(buffer, 0, scale, sizeof scale);
            vertex_uniforms = -1;
        }
        PipelineKey key = command->key;
        key.mirror = mirror != 0;
        SDL_GPUGraphicsPipeline *pipeline = pipeline_for(key);
        if (pipeline != last_pipeline) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            last_pipeline = pipeline;
            port_stats.pipeline_changes++;
        }
        if (!last || memcmp(&command->scissor, &last->scissor, sizeof command->scissor))
            SDL_SetGPUScissor(pass, &command->scissor);
        if (!last || command->blend_constant != last->blend_constant) {
            SDL_FColor constant = {command->blend_constant, command->blend_constant, command->blend_constant,
                                   command->blend_constant};
            SDL_SetGPUBlendConstants(pass, constant);
        }
        if (!last || command->color != last->color || command->sampler != last->sampler ||
            command->destination != last->destination) {
            SDL_GPUTextureSamplerBinding sampled[2] = {{command->color, samplers[command->sampler]},
                                                       {command->destination, samplers[0]}};
            SDL_BindGPUFragmentSamplers(pass, 0, sampled, 2);
        }
        if (!last || memcmp(&command->fragment, &last->fragment, sizeof command->fragment))
            SDL_PushGPUFragmentUniformData(buffer, 0, &command->fragment, sizeof command->fragment);
        uint32_t stride = command->key.mesh ? sizeof(MeshVertex) : sizeof(RenderVertex);
        SDL_DrawGPUPrimitives(pass, (Uint32)command->count, 1, command->offset / stride,
                              command->model >= 0 ? (Uint32)command->model : 0);
        last = command;
    }
}

static int grow(void **items, int *capacity, int needed, size_t size) {
    if (needed <= *capacity) return 1;
    int wanted = *capacity ? *capacity : 256;
    while (wanted < needed) wanted *= 2;
    void *grown = realloc(*items, (size_t)wanted * size);
    if (!grown) return 0;
    *items = grown;
    *capacity = wanted;
    return 1;
}

static Step *new_step(Recording *frame) {
    if (!grow((void **)&frame->steps, &frame->step_capacity, frame->step_count + 1, sizeof *frame->steps)) return NULL;
    Step *step = &frame->steps[frame->step_count++];
    memset(step, 0, sizeof *step);
    return step;
}

static void record_model(Recording *frame, Command *kept, const Command *command, int target) {
    int model = frame->model_count;
    if (!grow((void **)&frame->models, &frame->model_capacity, model + 1, sizeof *frame->models)) return;
    frame->keys = realloc(frame->keys, (size_t)frame->model_capacity * sizeof *frame->keys);
    frame->model_bones = realloc(frame->model_bones, (size_t)frame->model_capacity * sizeof *frame->model_bones);
    frame->matches = realloc(frame->matches, (size_t)frame->model_capacity * sizeof *frame->matches);
    if (!frame->keys || !frame->model_bones || !frame->matches) return;
    frame->models[model] = models[command->model];
    frame->keys[model] = 0;
    uint64_t edge = (uint64_t)models[command->model].edge;
    if (command->resident)
        frame->keys[model] = (((uint64_t)command->offset << 24 | (uint64_t)command->count) ^
                              ((uint64_t)command->key.value << 24 | (uint64_t)target) * 0x9e3779b97f4a7c15ull ^
                              edge * 0xc2b2ae3d27d4eb4full) | 1;
    frame->model_bones[model] = 0;
    if (command->bones > 0 &&
        grow((void **)&frame->bones, &frame->bone_capacity, frame->bone_count + 1, sizeof *frame->bones)) {
        memcpy(frame->bones[frame->bone_count], bone_sets[command->bones], sizeof frame->bones[0]);
        frame->model_bones[model] = frame->bone_count++;
    }
    kept->model = model;
    kept->bones = frame->model_bones[model];
    frame->models[model].flags.bone_set = (uint32_t)frame->model_bones[model];
    frame->model_count++;
}

static int keep_points(Recording *frame, const Command *kept, const Command *command) {
    uint32_t first = kept->offset / sizeof(RenderVertex), needed = first + (uint32_t)kept->count;
    if (needed > frame->point_capacity) {
        uint32_t wanted = frame->point_capacity ? frame->point_capacity : 1 << 14;
        while (wanted < needed) wanted *= 2;
        ShadowPoint *grown = realloc(frame->points, wanted * sizeof *grown);
        if (!grown) return 0;
        frame->points = grown;
        frame->point_capacity = wanted;
    }
    memcpy(frame->points + first, point_data + command->offset / sizeof(RenderVertex), (size_t)kept->count * sizeof *point_data);
    return 1;
}

static uint64_t draw_signature(const Command *command, const SDL_GPUTexture *target) {
    uint64_t signature = 0xcbf29ce484222325ull;
    uint64_t parts[6] = {command->key.value, (uint64_t)(uint32_t)command->texture, (uint64_t)(uint32_t)command->count,
                         (uint64_t)(uintptr_t)target, (uint64_t)(uintptr_t)command->destination,
                         (uint64_t)(uint32_t)command->sampler};
    for (int i = 0; i < 6; ++i) signature = (signature ^ parts[i]) * 0x100000001b3ull;
    return signature | 1;
}

static void record_pass(const Texture *target, int depth, SDL_GPULoadOp color_load, SDL_GPULoadOp depth_load, int mirror,
                        const Command *list, int count) {
    Recording *frame = &recordings[recording_slot];
    if (!grow((void **)&frame->commands, &frame->command_capacity, frame->command_count + count,
              sizeof *frame->commands))
        return;
    Step *step = new_step(frame);
    if (!step) return;
    step->kind = STEP_PASS;
    step->color = target->color;
    step->multi = target->samples > 1 ? target->multi : NULL;
    step->depth = depth;
    step->width = target->width;
    step->height = target->height;
    step->color_load = color_load;
    step->depth_load = depth_load;
    step->mirror = mirror;
    step->first = frame->command_count;
    for (int i = 0; i < count; ++i) {
        const Command *command = &list[i];
        Command *kept = &frame->commands[frame->command_count++];
        *kept = *command;
        kept->signature = command->model < 0 && !command->resident && command->shadow < 0 && !textures[command->texture].target
                              ? draw_signature(command, target->color)
                              : 0;
        kept->pair = -1;
        kept->copy = command->offset;
        if (!command->resident) {
            uint32_t stride = command->key.mesh ? sizeof(MeshVertex) : sizeof(RenderVertex);
            uint32_t start = (frame->vertex_used + stride - 1) / stride * stride;
            uint32_t bytes = (uint32_t)command->count * stride;
            if (start + bytes > VERTEX_BYTES) {
                frame->command_count--;
                continue;
            }
            if (start + bytes > frame->vertex_capacity) {
                uint32_t wanted = frame->vertex_capacity ? frame->vertex_capacity : 1 << 20;
                while (wanted < start + bytes) wanted *= 2;
                unsigned char *grown = realloc(frame->vertices, wanted);
                if (!grown) {
                    frame->command_count--;
                    continue;
                }
                frame->vertices = grown;
                frame->vertex_capacity = wanted;
            }
            memcpy(frame->vertices + start, vertex_data + command->offset, bytes);
            kept->offset = start;
            kept->copy = start;
            frame->vertex_used = start + bytes;
            if (command->shadow >= 0 && !keep_points(frame, kept, command)) kept->shadow = -1;
        }
        if (command->model >= 0) record_model(frame, kept, command, target->width << 12 | target->height);
    }
    step->count = frame->command_count - step->first;
}

static void record_blit(int source, int x, int y, int width, int height, int destination, int to_x, int to_y) {
    if (!smoothing) return;
    Step *step = new_step(&recordings[recording_slot]);
    if (!step) return;
    int from = textures[source].scale, to = textures[destination].scale;
    step->kind = STEP_BLIT;
    step->blit.source.texture = textures[source].color;
    step->blit.source.x = (Uint32)(x * from);
    step->blit.source.y = (Uint32)(y * from);
    step->blit.source.w = (Uint32)(width * from);
    step->blit.source.h = (Uint32)(height * from);
    step->blit.destination.texture = textures[destination].color;
    step->blit.destination.x = (Uint32)(to_x * to);
    step->blit.destination.y = (Uint32)(to_y * to);
    step->blit.destination.w = (Uint32)(width * to);
    step->blit.destination.h = (Uint32)(height * to);
    step->blit.load_op = SDL_GPU_LOADOP_LOAD;
    step->blit.filter = SDL_GPU_FILTER_NEAREST;
}

static SDL_GPUTextureLocation whole(SDL_GPUTexture *texture) {
    return (SDL_GPUTextureLocation){texture, 0, 0, 0, 0, 0};
}

static void copy_region(SDL_GPUCommandBuffer *buffer, SDL_GPUTextureLocation from, SDL_GPUTextureLocation to,
                        Uint32 width, Uint32 height) {
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(buffer);
    SDL_CopyGPUTextureToTexture(pass, &from, &to, width, height, 1, false);
    SDL_EndGPUCopyPass(pass);
}

static void copy_texture(SDL_GPUTextureLocation from, SDL_GPUTextureLocation to, Uint32 width, Uint32 height) {
    copy_region(command_buffer(), from, to, width, height);
    if (!smoothing) return;
    Step *step = new_step(&recordings[recording_slot]);
    if (!step) return;
    step->kind = STEP_COPY;
    step->from = from;
    step->to = to;
    step->copy_width = width;
    step->copy_height = height;
}

static void upload_models(SDL_GPUCopyPass *copy) {
    if (model_flushed == model_count && bone_flushed == bone_count) return;
    unsigned char *mapped = SDL_MapGPUTransferBuffer(device, model_transfer, model_cycle);
    if (!mapped) render_error("map models");
    uint32_t first = (uint32_t)model_flushed * sizeof *models, bytes = (uint32_t)(model_count - model_flushed) * sizeof *models;
    uint32_t bone_first = (uint32_t)bone_flushed * sizeof bone_sets[0], bone_bytes = (uint32_t)(bone_count - bone_flushed) * sizeof bone_sets[0];
    memcpy(mapped + first, &models[model_flushed], bytes);
    memcpy(mapped + MODEL_BYTES + bone_first, bone_sets[bone_flushed], bone_bytes);
    SDL_UnmapGPUTransferBuffer(device, model_transfer);
    if (bytes) {
        SDL_GPUTransferBufferLocation from = {model_transfer, first};
        SDL_GPUBufferRegion to = {model_buffer, first, bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, model_cycle);
    }
    if (bone_bytes) {
        SDL_GPUTransferBufferLocation from = {model_transfer, MODEL_BYTES + bone_first};
        SDL_GPUBufferRegion to = {bone_buffer, bone_first, bone_bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, model_cycle);
    }
    model_cycle = 0;
    model_flushed = model_count;
    bone_flushed = bone_count;
}

static void restart_models(void) {
    model_cycle = 1;
    model_count = model_flushed = 0;
    bone_count = 1;
    bone_flushed = 0;
}

static void restart_buffers(void) {
    vertex_cycle = 1;
    vertex_used = vertex_flushed = 0;
    restart_models();
}

static void encode_pass(SDL_GPUCommandBuffer *buffer, const Command *list, int count) {
    Texture *target = &textures[list[0].target];
    int shared = depth_for(list[0].target);
    if (shared < 0) {
        shared = 0;
        while (shared < 8 && depths[shared].texture) shared++;
        if (shared == 8) render_error("depth buffers");
        create_depth(shared, target->width * target->scale, target->height * target->scale, target->samples);
    }
    int multi = target->samples > 1;
    int mirror = multi && (writes_depth(list, count) || !depths[shared].cleared);
    SDL_GPUColorTargetInfo color[2];
    memset(color, 0, sizeof color);
    color[0].texture = multi ? target->multi : target->color;
    color[0].load_op = target->drawn ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    color[0].clear_color.a = 1.0f;
    color[0].store_op = multi ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
    color[0].resolve_texture = target->color;
    SDL_GPUDepthStencilTargetInfo depth = {0};
    depth.texture = depths[shared].texture;
    depth.load_op = depths[shared].cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    depths[shared].cleared = 1;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    color[1].texture = depths[shared].color;
    color[1].load_op = depth.load_op;
    color[1].store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
    color[1].resolve_texture = depths[shared].resolved;
    target->drawn = 1;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(buffer, color, mirror ? 2 : 1, &depth);
    pass_serial++;
    port_stats.render_passes++;
    port_stats.gpu_commands += (uint32_t)count;
    if (smoothing) record_pass(target, shared, color[0].load_op, depth.load_op, mirror, list, count);
    encode(buffer, pass, list, count, model_buffer, bone_buffer, vertex_buffer, target->width, target->height, mirror);
    SDL_EndGPURenderPass(pass);
}

static void queue(const Command *command, int step) {
    textures[command->target].queued += step;
    textures[command->texture].sampled += step;
}

static int split_side(void) {
    int side = 0, kept = 0;
    for (int i = 0; i < pending_count; ++i) {
        if (pending[i].target != drawing) ordered[side++] = pending[i];
        else pending[kept++] = pending[i];
    }
    pending_count = kept;
    return side;
}

static SDL_GPUCommandBuffer *upload_pending(void) {
    SDL_GPUCommandBuffer *buffer = command_buffer();
    upload_meshes(buffer);
    uint32_t size = vertex_used - vertex_flushed;
    unsigned char *mapped = SDL_MapGPUTransferBuffer(device, vertex_transfer, vertex_cycle);
    if (!mapped) render_error("map vertices");
    memcpy(mapped + vertex_flushed, vertex_data + vertex_flushed, size);
    SDL_UnmapGPUTransferBuffer(device, vertex_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    SDL_GPUTransferBufferLocation from = {vertex_transfer, vertex_flushed};
    SDL_GPUBufferRegion to = {vertex_buffer, vertex_flushed, size};
    SDL_UploadToGPUBuffer(copy, &from, &to, vertex_cycle);
    upload_models(copy);
    SDL_EndGPUCopyPass(copy);
    vertex_cycle = 0;
    vertex_flushed = vertex_used;
    return buffer;
}

static void encode_passes(SDL_GPUCommandBuffer *buffer, const Command *list, int count) {
    for (int first = 0, last = 0; first < count; first = last) {
        while (last < count && list[last].target == list[first].target) last++;
        encode_pass(buffer, list + first, last - first);
    }
}

static void flush(void) {
    if (!pending_count) return;
    if (held_valid && pending_count < COMMAND_LIMIT) {
        pending[pending_count++] = held;
        queue(&held, 1);
    }
    held_valid = 0;
    SDL_GPUCommandBuffer *buffer = upload_pending();
    int side = split_side();
    encode_passes(buffer, ordered, side);
    encode_passes(buffer, pending, pending_count);
    for (int i = 0; i < side; ++i) queue(&ordered[i], -1);
    for (int i = 0; i < pending_count; ++i) queue(&pending[i], -1);
    pending_count = 0;
    release_retired_meshes();
}

static void flush_side(void) {
    int side = split_side();
    if (!side) return;
    for (int i = 0; i < side; ++i) queue(&ordered[i], -1);
    encode_passes(upload_pending(), ordered, side);
}

static int used_by_pending(int texture) {
    return textures[texture].sampled > 0;
}

static int target_pending(int slot) {
    return textures[slot].queued > 0;
}

static void settle(int slot) {
    if (!target_pending(slot) && !used_by_pending(slot)) return;
    int side = 0;
    for (int i = 0; i < pending_count; ++i) {
        if (pending[i].texture != slot && pending[i].target != slot) continue;
        if (pending[i].target == drawing) {
            flush();
            return;
        }
        side = 1;
    }
    if (side) flush_side();
}

static int touches_depth(const RenderState *state) {
    return state->depth_write || (state->tests.depth_test && state->tests.depth_method != RENDER_DEPTH_ALWAYS);
}

static SDL_GPUTexture *spare_texture(int width, int height) {
    for (int i = 0; i < SPARE_LIMIT; ++i)
        if (spares[i].texture && spares[i].width == width && spares[i].height == height &&
            spares[i].frame + SPARE_FRAMES <= presented) {
            SDL_GPUTexture *texture = spares[i].texture;
            spares[i].texture = NULL;
            return texture;
        }
    return create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                          SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1);
}

static void retire_texture(SDL_GPUTexture *texture, int width, int height) {
    int oldest = 0;
    for (int i = 0; i < SPARE_LIMIT; ++i) {
        if (!spares[i].texture) {
            oldest = i;
            break;
        }
        if (spares[i].frame < spares[oldest].frame) oldest = i;
    }
    if (spares[oldest].texture && spares[oldest].frame == presented) flush();
    if (spares[oldest].texture) release_later(spares[oldest].texture);
    spares[oldest].texture = texture;
    spares[oldest].width = width;
    spares[oldest].height = height;
    spares[oldest].frame = presented;
}

static void rename_texture(int slot) {
    Texture *texture = &textures[slot];
    if (texture->samples > 1 || texture->multi) {
        flush();
        return;
    }
    int width = texture->width * texture->scale, height = texture->height * texture->scale;
    SDL_GPUTexture *fresh = spare_texture(width, height);
    copy_texture(whole(texture->color), whole(fresh), (Uint32)width, (Uint32)height);
    retire_texture(texture->color, width, height);
    texture->color = fresh;
    port_stats.texture_renames++;
}

static SDL_GPUTexture *destination_copy(void) {
    flush();
    Texture *target = &textures[bound];
    int width = target->width * target->scale, height = target->height * target->scale;
    SDL_GPUTexture *copy = spare_texture(width, height);
    copy_texture(whole(target->color), whole(copy), (Uint32)width, (Uint32)height);
    retire_texture(copy, width, height);
    port_stats.texture_renames++;
    return copy;
}

static void create_storage(Texture *texture) {
    int width = texture->width * texture->scale, height = texture->height * texture->scale;
    texture->color = create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1);
    texture->multi = texture->samples > 1 ? create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                           SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, texture->samples)
                                          : NULL;
}

static int new_texture(int width, int height, int scale, int samples) {
    int slot = free_slot();
    Texture *texture = &textures[slot];
    memset(texture, 0, sizeof *texture);
    texture->width = width;
    texture->height = height;
    texture->scale = scale;
    texture->samples = samples;
    texture->used = 1;
    texture->drawn = 1;
    create_storage(texture);
    port_stats.textures_live++;
    return slot;
}

int render_target(uint32_t base, int width, int height) {
    if (width < TARGET_WIDTH) width = TARGET_WIDTH;
    if (width < frame_width) width = frame_width;
    if (height < TARGET_HEIGHT) height = TARGET_HEIGHT;
    for (int i = 1; i < TEXTURE_LIMIT; ++i) {
        Texture *texture = &textures[i];
        if (!texture->used || !texture->target || texture->base != base) continue;
        if (texture->width >= width && texture->height >= height) return i;
        render_release(i);
        break;
    }
    int slot = new_texture(width, height, quality_scale, quality_samples);
    Texture *texture = &textures[slot];
    texture->base = base;
    texture->target = 1;
    texture->scaled = 1;
    texture->drawn = 0;
    port_stats.target_creates++;
    return slot;
}

int render_target_at(uint32_t base) {
    for (int i = 1; i < TEXTURE_LIMIT; ++i)
        if (textures[i].used && textures[i].target && textures[i].base == base) return i;
    return 0;
}

int render_texture(int width, int height) { return new_texture(width, height, 1, 1); }

int render_texture_at_scale(int width, int height, int scale) { return new_texture(width, height, scale, 1); }

int render_scaled_texture(int width, int height) {
    int slot = new_texture(width, height, quality_scale, 1);
    textures[slot].scaled = 1;
    return slot;
}

void render_release(int slot) {
    if (slot <= 0 || slot >= TEXTURE_LIMIT || !textures[slot].used) return;
    settle(slot);
    if (bound == slot) bound = 0;
    release_later(textures[slot].color);
    release_later(textures[slot].multi);
    memset(&textures[slot], 0, sizeof textures[slot]);
    port_stats.textures_live--;
}

int render_width(int slot) { return textures[slot].width; }

int render_scale(int slot) { return textures[slot].scale; }

struct SDL_GPUDevice *render_device(void) { return device; }

struct SDL_Window *render_window(void) { return window; }

struct SDL_GPUTexture *render_gpu_texture(int slot) {
    return slot > 0 && slot < TEXTURE_LIMIT && textures[slot].used ? textures[slot].color : NULL;
}

int render_height(int slot) { return textures[slot].height; }

static void blit(SDL_GPUTexture *source, int x, int y, int width, int height, SDL_GPUTexture *destination, int to_x,
                 int to_y, int to_width, int to_height, SDL_GPUFilter filter) {
    SDL_GPUBlitInfo info = {0};
    info.source.texture = source;
    info.source.x = (Uint32)x;
    info.source.y = (Uint32)y;
    info.source.w = (Uint32)width;
    info.source.h = (Uint32)height;
    info.destination.texture = destination;
    info.destination.x = (Uint32)to_x;
    info.destination.y = (Uint32)to_y;
    info.destination.w = (Uint32)to_width;
    info.destination.h = (Uint32)to_height;
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.filter = filter;
    SDL_BlitGPUTexture(command_buffer(), &info);
}

static void blit_slots(int source, int x, int y, int width, int height, int destination, int to_x, int to_y,
                       SDL_GPUFilter filter) {
    int from = textures[source].scale, to = textures[destination].scale;
    blit(textures[source].color, x * from, y * from, width * from, height * from, textures[destination].color, to_x * to,
         to_y * to, width * to, height * to, filter);
}

static void changed(int slot) {
    textures[slot].drawn = 1;
    textures[slot].stale = textures[slot].samples > 1;
}

static void upload(SDL_GPUTexture *texture, int x, int y, int width, int height, const uint32_t *pixels) {
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
    SDL_GPUTextureRegion to = {texture, 0, 0, (Uint32)x, (Uint32)y, 0, (Uint32)width, (Uint32)height, 1};
    SDL_UploadToGPUTexture(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
}

void render_upload(int slot, int x, int y, int width, int height, const uint32_t *pixels) {
    if (width <= 0 || height <= 0) return;
    settle(slot);
    if (textures[slot].scale == 1) upload(textures[slot].color, x, y, width, height, pixels);
    else {
        int temporary = render_texture(width, height);
        upload(textures[temporary].color, 0, 0, width, height, pixels);
        blit_slots(temporary, 0, 0, width, height, slot, x, y, SDL_GPU_FILTER_NEAREST);
        render_release(temporary);
    }
    changed(slot);
}

void render_upload_full(int slot, const uint32_t *pixels) {
    settle(slot);
    Texture *texture = &textures[slot];
    upload(texture->color, 0, 0, texture->width * texture->scale, texture->height * texture->scale, pixels);
    changed(slot);
}

static void download(SDL_GPUTexture *texture, int x, int y, int width, int height, uint32_t *pixels) {
    uint32_t size = (uint32_t)(width * height * 4);
    port_stats.downloads++;
    SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command_buffer());
    SDL_GPUTextureRegion from = {texture, 0, 0, (Uint32)x, (Uint32)y, 0, (Uint32)width, (Uint32)height, 1};
    SDL_GPUTextureTransferInfo to = {transfer, 0, (Uint32)width, (Uint32)height};
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = submit_timed(commands);
    commands = NULL;
    restart_buffers();
    if (fence) SDL_WaitForGPUFences(device, true, &fence, 1);
    else SDL_WaitForGPUIdle(device);
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    memcpy(pixels, mapped, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
}

void render_download(int slot, int x, int y, int width, int height, uint32_t *pixels) {
    if (width <= 0 || height <= 0) return;
    flush();
    if (textures[slot].scale == 1) {
        download(textures[slot].color, x, y, width, height, pixels);
        return;
    }
    int temporary = render_texture(width, height);
    blit_slots(slot, x, y, width, height, temporary, 0, 0, SDL_GPU_FILTER_LINEAR);
    download(textures[temporary].color, 0, 0, width, height, pixels);
    render_release(temporary);
}

void render_download_full(int slot, int width, int height, uint32_t *pixels) {
    if (width <= 0 || height <= 0) return;
    flush();
    int scale = textures[slot].scale;
    download(textures[slot].color, 0, 0, width * scale, height * scale, pixels);
}

void render_copy(int source, int source_x, int source_y, int destination, int destination_x, int destination_y,
                 int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (target_pending(drawing) && (source == drawing || destination == drawing)) flush();
    if (target_pending(source) || target_pending(destination)) flush_side();
    if (used_by_pending(destination)) rename_texture(destination);
    int scale = textures[source].scale;
    if (scale == textures[destination].scale) {
        SDL_GPUTextureLocation from = {textures[source].color, 0, 0, (Uint32)(source_x * scale),
                                       (Uint32)(source_y * scale), 0};
        SDL_GPUTextureLocation to = {textures[destination].color, 0, 0, (Uint32)(destination_x * scale),
                                     (Uint32)(destination_y * scale), 0};
        copy_texture(from, to, (Uint32)(width * scale), (Uint32)(height * scale));
    } else {
        blit_slots(source, source_x, source_y, width, height, destination, destination_x, destination_y,
                   SDL_GPU_FILTER_NEAREST);
        record_blit(source, source_x, source_y, width, height, destination, destination_x, destination_y);
    }
    changed(destination);
}

void render_download_depth(int target, int x, int y, int width, int height, uint32_t *pixels) {
    if (width <= 0 || height <= 0) return;
    memset(pixels, 0, (size_t)width * (size_t)height * sizeof *pixels);
    int found = target ? depth_for(target) : -1;
    if (found < 0) return;
    flush();
    int previous = bound;
    int source = free_slot();
    memset(&textures[source], 0, sizeof textures[source]);
    textures[source].color = depths[found].samples > 1 ? depths[found].resolved : depths[found].texture;
    textures[source].width = textures[target].width;
    textures[source].height = textures[target].height;
    textures[source].scale = textures[target].scale;
    textures[source].samples = 1;
    textures[source].used = 1;
    textures[source].drawn = 1;
    int tile = render_texture(DEPTH_TILE, DEPTH_TILE);
    uint32_t tile_pixels[DEPTH_TILE * DEPTH_TILE];
    for (int tile_y = 0; tile_y < height; tile_y += DEPTH_TILE)
        for (int tile_x = 0; tile_x < width; tile_x += DEPTH_TILE) {
            int w = width - tile_x < DEPTH_TILE ? width - tile_x : DEPTH_TILE;
            int h = height - tile_y < DEPTH_TILE ? height - tile_y : DEPTH_TILE;
            float u0 = (float)(x + tile_x) / (float)textures[source].width;
            float u1 = (float)(x + tile_x + w) / (float)textures[source].width;
            float v0 = (float)(y + tile_y) / (float)textures[source].height;
            float v1 = (float)(y + tile_y + h) / (float)textures[source].height;
            RenderVertex a = {0, 0, 0, u0, v0}, b = {(float)w, 0, 0, u1, v0};
            RenderVertex c = {0, (float)h, 0, u0, v1}, d = {(float)w, (float)h, 0, u1, v1};
            RenderVertex quad[6] = {a, b, c, b, d, c};
            RenderState state;
            memset(&state, 0, sizeof state);
            state.texture = source;
            state.decal = 1;
            state.depth_read = 1;
            state.scissor.right = w - 1;
            state.scissor.bottom = h - 1;
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
    bound = target;
}

static void draw_to_bound(void) {
    if (bound == drawing) return;
    if (target_pending(drawing) && used_by_pending(bound)) flush();
    if (!target_pending(drawing)) drawing = bound;
}

int render_bound(void) { return bound; }

static SDL_GPUCompareOp depth_compare(const RenderTests *tests) {
    if (!tests->depth_test) return SDL_GPU_COMPAREOP_ALWAYS;
    static const SDL_GPUCompareOp ops[] = {
        [RENDER_DEPTH_NEVER] = SDL_GPU_COMPAREOP_NEVER,
        [RENDER_DEPTH_ALWAYS] = SDL_GPU_COMPAREOP_ALWAYS,
        [RENDER_DEPTH_GEQUAL] = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL,
        [RENDER_DEPTH_GREATER] = SDL_GPU_COMPAREOP_GREATER,
    };
    return ops[tests->depth_method];
}

static void draw_to_bound(void);

static int prepare(Command *command, const RenderState *state, PipelineKey key) {
    draw_to_bound();
    if (state->texture && state->texture == bound) {
        fputs("render: a draw samples its own target\n", stderr);
        return 0;
    }
    if (bound != drawing &&
        (state->texture == drawing || (depth_for(bound) == depth_for(drawing) && touches_depth(state)))) {
        flush();
        drawing = bound;
    }
    uint32_t source, destination, op, flags = 0;
    SDL_GPUTexture *alpha_copy = textures[blank].color;
    if (state->tests.destination_test) {
        int pass_set = state->tests.destination_mode;
        if (!target_pending(bound) && !textures[bound].drawn) {
            if (!pass_set) return 0;
        } else {
            alpha_copy = destination_copy();
            flags |= DEST_TEST | (pass_set ? DEST_SET : 0);
        }
    }
    blend_equation(state, &source, &destination, &op, &flags);
    uint32_t mask = 0;
    if ((state->color_mask & 0x000000ff) != 0x000000ff) mask |= SDL_GPU_COLORCOMPONENT_R;
    if ((state->color_mask & 0x0000ff00) != 0x0000ff00) mask |= SDL_GPU_COLORCOMPONENT_G;
    if ((state->color_mask & 0x00ff0000) != 0x00ff0000) mask |= SDL_GPU_COLORCOMPONENT_B;
    if ((state->color_mask & 0xff000000) != 0xff000000) mask |= SDL_GPU_COLORCOMPONENT_A;
    memset(command, 0, sizeof *command);
    command->target = bound;
    key.source = source;
    key.destination = destination;
    key.op = op;
    key.mask = mask;
    key.depth_compare = depth_compare(&state->tests);
    key.depth_write = state->depth_write != 0;
    key.samples = sample_count(textures[bound].samples);
    command->key = key;
    int textured = state->texture && !(render_toggles.no_textures && !state->depth_read);
    if (textured) flags |= TEXTURED;
    if (state->texa) flags |= TEXA;
    if (state->expansion.aem) flags |= AEM;
    if (state->fog && !render_toggles.no_fog) flags |= FOG;
    if (state->tests.alpha_test) flags |= ALPHA_TEST;
    if (state->blending.c == RENDER_BLEND_FIX) flags |= FACTOR_FIX;
    if (state->decal) flags |= DECAL;
    if (state->vertex_alpha) flags |= VERTEX_ALPHA;
    if (state->depth_read) flags |= DEPTH_READ;
    if (state->tint) flags |= TINT;
    uint32_t fix = state->blending.fix;
    command->fragment.flags = flags;
    command->fragment.alpha_reference = state->tests.alpha_reference;
    command->fragment.alpha_method = state->tests.alpha_method;
    command->fragment.blend_fix = fix;
    command->fragment.texture_alpha = state->expansion.ta0;
    command->fragment.fog_color = state->fog_color;
    command->fragment.tint = state->tint;
    command->blend_constant = fix >= 128 ? 1.0f : (float)fix / 128.0f;
    command->texture = textured ? state->texture : blank;
    command->resident = 0;
    command->color = textures[command->texture].color;
    command->destination = alpha_copy;
    if (!state->depth_read) {
        if (render_toggles.overdraw) {
            command->key.source = SDL_GPU_BLENDFACTOR_ONE;
            command->key.destination = SDL_GPU_BLENDFACTOR_ONE;
            command->key.op = SDL_GPU_BLENDOP_ADD;
            command->key.mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
            command->fragment.flags |= OVERDRAW;
        }
        if (render_toggles.wireframe && key.topology == RENDER_TRIANGLES) command->key.wire = 1;
    }
    command->sampler = (state->linear ? 1 : 0) | (state->repeat_u ? 2 : 0) | (state->repeat_v ? 4 : 0);
    command->model = -1;
    command->shadow = -1;
    command->follow = -1;
    Texture *target = &textures[bound];
    int x0 = state->scissor.left < 0 ? 0 : state->scissor.left;
    int y0 = state->scissor.top < 0 ? 0 : state->scissor.top;
    int x1 = state->scissor.right >= target->width ? target->width - 1 : state->scissor.right;
    int y1 = state->scissor.bottom >= target->height ? target->height - 1 : state->scissor.bottom;
    command->scissor.x = x0 * target->scale;
    command->scissor.y = y0 * target->scale;
    command->scissor.w = x1 >= x0 ? (x1 - x0 + 1) * target->scale : 0;
    command->scissor.h = y1 >= y0 ? (y1 - y0 + 1) * target->scale : 0;
    return 1;
}

static int store_vertices(Command *command, const void *vertices, uint32_t bytes, int count, uint32_t stride) {
    uint32_t start = (vertex_used + stride - 1) / stride * stride;
    if (start + bytes > VERTEX_BYTES || pending_count == COMMAND_LIMIT) {
        fputs("render: too many vertices in one frame\n", stderr);
        return 0;
    }
    command->offset = start;
    command->count = count;
    memcpy(vertex_data + start, vertices, bytes);
    vertex_used = start + bytes;
    return 1;
}

static void append(const Command *command) {
    pending[pending_count++] = *command;
    queue(command, 1);
    command_serial++;
}

static PipelineKey opaque_key(PipelineKey shape) {
    return (PipelineKey){.source = SDL_GPU_BLENDFACTOR_ONE,
                         .destination = SDL_GPU_BLENDFACTOR_ZERO,
                         .op = SDL_GPU_BLENDOP_ADD,
                         .mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B |
                                 SDL_GPU_COLORCOMPONENT_A,
                         .topology = shape.topology,
                         .depth_compare = SDL_GPU_COMPAREOP_ALWAYS,
                         .mesh = shape.mesh,
                         .cull = shape.cull,
                         .samples = shape.samples};
}

static void highlight(const Command *command) {
    Command solid = *command;
    solid.key = opaque_key(command->key);
    solid.fragment.flags = SOLID;
    solid.texture = blank;
    solid.color = textures[blank].color;
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
    entry->uv[0] = entry->uv[2] = vertices[0].u;
    entry->uv[1] = entry->uv[3] = vertices[0].v;
    for (int i = 1; i < count; ++i) {
        if (vertices[i].x < entry->bounds[0]) entry->bounds[0] = vertices[i].x;
        if (vertices[i].y < entry->bounds[1]) entry->bounds[1] = vertices[i].y;
        if (vertices[i].x > entry->bounds[2]) entry->bounds[2] = vertices[i].x;
        if (vertices[i].y > entry->bounds[3]) entry->bounds[3] = vertices[i].y;
        if (vertices[i].u < entry->uv[0]) entry->uv[0] = vertices[i].u;
        if (vertices[i].v < entry->uv[1]) entry->uv[1] = vertices[i].v;
        if (vertices[i].u > entry->uv[2]) entry->uv[2] = vertices[i].u;
        if (vertices[i].v > entry->uv[3]) entry->uv[3] = vertices[i].v;
    }
}

static int slot_like(int slot, int width, int height, int scale) {
    if (slot && textures[slot].width == width && textures[slot].height == height && textures[slot].scale == scale)
        return slot;
    render_release(slot);
    return new_texture(width, height, scale, 1);
}

static void restore(void) {
    Texture *target = &textures[bound];
    target->stale = 0;
    int width = target->width, height = target->height, scale = target->scale;
    restore_slot = slot_like(restore_slot, width, height, scale);
    copy_texture(whole(target->color), whole(textures[restore_slot].color), (Uint32)(width * scale),
                 (Uint32)(height * scale));
    RenderVertex a = {0, 0, 0, 0, 0}, b = {(float)width, 0, 0, 1, 0};
    RenderVertex c = {0, (float)height, 0, 0, 1}, d = {(float)width, (float)height, 0, 1, 1};
    RenderVertex quad[6] = {a, b, c, b, d, c};
    RenderState state;
    memset(&state, 0, sizeof state);
    state.texture = restore_slot;
    state.scissor.right = width - 1;
    state.scissor.bottom = height - 1;
    Command command;
    if (!prepare(&command, &state, (PipelineKey){.topology = RENDER_TRIANGLES})) return;
    command.key = opaque_key((PipelineKey){.topology = RENDER_TRIANGLES, .samples = sample_count(target->samples)});
    command.fragment.flags = TEXTURED | DECAL;
    if (store_vertices(&command, quad, sizeof quad, 6, sizeof(RenderVertex))) append(&command);
}

static void submit(const RenderState *state, int topology, const RenderVertex *vertices, int count, int index,
                   int source) {
    draw_to_bound();
    if (!target_pending(bound) && textures[bound].stale) restore();
    Command command;
    if (!prepare(&command, state, (PipelineKey){.topology = (uint32_t)topology}) ||
        !store_vertices(&command, vertices, (uint32_t)(count * (int)sizeof(RenderVertex)), count, sizeof(RenderVertex))) {
        if (capturing) record(source, state, topology, 0, count, vertices, -1, 0, NULL);
        return;
    }
    command.shadow = drawing_shadow;
    command.follow = drawing_follow;
    if (drawing_shadow >= 0)
        memcpy(point_data + command.offset / sizeof(RenderVertex), shadow_points, (size_t)count * sizeof *point_data);
    Command *last = pending_count ? &pending[pending_count - 1] : NULL;
    int merged = last && last->target == command.target && last->model < 0 && last->offset + (uint32_t)last->count * sizeof(RenderVertex) == command.offset &&
                 last->shadow == command.shadow && last->follow == command.follow &&
                 last->key.value == command.key.value && last->texture == command.texture && last->color == command.color &&
                 last->destination == command.destination &&
                 last->sampler == command.sampler &&
                 last->blend_constant == command.blend_constant &&
                 !memcmp(&last->fragment, &command.fragment, sizeof last->fragment) &&
                 !memcmp(&last->scissor, &command.scissor, sizeof last->scissor);
    if (merged) last->count += count;
    else append(&command);
    if (capturing) record(source, state, topology, 0, count, vertices, command_serial - 1, merged, NULL);
    if (index == render_highlight) highlight(&command);
}

static void widen(const RenderState *state, int topology, const RenderVertex *vertices, int count, int index,
                  int source) {
    static RenderVertex quads[WIDEN_LIMIT * 6];
    int step = topology == RENDER_LINES ? 2 : 1;
    int used = 0;
    for (int i = 0; i + step <= count; i += step) {
        RenderVertex a = vertices[i], b = vertices[i + step - 1];
        if (step == 1) {
            a.x -= 0.5f;
            b.x += 0.5f;
        }
        float across_x = fabsf(b.x - a.x) >= fabsf(b.y - a.y) ? 0.0f : 0.5f;
        float across_y = 0.5f - across_x;
        RenderVertex corners[4] = {a, b, a, b};
        for (int k = 0; k < 4; ++k) {
            corners[k].x += k < 2 ? -across_x : across_x;
            corners[k].y += k < 2 ? -across_y : across_y;
        }
        RenderVertex *quad = &quads[used * 6];
        quad[0] = corners[0], quad[1] = corners[1], quad[2] = corners[2];
        quad[3] = corners[1], quad[4] = corners[3], quad[5] = corners[2];
        if (++used < WIDEN_LIMIT) continue;
        submit(state, RENDER_TRIANGLES, quads, used * 6, index, source);
        used = 0;
    }
    if (used) submit(state, RENDER_TRIANGLES, quads, used * 6, index, source);
}

void render_draw(const RenderState *state, int topology, const RenderVertex *vertices, int count) {
    if (!bound || count <= 0) return;
    port_stats.draw_calls++;
    port_stats.vertices += (uint32_t)count;
    int index = frame_draws++;
    if (index >= render_toggles.skip_from && index < render_toggles.skip_to) return;
    if (textures[bound].scale > 1 && topology != RENDER_TRIANGLES) widen(state, topology, vertices, count, index, render_source);
    else submit(state, topology, vertices, count, index, render_source);
}

static void shadow_vertex(RenderVertex *vertex, const float *p, const RenderScreenMapping *mapping, float depth) {
    float q = p[3] != 0.0f ? 1.0f / p[3] : 0.0f;
    vertex->x = (p[0] * q * mapping->scale_x + mapping->offset_x + 1.0f) / mapping->scale_x;
    vertex->y = (p[1] * q * mapping->scale_y + mapping->offset_y - 1.0f) / mapping->scale_y;
    float scaled = (float)(long long)(p[2] * q) * depth;
    vertex->z = scaled < 0.0f ? 0.0f : scaled > 1.0f ? 1.0f : scaled;
}

void render_shadow_place(RenderVertex *vertex, const float *clip, const float *screen, const RenderScreenMapping *mapping,
                         float depth) {
    float p[4];
    for (int k = 0; k < 4; k++)
        p[k] = clip[0] * screen[k] + clip[1] * screen[4 + k] + clip[2] * screen[8 + k] + clip[3] * screen[12 + k];
    shadow_vertex(vertex, p, mapping, depth);
}

int render_shadow_setup(const void *key, const float *world, const float *camera, const float *light, const float *screen,
                        const RenderScreenMapping *mapping, float depth, const float (*shape)[4], int count, int prisms) {
    if (!smoothing) return -1;
    if (!point_data) point_data = malloc(VERTEX_BYTES / sizeof(RenderVertex) * sizeof *point_data);
    Recording *frame = &recordings[recording_slot];
    if (!point_data || !grow((void **)&frame->shadows, &frame->shadow_capacity, frame->shadow_count + 1, sizeof *frame->shadows) ||
        !grow((void **)&frame->shapes, &frame->shape_capacity, frame->shape_count + count, sizeof *frame->shapes))
        return -1;
    ShadowRecord *record = &frame->shadows[frame->shadow_count];
    record->first = frame->shape_count;
    record->count = count;
    record->prism_first = frame->prism_count;
    frame->prism_count += prisms;
    record->model = -1;
    memcpy(frame->shapes + frame->shape_count, shape, (size_t)count * sizeof *frame->shapes);
    frame->shape_count += count;
    record->key = key;
    memcpy(record->place, world + 12, sizeof record->place);
    memcpy(record->world, world, sizeof record->world);
    memcpy(record->camera, camera, sizeof record->camera);
    memcpy(record->light, light, sizeof record->light);
    memcpy(record->screen, screen, sizeof record->screen);
    record->mapping = *mapping;
    record->depth = depth;
    record->match = -1;
    return frame->shadow_count++;
}

void render_sprite(const RenderState *state, const RenderVertex *vertices, int count, const RenderScreenMapping *mapping,
                   float depth) {
    Recording *frame = &recordings[recording_slot];
    int follow = -1;
    if (smoothing) {
        RenderFollow wanted = {*mapping, depth};
        int last = frame->follow_count - 1;
        if (last >= 0 && !memcmp(&frame->follows[last], &wanted, sizeof wanted)) follow = last;
        else if (grow((void **)&frame->follows, &frame->follow_capacity, frame->follow_count + 1, sizeof *frame->follows)) {
            frame->follows[frame->follow_count] = wanted;
            follow = frame->follow_count++;
        }
    }
    drawing_follow = follow;
    render_draw(state, RENDER_TRIANGLES, vertices, count);
    drawing_follow = -1;
}

void render_shadow(const RenderState *state, const RenderVertex *vertices, const ShadowPoint *points, int count, int setup) {
    drawing_shadow = setup;
    shadow_points = points;
    render_draw(state, RENDER_TRIANGLES, vertices, count);
    drawing_shadow = -1;
    shadow_points = NULL;
}

static int bone_set(const RenderBone *bones) {
    if (!bones) return 0;
    if (bone_count > 1 && !memcmp(bones, bone_sets[bone_count - 1], sizeof bone_sets[0])) return bone_count - 1;
    memcpy(bone_sets[bone_count], bones, sizeof bone_sets[0]);
    return bone_count++;
}

static int extends_last(const Command *command, const MeshUniforms *uniforms) {
    const Command *last = pending_count ? &pending[pending_count - 1] : NULL;
    if (!last || last->target != command->target || last->model < 0 || !command->resident || !last->resident) return 0;
    if (last->offset + (uint32_t)last->count * sizeof(MeshVertex) != command->offset) return 0;
    if (last->key.value != command->key.value || last->texture != command->texture || last->color != command->color ||
        last->destination != command->destination || last->sampler != command->sampler ||
        last->blend_constant != command->blend_constant || memcmp(&last->fragment, &command->fragment, sizeof last->fragment) ||
        memcmp(&last->scissor, &command->scissor, sizeof last->scissor))
        return 0;
    if (memcmp(uniforms, &models[last->model], offsetof(MeshUniforms, flags) + offsetof(RenderMeshFlags, bone_set))) return 0;
    return last->bones == command->bones;
}

void render_mesh(const RenderState *state, int cull, const MeshUniforms *uniforms, const RenderBone *bones,
                 const MeshVertex *vertices, int count, int resident) {
    if (!bound || count <= 0 || render_toggles.no_models) return;
    port_stats.mesh_calls++;
    port_stats.vertices += (uint32_t)count;
    int index = frame_draws++;
    if (index >= render_toggles.skip_from && index < render_toggles.skip_to) return;
    if (model_count == MODEL_LIMIT || (bones && bone_count == BONE_LIMIT)) {
        flush();
        restart_models();
    }
    draw_to_bound();
    if (!target_pending(bound) && textures[bound].stale) restore();
    Command command;
    int prepared = prepare(&command, state, (PipelineKey){.topology = RENDER_TRIANGLES, .mesh = 1, .cull = (uint32_t)cull});
    if (prepared && resident >= 0) {
        command.offset = (uint32_t)resident * sizeof(MeshVertex);
        command.count = count;
        command.resident = 1;
    }
    if (!prepared ||
        (resident < 0 && !store_vertices(&command, vertices, (uint32_t)(count * (int)sizeof(MeshVertex)), count, sizeof(MeshVertex)))) {
        if (capturing) record(RENDER_SOURCE_MESH, state, RENDER_TRIANGLES, cull, count, NULL, -1, 0, uniforms);
        return;
    }
    command.bones = bone_set(bones);
    if (extends_last(&command, uniforms)) {
        pending[pending_count - 1].count += count;
        if (capturing) record(RENDER_SOURCE_MESH, state, RENDER_TRIANGLES, cull, count, NULL, command_serial - 1, 1, uniforms);
        return;
    }
    command.model = model_count;
    models[model_count] = *uniforms;
    models[model_count++].flags.bone_set = (uint32_t)command.bones;
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
    info->scale = textures[slot].scale;
    info->samples = textures[slot].samples;
    return 1;
}

static int clamp_scale(int scale) {
    return scale < 1 ? 1 : scale > SCALE_LIMIT ? SCALE_LIMIT : scale;
}

static int clamp_samples(int samples) {
    int power = 1;
    while (power * 2 <= samples && power * 2 <= SAMPLE_LIMIT) power *= 2;
    return power;
}

void render_set_background(int enabled) {
    background = enabled;
    if (!enabled) return;
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
}

int render_background(void) { return background; }

void render_send_back(SDL_Window *target) {
#if defined(__APPLE__) && !defined(SDL_PLATFORM_IOS)
    void *cocoa = SDL_GetPointerProperty(SDL_GetWindowProperties(target), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (cocoa) ((void (*)(void *, SEL, void *))objc_msgSend)(cocoa, sel_registerName("orderBack:"), NULL);
#else
    (void)target;
#endif
}

void render_set_quality(int scale, int samples) {
    if (scale == RENDER_SCALE_AUTO) auto_scale = 1;
    if (scale > 0) {
        auto_scale = 0;
        wanted_scale = clamp_scale(scale);
    }
    if (samples > 0) wanted_samples = clamp_samples(samples);
}

static void SDLCALL apply_window_size(void *unused) {
    (void)unused;
    if (window && !fullscreen) SDL_SetWindowSize(window, window_width, window_height);
}

static void SDLCALL apply_fullscreen(void *unused) {
    (void)unused;
    if (window) SDL_SetWindowFullscreen(window, fullscreen != 0);
}

void render_set_window(int width, int height, int full) {
    if (width > 0 && height > 0) {
        window_width = width;
        window_height = height;
        SDL_RunOnMainThread(apply_window_size, NULL, false);
    }
    if (full >= 0) {
        fullscreen = full;
        SDL_RunOnMainThread(apply_fullscreen, NULL, false);
    }
}

void render_fill_screen(void) { fill_screen = 1; }

void render_set_aspect(int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) {
        frame_width = LAYOUT_WIDTH;
        return;
    }
    int width = (int)(LAYOUT_HEIGHT * (float)numerator / (float)denominator / PIXEL_ASPECT / 2.0f + 0.5f) * 2;
    frame_width = width < LAYOUT_WIDTH ? LAYOUT_WIDTH : width > FRAME_WIDTH_LIMIT ? FRAME_WIDTH_LIMIT : width;
}

int render_frame_width(void) { return frame_width; }

void render_toggle_fullscreen(void) {
    render_set_window(0, 0, !fullscreen);
}

static void SDLCALL apply_vsync(void *unused) {
    (void)unused;
    if (window) apply_present_mode();
}

void render_set_vsync(int enabled) {
    vsync = enabled;
    SDL_RunOnMainThread(apply_vsync, NULL, false);
}

void render_set_filter(int use_nearest) {
    nearest = use_nearest;
}

void render_display(RenderDisplay *display) {
    display->auto_scale = auto_scale;
    display->fullscreen = fullscreen;
    display->vsync = vsync;
    display->nearest = nearest;
    display->window_width = window_width;
    display->window_height = window_height;
}

void render_quality(int *scale, int *samples) {
    *scale = quality_scale;
    *samples = quality_samples;
}

static void apply_quality(void) {
    if (wanted_scale == quality_scale && wanted_samples == quality_samples) return;
    forget_recordings();
    quality_scale = wanted_scale;
    quality_samples = wanted_samples = supported_samples(wanted_samples);
    for (int i = 1; i < TEXTURE_LIMIT; ++i) {
        Texture *texture = &textures[i];
        if (!texture->used || !texture->scaled) continue;
        Texture old = *texture;
        texture->scale = quality_scale;
        texture->samples = texture->target ? quality_samples : 1;
        create_storage(texture);
        if (old.drawn)
            blit(old.color, 0, 0, old.width * old.scale, old.height * old.scale, texture->color, 0, 0,
                 texture->width * texture->scale, texture->height * texture->scale, SDL_GPU_FILTER_LINEAR);
        texture->stale = texture->samples > 1 && texture->drawn;
        SDL_ReleaseGPUTexture(device, old.color);
        if (old.multi) SDL_ReleaseGPUTexture(device, old.multi);
    }
    release_depths();
    render_release(restore_slot);
    restore_slot = 0;
}

typedef struct {
    float position[2];
    float local[2];
    float half[2];
    float color[4];
    float shape[4];
    float uv[2];
} TouchVertex;

static SDL_GPUGraphicsPipeline *touch_pipeline;
static SDL_GPUTextureFormat touch_format;
static SDL_GPUBuffer *touch_buffer;
static SDL_GPUTransferBuffer *touch_transfer;
static SDL_GPUTexture *touch_atlas;
static const unsigned char *touch_atlas_pixels;
static TouchShape touch_shapes[TOUCH_SHAPE_LIMIT];

static int prepare_touch(SDL_GPUTextureFormat format) {
    if (touch_pipeline && touch_format == format) return 1;
    if (touch_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, touch_pipeline);
    SDL_GPUShader *vertex = create_shader(touch_vert_spirv, sizeof touch_vert_spirv, touch_vert_msl, sizeof touch_vert_msl,
                                          SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0);
    SDL_GPUShader *fragment = create_shader(touch_frag_spirv, sizeof touch_frag_spirv, touch_frag_msl,
                                            sizeof touch_frag_msl, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 0);
    SDL_GPUVertexBufferDescription buffer = {0, sizeof(TouchVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attributes[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TouchVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TouchVertex, local)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TouchVertex, half)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(TouchVertex, color)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(TouchVertex, shape)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TouchVertex, uv)},
    };
    SDL_GPUColorTargetDescription target = {0};
    target.format = format;
    target.blend_state.enable_blend = true;
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 6;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    touch_pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    touch_format = format;
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    Uint32 bytes = TOUCH_SHAPE_LIMIT * 6 * (Uint32)sizeof(TouchVertex);
    SDL_GPUBufferCreateInfo vertices = {SDL_GPU_BUFFERUSAGE_VERTEX, bytes, 0};
    SDL_GPUTransferBufferCreateInfo transfer = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    if (!touch_buffer) touch_buffer = SDL_CreateGPUBuffer(device, &vertices);
    if (!touch_transfer) touch_transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
    return touch_pipeline && touch_buffer && touch_transfer;
}

static void upload_touch_atlas(SDL_GPUCopyPass *copy) {
    const LauncherPicture *picture = port_touch_atlas();
    if (!picture || !picture->pixels || picture->pixels == touch_atlas_pixels) return;
    if (touch_atlas) SDL_ReleaseGPUTexture(device, touch_atlas);
    touch_atlas = create_texture(picture->width, picture->height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER, 1);
    Uint32 size = (Uint32)(picture->width * picture->height * 4);
    SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
    memcpy(SDL_MapGPUTransferBuffer(device, transfer, false), picture->pixels, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_GPUTextureTransferInfo from = {transfer, 0, (Uint32)picture->width, (Uint32)picture->height};
    SDL_GPUTextureRegion to = {touch_atlas, 0, 0, 0, 0, 0, (Uint32)picture->width, (Uint32)picture->height, 1};
    SDL_UploadToGPUTexture(copy, &from, &to, false);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    touch_atlas_pixels = picture->pixels;
}

static void touch_quad(TouchVertex *out, const TouchShape *shape) {
    static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    float margin = shape->kind == TOUCH_TEXT ? 0 : TOUCH_MARGIN;
    float reach_x = shape->half_width + margin, reach_y = shape->half_height + margin;
    float turn_cos = cosf(shape->angle), turn_sin = sinf(shape->angle);
    for (int corner = 0; corner < 6; ++corner) {
        float x = corners[corner][0] * reach_x, y = corners[corner][1] * reach_y;
        float across = (corners[corner][0] + 1) / 2, down = (corners[corner][1] + 1) / 2;
        out[corner] = (TouchVertex){{shape->x + x * turn_cos - y * turn_sin, shape->y + x * turn_sin + y * turn_cos},
                                    {x, y},
                                    {shape->half_width, shape->half_height},
                                    {shape->red, shape->green, shape->blue, shape->alpha},
                                    {(float)shape->kind, shape->stroke, shape->radius, 0},
                                    {shape->u0 + (shape->u1 - shape->u0) * across, shape->v0 + (shape->v1 - shape->v0) * down}};
    }
}

static void draw_touch(SDL_GPUCommandBuffer *buffer, SDL_GPUTexture *swapchain, SDL_GPUTextureFormat format, Uint32 width,
                       Uint32 height, int count) {
    if (!count || !prepare_touch(format)) return;
    TouchVertex *mapped = SDL_MapGPUTransferBuffer(device, touch_transfer, true);
    for (int index = 0; index < count; ++index) touch_quad(&mapped[index * 6], &touch_shapes[index]);
    SDL_UnmapGPUTransferBuffer(device, touch_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    SDL_GPUTransferBufferLocation from = {touch_transfer, 0};
    SDL_GPUBufferRegion to = {touch_buffer, 0, (Uint32)(count * 6 * sizeof(TouchVertex))};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_LockMutex(screen_lock);
    upload_touch_atlas(copy);
    SDL_UnlockMutex(screen_lock);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUColorTargetInfo target = {0};
    target.texture = swapchain;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(buffer, &target, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, touch_pipeline);
    SDL_GPUBufferBinding vertices = {touch_buffer, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
    SDL_GPUTextureSamplerBinding atlas = {touch_atlas ? touch_atlas : textures[blank].color, samplers[1]};
    SDL_BindGPUFragmentSamplers(pass, 0, &atlas, 1);
    float scale[4] = {2.0f / (float)width, 2.0f / (float)height, 0, 0};
    SDL_PushGPUVertexUniformData(buffer, 0, scale, sizeof scale);
    SDL_DrawGPUPrimitives(pass, (Uint32)count * 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

static void fit_texture(SDL_GPUCommandBuffer *buffer, SDL_GPUTexture *swapchain, Uint32 window_width,
                        Uint32 window_height, SDL_GPUTexture *color, int scale, int width, int height) {
    float aspect = (float)width * PIXEL_ASPECT / (float)height;
    float shown_height = (float)window_width / aspect < (float)window_height ? (float)window_width / aspect
                                                                             : (float)window_height;
    SDL_GPUBlitInfo blit = {0};
    blit.source.texture = color;
    blit.source.w = (Uint32)(width * scale);
    blit.source.h = (Uint32)(height * scale);
    blit.destination.texture = swapchain;
    blit.destination.w = (Uint32)(shown_height * aspect + 0.5f);
    blit.destination.h = (Uint32)(shown_height + 0.5f);
    blit.destination.x = (window_width - blit.destination.w) / 2;
    blit.destination.y = (window_height - blit.destination.h) / 2;
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(buffer, &blit);
    if (auto_scale) wanted_scale = clamp_scale(((int)blit.destination.h + height - 1) / height);
}

static void fill_texture(SDL_GPUCommandBuffer *buffer, SDL_GPUTexture *swapchain, Uint32 window_width,
                         Uint32 window_height, SDL_GPUTexture *color, int width, int height) {
    SDL_GPUBlitInfo blit = {0};
    blit.source.texture = color;
    blit.source.w = (Uint32)width;
    blit.source.h = (Uint32)height;
    blit.destination.texture = swapchain;
    blit.destination.w = window_width;
    blit.destination.h = window_height;
    SDL_BlitGPUTexture(buffer, &blit);
}

static void show_screen(int which) {
    Screen *screen = &screens[which];
    SDL_LockMutex(screen_lock);
    int index = screen->published;
    SDL_Window *target = screen->window;
    if (index < 0 || !target || screen->shown == screen->serial) {
        SDL_UnlockMutex(screen_lock);
        return;
    }
    screen->reading = index;
    screen->shown = screen->serial;
    SDL_GPUTexture *color = screen->pool[index];
    int scale = screen->scale, width = screen->width, height = screen->height, count = screen->shape_count;
    memcpy(touch_shapes, screen->shapes, (size_t)count * sizeof *touch_shapes);
    SDL_UnlockMutex(screen_lock);
    SDL_GPUCommandBuffer *buffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUTexture *swapchain = NULL;
    Uint32 window_width = 0, window_height = 0;
    if (buffer && !SDL_WaitAndAcquireGPUSwapchainTexture(buffer, target, &swapchain, &window_width, &window_height)) {
        SDL_CancelGPUCommandBuffer(buffer);
        buffer = NULL;
    }
    if (buffer && swapchain && which == SCREEN_GAME) {
        fit_texture(buffer, swapchain, window_width, window_height, color, scale, width, height);
        draw_touch(buffer, swapchain, SDL_GetGPUSwapchainTextureFormat(device, target), window_width, window_height, count);
    }
    if (buffer && swapchain && which == SCREEN_DEBUG)
        fill_texture(buffer, swapchain, window_width, window_height, color, width, height);
    if (buffer) SDL_SubmitGPUCommandBuffer(buffer);
    SDL_LockMutex(screen_lock);
    screen->reading = -1;
    if (swapchain) {
        screen->pixel_width = (int)window_width;
        screen->pixel_height = (int)window_height;
    }
    SDL_UnlockMutex(screen_lock);
}

void render_show_screens(void) {
    for (int which = 0; which < SCREEN_LIMIT; ++which) show_screen(which);
}

void render_redraw(void) {
    SDL_LockMutex(screen_lock);
    for (int which = 0; which < SCREEN_LIMIT; ++which) screens[which].shown = 0;
    SDL_UnlockMutex(screen_lock);
}

uint32_t render_wake_event(void) { return wake_event; }

static int free_screen(const Screen *screen) {
    SDL_LockMutex(screen_lock);
    int index = 0;
    while (index == screen->published || index == screen->reading) index++;
    SDL_UnlockMutex(screen_lock);
    return index;
}

static void publish(SDL_GPUCommandBuffer *buffer, int which, SDL_GPUTexture *color, SDL_GPUTextureFormat format,
                    int scale, int width, int height) {
    Screen *screen = &screens[which];
    int index = free_screen(screen);
    Uint32 pixel_width = (Uint32)(width * scale), pixel_height = (Uint32)(height * scale);
    if (!screen->pool[index] || screen->pool_width[index] != pixel_width || screen->pool_height[index] != pixel_height ||
        screen->pool_format[index] != format) {
        if (screen->pool[index]) SDL_ReleaseGPUTexture(device, screen->pool[index]);
        screen->pool[index] = create_texture((int)pixel_width, (int)pixel_height, format, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1);
        screen->pool_width[index] = pixel_width;
        screen->pool_height[index] = pixel_height;
        screen->pool_format[index] = format;
    }
    copy_region(buffer, whole(color), whole(screen->pool[index]), pixel_width, pixel_height);
    if (which == SCREEN_GAME) submit_timed(buffer);
    else SDL_SubmitGPUCommandBuffer(buffer);
    SDL_LockMutex(screen_lock);
    screen->published = index;
    screen->serial++;
    screen->scale = scale;
    screen->width = width;
    screen->height = height;
    screen->shape_count = which == SCREEN_GAME && screen->pixel_width
                              ? port_touch_shapes(screen->shapes, TOUCH_SHAPE_LIMIT, screen->pixel_width, screen->pixel_height)
                              : 0;
    SDL_UnlockMutex(screen_lock);
    SDL_Event wake = {0};
    wake.type = wake_event;
    SDL_PushEvent(&wake);
}

static void present_texture(SDL_GPUCommandBuffer *buffer, SDL_GPUTexture *color, int scale, int width, int height) {
    static uint64_t shown_at;
    uint64_t now = SDL_GetTicksNS();
    if (!color || (render_toggles.unpaced && now - shown_at < SDL_NS_PER_SECOND / (uint64_t)render_refresh_rate())) {
        submit_timed(buffer);
        return;
    }
    shown_at = now;
    publish(buffer, SCREEN_GAME, color, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, scale, width, height);
}

void render_show_debug(SDL_Window *target, SDL_GPUTexture *canvas, int format, int width, int height) {
    SDL_LockMutex(screen_lock);
    screens[SCREEN_DEBUG].window = target;
    SDL_UnlockMutex(screen_lock);
    SDL_GPUCommandBuffer *buffer = SDL_AcquireGPUCommandBuffer(device);
    if (buffer) publish(buffer, SCREEN_DEBUG, canvas, (SDL_GPUTextureFormat)format, 1, width, height);
}

uint32_t *render_download_screen(int *width, int *height) {
    Screen *screen = &screens[SCREEN_GAME];
    SDL_LockMutex(screen_lock);
    int index = screen->published;
    SDL_UnlockMutex(screen_lock);
    if (index < 0) return NULL;
    *width = (int)screen->pool_width[index];
    *height = (int)screen->pool_height[index];
    uint32_t *pixels = malloc((size_t)*width * (size_t)*height * sizeof *pixels);
    if (pixels) download(screen->pool[index], 0, 0, *width, *height, pixels);
    return pixels;
}

void render_finish(void) {
    if (commands) submit_timed(commands);
    commands = NULL;
}

static void start_recording(Recording *frame) {
    frame->step_count = 0;
    frame->command_count = 0;
    frame->model_count = 0;
    frame->vertex_used = 0;
    frame->valid = 0;
    frame->has_camera = 0;
    frame->shadow_count = 0;
    frame->shape_count = 0;
    frame->prism_count = 0;
    frame->follow_count = 0;
    frame->bone_count = 0;
    if (grow((void **)&frame->bones, &frame->bone_capacity, 1, sizeof *frame->bones)) {
        memset(frame->bones[0], 0, sizeof frame->bones[0]);
        frame->bone_count = 1;
    }
}

static float distance(const Recording *frame, int model, const Recording *before, int found) {
    const float *now = frame->models[model].model_world, *was = before->models[found].model_world;
    float jumped = 0.0f;
    for (int i = 12; i < 15; ++i) jumped = fmaxf(jumped, fabsf(now[i] - was[i]));
    for (int i = 0; i < 12; ++i) jumped = fmaxf(jumped, fabsf(now[i] - was[i]) * CUT_DISTANCE / BONE_FLIP);
    int bones = frame->model_bones[model], old_bones = before->model_bones[found];
    if ((bones > 0) != (old_bones > 0)) return SNAP_FRACTION;
    for (int bone = 0; bones > 0 && bone < RENDER_BONE_SLOTS; ++bone)
        for (int row = 0; row < 3; ++row)
            jumped = fmaxf(jumped, fabsf(frame->bones[bones][bone].rows[row][3] - before->bones[old_bones][bone].rows[row][3]));
    return jumped / CUT_DISTANCE * SNAP_FRACTION;
}

static float screen_gap(const Recording *frame, int model, const Recording *before, int found) {
    const float *now = frame->models[model].model_screen, *was = before->models[found].model_screen;
    float gap = 0.0f;
    for (int i = 0; i < 16; ++i) gap += fabsf(now[i] - was[i]);
    return gap;
}

static int camera_cut(const Recording *frame, const Recording *before) {
    if (!frame->has_camera || !before->has_camera) return 0;
    float moved = 0.0f, turned = 0.0f;
    for (int i = 0; i < 3; ++i) moved += (frame->eye[i] - before->eye[i]) * (frame->eye[i] - before->eye[i]);
    for (int i = 0; i < 9; ++i) turned = fmaxf(turned, fabsf(frame->pose[i] - before->pose[i]));
    return moved > CUT_DISTANCE * CUT_DISTANCE || turned > CUT_TURN;
}

void render_set_camera(const float *eye, const float (*pose)[4], const float (*view)[4]) {
    Recording *frame = &recordings[recording_slot];
    for (int i = 0; i < 3; ++i) {
        frame->eye[i] = eye[i];
        for (int k = 0; k < 3; ++k) frame->pose[i * 3 + k] = pose[i][k];
    }
    memcpy(frame->view, view, sizeof frame->view);
    frame->has_camera = 1;
}

static void match_models(void) {
    static int *table;
    static int table_capacity;
    static int *chain;
    static int chain_capacity;
    static int *claimed;
    static int claimed_capacity;
    static float *apart;
    static int apart_capacity;
    Recording *frame = &recordings[shown_slot], *before = &recordings[previous_slot];
    for (int i = 0; i < frame->model_count; ++i) frame->matches[i] = -1;
    if (!before->valid || !before->model_count || !frame->model_count || camera_cut(frame, before)) return;
    int size = 1;
    while (size < before->model_count * 2) size *= 2;
    if (!grow((void **)&table, &table_capacity, size, sizeof *table) ||
        !grow((void **)&chain, &chain_capacity, before->model_count, sizeof *chain))
        return;
    for (int i = 0; i < size; ++i) table[i] = -1;
    for (int i = 0; i < before->model_count; ++i) {
        chain[i] = -1;
        if (!before->keys[i]) continue;
        uint32_t slot = (uint32_t)(before->keys[i] * 0x9e3779b97f4a7c15ull >> 40) & (uint32_t)(size - 1);
        while (table[slot] >= 0 && before->keys[table[slot]] != before->keys[i]) slot = (slot + 1) & (uint32_t)(size - 1);
        chain[i] = table[slot];
        table[slot] = i;
    }
    if (!grow((void **)&claimed, &claimed_capacity, before->model_count, sizeof *claimed) ||
        !grow((void **)&apart, &apart_capacity, frame->model_count, sizeof *apart))
        return;
    for (int i = 0; i < before->model_count; ++i) claimed[i] = -1;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < frame->model_count; ++i) {
            if (!frame->keys[i] || frame->matches[i] >= 0) continue;
            uint32_t slot = (uint32_t)(frame->keys[i] * 0x9e3779b97f4a7c15ull >> 40) & (uint32_t)(size - 1);
            while (table[slot] >= 0 && before->keys[table[slot]] != frame->keys[i]) slot = (slot + 1) & (uint32_t)(size - 1);
            float best = 1e30f;
            int chosen = -1;
            for (int found = table[slot]; found >= 0; found = chain[found]) {
                int exact = claimed[found] >= 0 && claimed[found] < CLAIM_NEAR;
                if (exact || (round == 0 && claimed[found] >= 0)) continue;
                float moved = distance(frame, i, before, found);
                if (moved >= (round == 0 ? EXACT_FRACTION : SNAP_FRACTION)) continue;
                float score = moved + screen_gap(frame, i, before, found) * SCREEN_WEIGHT;
                if (claimed[found] >= CLAIM_NEAR && apart[claimed[found] - CLAIM_NEAR] <= score) continue;
                if (score < best) {
                    best = score;
                    chosen = found;
                }
            }
            if (chosen < 0) continue;
            if (claimed[chosen] >= CLAIM_NEAR) frame->matches[claimed[chosen] - CLAIM_NEAR] = -1;
            claimed[chosen] = round == 0 ? i : CLAIM_NEAR + i;
            apart[i] = best;
            frame->matches[i] = chosen;
        }
    }
}

static int draw_close(const Recording *frame, const Command *now, const Recording *before, const Command *was) {
    const RenderVertex *a = (const RenderVertex *)(frame->vertices + now->copy);
    const RenderVertex *b = (const RenderVertex *)(before->vertices + was->copy);
    for (int i = 0; i < now->count; ++i)
        if (fabsf(a[i].x - b[i].x) > DRAW_JUMP || fabsf(a[i].y - b[i].y) > DRAW_JUMP) return 0;
    return 1;
}

static void forget_hidden_draws(Recording *frame) {
    for (int i = 0; i < frame->step_count; ++i) {
        const Step *step = &frame->steps[i];
        if (step->kind != STEP_PASS || step->color == frame->shown) continue;
        for (int k = step->first; k < step->first + step->count; ++k) frame->commands[k].signature = 0;
    }
}

static void match_draws(void) {
    static uint64_t *taken;
    static int *heads;
    static int *next;
    static int *balance;
    static int taken_capacity;
    static int heads_capacity;
    static int next_capacity;
    static int balance_capacity;
    Recording *frame = &recordings[shown_slot], *before = &recordings[previous_slot];
    forget_hidden_draws(frame);
    frame->paired = 0;
    if (!before->valid || !before->command_count || camera_cut(frame, before)) return;
    int size = 1;
    while (size < before->command_count * 2) size *= 2;
    if (!grow((void **)&taken, &taken_capacity, size, sizeof *taken) ||
        !grow((void **)&heads, &heads_capacity, size, sizeof *heads) ||
        !grow((void **)&next, &next_capacity, before->command_count, sizeof *next) ||
        !grow((void **)&balance, &balance_capacity, size, sizeof *balance))
        return;
    for (int i = 0; i < size; ++i) {
        taken[i] = 0;
        heads[i] = -1;
        balance[i] = 0;
    }
    for (int i = before->command_count - 1; i >= 0; --i) {
        uint64_t signature = before->commands[i].signature;
        if (!signature) continue;
        uint32_t slot = (uint32_t)(signature >> 40) & (uint32_t)(size - 1);
        while (taken[slot] && taken[slot] != signature) slot = (slot + 1) & (uint32_t)(size - 1);
        taken[slot] = signature;
        next[i] = heads[slot];
        heads[slot] = i;
        balance[slot]++;
    }
    for (int i = 0; i < frame->command_count; ++i) {
        uint64_t signature = frame->commands[i].signature;
        if (!signature) continue;
        uint32_t slot = (uint32_t)(signature >> 40) & (uint32_t)(size - 1);
        while (taken[slot] && taken[slot] != signature) slot = (slot + 1) & (uint32_t)(size - 1);
        if (taken[slot]) balance[slot]--;
    }
    for (int i = 0; i < frame->command_count; ++i) {
        Command *command = &frame->commands[i];
        if (!command->signature) continue;
        uint32_t slot = (uint32_t)(command->signature >> 40) & (uint32_t)(size - 1);
        while (taken[slot] && taken[slot] != command->signature) slot = (slot + 1) & (uint32_t)(size - 1);
        int found = heads[slot];
        if (found < 0 || balance[slot]) continue;
        heads[slot] = next[found];
        if (!draw_close(frame, command, before, &before->commands[found])) continue;
        command->pair = found;
        frame->paired++;
    }
}

static void match_shadows(void) {
    static int *claimed;
    static int claimed_capacity;
    static float *gaps;
    static int gaps_capacity;
    Recording *frame = &recordings[shown_slot], *before = &recordings[previous_slot];
    frame->shadows_matched = 0;
    if (!before->valid || camera_cut(frame, before)) return;
    if (!grow((void **)&claimed, &claimed_capacity, before->shadow_count, sizeof *claimed) ||
        !grow((void **)&gaps, &gaps_capacity, frame->shadow_count, sizeof *gaps))
        return;
    for (int k = 0; k < before->shadow_count; ++k) claimed[k] = -1;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < frame->shadow_count; ++i) {
            ShadowRecord *record = &frame->shadows[i];
            if (record->match >= 0) continue;
            float best = round == 0 ? 1e-6f : CUT_DISTANCE;
            int chosen = -1;
            for (int k = 0; k < before->shadow_count; ++k) {
                const ShadowRecord *old = &before->shadows[k];
                int exact = claimed[k] >= 0 && claimed[k] < CLAIM_NEAR;
                if (old->key != record->key || exact || (round == 0 && claimed[k] >= 0)) continue;
                float apart = 0.0f;
                for (int j = 0; j < 3; ++j) apart = fmaxf(apart, fabsf(old->place[j] - record->place[j]));
                if (claimed[k] >= CLAIM_NEAR && gaps[claimed[k] - CLAIM_NEAR] <= apart) continue;
                if (apart < best) {
                    best = apart;
                    chosen = k;
                }
            }
            if (chosen < 0) continue;
            if (claimed[chosen] >= CLAIM_NEAR) frame->shadows[claimed[chosen] - CLAIM_NEAR].match = -1;
            claimed[chosen] = round == 0 ? i : CLAIM_NEAR + i;
            gaps[i] = best;
            record->match = chosen;
        }
    }
    for (int i = 0; i < frame->shadow_count; ++i) frame->shadows_matched += frame->shadows[i].match >= 0;
}

static void bone_matrix(float *out, const RenderBone *bone) {
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) out[c * 4 + k] = bone->rows[k][c];
        out[c * 4 + 3] = 0.0f;
    }
    for (int k = 0; k < 3; ++k) out[12 + k] = bone->rows[k][3];
    out[15] = 1.0f;
}

static float matrix_gap(const float *a, const float *b) {
    float gap = 0.0f;
    for (int k = 0; k < 12; ++k) gap = fmaxf(gap, fabsf(a[k] - b[k]));
    for (int k = 12; k < 15; ++k) gap = fmaxf(gap, fabsf(a[k] - b[k]) / CUT_DISTANCE);
    return gap;
}

static void attach_shadows(void) {
    Recording *frame = &recordings[shown_slot];
    for (int s = 0; s < frame->shadow_count; ++s) {
        ShadowRecord *record = &frame->shadows[s];
        float best = ATTACH_GAP, attached[16];
        for (int i = 0; i < frame->model_count; ++i) {
            int bones = frame->model_bones[i];
            const float *placed = frame->models[i].model_world;
            if (bones <= 0 || fabsf(placed[12] - record->world[12]) > ATTACH_REACH ||
                fabsf(placed[14] - record->world[14]) > ATTACH_REACH)
                continue;
            for (int bone = 0; bone < RENDER_BONE_SLOTS; ++bone) {
                float local[16], world[16];
                bone_matrix(local, &frame->bones[bones][bone]);
                multiply(world, placed, local);
                float gap = matrix_gap(world, record->world);
                if (gap >= best) continue;
                best = gap;
                record->model = i;
                record->bone = bone;
                memcpy(attached, world, sizeof attached);
            }
        }
        float inverse[16];
        if (record->model >= 0 && invert(inverse, attached)) multiply(record->offset, inverse, record->world);
        else record->model = -1;
    }
}

static void layout_moving(Recording *frame) {
    frame->moving_bytes = 0;
    for (int i = 0; i < frame->command_count; ++i) {
        Command *command = &frame->commands[i];
        command->moving = !command->resident && command->model < 0 &&
                          (command->pair >= 0 || command->shadow >= 0 || command->follow >= 0);
        command->offset = command->moving ? frame->moving_bytes : command->copy;
        if (command->moving) frame->moving_bytes += (uint32_t)command->count * sizeof(RenderVertex);
    }
}

static void finish_recording(int target, int width, int height) {
    Recording *frame = &recordings[recording_slot];
    frame->shown = textures[target].color;
    frame->shown_target = target;
    frame->shown_width = width;
    frame->shown_height = height;
    frame->shown_scale = textures[target].scale;
    frame->valid = 1;
    int spare = previous_slot;
    previous_slot = shown_slot;
    shown_slot = recording_slot;
    recording_slot = spare;
    start_recording(&recordings[recording_slot]);
    match_models();
    match_draws();
    match_shadows();
    attach_shadows();
    layout_moving(&recordings[shown_slot]);
    replay_uploaded = 0;
}

static void forget_recordings(void) {
    for (int i = 0; i < 3; ++i) recordings[i].valid = 0;
}

static float length_of(const float *vector) {
    return sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
}

static int unit(float *vector) {
    float length = length_of(vector);
    if (length < 1e-6f) return 0;
    for (int i = 0; i < 3; ++i) vector[i] /= length;
    return 1;
}

static void blend_bone(RenderBone *out, const RenderBone *was, const RenderBone *now, float alpha) {
    float turned = 0.0f, column[3][3], size[3];
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 4; ++c) out->rows[k][c] = was->rows[k][c] + (now->rows[k][c] - was->rows[k][c]) * alpha;
    for (int c = 0; c < 3; ++c) {
        float before[3], after[3];
        for (int k = 0; k < 3; ++k) {
            before[k] = was->rows[k][c];
            after[k] = now->rows[k][c];
            column[c][k] = out->rows[k][c];
            turned = fmaxf(turned, fabsf(after[k] - before[k]));
        }
        size[c] = length_of(before) + (length_of(after) - length_of(before)) * alpha;
    }
    if (turned > BONE_FLIP) {
        *out = *now;
        return;
    }
    if (!unit(column[0])) return;
    float along = column[1][0] * column[0][0] + column[1][1] * column[0][1] + column[1][2] * column[0][2];
    for (int k = 0; k < 3; ++k) column[1][k] -= along * column[0][k];
    if (!unit(column[1])) return;
    float cross[3] = {column[0][1] * column[1][2] - column[0][2] * column[1][1],
                      column[0][2] * column[1][0] - column[0][0] * column[1][2],
                      column[0][0] * column[1][1] - column[0][1] * column[1][0]};
    float side = cross[0] * column[2][0] + cross[1] * column[2][1] + cross[2] * column[2][2] < 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 3; ++k) column[2][k] = cross[k] * side;
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) out->rows[k][c] = column[c][k] * size[c];
}

static void multiply(float *out, const float *a, const float *b) {
    float result[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            result[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    memcpy(out, result, sizeof result);
}

static int invert(float *out, const float *matrix) {
    double rows[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            rows[i][j] = matrix[i * 4 + j];
            rows[i][4 + j] = i == j;
        }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int r = column + 1; r < 4; ++r)
            if (fabs(rows[r][column]) > fabs(rows[pivot][column])) pivot = r;
        if (fabs(rows[pivot][column]) < 1e-12) return 0;
        for (int j = 0; j < 8; ++j) {
            double swap = rows[column][j];
            rows[column][j] = rows[pivot][j];
            rows[pivot][j] = swap;
        }
        double scale = rows[column][column];
        for (int j = 0; j < 8; ++j) rows[column][j] /= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == column) continue;
            double factor = rows[r][column];
            for (int j = 0; j < 8; ++j) rows[r][j] -= factor * rows[column][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (float)rows[i][4 + j];
    return 1;
}

static int camera_shift(float *shift, const Recording *frame, const Recording *before, float alpha) {
    if (!frame->has_camera || !before->has_camera || camera_cut(frame, before)) return 0;
    float view[16], inverse[16];
    for (int k = 0; k < 16; ++k) view[k] = before->view[k] + (frame->view[k] - before->view[k]) * alpha;
    if (!invert(inverse, frame->view)) return 0;
    multiply(shift, view, inverse);
    return 1;
}

static void blend_draws(unsigned char *mapped, const Recording *frame, const Recording *before, float alpha) {
    for (int i = 0; i < frame->command_count; ++i) {
        const Command *command = &frame->commands[i];
        if (command->pair < 0) continue;
        RenderVertex *out = (RenderVertex *)(mapped + command->offset);
        const RenderVertex *now = (const RenderVertex *)(frame->vertices + command->copy);
        const RenderVertex *was = (const RenderVertex *)(before->vertices + before->commands[command->pair].copy);
        for (int k = 0; k < command->count; ++k) {
            out[k].x = was[k].x + (now[k].x - was[k].x) * alpha;
            out[k].y = was[k].y + (now[k].y - was[k].y) * alpha;
            out[k].z = was[k].z + (now[k].z - was[k].z) * alpha;
        }
    }
}

static void blend_frame(float *out, const float *was, const float *now, float alpha) {
    RenderBone a, b, blended;
    for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) {
            a.rows[k][c] = was[c * 4 + k];
            b.rows[k][c] = now[c * 4 + k];
        }
        a.rows[k][3] = was[12 + k];
        b.rows[k][3] = now[12 + k];
    }
    blend_bone(&blended, &a, &b, alpha);
    for (int i = 0; i < 16; ++i) out[i] = was[i] + (now[i] - was[i]) * alpha;
    for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) out[c * 4 + k] = blended.rows[k][c];
        out[12 + k] = blended.rows[k][3];
    }
}

static int place_shadow(float (*placed)[16], const Recording *frame, const Recording *before, int record, float alpha,
                        const float *shift) {
    const ShadowRecord *now = &frame->shadows[record];
    const ShadowRecord *was = now->match >= 0 ? &before->shadows[now->match] : NULL;
    if (!was && !shift) return 0;
    float world[16], camera[16], light[16], screen[16];
    memcpy(world, now->world, sizeof world);
    memcpy(camera, now->camera, sizeof camera);
    memcpy(light, now->light, sizeof light);
    memcpy(screen, now->screen, sizeof screen);
    if (was && now->model >= 0 && frame->matches[now->model] >= 0) {
        float local[16], body[16];
        bone_matrix(local, &replay_bones[frame->model_bones[now->model]][now->bone]);
        multiply(body, replay_models[now->model].model_world, local);
        multiply(world, body, now->offset);
    } else if (was) {
        blend_frame(world, was->world, now->world, alpha);
    }
    if (was) {
        for (int k = 0; k < 16; ++k) {
            camera[k] = was->camera[k] + (now->camera[k] - was->camera[k]) * alpha;
            light[k] = was->light[k] + (now->light[k] - was->light[k]) * alpha;
        }
    } else {
        multiply(screen, shift, now->screen);
    }
    multiply(placed[2], camera, world);
    multiply(placed[0], screen, placed[2]);
    multiply(placed[3], light, world);
    multiply(placed[1], screen, placed[3]);
    return 1;
}

static void shadow_corner(float *out, const float *matrix, const float *position) {
    for (int j = 0; j < 4; ++j)
        out[j] = position[0] * matrix[j] + position[1] * matrix[4 + j] + position[2] * matrix[8 + j] + matrix[12 + j];
}

static void blend_shadows(unsigned char *mapped, const Recording *frame, const Recording *before, float alpha,
                          const float *shift) {
    static float (*placements)[4][16];
    static int *placed;
    static unsigned char *hidden;
    static int placement_capacity;
    static int placed_capacity;
    static int hidden_capacity;
    if (!grow((void **)&placements, &placement_capacity, frame->shadow_count, sizeof *placements) ||
        !grow((void **)&placed, &placed_capacity, frame->shadow_count, sizeof *placed) ||
        !grow((void **)&hidden, &hidden_capacity, frame->prism_count, sizeof *hidden))
        return;
    memset(hidden, 0, (size_t)frame->prism_count);
    for (int s = 0; s < frame->shadow_count; ++s) placed[s] = place_shadow(placements[s], frame, before, s, alpha, shift);
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < frame->command_count; ++i) {
            const Command *command = &frame->commands[i];
            if (command->shadow < 0 || !placed[command->shadow]) continue;
            const ShadowRecord *now = &frame->shadows[command->shadow];
            const ShadowRecord *was = now->match >= 0 ? &before->shadows[now->match] : NULL;
            const float(*old_shape)[4] = was && was->count == now->count ? before->shapes + was->first : NULL;
            const float(*shape)[4] = frame->shapes + now->first;
            RenderVertex *out = (RenderVertex *)(mapped + command->offset);
            const ShadowPoint *points = frame->points + command->copy / sizeof(RenderVertex);
            if (pass == 2) {
                for (int k = 0; k + 6 <= command->count; k += 6) {
                    const RenderVertex *a = &out[k], *b = &out[k + 1], *c = &out[k + 2];
                    int prism = now->prism_first + points[k].prism;
                    int back = (c->x - a->x) * (b->y - a->y) - (c->y - a->y) * (b->x - a->x) < 0.0f;
                    int skipped = prism >= render_toggles.prism_from && prism < render_toggles.prism_to;
                    if (back == points[k].back && !hidden[prism] && !skipped) continue;
                    for (int j = 1; j < 6; ++j) out[k + j] = out[k];
                }
                continue;
            }
            for (int k = 0; k < command->count; ++k) {
                int index = points[k].index, prism = now->prism_first + points[k].prism;
                float position[3] = {points[k].position[0], points[k].position[1], points[k].position[2]};
                if (old_shape && index < now->count)
                    for (int j = 0; j < 3; ++j) position[j] = old_shape[index][j] + (shape[index][j] - old_shape[index][j]) * alpha;
                float p[4];
                if (pass == 0) {
                    shadow_corner(p, placements[command->shadow][2 + points[k].cast_side], position);
                    float w = fabsf(p[3]);
                    if (p[0] > w || p[0] < -w || p[1] > w || p[1] < -w || p[2] > w || p[2] < -w) hidden[prism] = 1;
                    continue;
                }
                shadow_corner(p, placements[command->shadow][points[k].cast_side], position);
                shadow_vertex(&out[k], p, &now->mapping, now->depth);
            }
        }
}

static void follow_camera(unsigned char *mapped, const Recording *frame, const float *shift) {
    for (int i = 0; i < frame->command_count; ++i) {
        const Command *command = &frame->commands[i];
        if (command->follow < 0 || command->pair >= 0) continue;
        const RenderScreenMapping *mapping = &frame->follows[command->follow].mapping;
        float depth = frame->follows[command->follow].depth;
        RenderVertex *out = (RenderVertex *)(mapped + command->offset);
        for (int k = 0; k < command->count; ++k) {
            float v[4] = {(out[k].x * mapping->scale_x - mapping->offset_x - 1.0f) / mapping->scale_x,
                          (out[k].y * mapping->scale_y - mapping->offset_y + 1.0f) / mapping->scale_y, out[k].z / depth, 1.0f};
            float p[4];
            for (int r = 0; r < 4; ++r) p[r] = shift[r] * v[0] + shift[4 + r] * v[1] + shift[8 + r] * v[2] + shift[12 + r];
            if (p[3] == 0.0f) continue;
            out[k].x = (p[0] / p[3] * mapping->scale_x + mapping->offset_x + 1.0f) / mapping->scale_x;
            out[k].y = (p[1] / p[3] * mapping->scale_y + mapping->offset_y - 1.0f) / mapping->scale_y;
            float z = p[2] / p[3] * depth;
            out[k].z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
        }
    }
}

static int upload_static_vertices(SDL_GPUCommandBuffer *buffer, const Recording *frame) {
    if (replay_uploaded || !frame->vertex_used) return 1;
    if (!replay_buffer) {
        SDL_GPUBufferCreateInfo info = {SDL_GPU_BUFFERUSAGE_VERTEX, VERTEX_BYTES, 0};
        replay_buffer = SDL_CreateGPUBuffer(device, &info);
        SDL_GPUTransferBufferCreateInfo transfer = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, VERTEX_BYTES, 0};
        replay_transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
        if (!replay_buffer || !replay_transfer) return 0;
    }
    unsigned char *mapped = SDL_MapGPUTransferBuffer(device, replay_transfer, true);
    if (!mapped) return 0;
    memcpy(mapped, frame->vertices, frame->vertex_used);
    SDL_UnmapGPUTransferBuffer(device, replay_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    SDL_GPUTransferBufferLocation from = {replay_transfer, 0};
    SDL_GPUBufferRegion to = {replay_buffer, 0, frame->vertex_used};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    replay_uploaded = 1;
    return 1;
}

static int make_moving_room(uint32_t bytes) {
    if (bytes <= moving_capacity) return 1;
    uint32_t wanted = moving_capacity ? moving_capacity : 1 << 20;
    while (wanted < bytes) wanted *= 2;
    if (moving_buffer) SDL_ReleaseGPUBuffer(device, moving_buffer);
    if (moving_transfer) SDL_ReleaseGPUTransferBuffer(device, moving_transfer);
    SDL_GPUBufferCreateInfo info = {SDL_GPU_BUFFERUSAGE_VERTEX, wanted, 0};
    moving_buffer = SDL_CreateGPUBuffer(device, &info);
    SDL_GPUTransferBufferCreateInfo transfer = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, wanted, 0};
    moving_transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
    moving_capacity = moving_buffer && moving_transfer ? wanted : 0;
    return moving_capacity != 0;
}

static int upload_replay_models(SDL_GPUCommandBuffer *buffer, const Recording *frame) {
    uint32_t model_bytes = (uint32_t)frame->model_count * sizeof *replay_models;
    uint32_t bone_bytes = (uint32_t)frame->bone_count * sizeof *replay_bones;
    uint32_t wanted = model_bytes > bone_bytes ? model_bytes : bone_bytes;
    if (wanted > replay_storage_capacity) {
        uint32_t size = replay_storage_capacity ? replay_storage_capacity : 1 << 20;
        while (size < wanted) size *= 2;
        if (replay_model_buffer) SDL_ReleaseGPUBuffer(device, replay_model_buffer);
        if (replay_bone_buffer) SDL_ReleaseGPUBuffer(device, replay_bone_buffer);
        if (replay_model_transfer) SDL_ReleaseGPUTransferBuffer(device, replay_model_transfer);
        SDL_GPUBufferCreateInfo info = {SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, size, 0};
        replay_model_buffer = SDL_CreateGPUBuffer(device, &info);
        replay_bone_buffer = SDL_CreateGPUBuffer(device, &info);
        SDL_GPUTransferBufferCreateInfo transfer = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size * 2, 0};
        replay_model_transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
        replay_storage_capacity = replay_model_buffer && replay_bone_buffer && replay_model_transfer ? size : 0;
        if (!replay_storage_capacity) return 0;
    }
    unsigned char *mapped = SDL_MapGPUTransferBuffer(device, replay_model_transfer, true);
    if (!mapped) return 0;
    memcpy(mapped, replay_models, model_bytes);
    memcpy(mapped + replay_storage_capacity, replay_bones, bone_bytes);
    SDL_UnmapGPUTransferBuffer(device, replay_model_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    if (model_bytes) {
        SDL_GPUTransferBufferLocation from = {replay_model_transfer, 0};
        SDL_GPUBufferRegion to = {replay_model_buffer, 0, model_bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, true);
    }
    if (bone_bytes) {
        SDL_GPUTransferBufferLocation from = {replay_model_transfer, replay_storage_capacity};
        SDL_GPUBufferRegion to = {replay_bone_buffer, 0, bone_bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, true);
    }
    SDL_EndGPUCopyPass(copy);
    return 1;
}

static int upload_replay_vertices(SDL_GPUCommandBuffer *buffer, const Recording *frame, const Recording *before,
                                  float alpha) {
    if (!upload_static_vertices(buffer, frame)) return 0;
    if (!frame->moving_bytes) return 1;
    if (!make_moving_room(frame->moving_bytes)) return 0;
    static unsigned char *scratch;
    static uint32_t scratch_capacity;
    if (frame->moving_bytes > scratch_capacity) {
        unsigned char *grown = realloc(scratch, frame->moving_bytes);
        if (!grown) return 0;
        scratch = grown;
        scratch_capacity = frame->moving_bytes;
    }
    for (int i = 0; i < frame->command_count; ++i) {
        const Command *command = &frame->commands[i];
        if (command->moving)
            memcpy(scratch + command->offset, frame->vertices + command->copy, (size_t)command->count * sizeof(RenderVertex));
    }
    float shift[16];
    int follow = camera_shift(shift, frame, before, alpha);
    if (frame->paired) blend_draws(scratch, frame, before, alpha);
    if (frame->shadow_count) blend_shadows(scratch, frame, before, alpha, follow ? shift : NULL);
    if (frame->follow_count && follow) follow_camera(scratch, frame, shift);
    unsigned char *mapped = SDL_MapGPUTransferBuffer(device, moving_transfer, true);
    if (!mapped) return 0;
    memcpy(mapped, scratch, frame->moving_bytes);
    SDL_UnmapGPUTransferBuffer(device, moving_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(buffer);
    SDL_GPUTransferBufferLocation from = {moving_transfer, 0};
    SDL_GPUBufferRegion to = {moving_buffer, 0, frame->moving_bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    return 1;
}

static void place_model(MeshUniforms *out, const MeshUniforms *now, const MeshUniforms *was, float alpha) {
    if (!memcmp(now->model_world, was->model_world, sizeof now->model_world)) return;
    float now_inverse[16], was_inverse[16], now_camera[16], was_camera[16], camera[16];
    if (!invert(now_inverse, now->model_world) || !invert(was_inverse, was->model_world)) return;
    multiply(now_camera, now->model_screen, now_inverse);
    multiply(was_camera, was->model_screen, was_inverse);
    for (int k = 0; k < 16; ++k) camera[k] = was_camera[k] + (now_camera[k] - was_camera[k]) * alpha;
    blend_frame(out->model_world, was->model_world, now->model_world, alpha);
    multiply(out->model_screen, camera, out->model_world);
}

static void blend_models(const Recording *frame, const Recording *before, float alpha) {
    int floats = (int)(offsetof(MeshUniforms, flags) / sizeof(float));
    float shift[16];
    int follow = camera_shift(shift, frame, before, alpha);
    memcpy(replay_bones, frame->bones, (size_t)frame->bone_count * sizeof *frame->bones);
    for (int i = 0; i < frame->model_count; ++i) {
        replay_models[i] = frame->models[i];
        int found = frame->matches[i];
        if (found < 0 && follow) multiply(replay_models[i].model_screen, shift, frame->models[i].model_screen);
        if (found < 0) continue;
        float *out = (float *)&replay_models[i];
        const float *now = (const float *)&frame->models[i];
        const float *was = (const float *)&before->models[found];
        if (memcmp(now, was, (size_t)floats * sizeof *now)) {
            for (int k = 0; k < floats; ++k) out[k] = was[k] + (now[k] - was[k]) * alpha;
            place_model(&replay_models[i], &frame->models[i], &before->models[found], alpha);
        }
        int bones = frame->model_bones[i], old_bones = before->model_bones[found];
        if (bones <= 0 || old_bones <= 0) continue;
        for (int bone = 0; bone < RENDER_BONE_SLOTS; ++bone)
            if (memcmp(&before->bones[old_bones][bone], &frame->bones[bones][bone], sizeof(RenderBone)))
                blend_bone(&replay_bones[bones][bone], &before->bones[old_bones][bone], &frame->bones[bones][bone], alpha);
    }
}

static void replay_pass(SDL_GPUCommandBuffer *buffer, const Recording *frame, const Step *step) {
    SDL_GPUColorTargetInfo color[2];
    memset(color, 0, sizeof color);
    color[0].texture = step->multi ? step->multi : step->color;
    color[0].load_op = step->color_load;
    color[0].clear_color.a = 1.0f;
    color[0].store_op = step->multi ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
    color[0].resolve_texture = step->color;
    SDL_GPUDepthStencilTargetInfo depth = {0};
    depth.texture = depths[step->depth].texture;
    depth.load_op = step->depth_load;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    color[1].texture = depths[step->depth].color;
    color[1].load_op = depth.load_op;
    color[1].store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
    color[1].resolve_texture = depths[step->depth].resolved;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(buffer, color, step->mirror ? 2 : 1, &depth);
    encode(buffer, pass, frame->commands + step->first, step->count, replay_model_buffer, replay_bone_buffer,
           replay_buffer, step->width, step->height, step->mirror);
    SDL_EndGPURenderPass(pass);
}

int render_replay(float alpha) {
    Recording *frame = &recordings[shown_slot], *before = &recordings[previous_slot];
    if (!smoothing || !frame->valid) return 0;
    if (!grow((void **)&replay_models, &replay_model_capacity, frame->model_count, sizeof *replay_models) ||
        !grow((void **)&replay_bones, &replay_bone_capacity, frame->bone_count, sizeof *replay_bones))
        return 0;
    alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    blend_models(frame, before, alpha);
    SDL_GPUCommandBuffer *buffer = SDL_AcquireGPUCommandBuffer(device);
    if (!buffer) return 0;
    int paced = !render_toggles.unpaced;
    if (!upload_replay_vertices(buffer, frame, before, alpha) || !upload_replay_models(buffer, frame)) {
        present_texture(buffer, frame->shown, frame->shown_scale, frame->shown_width, frame->shown_height);
        return 0;
    }
    for (int i = 0; i < frame->step_count; ++i) {
        const Step *step = &frame->steps[i];
        if (step->kind == STEP_PASS) {
            replay_pass(buffer, frame, step);
            continue;
        }
        if (step->kind == STEP_BLIT) {
            SDL_BlitGPUTexture(buffer, &step->blit);
            continue;
        }
        copy_region(buffer, step->from, step->to, step->copy_width, step->copy_height);
    }
    if (paced) present_texture(buffer, frame->shown, frame->shown_scale, frame->shown_width, frame->shown_height);
    else submit_timed(buffer);
    smooth_shown();
    port_stats.replays++;
    port_debug_replayed(frame->shown_target, alpha);
    return 1;
}

void render_set_smoothing(int enabled) {
    smoothing = enabled;
    forget_recordings();
    start_recording(&recordings[recording_slot]);
}

int render_smoothing(void) { return smoothing; }

int render_refresh_rate(void) {
    const SDL_DisplayMode *mode = window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : NULL;
    return mode && mode->refresh_rate > 0.0f ? (int)(mode->refresh_rate + 0.5f) : 60;
}

static void keep_frame(int target) {
    Texture *source = &textures[target];
    kept_now ^= 1;
    int slot = kept[kept_now] = slot_like(kept[kept_now], source->width, source->height, source->scale);
    copy_region(command_buffer(), whole(source->color), whole(textures[slot].color),
                (Uint32)(source->width * source->scale), (Uint32)(source->height * source->scale));
}

static int show_kept(void) {
    const Recording *before = &recordings[previous_slot];
    int slot = kept[kept_now ^ 1];
    if (!before->valid || !slot) return 0;
    SDL_GPUCommandBuffer *buffer = command_buffer();
    present_texture(buffer, textures[slot].color, textures[slot].scale, before->shown_width, before->shown_height);
    commands = NULL;
    smooth_shown();
    port_debug_replayed(slot, 0.0f);
    return 1;
}

void render_present(int target, int width, int height) {
    flush();
    port_stats.gpu_ns += (uint64_t)(unsigned)SDL_SetAtomicInt(&gpu_busy_ns, 0);
    presented++;
    frame_draws = 0;
    int replayed = smoothing && target;
    if (replayed) finish_recording(target, width, height);
    SDL_GPUCommandBuffer *buffer = command_buffer();
    if (replayed) {
        keep_frame(target);
        submit_timed(buffer);
    } else {
        present_texture(buffer, target ? textures[target].color : NULL, target ? textures[target].scale : 1, width, height);
    }
    commands = NULL;
    restart_buffers();
    apply_quality();
    bury(presented);
    if (!replayed) {
        if (target) port_debug_replayed(target, 1.0f);
        return;
    }
    smooth_presented();
    if (recordings[shown_slot].valid && !smooth_due()) return;
    if (show_kept()) return;
    if (render_replay(0.0f)) return;
    buffer = command_buffer();
    present_texture(buffer, textures[target].color, textures[target].scale, width, height);
    commands = NULL;
}
