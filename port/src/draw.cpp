#include "draw.hpp"
#include "port_stats.h"
#include "mg_texture.hpp"
#include "mglib.hpp"
#include "mg_drawprim.hpp"
#include "port_graphics.h"
#include "port_overrides.h"
#include "port_present.h"
#include "sce/libgraph.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

enum { CACHE_LIMIT = 4096, CACHE_IDLE_FRAMES = 300, COPY_LIMIT = 8 };
enum { LAYOUT_WIDTH = 512, LAYOUT_RIGHT = 511, WIDE_FRAME_LIMIT = 16, UV_WRAP = 1 << 14, HUD_SIDE = 85 };
enum { COORDINATE_CENTER = 2048 };
static const float SAMPLE_OFFSET = 0.46875f;
static const float TEXEL_TIE = 0.0625f;
static const float ONE_TO_ONE = 1.0f / 32.0f;

union GifPacked {
    struct {
        u_long prim : 11;
        u_long : 53;
        u_long : 64;
    } prim;
    struct {
        u_int red : 8;
        u_int : 24;
        u_int green : 8;
        u_int : 24;
        u_int blue : 8;
        u_int : 24;
        u_int alpha : 8;
        u_int : 24;
    } rgbaq;
    struct {
        float s;
        float t;
        float q;
        u_int : 32;
    } st;
    struct {
        u_int u : 14;
        u_int : 18;
        u_int v : 14;
        u_int : 18;
        u_long : 64;
    } uv;
    struct {
        u_int x : 16;
        u_int : 16;
        u_int y : 16;
        u_int : 16;
        u_int : 4;
        u_int z : 24;
        u_int : 4;
        u_int : 4;
        u_int f : 8;
        u_int : 3;
        u_int disable_draw : 1;
        u_int : 16;
    } xyzf;
    struct {
        u_int x : 16;
        u_int : 16;
        u_int y : 16;
        u_int : 16;
        u_int z;
        u_int : 15;
        u_int disable_draw : 1;
        u_int : 16;
    } xyz;
    struct {
        u_long : 64;
        u_int : 32;
        u_int : 4;
        u_int f : 8;
        u_int : 20;
    } fog;
    struct {
        u_long data;
        u_long address : 8;
        u_long : 56;
    } ad;
};

struct HdPictureHeader {
    char magic[6];
    u_int width;
    u_int height;
} __attribute__((packed));

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
    int hd;
    int drawn;
    int reloaded;
    int pristine;
};

struct TargetCopy {
    int render;
    int width;
    int height;
};

PORT_GAME_BEGIN
GsRegisters gs;
static int frame_shift;
static int screen_shift_x;
static int screen_shift_y;
static struct {
    u_int base;
    int shift;
} wide_frames[WIDE_FRAME_LIMIT];
static RenderVertex queue[3];
static int queued;
static mgCTexture source_texture;
static const mgCTexture *source;
PORT_GAME_END

static int frame_target;
static CachedTexture cache[CACHE_LIMIT];
static std::unordered_map<const void *, std::vector<CachedTexture *>> by_image;
static std::unordered_map<int, CachedTexture *> by_render;
static TargetCopy copies[COPY_LIMIT];
static u_int frame_number = 1;
static std::unordered_map<const void *, std::vector<u_int>> palettes;

static int smaller(int a, int b) {
    return a < b ? a : b;
}

static mgCTexture *resident_texture(u_int base, int clut);

void gs_reset() {
    memset(&gs, 0, sizeof gs);
    gs.q = 1.0f;
    gs.tex1.MMAG = 1;
    gs.texa.TA1 = GS_ALPHA_ONE;
    gs.test.ZTE = 1;
    gs.test.ZTST = SCE_GS_ALWAYS;
    gs.zbuf.ZMSK = 1;
    queued = 0;
}

