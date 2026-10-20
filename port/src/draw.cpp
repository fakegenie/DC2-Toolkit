#include "draw.hpp"
#include "port_stats.h"
#include "mg_texture.hpp"
#include "mglib.hpp"
#include "port_graphics.h"
#include "sce/libgraph.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

enum { CACHE_LIMIT = 4096, CACHE_IDLE_FRAMES = 300, COPY_LIMIT = 8 };
enum {
    GS_PRIM = 0x00, GS_RGBAQ = 0x01, GS_ST = 0x02, GS_UV = 0x03, GS_XYZF2 = 0x04, GS_XYZ2 = 0x05, GS_TEX0 = 0x06,
    GS_CLAMP = 0x08, GS_FOG = 0x0a, GS_XYZF3 = 0x0c, GS_XYZ3 = 0x0d, GS_TEX1 = 0x14, GS_XYOFFSET = 0x18,
    GS_TEXA = 0x3b, GS_BITBLTBUF = 0x50, GS_TRXPOS = 0x51, GS_TRXREG = 0x52, GS_TRXDIR = 0x53, GS_FOGCOL = 0x3d, GS_SCISSOR = 0x40, GS_ALPHA = 0x42, GS_TEST = 0x47, GS_FRAME = 0x4c,
    GS_ZBUF = 0x4e
};
static const float SAMPLE_OFFSET = 0.46875f;

enum { PRIM_POINT, PRIM_LINE, PRIM_LINE_STRIP, PRIM_TRIANGLE, PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_FAN, PRIM_SPRITE };

struct CachedTexture {
    const void *image;
    const void *clut;
    int psm;
    int width;
    int height;
    u_long checksum;
    int render;
    u_int checked;
    u_int used;
    char name[32];
};

struct TargetCopy {
    int render;
    int width;
    int height;
};

GsRegisters gs;

static int frame_target;
static RenderVertex queue[3];
static int queued;
static mgCTexture source_texture;
static const mgCTexture *source;
static CachedTexture cache[CACHE_LIMIT];
static TargetCopy copies[COPY_LIMIT];
static u_int frame_number = 1;
static std::unordered_map<const void *, std::vector<u_int>> palettes;

static u_int field(u_long value, int shift, int bits) {
    return (u_int)((value >> shift) & ((1ull << bits) - 1));
}

static int smaller(int a, int b) {
    return a < b ? a : b;
}

static mgCTexture *resident_texture(u_int base, int clut);

void gs_reset() {
    memset(&gs, 0, sizeof gs);
    gs.q = 1.0f;
    gs.tex1 = 0x20;
    gs.texa = (u_long)0x80 << 32;
    gs.test = (u_long)3 << 16;
    gs.zbuf = (u_long)1 << 32;
    queued = 0;
}

void gs_frame(int fbp, int width, int height, int psm) {
    const mgCTexture *texture = resident_texture((u_int)fbp * 32, 0);
    if (texture) frame_target = gs_texture_for(texture);
    else frame_target = render_target((u_int)fbp * 32, width, height);
    render_bind(frame_target);
    gs.frame = (u_long)fbp | (u_long)((width + 63) / 64) << 16 | (u_long)psm << 24;
    gs.xyoffset = (u_long)((0x800 - width / 2) * 16) | (u_long)((0x800 - height / 2) * 16) << 32;
    int clipped_width = smaller(width, render_width(frame_target));
    int clipped_height = smaller(height, render_height(frame_target));
    gs.scissor = (u_long)(clipped_width - 1) << 16 | (u_long)(clipped_height - 1) << 48;
}

int gs_frame_target() {
    return frame_target;
}

static u_int gs_alpha(u_int alpha) {
    return alpha >= 0x80 ? 255 : alpha * 2;
}

static u_int clut_color(const u_int *clut, int index) {
    u_int color = clut[index];
    return (color & 0xffffff) | gs_alpha(color >> 24) << 24;
}

static int image_bytes(int width, int height, int bpp) {
    return width * height * (bpp == 24 ? 3 : bpp == 4 ? 1 : bpp / 8) / (bpp == 4 ? 2 : 1);
}

