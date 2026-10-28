#include "mglib.hpp"
#include "draw.hpp"
#include "port_present.h"
#include "port_state.h"
#include "port_mod.h"
#include "port_game.h"
#include "sce/eekernel.h"
#include "sce/libgraph.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

void mgInitRetail(int screen_mode, int video_mode);
void mgEndFrameRetail(mgCDrawManager *manager);
void mgSetPkFrameBufferRetail(int fbp, int width, int height, int psm);
int mgTransWorldPrimRetail(int *out, float *pos);
int mgTransWorldPrim3DSpriteRetail(int *top_left, int *bottom_right, float *position, float width, float height,
                                   int unused);

enum { LAYOUT_WIDTH = 512, UV_MASK = 0x3fff, WIDE_IMAGE_PIXELS = 1024 * 512, STILL_HEIGHT = 416 };

static int rotate_priority = -1;
static int display_target;
static int display_width;
static int display_height;

PORT_GAME_BEGIN
static int world_wide;
PORT_GAME_END

PORT_OVERRIDE void mgSetRotateThread(int priority) {
    rotate_priority = priority;
}

PORT_OVERRIDE void WaitVSync(int start, int frames) {
    while (mgGetVSyncCount() - start < frames) {
        if (rotate_priority > 0) RotateThreadReadyQueue(rotate_priority);
        else sceGsSyncV(0);
    }
}

PORT_OVERRIDE void mgInit(int screen_mode, int video_mode) {
    gs_reset();
    mgInitRetail(screen_mode, video_mode);
}

extern "C" int port_world_shown(void);
extern int DebugFlag;
extern "C" int port_debug;
int port_debug;

PORT_OVERRIDE void mgEndFrame(mgCDrawManager *manager) {
    if (port_debug) DebugFlag = 1;
    port_mods_frame_enter();
    mgEndFrameRetail(manager);
    port_mods_frame_drawn();
    render_set_camera(mgRenderInfo.camera_pos, mgRenderInfo.camera_pose, mgRenderInfo.world_screen);
    port_present_show(display_target, display_width, display_height);
    gs_next_frame();
    visual_next_frame();
    sprite_next_frame();
    port_state_frame();
    port_mods_frame_exit();
    world_wide = port_world_shown();
}

PORT_OVERRIDE int mgTransWorldPrim(int *out, float *pos) {
    int visible = mgTransWorldPrimRetail(out, pos);
    int record = render_project(pos, __builtin_return_address(0), &mgRenderInfo.world_screen[0][0], 0.0f, 0.0f);
    if (record >= 0) gs_projected(out, out, record);
    return visible;
}

PORT_OVERRIDE int mgTransWorldPrim3DSprite(int *top_left, int *bottom_right, float *position, float width, float height,
                                           int unused) {
    int visible = mgTransWorldPrim3DSpriteRetail(top_left, bottom_right, position, width, height, unused);
    float (*world_screen)[4] = mgRenderInfo.world_screen;
    float w = position[0] * world_screen[0][3] + position[1] * world_screen[1][3] + position[2] * world_screen[2][3] +
              position[3] * world_screen[3][3];
    if (w < 1.0f) return visible;
    int record = render_project(position, __builtin_return_address(0), &mgRenderInfo.world_screen[0][0],
                                0.5f * width * mgRenderInfo.view_screen[0][0],
                                0.5f * height * mgRenderInfo.view_screen[1][1]);
    if (record >= 0) gs_projected(top_left, bottom_right, record);
    return visible;
}

PORT_OVERRIDE void mgSetPkTextureRepeat(sceGsClamp clamp) {
    gs_write(SCE_GS_CLAMP_1, clamp.value);
}

extern "C" int port_screen_width(void) {
    return mgScreenWidth == LAYOUT_WIDTH && world_wide ? render_frame_width() : mgScreenWidth;
}

extern "C" int port_texture_shift(unsigned int base) { return gs_read_shift(base); }