void gs_frame(int fbp, int width, int height, int psm) {
    frame_shift = 0;
    const mgCTexture *texture = resident_texture((u_int)fbp * GS_BLOCKS_PER_PAGE, 0);
    if (texture) frame_target = gs_texture_for(texture);
    else frame_target = render_target((u_int)fbp * GS_BLOCKS_PER_PAGE, width, height);
    render_bind(frame_target);
    gs.frame = {};
    gs.frame.FBP = fbp;
    gs.frame.FBW = (width + GS_FRAME_WIDTH_UNIT - 1) / GS_FRAME_WIDTH_UNIT;
    gs.frame.PSM = psm;
    gs.xyoffset = {};
    gs.xyoffset.OFX = (COORDINATE_CENTER - width / 2) * GS_SUBPIXELS;
    gs.xyoffset.OFY = (COORDINATE_CENTER - height / 2) * GS_SUBPIXELS;
    gs.scissor = {};
    gs.scissor.SCAX1 = smaller(width, render_width(frame_target)) - 1;
    gs.scissor.SCAY1 = smaller(height, render_height(frame_target)) - 1;
}

int gs_frame_target() {
    return frame_target;
}

void gs_frame_shift(int pixels) {
    frame_shift = pixels;
    u_int base = gs.frame.FBP * GS_BLOCKS_PER_PAGE;
    int slot = WIDE_FRAME_LIMIT - 1;
    for (int i = 0; i < WIDE_FRAME_LIMIT; ++i)
        if (wide_frames[i].base == base) slot = i;
    for (int i = slot; i > 0; --i) wide_frames[i] = wide_frames[i - 1];
    wide_frames[0].base = base;
    wide_frames[0].shift = pixels;
}

int gs_read_shift(u_int base) {
    for (int i = 0; i < WIDE_FRAME_LIMIT; ++i)
        if (wide_frames[i].base == base) return wide_frames[i].shift;
    return 0;
}

static int hud_spread;

extern "C" void port_hud(int spread) {
    hud_spread = spread;
}

static void spread_hud(RenderVertex *vertices, int count) {
    if (!hud_spread || !frame_shift) return;
    float middle = (float)(frame_shift + LAYOUT_WIDTH / 2), low = vertices[0].x, high = vertices[0].x;
    for (int i = 1; i < count; ++i) {
        low = std::fmin(low, vertices[i].x);
        high = std::fmax(high, vertices[i].x);
    }
    int side = frame_shift < HUD_SIDE ? frame_shift : HUD_SIDE;
    float move = high <= middle ? (float)-side : low >= middle ? (float)side : 0.0f;
    for (int i = 0; i < count; ++i) vertices[i].x += move;
}

static sceGsScissor shifted_scissor(sceGsScissor scissor) {
    if (scissor.SCAX0 == 0 && scissor.SCAX1 == LAYOUT_RIGHT) scissor.SCAX1 += 2 * frame_shift;
    else {
        scissor.SCAX0 += frame_shift;
        scissor.SCAX1 += frame_shift;
    }
    return scissor;
}