static u_long checksum(const void *data, int bytes) {
    const unsigned char *p = (const unsigned char *)data;
    u_long hash = 0xcbf29ce484222325ull;
    int i = 0;
    for (; i + 8 <= bytes; i += 8) {
        u_long word;
        memcpy(&word, p + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    for (; i < bytes; ++i) hash = (hash ^ p[i]) * 0x100000001b3ull;
    return hash;
}

static const u_int *palette_of(const mgCTexture *texture) {
    auto found = palettes.find(texture->clut);
    return found != palettes.end() ? found->second.data() : (const u_int *)texture->clut;
}

static u_long texture_checksum(const mgCTexture *texture) {
    u_long hash = checksum(texture->image[0], image_bytes(texture->width, texture->height, texture->bpp));
    if (texture->clut) hash ^= checksum(palette_of(texture), (texture->bpp == 4 ? 16 : 256) * 4) * 31;
    return hash;
}

static const unsigned short column32_x[64] = {
    0, 4, 16, 20, 32, 36, 48, 52, 256, 260, 272, 276, 288, 292, 304, 308,
    1024, 1028, 1040, 1044, 1056, 1060, 1072, 1076, 1280, 1284, 1296, 1300, 1312, 1316, 1328, 1332,
    4096, 4100, 4112, 4116, 4128, 4132, 4144, 4148, 4352, 4356, 4368, 4372, 4384, 4388, 4400, 4404,
    5120, 5124, 5136, 5140, 5152, 5156, 5168, 5172, 5376, 5380, 5392, 5396, 5408, 5412, 5424, 5428};

static const unsigned short column32_y[32] = {
    0, 8, 64, 72, 128, 136, 192, 200, 512, 520, 576, 584, 640, 648, 704, 712,
    2048, 2056, 2112, 2120, 2176, 2184, 2240, 2248, 2560, 2568, 2624, 2632, 2688, 2696, 2752, 2760};

static u_int address32(u_int pages_wide, u_int x, u_int y) {
    u_int page = ((x >> 6) + (y >> 5) * pages_wide) * 32;
    return (page << 8) + column32_x[x & 63] + column32_y[y & 31];
}

static u_int address8(u_int pages_wide, u_int x, u_int y) {
    u_int bx = (x >> 4) & 7;
    u_int by = (y >> 4) & 3;
    u_int block = (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2);
    block += ((x >> 7) + (y >> 6) * pages_wide) * 32;
    u_int column = ((y & 2) >> 1) | ((x & 8) >> 2) | ((x & 1) << 2) | ((y & 1) << 3) | ((x & 2) << 3) |
                   (((x ^ y ^ (y << 1)) & 4) << 3) | ((y & 12) << 4);
    return (block << 8) + column;
}

static void unswizzle8(const unsigned char *image, int width, int height, std::vector<unsigned char> &linear) {
    u_int pages_wide = (u_int)(width + 127) / 128;
    int words_wide = width / 2, words_high = height / 2;
    std::vector<int> source((size_t)pages_wide * (size_t)((height + 63) / 64) * 8192 / 4, -1);
    for (int y = 0; y < words_high; ++y)
        for (int x = 0; x < words_wide; ++x) {
            u_int word = address32(pages_wide, (u_int)x, (u_int)y) / 4;
            if (word < source.size()) source[word] = y * words_wide + x;
        }
    linear.assign((size_t)width * (size_t)height, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            u_int address = address8(pages_wide, (u_int)x, (u_int)y);
            if (address / 4 >= source.size() || source[address / 4] < 0) continue;
            linear[(size_t)(y * width + x)] = image[source[address / 4] * 4 + address % 4];
        }
}

static void decode(const mgCTexture *texture, std::vector<u_int> &pixels) {
    int width = texture->width, height = texture->height, count = width * height;
    const unsigned char *image = (const unsigned char *)texture->image[0];
    std::vector<unsigned char> linear;
    if (texture->swizzled && texture->bpp == 8) {
        unswizzle8(image, width, height, linear);
        image = linear.data();
    }
    const u_int *clut = texture->clut ? palette_of(texture) : NULL;
    pixels.resize((size_t)count);
    for (int i = 0; i < count; ++i) {
        u_int color = 0;
        switch (texture->bpp) {
            case 32: {
                u_int raw;
                memcpy(&raw, image + i * 4, 4);
                color = (raw & 0xffffff) | gs_alpha(raw >> 24) << 24;
                break;
            }
            case 24:
                color = image[i * 3] | image[i * 3 + 1] << 8 | image[i * 3 + 2] << 16 | 0xffu << 24;
                break;
            case 16: {
                u_int raw = image[i * 2] | image[i * 2 + 1] << 8;
                color = (raw & 31) << 3 | ((raw >> 5) & 31) << 11 | ((raw >> 10) & 31) << 19 |
                        (raw & 0x8000 ? 0xffu << 24 : 0);
                break;
            }
            case 8: {
                int index = image[i];
                color = clut ? clut_color(clut, (index & 0xe7) | ((index & 0x08) << 1) | ((index & 0x10) >> 1)) : 0;
                break;
            }
            case 4: {
                int index = (image[i / 2] >> ((i & 1) * 4)) & 15;
                color = clut ? clut_color(clut, index) : 0;
                break;
            }
        }
        pixels[(size_t)i] = color;
    }
}

static CachedTexture *cached(const mgCTexture *texture) {
    size_t start = ((size_t)texture->image[0] >> 4) % CACHE_LIMIT;
    CachedTexture *empty = NULL;
    for (size_t step = 0; step < CACHE_LIMIT; ++step) {
        CachedTexture *entry = &cache[(start + step) % CACHE_LIMIT];
        if (!entry->render) {
            if (!empty) empty = entry;
            continue;
        }
        if (entry->image == texture->image[0] && entry->clut == texture->clut && entry->psm == texture->bpp &&
            entry->width == texture->width && entry->height == texture->height)
            return entry;
    }
    if (!empty) {
        fputs("draw: the texture cache is full\n", stderr);
        return NULL;
    }
    empty->image = texture->image[0];
    empty->clut = texture->clut;
    empty->psm = texture->bpp;
    empty->width = texture->width;
    empty->height = texture->height;
    empty->checksum = 0;
    empty->checked = 0;
    snprintf(empty->name, sizeof empty->name, "%s", texture->name);
    empty->render = render_texture(texture->width, texture->height);
    port_stats.cached_textures++;
    return empty;
}

static mgCTexture *list_match(mgCTexture *list, u_int base, int clut) {
    for (; list; list = list->next) {
        if (clut) {
            if (list->clut && list->bpp <= 8 && (u_int)list->tex0.CBP == base) return list;
        } else if (list->image[0] && list->width > 0 && list->height > 0 && (u_int)list->tex0.TBP0 == base) {
            return list;
        }
    }
    return NULL;
}

static mgCTexture *resident_texture(u_int base, int clut) {
    mgCTexture *found = NULL;
    int block = mgTexManager.last_block;
    if (mgTexManager.blocks && block >= 0 && block < mgTexManager.block_max)
        found = list_match(mgTexManager.blocks[block].texture, base, clut);
    if (!found) found = list_match(mgTexManager.fix_block.texture, base, clut);
    return found;
}

int gs_texture(u_long tex0, int width, int height) {
    u_int base = field(tex0, 0, 14);
    const mgCTexture *texture = resident_texture(base, 0);
    if (texture) return gs_texture_for(texture);
    return render_target(base, width, height);
}

static int target_copy(int target, int width, int height) {
    TargetCopy *copy = NULL;
    for (int i = 0; i < COPY_LIMIT && !copy; ++i)
        if (!copies[i].render || (copies[i].width == width && copies[i].height == height)) copy = &copies[i];
    if (!copy) return target;
    if (!copy->render) {
        copy->render = render_scaled_texture(width, height);
        copy->width = width;
        copy->height = height;
    }
    render_copy(target, 0, 0, copy->render, 0, 0, width, height);
    return copy->render;
}

int gs_sampled(int texture, int width, int height, int repeat) {
    RenderSlot slot;
    if (!render_slot(texture, &slot) || !slot.target) return texture;
    int copy_width = smaller(width, slot.width);
    int copy_height = smaller(height, slot.height);
    int partial = copy_width < slot.width || copy_height < slot.height;
    if (texture != render_bound() && !(repeat && partial)) return texture;
    return target_copy(texture, copy_width, copy_height);
}

void gs_copy_image(u_int source_base, int source_x, int source_y, u_int destination_base, int destination_x,
                   int destination_y, int width, int height) {
    mgCTexture *source_clut = resident_texture(source_base, 1);
    mgCTexture *destination_clut = resident_texture(destination_base, 1);
    if (source_clut && destination_clut) {
        const u_int *colors = palette_of(source_clut);
        palettes[destination_clut->clut].assign(colors, colors + (destination_clut->bpp == 8 ? 256 : 16));
        return;
    }
    if (width <= 0 || height <= 0 || source_x < 0 || source_y < 0 || destination_x < 0 || destination_y < 0) return;
    int source = gs_texture(source_base, source_x + width, source_y + height);
    int destination = gs_texture(destination_base, destination_x + width, destination_y + height);
    if (!source || !destination) return;
    if (source == destination && source_x == destination_x && source_y == destination_y) return;
    width = smaller(width, smaller(render_width(source) - source_x, render_width(destination) - destination_x));
    height = smaller(height, smaller(render_height(source) - source_y, render_height(destination) - destination_y));
    render_copy(source, source_x, source_y, destination, destination_x, destination_y, width, height);
}

extern "C" void port_depth_download(int x, int y, int width, int height, unsigned *pixels) {
    render_download_depth(frame_target, x, y, width, height, pixels);
}

int gs_texture_for(const mgCTexture *texture) {
    if (!texture) return 0;
    if (!texture->image[0] || texture->width <= 0 || texture->height <= 0)
        return gs_texture(texture->tex0_bits, texture->width > 0 ? texture->width : 64,
                          texture->height > 0 ? texture->height : 64);
    CachedTexture *entry = cached(texture);
    if (!entry) return 0;
    entry->used = frame_number;
    if (entry->checked == frame_number) return entry->render;
    entry->checked = frame_number;
    u_long hash = texture_checksum(texture);
    if (hash == entry->checksum) return entry->render;
    entry->checksum = hash;
    std::vector<u_int> pixels;
    decode(texture, pixels);
    port_stats.texture_decodes++;
    render_upload(entry->render, 0, 0, texture->width, texture->height, pixels.data());
    return entry->render;
}

void gs_texture_source(const mgCTexture *texture) {
    source = NULL;
    if (texture == NULL) return;
    memcpy((void *)&source_texture, (const void *)texture, sizeof source_texture);
    source = &source_texture;
}

void gs_next_frame() {
    frame_number++;
    for (int i = 0; i < CACHE_LIMIT; ++i) {
        CachedTexture *entry = &cache[i];
        if (!entry->render || frame_number - entry->used < CACHE_IDLE_FRAMES) continue;
        render_release(entry->render);
        memset(entry, 0, sizeof *entry);
        port_stats.cached_textures--;
    }
}

static int bound_texture(int repeat, int *width, int *height) {
    int texture_width = 1 << field(gs.tex0, 26, 4);
    int texture_height = 1 << field(gs.tex0, 30, 4);
    int texture = 0;
    if (source && source->tex0.TBP0 == field(gs.tex0, 0, 14) && source->image[0]) texture = gs_texture_for(source);
    else texture = gs_texture(gs.tex0, texture_width, texture_height);
    texture = gs_sampled(texture, texture_width, texture_height, repeat);
    *width = texture ? render_width(texture) : 1;
    *height = texture ? render_height(texture) : 1;
    return texture;
}

static void state_for(RenderState *state, int *texture_width, int *texture_height) {
    memset(state, 0, sizeof *state);
    u_long prim = gs.prim;
    *texture_width = *texture_height = 1;
    if (field(prim, 4, 1)) {
        state->repeat_u = field(gs.clamp, 0, 2) == 0;
        state->repeat_v = field(gs.clamp, 2, 2) == 0;
        state->texture = bound_texture(state->repeat_u || state->repeat_v, texture_width, texture_height);
        state->linear = field(gs.tex1, 5, 1);
        state->decal = field(gs.tex0, 35, 2) == 1;
        state->vertex_alpha = !field(gs.tex0, 34, 1);
        state->texa = field(gs.tex0, 20, 6) == SCE_GS_PSMCT24;
    }
    state->blend = field(prim, 6, 1);
    state->alpha = gs.alpha;
    state->test = gs.test;
    state->texa_register = gs.texa;
    state->fog = field(prim, 5, 1);
    state->fog_color = (u_int)(gs.fogcol & 0xffffff);
    state->depth_write = !field(gs.zbuf, 32, 1);
    state->color_mask = (u_int)(gs.frame >> 32);
    state->scissor[0] = (int)field(gs.scissor, 0, 11);
    state->scissor[1] = (int)field(gs.scissor, 32, 11);
    state->scissor[2] = (int)field(gs.scissor, 16, 11);
    state->scissor[3] = (int)field(gs.scissor, 48, 11);
}

void gs_base_state(RenderState *state) {
    memset(state, 0, sizeof *state);
    state->texa_register = gs.texa;
    state->color_mask = (u_int)(gs.frame >> 32);
    state->fog_color = (u_int)(gs.fogcol & 0xffffff);
    state->scissor[0] = (int)field(gs.scissor, 0, 11);
    state->scissor[1] = (int)field(gs.scissor, 32, 11);
    state->scissor[2] = (int)field(gs.scissor, 16, 11);
    state->scissor[3] = (int)field(gs.scissor, 48, 11);
}

void gs_texture_state(RenderState *state, const mgCTexture *texture, float *scale_u, float *scale_v) {
    *scale_u = *scale_v = 1.0f;
    if (!texture) return;
    state->repeat_u = field(texture->clamp_bits, 0, 2) == 0;
    state->repeat_v = field(texture->clamp_bits, 2, 2) == 0;
    state->texture = gs_sampled(gs_texture_for(texture), 1 << field(texture->tex0_bits, 26, 4),
                                1 << field(texture->tex0_bits, 30, 4), state->repeat_u || state->repeat_v);
    if (!state->texture) return;
    state->linear = field(texture->tex1_bits, 5, 1);
    state->decal = field(texture->tex0_bits, 35, 2) == 1;
    state->vertex_alpha = !field(texture->tex0_bits, 34, 1);
    state->texa = field(texture->tex0_bits, 20, 6) == SCE_GS_PSMCT24;
    *scale_u = (float)(1 << field(texture->tex0_bits, 26, 4)) / (float)render_width(state->texture);
    *scale_v = (float)(1 << field(texture->tex0_bits, 30, 4)) / (float)render_height(state->texture);
}

static float depth_scale();

void gs_screen_mapping(float screen[4], float *depth) {
    float width = (float)(frame_target ? render_width(frame_target) : 1);
    float height = (float)(frame_target ? render_height(frame_target) : 1);
    float offset_x = (float)field(gs.xyoffset, 0, 16) / 16.0f;
    float offset_y = (float)field(gs.xyoffset, 32, 16) / 16.0f;
    screen[0] = 2.0f / width;
    screen[1] = -2.0f * offset_x / width - 1.0f;
    screen[2] = -2.0f / height;
    screen[3] = 1.0f + 2.0f * offset_y / height;
    *depth = depth_scale();
}

static float depth_scale() {
    switch (field(gs.zbuf, 24, 4)) {
        case 0: return 1.0f / 4294967296.0f;
        case 1: return 1.0f / 16777216.0f;
        default: return 1.0f / 65536.0f;
    }
}

static int sample_range(float p0, float p1, float t0, float t1, float *low, float *high) {
    float start = p0 < p1 ? p0 : p1, end = p0 < p1 ? p1 : p0;
    float first = ceilf(start - 0.5f) + 0.5f, last = ceilf(end - 0.5f) - 0.5f;
    if (last < first) return 0;
    float at_first = t0 + (first - p0) * (t1 - t0) / (p1 - p0);
    float at_last = t0 + (last - p0) * (t1 - t0) / (p1 - p0);
    *low = at_first < at_last ? at_first : at_last;
    *high = at_first < at_last ? at_last : at_first;
    return 1;
}

static void align_sprite(RenderVertex *a, RenderVertex *b, int textured) {
    float limits[4];
    int clamped = textured && sample_range(a->x, b->x, a->u, b->u, &limits[0], &limits[2]) &&
                  sample_range(a->y, b->y, a->v, b->v, &limits[1], &limits[3]);
    float shift_u = b->x != a->x ? SAMPLE_OFFSET * (b->u - a->u) / (b->x - a->x) : 0.0f;
    float shift_v = b->y != a->y ? SAMPLE_OFFSET * (b->v - a->v) / (b->y - a->y) : 0.0f;
    a->x -= SAMPLE_OFFSET, b->x -= SAMPLE_OFFSET, a->y -= SAMPLE_OFFSET, b->y -= SAMPLE_OFFSET;
    a->u -= shift_u, b->u -= shift_u, a->v -= shift_v, b->v -= shift_v;
    if (!clamped) return;
    memcpy(a->limits, limits, sizeof limits);
    memcpy(b->limits, limits, sizeof limits);
}

static void kick(u_int x, u_int y, u_int z, int fog, int draw) {
    RenderVertex *vertex = &queue[queued < 3 ? queued : 2];
    if (queued == 3) {
        queue[0] = queue[1];
        queue[1] = queue[2];
        queued = 2;
        vertex = &queue[2];
    }
    vertex->x = (float)((int)(x & 0xffff) - (int)field(gs.xyoffset, 0, 16)) / 16.0f + SAMPLE_OFFSET;
    vertex->y = (float)((int)(y & 0xffff) - (int)field(gs.xyoffset, 32, 16)) / 16.0f + SAMPLE_OFFSET;
    float depth = (float)z * depth_scale();
    vertex->z = depth > 1.0f ? 1.0f : depth;
    vertex->r = (unsigned char)field(gs.rgbaq, 0, 8);
    vertex->g = (unsigned char)field(gs.rgbaq, 8, 8);
    vertex->b = (unsigned char)field(gs.rgbaq, 16, 8);
    vertex->a = (unsigned char)field(gs.rgbaq, 24, 8);
    vertex->fog = (float)fog / 255.0f;
    if (field(gs.prim, 8, 1)) {
        vertex->u = (float)field(gs.uv, 0, 14) / 16.0f;
        vertex->v = (float)field(gs.uv, 16, 14) / 16.0f;
    } else {
        float s, t;
        u_int bits = (u_int)gs.st;
        memcpy(&s, &bits, 4);
        bits = (u_int)(gs.st >> 32);
        memcpy(&t, &bits, 4);
        float q = gs.q != 0.0f ? gs.q : 1.0f;
        vertex->u = s / q * (float)(1 << field(gs.tex0, 26, 4));
        vertex->v = t / q * (float)(1 << field(gs.tex0, 30, 4));
    }
    queued++;
    int type = (int)field(gs.prim, 0, 3);
    int needed = type == PRIM_POINT ? 1 : type <= PRIM_LINE_STRIP || type == PRIM_SPRITE ? 2 : 3;
    if (queued < needed) return;
    if (draw && !render_toggles.no_2d) {
        RenderState state;
        int width, height;
        state_for(&state, &width, &height);
        RenderVertex out[6];
        int count = needed;
        for (int i = 0; i < needed; ++i) out[i] = queue[queued - needed + i];
        for (int i = 0; i < count; ++i) {
            out[i].u /= (float)width;
            out[i].v /= (float)height;
        }
        if (!field(gs.prim, 3, 1))
            for (int i = 0; i < count - 1; ++i) {
                out[i].r = out[count - 1].r;
                out[i].g = out[count - 1].g;
                out[i].b = out[count - 1].b;
                out[i].a = out[count - 1].a;
            }
        int topology = type == PRIM_POINT ? RENDER_POINTS : type <= PRIM_LINE_STRIP ? RENDER_LINES : RENDER_TRIANGLES;
        if (type == PRIM_SPRITE) {
            RenderVertex a = out[0], b = out[1];
            RenderSlot target;
            if (render_slot(render_bound(), &target) && (target.scale > 1 || target.samples > 1))
                align_sprite(&a, &b, state.texture != 0);
            RenderVertex corner_b = b, corner_c = b;
            corner_b.x = b.x, corner_b.y = a.y, corner_b.u = b.u, corner_b.v = a.v;
            corner_c.x = a.x, corner_c.y = b.y, corner_c.u = a.u, corner_c.v = b.v;
            a.r = b.r, a.g = b.g, a.b = b.b, a.a = b.a, a.z = b.z, a.fog = b.fog;
            out[0] = a, out[1] = corner_b, out[2] = corner_c;
            out[3] = corner_b, out[4] = b, out[5] = corner_c;
            count = 6;
        }
        render_source = RENDER_SOURCE_GS;
        render_draw(&state, topology, out, count);
    }
    if (type == PRIM_LINE || type == PRIM_TRIANGLE || type == PRIM_SPRITE || type == PRIM_POINT) queued = 0;
    else if (type == PRIM_TRIANGLE_FAN) {
        queue[1] = queue[2];
        queued = 2;
    } else if (type == PRIM_LINE_STRIP) {
        queue[0] = queue[1];
        queued = 1;
    }
}

void gs_write(int reg, u_long value) {
    switch (reg) {
        case GS_PRIM:
            gs.prim = value;
            queued = 0;
            break;
        case GS_RGBAQ: {
            gs.rgbaq = value;
            u_int bits = (u_int)(value >> 32);
            memcpy(&gs.q, &bits, 4);
            break;
        }
        case GS_ST: gs.st = value; break;
        case GS_UV: gs.uv = value; break;
        case GS_XYZF2:
            kick(field(value, 0, 16), field(value, 16, 16), field(value, 32, 24), (int)field(value, 56, 8), 1);
            break;
        case GS_XYZ2:
            kick(field(value, 0, 16), field(value, 16, 16), (u_int)(value >> 32), (int)field(gs.fog, 56, 8), 1);
            break;
        case GS_XYZF3:
            kick(field(value, 0, 16), field(value, 16, 16), field(value, 32, 24), (int)field(value, 56, 8), 0);
            break;
        case GS_XYZ3:
            kick(field(value, 0, 16), field(value, 16, 16), (u_int)(value >> 32), (int)field(gs.fog, 56, 8), 0);
            break;
        case GS_FOG: gs.fog = value; break;
        case GS_TEX0: gs.tex0 = value; break;
        case GS_TEX1: gs.tex1 = value; break;
        case GS_CLAMP: gs.clamp = value; break;
        case GS_TEXA: gs.texa = value; break;
        case GS_FOGCOL: gs.fogcol = value; break;
        case GS_SCISSOR: gs.scissor = value; break;
        case GS_ALPHA: gs.alpha = value; break;
        case GS_TEST: gs.test = value; break;
        case GS_ZBUF: gs.zbuf = value; break;
        case GS_XYOFFSET: gs.xyoffset = value; break;
        case GS_BITBLTBUF: gs.bitbltbuf = value; break;
        case GS_TRXPOS: gs.trxpos = value; break;
        case GS_TRXREG: gs.trxreg = value; break;
        case GS_TRXDIR:
            if ((value & 3) == 2)
                gs_copy_image(field(gs.bitbltbuf, 0, 14), (int)field(gs.trxpos, 0, 11), (int)field(gs.trxpos, 16, 11),
                              field(gs.bitbltbuf, 32, 14), (int)field(gs.trxpos, 32, 11),
                              (int)field(gs.trxpos, 48, 11), (int)field(gs.trxreg, 0, 12),
                              (int)field(gs.trxreg, 32, 12));
            break;
        case GS_FRAME:
            gs.frame = value;
            break;
    }
}

void gs_packed(int reg, const u_int *words) {
    switch (reg) {
        case 0x0: gs_write(GS_PRIM, words[0] & 0x7ff); break;
        case 0x1:
            gs.rgbaq = (u_long)(words[0] & 255) | (u_long)(words[1] & 255) << 8 | (u_long)(words[2] & 255) << 16 |
                       (u_long)(words[3] & 255) << 24;
            break;
        case 0x2: {
            gs.st = (u_long)words[0] | (u_long)words[1] << 32;
            memcpy(&gs.q, &words[2], 4);
            break;
        }
        case 0x3: gs.uv = (u_long)(words[0] & 0x3fff) | (u_long)(words[1] & 0x3fff) << 16; break;
        case 0x4:
            kick(words[0] & 0xffff, words[1] & 0xffff, (words[2] >> 4) & 0xffffff, (int)((words[3] >> 4) & 255),
                 !((words[3] >> 15) & 1));
            break;
        case 0x5: kick(words[0] & 0xffff, words[1] & 0xffff, words[2], (int)field(gs.fog, 56, 8), !((words[3] >> 15) & 1)); break;
        case 0xa: gs.fog = (u_long)((words[3] >> 4) & 255) << 56; break;
        case 0xe: gs_write((int)(words[2] & 255), (u_long)words[0] | (u_long)words[1] << 32); break;
    }
}

int gs_cache_info(int slot, CacheInfo *info) {
    for (int i = 0; i < CACHE_LIMIT; ++i) {
        const CachedTexture *entry = &cache[i];
        if (!entry->render || entry->render != slot) continue;
        snprintf(info->name, sizeof info->name, "%s", entry->name);
        info->image = entry->image;
        info->clut = entry->clut;
        info->psm = entry->psm;
        info->age = frame_number - entry->used;
        return 1;
    }
    return 0;
}