extern "C" int port_wide_width(void) {
    return mgScreenWidth == LAYOUT_WIDTH ? render_frame_width() : mgScreenWidth;
}

PORT_OVERRIDE void mgSetPkFrameBuffer(int fbp, int width, int height, int psm) {
    mgSetPkFrameBufferRetail(fbp, width, height, psm);
    int texture = gs_resident_at((u_int)mgFRAME_1.FBP * GS_BLOCKS_PER_PAGE);
    int wide = mgScreenWidth == LAYOUT_WIDTH && !texture ? port_screen_width() : mgScreenWidth;
    gs_frame((int)mgFRAME_1.FBP, wide, mgScreenHeight, (int)mgFRAME_1.PSM);
    gs_frame_shift((wide - mgScreenWidth) / 2);
    if (fbp >= 0) return;
    display_target = gs_frame_target();
    display_width = wide;
    display_height = mgScreenHeight;
}

PORT_OVERRIDE void mgSetPkClearScreen(u_char red, u_char green, u_char blue, u_char alpha) {
    sceGsTest test = mgTEST_1;
    test.bits.ate = 0;
    test.bits.zte = 1;
    test.bits.ztst = SCE_GS_ALWAYS;
    test.bits.date = 0;
    sceGsZbuf zbuf = mgZBUF_1;
    zbuf.bits.zmsk = 0;
    sceGsAlpha blend = mgALPHA_1;
    blend.bits.a = SCE_GS_ALPHA_ZERO;
    blend.bits.b = SCE_GS_ALPHA_ZERO;
    blend.bits.c = SCE_GS_ALPHA_FIX;
    blend.bits.d = SCE_GS_ALPHA_CS;
    gs_write(SCE_GS_TEST_1, test.value);
    gs_write(SCE_GS_ZBUF_1, zbuf.value);
    gs_write(SCE_GS_ALPHA_1, blend.value);
    gs_write(SCE_GS_TEX1_1, SCE_GS_SET_TEX1(1, 0, 0, 0, 0, 0, 0));
    gs_write(SCE_GS_PRIM, SCE_GS_SET_PRIM(SCE_GS_PRIM_SPRITE, 0, 0, 0, 1, 0, 1, 0, 0));
    gs_write(SCE_GS_RGBAQ, SCE_GS_SET_RGBAQ(red, green, blue, alpha, 0));
    int left = mgScreenOffx - (port_screen_width() - mgScreenWidth) / 2;
    gs_write(SCE_GS_XYZF2, SCE_GS_SET_XYZF2(left * GS_SUBPIXELS, mgScreenOffy * GS_SUBPIXELS, 0, 0));
    gs_write(SCE_GS_XYZF2, SCE_GS_SET_XYZF2((left + port_screen_width()) * GS_SUBPIXELS,
                                            (mgScreenOffy + mgScreenHeight) * GS_SUBPIXELS, 0, 0));
}

PORT_OVERRIDE void mgSetPkMoveImage(sceGsTex0 *src, mgRect<int> src_rect, sceGsTex0 *dst, int dst_x, int dst_y,
                                    int direction) {
    int left = src_rect.left / GS_SUBPIXELS;
    int top = src_rect.top / GS_SUBPIXELS;
    int width = src_rect.right / GS_SUBPIXELS - left + 1;
    int height = src_rect.bottom / GS_SUBPIXELS - top + 1;
    if (width <= 0 || height <= 0) return;
    gs_copy_image((u_int)src->TBP0, left, top, (u_int)dst->TBP0, dst_x / GS_SUBPIXELS, dst_y / GS_SUBPIXELS, width,
                  height);
}