static u_int gs_alpha(u_int alpha) {
    return alpha >= GS_ALPHA_ONE ? 255 : alpha * 2;
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
    u_long lanes[4] = {0xcbf29ce484222325ull, 0x84222325cbf29ce4ull, 0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full};
    int i = 0;
    for (; i + 32 <= bytes; i += 32) {
        u_long words[4];
        memcpy(words, p + i, 32);
        for (int lane = 0; lane < 4; ++lane) lanes[lane] = (lanes[lane] ^ words[lane]) * 0x100000001b3ull;
    }
    u_long hash = lanes[0] ^ lanes[1] * 3 ^ lanes[2] * 5 ^ lanes[3] * 7;
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

static int hd_picture(const char *name, int width, int height, std::vector<u_int> &pixels) {
    char lower[32];
    size_t length = 0;
    for (; name[length] && length + 1 < sizeof lower; ++length)
        lower[length] = (char)((name[length] >= 'A' && name[length] <= 'Z') ? name[length] + 32 : name[length]);
    lower[length] = 0;
    char path[1400];
    snprintf(path, sizeof path, "%s/dc2tk-hd/%s_%dx%d.rgba", port_overrides_folder(), lower, width, height);
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    HdPictureHeader header;
    int scale = 0;
    if (fread(&header, sizeof header, 1, file) == 1 && !memcmp(header.magic, "DC2HD1", sizeof header.magic)) {
        int found = width > 0 ? (int)(header.width / (u_int)width) : 0;
        if (found > 0 && header.width == (u_int)(found * width) && header.height == (u_int)(found * height)) {
            pixels.resize((size_t)header.width * header.height);
            if (fread(pixels.data(), 4, pixels.size(), file) == pixels.size()) scale = found;
        }
    }
    fclose(file);
    return scale;
}

static CachedTexture *cached(const mgCTexture *texture) {
    auto found = by_image.find(texture->image[0]);
    if (found != by_image.end())
        for (CachedTexture *entry : found->second)
            if (entry->clut == texture->clut && entry->psm == texture->bpp && entry->width == texture->width &&
                entry->height == texture->height)
                return entry;
    CachedTexture *empty = NULL;
    for (int i = 0; i < CACHE_LIMIT && !empty; ++i)
        if (!cache[i].render) empty = &cache[i];
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
    std::vector<u_int> hd;
    int scale = hd_picture(texture->name, texture->width, texture->height, hd);
    empty->hd = scale > 0;
    empty->render = scale > 0 ? render_texture_at_scale(texture->width, texture->height, scale)
                              : render_texture(texture->width, texture->height);
    if (scale > 0) render_upload_full(empty->render, (const uint32_t *)hd.data());
    by_image[texture->image[0]].push_back(empty);
    by_render[empty->render] = empty;
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

int gs_texture(u_int base, int width, int height) {
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

static int scaled_cache_texture(int slot) {
    int scale, samples;
    render_quality(&scale, &samples);
    if (scale == 1 || render_scaled(slot)) return slot;
    auto found = by_render.find(slot);
    if (found == by_render.end() || found->second->hd) return slot;
    CachedTexture *entry = found->second;
    int scaled = render_scaled_texture(entry->width, entry->height);
    render_copy(slot, 0, 0, scaled, 0, 0, entry->width, entry->height);
    by_render.erase(slot);
    by_render[scaled] = entry;
    entry->render = scaled;
    if (render_bound() == slot) render_bind(scaled);
    if (frame_target == slot) frame_target = scaled;
    render_release(slot);
    return scaled;
}

void gs_drawing_scaled(int texture) {
    if (texture != frame_target) scaled_cache_texture(frame_target);
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
    source_x += gs_read_shift(source_base);
    destination_x += gs_read_shift(destination_base);
    if (width <= 0 || height <= 0 || source_x < 0 || source_y < 0 || destination_x < 0 || destination_y < 0) return;
    int source = gs_texture(source_base, source_x + width, source_y + height);
    int destination = gs_texture(destination_base, destination_x + width, destination_y + height);
    if (!source || !destination) return;
    if (source == destination && source_x == destination_x && source_y == destination_y) return;
    if (source != destination && render_scaled(source)) destination = scaled_cache_texture(destination);
    width = smaller(width, smaller(render_width(source) - source_x, render_width(destination) - destination_x));
    height = smaller(height, smaller(render_height(source) - source_y, render_height(destination) - destination_y));
    render_copy(source, source_x, source_y, destination, destination_x, destination_y, width, height);
    auto drawn = by_render.find(destination);
    if (drawn != by_render.end()) drawn->second->drawn = 1;
    const mgCTexture *texture = resident_texture(source_base, 0);
    if (source_base != destination_base || !texture) return;
    auto variants = by_image.find(texture->image[0]);
    if (variants == by_image.end()) return;
    for (CachedTexture *palette : variants->second) {
        if (palette->render == destination) continue;
        mgCTexture variant = *texture;
        variant.clut = (decltype(variant.clut))palette->clut;
        gs_texture_for(&variant);
        render_copy(palette->render, source_x, source_y, palette->render, destination_x, destination_y, width, height);
        palette->drawn = 1;
    }
}

extern "C" void port_texture_loaded(const void *image) {
    auto found = by_image.find(image);
    if (found == by_image.end()) return;
    for (CachedTexture *entry : found->second) {
        if (!entry->drawn) continue;
        entry->drawn = 0;
        entry->reloaded = 1;
        entry->checked = 0;
    }
}

extern "C" void port_depth_download(int x, int y, int width, int height, unsigned *pixels) {
    render_download_depth(frame_target, x, y, width, height, pixels);
}

int gs_texture_for(const mgCTexture *texture) {
    if (!texture) return 0;
    if (!texture->image[0] || texture->width <= 0 || texture->height <= 0)
        return gs_texture(texture->tex0.TBP0, texture->width > 0 ? texture->width : 64,
                          texture->height > 0 ? texture->height : 64);
    CachedTexture *entry = cached(texture);
    if (!entry) return 0;
    entry->used = frame_number;
    if (entry->hd) return entry->render;
    if (entry->checked == frame_number) return entry->render;
    entry->checked = frame_number;
    u_long hash = texture_checksum(texture);
    int reloaded = entry->reloaded;
    entry->reloaded = 0;
    if (hash == entry->checksum && !reloaded) return entry->render;
    if (hash == entry->checksum && entry->pristine) {
        render_copy(entry->pristine, 0, 0, entry->render, 0, 0, texture->width, texture->height);
        return entry->render;
    }
    entry->checksum = hash;
    std::vector<u_int> pixels;
    decode(texture, pixels);
    port_stats.texture_decodes++;
    if (!reloaded && !entry->pristine) {
        render_upload(entry->render, 0, 0, texture->width, texture->height, pixels.data());
        return entry->render;
    }
    if (!entry->pristine) entry->pristine = render_texture(texture->width, texture->height);
    render_upload(entry->pristine, 0, 0, texture->width, texture->height, pixels.data());
    render_copy(entry->pristine, 0, 0, entry->render, 0, 0, texture->width, texture->height);
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
        std::vector<CachedTexture *> &entries = by_image[entry->image];
        for (size_t at = 0; at < entries.size(); ++at)
            if (entries[at] == entry) entries.erase(entries.begin() + (long)at);
        if (entries.empty()) by_image.erase(entry->image);
        by_render.erase(entry->render);
        render_release(entry->render);
        if (entry->pristine) render_release(entry->pristine);
        memset(entry, 0, sizeof *entry);
        port_stats.cached_textures--;
    }
}

static int sampled_texture(u_int base, int width, int height) {
    int target = resident_texture(base, 0) ? 0 : render_target_at(base);
    return target ? target : gs_texture(base, width, height);
}

static int bound_texture(int repeat, int *width, int *height) {
    int texture_width = 1 << gs.tex0.TW;
    int texture_height = 1 << gs.tex0.TH;
    int texture = 0;
    if (source && source->tex0.TBP0 == gs.tex0.TBP0 && source->image[0]) texture = gs_texture_for(source);
    else texture = sampled_texture(gs.tex0.TBP0, texture_width, texture_height);
    texture = gs_sampled(texture, texture_width, texture_height, repeat);
    *width = texture ? render_width(texture) : 1;
    *height = texture ? render_height(texture) : 1;
    return texture;
}

RenderBlending gs_blending(sceGsAlpha alpha) {
    return {(uint8_t)alpha.bits.a, (uint8_t)alpha.bits.b, (uint8_t)alpha.bits.c, (uint8_t)alpha.bits.d,
            (uint8_t)alpha.FIX};
}

RenderTests gs_tests(sceGsTest test) {
    return {(uint8_t)test.ATE, (uint8_t)test.ATST, (uint8_t)test.AREF, (uint8_t)test.AFAIL,
            (uint8_t)test.DATE, (uint8_t)test.DATM, (uint8_t)test.ZTE, (uint8_t)test.ZTST};
}

RenderExpansion gs_expansion(sceGsTexa texa) {
    return {(uint8_t)texa.TA0, (uint8_t)texa.AEM, (uint8_t)texa.TA1};
}

static u_int fog_color() {
    return gs.fogcol.FCR | gs.fogcol.FCG << 8 | gs.fogcol.FCB << 16;
}

static void scissor_state(RenderState *state) {
    state->scissor = {(int)gs.scissor.SCAX0, (int)gs.scissor.SCAY0, (int)gs.scissor.SCAX1, (int)gs.scissor.SCAY1};
}

static void sampling_state(RenderState *state, sceGsTex0 tex0, sceGsTex1 tex1) {
    state->linear = tex1.MMAG;
    state->decal = tex0.TFX == GS_TFX_DECAL;
    state->vertex_alpha = !tex0.TCC;
    state->texa = tex0.PSM == SCE_GS_PSMCT24;
}

static void state_for(RenderState *state, int *texture_width, int *texture_height) {
    memset(state, 0, sizeof *state);
    *texture_width = *texture_height = 1;
    if (gs.prim.TME) {
        state->repeat_u = gs.clamp.WMS == GS_WRAP_REPEAT;
        state->repeat_v = gs.clamp.WMT == GS_WRAP_REPEAT;
        state->texture = bound_texture(state->repeat_u || state->repeat_v, texture_width, texture_height);
        sampling_state(state, gs.tex0, gs.tex1);
    }
    state->blend = gs.prim.ABE;
    state->blending = gs_blending(gs.alpha);
    state->tests = gs_tests(gs.test);
    state->expansion = gs_expansion(gs.texa);
    state->fog = gs.prim.FGE;
    state->fog_color = fog_color();
    state->depth_write = !gs.zbuf.ZMSK;
    state->color_mask = gs.frame.FBMSK;
    scissor_state(state);
}

void gs_base_state(RenderState *state) {
    memset(state, 0, sizeof *state);
    state->expansion = gs_expansion(gs.texa);
    state->color_mask = gs.frame.FBMSK;
    state->fog_color = fog_color();
    scissor_state(state);
}

void gs_texture_state(RenderState *state, const mgCTexture *texture, float *scale_u, float *scale_v) {
    *scale_u = *scale_v = 1.0f;
    if (!texture) return;
    state->repeat_u = texture->clamp.WMS == GS_WRAP_REPEAT;
    state->repeat_v = texture->clamp.WMT == GS_WRAP_REPEAT;
    int width = 1 << texture->tex0.TW, height = 1 << texture->tex0.TH;
    state->texture = gs_sampled(gs_texture_for(texture), width, height, state->repeat_u || state->repeat_v);
    if (!state->texture) return;
    sampling_state(state, texture->tex0, texture->tex1);
    *scale_u = (float)width / (float)render_width(state->texture);
    *scale_v = (float)height / (float)render_height(state->texture);
}

void gs_sample_limits(const RenderState *state, float scale_u, float scale_v, RenderTexelLimits *limits) {
    RenderSlot slot;
    *limits = {};
    if (!render_slot(state->texture, &slot) || !slot.target || state->repeat_u || state->repeat_v) return;
    limits->low_u = 0.5f / (float)slot.width;
    limits->low_v = 0.5f / (float)slot.height;
    limits->high_u = scale_u - limits->low_u;
    limits->high_v = scale_v - limits->low_v;
}

static float depth_scale();

void gs_shift_screen(int x, int y) {
    screen_shift_x = x;
    screen_shift_y = y;
}

void gs_screen_mapping(RenderScreenMapping *screen, float *depth) {
    float width = (float)(frame_target ? render_width(frame_target) : 1);
    float height = (float)(frame_target ? render_height(frame_target) : 1);
    float offset_x = (float)((int)gs.xyoffset.OFX - screen_shift_x) / GS_SUBPIXELS;
    float offset_y = (float)((int)gs.xyoffset.OFY - screen_shift_y) / GS_SUBPIXELS;
    screen->scale_x = 2.0f / width;
    screen->offset_x = -2.0f * offset_x / width - 1.0f;
    screen->scale_y = -2.0f / height;
    screen->offset_y = 1.0f + 2.0f * offset_y / height;
    *depth = depth_scale();
}

static float depth_scale() {
    switch (SCE_GS_PSMZ32 + gs.zbuf.PSM) {
        case SCE_GS_PSMZ32: return 1.0f / 4294967296.0f;
        case SCE_GS_PSMZ24: return 1.0f / 16777216.0f;
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

static void round_texels_down(RenderVertex *a, RenderVertex *b) {
    RenderTexelLimits limits = {fminf(a->u, b->u), fminf(a->v, b->v), fmaxf(a->u, b->u), fmaxf(a->v, b->v)};
    a->limits = limits;
    b->limits = limits;
    float shift_u = b->x != a->x ? TEXEL_TIE * (b->u - a->u) / (b->x - a->x) : 0.0f;
    float shift_v = b->y != a->y ? TEXEL_TIE * (b->v - a->v) / (b->y - a->y) : 0.0f;
    a->u -= shift_u, b->u -= shift_u, a->v -= shift_v, b->v -= shift_v;
}

static void align_sprite(RenderVertex *a, RenderVertex *b, int textured) {
    RenderTexelLimits limits;
    int clamped = textured && sample_range(a->x, b->x, a->u, b->u, &limits.low_u, &limits.high_u) &&
                  sample_range(a->y, b->y, a->v, b->v, &limits.low_v, &limits.high_v);
    float shift_u = b->x != a->x ? SAMPLE_OFFSET * (b->u - a->u) / (b->x - a->x) : 0.0f;
    float shift_v = b->y != a->y ? SAMPLE_OFFSET * (b->v - a->v) / (b->y - a->y) : 0.0f;
    a->x -= SAMPLE_OFFSET, b->x -= SAMPLE_OFFSET, a->y -= SAMPLE_OFFSET, b->y -= SAMPLE_OFFSET;
    a->u -= shift_u, b->u -= shift_u, a->v -= shift_v, b->v -= shift_v;
    if (!clamped) return;
    limits.low_u = fmaxf(limits.low_u, a->limits.low_u);
    limits.low_v = fmaxf(limits.low_v, a->limits.low_v);
    limits.high_u = fminf(limits.high_u, a->limits.high_u);
    limits.high_v = fminf(limits.high_v, a->limits.high_v);
    a->limits = limits;
    b->limits = limits;
}

static int one_to_one(const RenderVertex *a, const RenderVertex *b) {
    float across = b->x - a->x, down = b->y - a->y;
    return across != 0.0f && down != 0.0f && std::fabs(std::fabs((b->u - a->u) / across) - 1.0f) < ONE_TO_ONE &&
           std::fabs(std::fabs((b->v - a->v) / down) - 1.0f) < ONE_TO_ONE;
}

static float centre_shift(float start, float end, float texel, float last_texel, float size) {
    float at = texel * size + (std::floor(start) + 0.5f - start) * (last_texel - texel) * size / (end - start);
    return (0.5f - (at - std::floor(at))) / size;
}

static void snap_texels(RenderVertex *a, RenderVertex *b, float width, float height) {
    float shift_u = centre_shift(a->x, b->x, a->u, b->u, width);
    float shift_v = centre_shift(a->y, b->y, a->v, b->v, height);
    a->u += shift_u, b->u += shift_u, a->v += shift_v, b->v += shift_v;
    if (a->limits.high_u > a->limits.low_u) a->limits.low_u += shift_u, a->limits.high_u += shift_u;
    if (a->limits.high_v > a->limits.low_v) a->limits.low_v += shift_v, a->limits.high_v += shift_v;
    b->limits = a->limits;
}

static void kick(u_int x, u_int y, u_int z, int fog, int draw) {
    RenderVertex *vertex = &queue[queued < 3 ? queued : 2];
    if (queued == 3) {
        queue[0] = queue[1];
        queue[1] = queue[2];
        queued = 2;
        vertex = &queue[2];
    }
    vertex->x = (float)((int)x - (int)gs.xyoffset.OFX) / GS_SUBPIXELS + SAMPLE_OFFSET;
    vertex->y = (float)((int)y - (int)gs.xyoffset.OFY) / GS_SUBPIXELS + SAMPLE_OFFSET;
    float depth = (float)z * depth_scale();
    vertex->z = depth > 1.0f ? 1.0f : depth;
    vertex->r = gs.rgbaq.bytes.red;
    vertex->g = gs.rgbaq.bytes.green;
    vertex->b = gs.rgbaq.bytes.blue;
    vertex->a = gs.rgbaq.bytes.alpha;
    vertex->fog = (float)fog / 255.0f;
    int lift = gs.prim.TME ? gs_read_shift(gs.tex0.TBP0) : 0;
    if (gs.prim.FST) {
        int u = (int)gs.uv.U;
        if (lift > 0 && u >= UV_WRAP - lift * GS_SUBPIXELS) u -= UV_WRAP;
        vertex->u = (float)u / GS_SUBPIXELS + (float)lift;
        vertex->v = (float)gs.uv.V / GS_SUBPIXELS;
    } else {
        float q = gs.q != 0.0f ? gs.q : 1.0f;
        vertex->u = gs.st.S / q * (float)(1 << gs.tex0.TW) + (float)lift;
        vertex->v = gs.st.T / q * (float)(1 << gs.tex0.TH);
    }
    queued++;
    int type = (int)gs.prim.PRIM;
    int needed = type == MG_PRIM_POINT ? 1 : type <= MG_PRIM_LINE_STRIP || type == MG_PRIM_SPRITE ? 2 : 3;
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
        if (!gs.prim.IIP)
            for (int i = 0; i < count - 1; ++i) {
                out[i].r = out[count - 1].r;
                out[i].g = out[count - 1].g;
                out[i].b = out[count - 1].b;
                out[i].a = out[count - 1].a;
            }
        int topology = type == MG_PRIM_POINT ? RENDER_POINTS : type <= MG_PRIM_LINE_STRIP ? RENDER_LINES : RENDER_TRIANGLES;
        if (!state.texture || render_scaled(state.texture)) gs_drawing_scaled(state.texture);
        if (type == MG_PRIM_SPRITE) {
            RenderVertex a = out[0], b = out[1];
            RenderSlot target;
            int shown_width, shown_height;
            int sharp = state.linear && render_scale(state.texture) == 1 &&
                        render_bound() == port_display_target(&shown_width, &shown_height) &&
                        render_slot(render_bound(), &target) && target.scale > 1 &&
                        one_to_one(&queue[queued - 2], &queue[queued - 1]);
            if (state.texture) round_texels_down(&a, &b);
            if (render_slot(render_bound(), &target) && (target.scale > 1 || target.samples > 1))
                align_sprite(&a, &b, state.texture != 0);
            if (sharp) {
                state.linear = 0;
                snap_texels(&a, &b, (float)width, (float)height);
            }
            RenderVertex corner_b = b, corner_c = b;
            corner_b.x = b.x, corner_b.y = a.y, corner_b.u = b.u, corner_b.v = a.v;
            corner_c.x = a.x, corner_c.y = b.y, corner_c.u = a.u, corner_c.v = b.v;
            a.r = b.r, a.g = b.g, a.b = b.b, a.a = b.a, a.z = b.z, a.fog = b.fog;
            out[0] = a, out[1] = corner_b, out[2] = corner_c;
            out[3] = corner_b, out[4] = b, out[5] = corner_c;
            count = 6;
        }
        spread_hud(out, count);
        render_source = RENDER_SOURCE_GS;
        render_draw(&state, topology, out, count);
    }
    if (type == MG_PRIM_LINE || type == MG_PRIM_TRIANGLE || type == MG_PRIM_SPRITE || type == MG_PRIM_POINT) queued = 0;
    else if (type == MG_PRIM_TRIANGLE_FAN) {
        queue[1] = queue[2];
        queued = 2;
    } else if (type == MG_PRIM_LINE_STRIP) {
        queue[0] = queue[1];
        queued = 1;
    }
}

void gs_write(int reg, u_long value) {
    switch (reg) {
        case SCE_GS_PRIM:
            gs.prim.value = value;
            queued = 0;
            break;
        case SCE_GS_RGBAQ:
            gs.rgbaq.value = value;
            memcpy(&gs.q, &gs.rgbaq.bytes.q, sizeof gs.q);
            break;
        case SCE_GS_ST: gs.st.value = value; break;
        case SCE_GS_UV: gs.uv.value = value; break;
        case SCE_GS_XYZF2:
        case SCE_GS_XYZF3: {
            sceGsXyzf xyzf;
            xyzf.value = value;
            kick(xyzf.X, xyzf.Y, xyzf.Z, xyzf.F, reg == SCE_GS_XYZF2);
            break;
        }
        case SCE_GS_XYZ2:
        case SCE_GS_XYZ3: {
            sceGsXyz xyz;
            xyz.value = value;
            kick(xyz.X, xyz.Y, xyz.Z, gs.fog.F, reg == SCE_GS_XYZ2);
            break;
        }
        case SCE_GS_FOG: gs.fog.value = value; break;
        case SCE_GS_TEX0_1: gs.tex0.value = value; break;
        case SCE_GS_TEX1_1: gs.tex1.value = value; break;
        case SCE_GS_CLAMP_1: gs.clamp.value = value; break;
        case SCE_GS_TEXA: gs.texa.value = value; break;
        case SCE_GS_FOGCOL: gs.fogcol.value = value; break;
        case SCE_GS_SCISSOR_1:
            gs.scissor.value = value;
            if (frame_shift) gs.scissor = shifted_scissor(gs.scissor);
            break;
        case SCE_GS_ALPHA_1: gs.alpha.value = value; break;
        case SCE_GS_TEST_1: gs.test.value = value; break;
        case SCE_GS_ZBUF_1: gs.zbuf.value = value; break;
        case SCE_GS_XYOFFSET_1:
            gs.xyoffset.value = value;
            gs.xyoffset.OFX -= frame_shift * GS_SUBPIXELS;
            break;
        case SCE_GS_BITBLTBUF: gs.bitbltbuf.value = value; break;
        case SCE_GS_TRXPOS: gs.trxpos.value = value; break;
        case SCE_GS_TRXREG: gs.trxreg.value = value; break;
        case SCE_GS_TRXDIR: {
            sceGsTrxdir direction;
            direction.value = value;
            if (direction.XDIR == SCE_GS_LOCAL_LOCAL)
                gs_copy_image(gs.bitbltbuf.SBP, gs.trxpos.SSAX, gs.trxpos.SSAY, gs.bitbltbuf.DBP, gs.trxpos.DSAX,
                              gs.trxpos.DSAY, gs.trxreg.RRW, gs.trxreg.RRH);
            break;
        }
        case SCE_GS_FRAME_1: gs.frame.value = value; break;
    }
}

void gs_packed(int reg, const void *qword) {
    GifPacked packed;
    memcpy(&packed, qword, sizeof packed);
    switch (reg) {
        case SCE_GS_PRIM: gs_write(SCE_GS_PRIM, packed.prim.prim); break;
        case SCE_GS_RGBAQ:
            gs.rgbaq.value = 0;
            gs.rgbaq.bytes.red = packed.rgbaq.red;
            gs.rgbaq.bytes.green = packed.rgbaq.green;
            gs.rgbaq.bytes.blue = packed.rgbaq.blue;
            gs.rgbaq.bytes.alpha = packed.rgbaq.alpha;
            break;
        case SCE_GS_ST:
            gs.st.S = packed.st.s;
            gs.st.T = packed.st.t;
            gs.q = packed.st.q;
            break;
        case SCE_GS_UV:
            gs.uv.value = 0;
            gs.uv.U = packed.uv.u;
            gs.uv.V = packed.uv.v;
            break;
        case SCE_GS_XYZF2:
            kick(packed.xyzf.x, packed.xyzf.y, packed.xyzf.z, packed.xyzf.f, !packed.xyzf.disable_draw);
            break;
        case SCE_GS_XYZ2: kick(packed.xyz.x, packed.xyz.y, packed.xyz.z, gs.fog.F, !packed.xyz.disable_draw); break;
        case SCE_GS_FOG:
            gs.fog.value = 0;
            gs.fog.F = packed.fog.f;
            break;
        case SCE_GIF_PACKED_AD: gs_write((int)packed.ad.address, packed.ad.data); break;
    }
}
int gs_cache_info(int slot, CacheInfo *info) {
    auto found = by_render.find(slot);
    if (found == by_render.end()) return 0;
    const CachedTexture *entry = found->second;
    snprintf(info->name, sizeof info->name, "%s", entry->name);
    info->image = entry->image;
    info->clut = entry->clut;
    info->psm = entry->psm;
    info->age = frame_number - entry->used;
    return 1;
}
