#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RENDER_POINTS, RENDER_LINES, RENDER_TRIANGLES };

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
    float fog;
    float limits[4];
} RenderVertex;

typedef struct {
    float position[3];
    float normal[3];
    float uv[2];
    uint8_t color[4];
    uint8_t bone[4];
    float weight[4];
} MeshVertex;

typedef struct {
    float model_screen[16];
    float model_world[16];
    float clip_or_view[16];
    float light_direction[3][4];
    float light_color[4][4];
    float ambient[4];
    float object_color[4];
    float eye[4];
    float diffuse[4];
    float specular[4];
    float point_light[4][4];
    float point_color[4][4];
    float fog[4];
    float screen[4];
    float depth[4];
    float limits[4];
    uint32_t flags[4];
} MeshUniforms;

enum { RENDER_CULL_NONE, RENDER_CULL_FRONT, RENDER_CULL_BACK };

typedef struct {
    int texture;
    int linear;
    int repeat_u;
    int repeat_v;
    int decal;
    int vertex_alpha;
    int depth_read;
    int texa;
    int blend;
    uint64_t alpha;
    uint64_t test;
    uint64_t texa_register;
    int fog;
    uint32_t fog_color;
    int depth_write;
    uint32_t color_mask;
    int scissor[4];
} RenderState;

enum { RENDER_SOURCE_GS, RENDER_SOURCE_MESH, RENDER_SOURCE_SHADOW, RENDER_SOURCE_SPRITE, RENDER_SOURCE_WATER,
       RENDER_SOURCE_DEPTH };

typedef struct {
    int no_models;
    int no_2d;
    int no_shadows;
    int no_water;
    int no_sprites;
    int no_fog;
    int no_textures;
    int wireframe;
    int overdraw;
    int skip_from;
    int skip_to;
    int unpaced;
    int prism_from;
    int prism_to;
} RenderToggles;

typedef struct {
    int source;
    int topology;
    int count;
    int cull;
    int target;
    int target_width;
    int target_height;
    uint32_t target_base;
    RenderState state;
    float bounds[4];
    float uv[4];
    int command;
    int pass;
    int merged;
    MeshUniforms uniforms;
} RenderRecord;

typedef struct {
    int used;
    int target;
    int width;
    int height;
    uint32_t base;
    int drawn;
    int scale;
    int samples;
} RenderSlot;

extern RenderToggles render_toggles;
extern int render_source;
extern int render_highlight;

int render_init(void);
void render_shutdown(void);
int render_poll(void);
void render_quality(int *scale, int *samples);
int render_replay(float alpha);
int render_refresh_rate(void);
void render_set_camera(const float *eye, const float (*pose)[4], const float (*view)[4]);
void port_debug_replayed(float alpha);

int render_target(uint32_t base, int width, int height);
int render_target_at(uint32_t base);
int render_texture(int width, int height);
int render_texture_at_scale(int width, int height, int scale);
void render_upload_full(int texture, const uint32_t *pixels);
int render_scaled_texture(int width, int height);
void render_release(int texture);
int render_width(int texture);
int render_height(int texture);
int render_scale(int texture);
void render_upload(int texture, int x, int y, int width, int height, const uint32_t *pixels);
void render_download(int texture, int x, int y, int width, int height, uint32_t *pixels);
void render_download_full(int texture, int width, int height, uint32_t *pixels);
void render_download_depth(int target, int x, int y, int width, int height, uint32_t *pixels);
void render_copy(int source, int source_x, int source_y, int destination, int destination_x, int destination_y,
                 int width, int height);

void render_bind(int target);
int render_bound(void);
void render_draw(const RenderState *state, int topology, const RenderVertex *vertices, int count);
void render_mesh(const RenderState *state, int cull, const MeshUniforms *uniforms, const float *bones,
                 const MeshVertex *vertices, int count, int resident);
int render_mesh_store(const MeshVertex *vertices, int count);
void render_mesh_forget(int first, int count);
void render_shadow_place(RenderVertex *vertex, const float *clip, const float *screen, const float *mapping, float depth);
enum { SHADOW_PRISM = 4096 };
int render_shadow_setup(const void *key, const float *world, const float *camera, const float *light, const float *screen,
                        const float *mapping, float depth, const float (*shape)[4], int count, int prisms);
void render_sprite(const RenderState *state, const RenderVertex *vertices, int count, const float *mapping, float depth);
void render_shadow(const RenderState *state, const RenderVertex *vertices, const float (*points)[4], int count, int setup);
void render_present(int target, int width, int height);

void render_capture_begin(void);
void render_capture_end(void);
int render_capturing(void);
int render_capture_count(void);
int render_capture_dropped(void);
int render_capture_commands(void);
int render_capture_passes(void);
const RenderRecord *render_capture_records(void);
int render_slot_limit(void);
int render_slot(int slot, RenderSlot *info);

struct SDL_GPUDevice;
struct SDL_Window;
struct SDL_GPUTexture;
struct SDL_GPUDevice *render_device(void);
struct SDL_Window *render_window(void);
void render_set_background(int enabled);
int render_background(void);
void render_send_back(struct SDL_Window *target);
struct SDL_GPUTexture *render_gpu_texture(int texture);

#ifdef __cplusplus
}
#endif
