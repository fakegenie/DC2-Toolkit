#pragma once
#include "render.h"
#include "sce/libgraph.h"
#include <vector>

class mgCTexture;
class mgCVisualMDT;
struct mgFACE_GROUP;

constexpr int GS_BLOCKS_PER_PAGE = 32;
constexpr int GS_FRAME_WIDTH_UNIT = 64;
constexpr int GS_SUBPIXELS = 16;
constexpr int GS_ALPHA_ONE = 0x80;
constexpr int GS_WRAP_REPEAT = 0;
constexpr int GS_TFX_DECAL = 1;

struct GsRegisters {
    sceGsPrim prim;
    sceGsRgbaq rgbaq;
    sceGsSt st;
    sceGsUv uv;
    sceGsTex0 tex0;
    sceGsTex1 tex1;
    sceGsClamp clamp;
    sceGsTexa texa;
    sceGsFogcol fogcol;
    sceGsScissor scissor;
    sceGsAlpha alpha;
    sceGsTest test;
    sceGsZbuf zbuf;
    sceGsFrame frame;
    sceGsXyOffset xyoffset;
    sceGsFog fog;
    sceGsBitbltbuf bitbltbuf;
    sceGsTrxpos trxpos;
    sceGsTrxreg trxreg;
    float q;
};

extern GsRegisters gs;

void gs_reset();
void gs_frame(int fbp, int width, int height, int psm);
int gs_frame_target();
void gs_frame_shift(int pixels);
int gs_resident_at(u_int base);
int gs_read_shift(u_int base);
extern "C" int port_screen_width(void);
void gs_write(int reg, u_long value);
void gs_packed(int reg, const void *qword);
void gs_texture_source(const mgCTexture *texture);
int gs_texture(u_int base, int width, int height);
int gs_sampled(int texture, int width, int height, int repeat);
void gs_drawing_scaled(int texture);
void gs_copy_image(u_int source_base, int source_x, int source_y, u_int destination_base, int destination_x,
                   int destination_y, int width, int height);
int gs_texture_for(const mgCTexture *texture);
void gs_next_frame();
void gs_projected(const int *first, const int *second, int record);
void gs_base_state(RenderState *state);
void gs_texture_state(RenderState *state, const mgCTexture *texture, float *scale_u, float *scale_v);
RenderBlending gs_blending(sceGsAlpha alpha);
RenderTests gs_tests(sceGsTest test);
RenderExpansion gs_expansion(sceGsTexa texa);
void gs_sample_limits(const RenderState *state, float scale_u, float scale_v, RenderTexelLimits *limits);
void gs_screen_mapping(RenderScreenMapping *screen, float *depth);
void gs_shift_screen(int x, int y);
float gs_screen_left();

int visual_packet_draw(const void *common, const void *packet);
void visual_next_frame();
void visual_outline_begin();
void visual_outline_end(int offset, u_int edge_tint, u_int body_tint);
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