PORT_OVERRIDE void mgSetPkMoveImage(sceGsTex0 *src, mgRect<int> src_rect, sceGsTex0 *dst, int dst_height,
                                    mgRect<int> dst_rect, mgCDrawEnv *env) {
    int width = dst->TBW * GS_FRAME_WIDTH_UNIT;
    if (dst_height % GS_FRAME_WIDTH_UNIT != 0) dst_height += GS_FRAME_WIDTH_UNIT - dst_height % GS_FRAME_WIDTH_UNIT;
    mgSetPkFrameBuffer(dst->TBP0 / GS_BLOCKS_PER_PAGE, width, dst_height, dst->PSM);
    if (env != NULL) {
        sceGsZbuf zbuf = mgZBUF_1;
        zbuf.bits.zmsk = env->zbuf.bits.zmsk;
        gs_write(SCE_GS_TEST_1, env->test.value);
        gs_write(SCE_GS_ZBUF_1, zbuf.value);
        gs_write(SCE_GS_ALPHA_1, env->alpha.value);
    } else {
        sceGsTest test = mgTEST_1;
        test.bits.ate = 0;
        test.bits.zte = 1;
        test.bits.ztst = SCE_GS_ALWAYS;
        test.bits.date = 0;
        sceGsZbuf zbuf = mgZBUF_1;
        zbuf.bits.zmsk = 1;
        sceGsAlpha alpha = mgALPHA_1;
        alpha.bits.a = SCE_GS_ALPHA_ZERO;
        alpha.bits.b = SCE_GS_ALPHA_ZERO;
        alpha.bits.c = SCE_GS_ALPHA_FIX;
        alpha.bits.d = SCE_GS_ALPHA_CS;
        gs_write(SCE_GS_TEST_1, test.value);
        gs_write(SCE_GS_ZBUF_1, zbuf.value);
        gs_write(SCE_GS_ALPHA_1, alpha.value);
    }
    gs_texture_source(NULL);
    gs_write(SCE_GS_TEX1_1, SCE_GS_SET_TEX1(1, 0, 1, 0, 0, 0, 0));
    gs_write(SCE_GS_TEX0_1, src->value);
    gs_write(SCE_GS_PRIM, SCE_GS_SET_PRIM(SCE_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
    gs_write(SCE_GS_RGBAQ, SCE_GS_SET_RGBAQ(GS_ALPHA_ONE, GS_ALPHA_ONE, GS_ALPHA_ONE, GS_ALPHA_ONE, 0));
    gs_write(SCE_GS_UV, SCE_GS_SET_UV(src_rect.left & UV_MASK, src_rect.top & UV_MASK));
    gs_write(SCE_GS_XYZF2, SCE_GS_SET_XYZF2(mgScreenOffx * GS_SUBPIXELS + dst_rect.left,
                                            mgScreenOffy * GS_SUBPIXELS + dst_rect.top, 0, 0));
    gs_write(SCE_GS_UV, SCE_GS_SET_UV(src_rect.right & UV_MASK, src_rect.bottom & UV_MASK));
    gs_write(SCE_GS_XYZF2, SCE_GS_SET_XYZF2(mgScreenOffx * GS_SUBPIXELS + dst_rect.right,
                                            mgScreenOffy * GS_SUBPIXELS + dst_rect.bottom, 0, 0));
    mgSetPkFrameBuffer(-1, -1, -1, -1);
}

static void store_pixels(int psm, const u_int *pixels, int count, u_long128 *buffer) {
    unsigned char *out = (unsigned char *)buffer;
    for (int i = 0; i < count; ++i) {
        u_int color = pixels[i];
        u_int alpha = ((color >> 24) + 1) / 2;
        switch (psm) {
            case SCE_GS_PSMCT24:
                out[i * 3] = (unsigned char)color;
                out[i * 3 + 1] = (unsigned char)(color >> 8);
                out[i * 3 + 2] = (unsigned char)(color >> 16);
                break;
            case SCE_GS_PSMCT16:
            case SCE_GS_PSMCT16S: {
                u_int packed = ((color >> 3) & 31) | ((color >> 11) & 31) << 5 | ((color >> 19) & 31) << 10 |
                               (alpha >= 0x80 ? 0x8000u : 0);
                out[i * 2] = (unsigned char)packed;
                out[i * 2 + 1] = (unsigned char)(packed >> 8);
                break;
            }
            default: {
                u_int value = (color & 0xffffff) | alpha << 24;
                std::memcpy(out + i * 4, &value, 4);
                break;
            }
        }
    }
}

PORT_OVERRIDE int mgStoreImage(mgCTexture *texture, u_long128 *buffer) {
    if (texture == NULL || buffer == NULL) return 0;
    int width = texture->width, height = texture->height;
    int source = gs_texture(texture->tex0.TBP0, width, height);
    u_int *pixels = (u_int *)std::malloc((size_t)width * (size_t)height * 4);
    render_download(source, gs_read_shift((u_int)texture->tex0.TBP0), 0, width, height, pixels);
    store_pixels((int)texture->tex0.PSM, pixels, width * height, buffer);
    std::free(pixels);
    return texture->bpp * (width * height);
}

extern "C" void port_store_screen(mgCTexture *screen, int width, u_long128 *buffer) {
    if (screen == NULL || buffer == NULL) return;
    int height = screen->height;
    int source = gs_texture(screen->tex0.TBP0, screen->width, height);
    int left = gs_read_shift((u_int)screen->tex0.TBP0) - (width - screen->width) / 2;
    int from = left < 0 ? 0 : left;
    int to = left + width < render_width(source) ? left + width : render_width(source);
    u_int *pixels = (u_int *)std::calloc((size_t)width * (size_t)height, 4);
    if (to > from) {
        u_int *shown = (u_int *)std::malloc((size_t)(to - from) * (size_t)height * 4);
        render_download(source, from, 0, to - from, height, shown);
        for (int y = 0; y < height; ++y)
            std::memcpy(pixels + y * width + (from - left), shown + y * (to - from), (size_t)(to - from) * 4);
        std::free(shown);
    }
    store_pixels((int)screen->tex0.PSM, pixels, width * height, buffer);
    std::free(pixels);
}

PORT_GAME_BEGIN
static u_long128 cross_image[WIDE_IMAGE_PIXELS / 4];
static u_long128 sepia_image[WIDE_IMAGE_PIXELS / 4];
PORT_GAME_END

extern "C" u_long128 *port_cross_image(u_long128 *buffer) {
    return port_wide_width() == mgScreenWidth ? buffer : cross_image;
}

extern "C" u_long128 *port_sepia_image(u_long128 *buffer) {
    return port_wide_width() == mgScreenWidth ? buffer : sepia_image;
}

extern "C" float port_screen_left(float x) {
    return x <= 0.0f ? x - (float)((port_screen_width() - mgScreenWidth) / 2) : x;
}

extern "C" float port_screen_right(float x) {
    return x >= (float)mgScreenWidth ? x + (float)((port_screen_width() - mgScreenWidth) / 2) : x;
}

extern "C" void port_screen_bars(int textured, float left, float top, float right, float bottom, int alpha, int blend) {
    int side = (port_screen_width() - mgScreenWidth) / 2;
    if (!textured || side == 0 || left > 0.0f || right < (float)mgScreenWidth || top > 0.0f ||
        bottom < (float)STILL_HEIGHT)
        return;
    mgCDrawPrim prim;
    prim.Initialize(NULL, NULL);
    prim.DepthTestEnable(1);
    prim.DepthTest(MG_DEPTH_TEST_ALWAYS);
    prim.TextureMapEnable(0);
    prim.AlphaBlendEnable(1);
    prim.AlphaBlend(blend);
    prim.AlphaTestEnable(0);
    prim.Begin(MG_PRIM_SPRITE);
    prim.Color(0, 0, 0, alpha);
    prim.Vertex((float)-side, top, 0.0f);
    prim.Vertex(left, bottom, 0.0f);
    prim.Vertex(right, top, 0.0f);
    prim.Vertex((float)(mgScreenWidth + side), bottom, 0.0f);
    prim.End();
}

int port_display_target(int *width, int *height) {
    *width = display_width;
    *height = display_height;
    return display_target;
}
