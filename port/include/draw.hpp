#pragma once
#include "render.h"
#include <vector>

class mgCTexture;
class mgCVisualMDT;
struct mgFACE_GROUP;

struct GsRegisters {
    u_long prim;
    u_long rgbaq;
    u_long st;
    u_long uv;
    u_long tex0;
    u_long tex1;
    u_long clamp;
    u_long texa;
    u_long fogcol;
    u_long scissor;
    u_long alpha;
    u_long test;
    u_long zbuf;
    u_long frame;
    u_long xyoffset;
    u_long fog;
    u_long bitbltbuf;
    u_long trxpos;
    u_long trxreg;
    float q;
};

extern GsRegisters gs;

void gs_reset();
void gs_frame(int fbp, int width, int height, int psm);
int gs_frame_target();
void gs_frame_shift(int pixels);
int gs_read_shift(u_int base);
extern "C" int port_screen_width(void);
void gs_write(int reg, u_long value);
void gs_packed(int reg, const u_int *words);
void gs_texture_source(const mgCTexture *texture);
int gs_texture(u_long tex0, int width, int height);
int gs_sampled(int texture, int width, int height, int repeat);
void gs_copy_image(u_int source_base, int source_x, int source_y, u_int destination_base, int destination_x,
                   int destination_y, int width, int height);
int gs_texture_for(const mgCTexture *texture);
void gs_next_frame();
void gs_base_state(RenderState *state);
void gs_texture_state(RenderState *state, const mgCTexture *texture, float *scale_u, float *scale_v);
void gs_sample_limits(const RenderState *state, float scale_u, float scale_v, float limits[4]);
void gs_screen_mapping(float screen[4], float *depth);

int visual_packet_draw(const void *common, const void *packet);
void visual_next_frame();
size_t visual_save(unsigned char *out);
size_t visual_load(const unsigned char *in);
size_t visual_keys(const unsigned char *in, std::vector<size_t> *offsets);
void sprite_next_frame();

struct ShadowSetup {
    float clip[4][4];
    float shadow[4][4];
    float screen[4][4];
    float light[4];
    float world[4][4];
    float camera[4][4];
    float caster[4][4];
    const void *key;
    RenderState state;
};

extern ShadowSetup shadow_setup;
void shadow_draw(const mgCVisualMDT *visual, const ShadowSetup &setup, const mgFACE_GROUP *group);

struct CacheInfo {
    char name[32];
    const void *image;
    const void *clut;
    int psm;
    u_int age;
};

int gs_cache_info(int slot, CacheInfo *info);
