#include "gs_raster.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static _Thread_local uint32_t palette_colors[2][256];
static _Thread_local uint64_t palette_generation[2] = {UINT64_MAX, UINT64_MAX};
static _Thread_local uint64_t palette_tex[2];
static _Thread_local uint64_t palette_texa[2];

typedef struct {
    unsigned base, stride, width, height;
    unsigned xmode, ymode, xmin, xmax, ymin, ymax;
} GsTextureLevel;

typedef struct {
    GsTextureLevel levels[7];
    uint64_t texa;
    unsigned psm;
} GsTextureState;

static _Thread_local GsTextureState texture_states[2];
static _Thread_local uint64_t texture_keys[2][5];
static _Thread_local unsigned texture_valid[2];

static void gs_texture_state(const uint64_t *regs, unsigned context)
{
    uint64_t key[5] = {regs[6 + context], regs[8 + context], regs[0x34 + context], regs[0x36 + context], regs[0x3b]};
    if (texture_valid[context] && !memcmp(texture_keys[context], key, sizeof(key))) return;
    GsTextureState *state = &texture_states[context];
    uint64_t tex = key[0], clamp = key[1];
    unsigned tw = (tex >> 26) & 15, th = (tex >> 30) & 15;
    state->psm = (tex >> 20) & 63;
    state->texa = key[4];
    for (unsigned i = 0; i < 7; i++) {
        GsTextureLevel *level = &state->levels[i];
        level->base = tex & 16383;
        level->stride = (tex >> 14) & 63;
        level->width = 1u << (tw > i ? tw - i : 0);
        level->height = 1u << (th > i ? th - i : 0);
        level->xmode = clamp & 3;
        level->ymode = (clamp >> 2) & 3;
        level->xmin = ((clamp >> 4) & 1023) >> i;
        level->xmax = ((clamp >> 14) & 1023) >> i;
        level->ymin = ((clamp >> 24) & 1023) >> i;
        level->ymax = ((clamp >> 34) & 1023) >> i;
        if (i) {
            unsigned shift = ((i - 1) % 3) * 20;
            uint64_t mip = key[i <= 3 ? 2 : 3];
            level->base = (mip >> shift) & 16383;
            level->stride = (mip >> (shift + 14)) & 63;
            if (!level->stride) level->stride = 1;
        }
    }
    memcpy(texture_keys[context], key, sizeof(key));
    texture_valid[context] = 1;
}

static int gs_limit(int value, int low, int high);
static int gs_raster_valid(const uint64_t *regs, unsigned prim, const PortGsVertex *vertices, unsigned count);

typedef struct {
    uint64_t regs[128];
    PortGsVertex vertices[3];
    unsigned prim;
    unsigned count;
    unsigned epoch;
    unsigned active;
    unsigned batch;
    unsigned first_y;
    unsigned last_y;
    int result;
    pthread_t thread;
} GsBand;

static GsBand bands[11];
static pthread_mutex_t band_mutex;
static pthread_cond_t band_work;
static pthread_cond_t band_complete;
static unsigned band_initialized;
static unsigned band_count;
static unsigned band_threads;
static unsigned band_epoch;
static unsigned band_pending;
static unsigned band_minimum;
static uint64_t parallel_calls;
static _Thread_local unsigned in_band;
static PortGsDraw draw_queue[512];
static unsigned draw_y_bounds[512][2];
static unsigned draw_count;
static unsigned batching;
static unsigned batch_first_y;
static unsigned batch_last_y;
static unsigned batch_first_x;
static unsigned batch_last_x;
static uint64_t batch_frame;
static uint64_t batch_zbuf;
static uint64_t batch_resources[4][8];
static double batch_work;
static double batch_strip_work[256];
static unsigned band_balancing;
static PortGsRasterStats raster_stats;

void port_gs_raster_stats(PortGsRasterStats *stats)
{
    *stats = raster_stats;
}

static int gs_batch_range(unsigned first_y, unsigned last_y)
{
    int result = 0;
    for (unsigned i = 0; i < draw_count; i++) {
        if (first_y > draw_y_bounds[i][1] || last_y < draw_y_bounds[i][0]) continue;
        const PortGsDraw *draw = &draw_queue[i];
        unsigned context = (draw->prim >> 9) & 1;
        uint64_t scissor = draw->regs[0x40 + context];
        unsigned first = (scissor >> 32) & 2047, last = (scissor >> 48) & 2047;
        if (first < first_y) first = first_y;
        if (last > last_y) last = last_y;
        if (first > last) continue;
        uint64_t regs[128];
        memcpy(regs, draw->regs, sizeof(regs));
        regs[0x40 + context] = (scissor & UINT64_C(0x00000000ffffffff)) | (uint64_t)first << 32 | (uint64_t)last << 48;
        int status = port_gs_raster(regs, draw->prim, draw->vertices, draw->count);
        if (status) result = status;
    }
    return result;
}

void port_gs_raster_threads(unsigned count)
{
    band_threads = count < 1 ? 1 : count > 12 ? 12 : count;
}

uint64_t port_gs_raster_parallel_calls(void)
{
    return parallel_calls;
}

static void *gs_band_run(void *argument)
{
    GsBand *band = argument;
    in_band = 1;
    pthread_mutex_lock(&band_mutex);
    for (;;) {
        while (band->epoch == band_epoch) pthread_cond_wait(&band_work, &band_mutex);
        band->epoch = band_epoch;
        if (!band->active) continue;
        pthread_mutex_unlock(&band_mutex);
        band->result = band->batch ? gs_batch_range(band->first_y, band->last_y) :
            port_gs_raster(band->regs, band->prim, band->vertices, band->count);
        pthread_mutex_lock(&band_mutex);
        band_pending--;
        if (!band_pending) pthread_cond_signal(&band_complete);
    }
    return NULL;
}

static void gs_band_init(void)
{
    if (!band_threads) {
        const char *setting = getenv("DC2TK_GS_THREADS");
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        port_gs_raster_threads(setting ? (unsigned)strtoul(setting, NULL, 10) : online > 0 ? (unsigned)online : 1);
    }
    if (band_threads == 1) return;
    if (!band_initialized) {
        if (pthread_mutex_init(&band_mutex, NULL) != 0) {
            band_threads = 1;
            return;
        }
        if (pthread_cond_init(&band_work, NULL) != 0) {
            pthread_mutex_destroy(&band_mutex);
            band_threads = 1;
            return;
        }
        if (pthread_cond_init(&band_complete, NULL) != 0) {
            pthread_cond_destroy(&band_work);
            pthread_mutex_destroy(&band_mutex);
            band_threads = 1;
            return;
        }
        band_initialized = 1;
    }
    while (band_count + 1 < band_threads) {
        if (pthread_create(&bands[band_count].thread, NULL, gs_band_run, &bands[band_count]) != 0) {
            band_threads = band_count + 1;
            break;
        }
        band_count++;
    }
}

static void gs_pages(uint64_t pages[8], unsigned base, unsigned stride, unsigned psm,
                     unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    unsigned indexed = psm == 19 || psm == 20;
    unsigned pw = indexed ? 128 : 64;
    unsigned ph = psm == 20 ? 128 : psm == 19 || psm == 2 || psm == 10 || psm == 50 || psm == 58 ? 64 : 32;
    unsigned step = indexed ? stride >> 1 : stride;
    for (unsigned y = y0 / ph; y <= y1 / ph; y++) for (unsigned x = x0 / pw; x <= x1 / pw; x++) {
        unsigned page = ((base >> 5) + x + y * step) & 511;
        pages[page >> 6] |= UINT64_C(1) << (page & 63);
        if (base & 31) {
            page = (page + 1) & 511;
            pages[page >> 6] |= UINT64_C(1) << (page & 63);
        }
    }
}

static int gs_disjoint(const uint64_t a[8], const uint64_t b[8])
{
    for (unsigned i = 0; i < 8; i++) if (a[i] & b[i]) return 0;
    return 1;
}

static int gs_band_safe(const uint64_t *regs, unsigned prim, unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                        uint64_t resources[4][8])
{
    unsigned context = (prim >> 9) & 1;
    uint64_t frame = regs[0x4c + context];
    uint64_t zbuf = regs[0x4e + context];
    uint64_t test = regs[0x47 + context];
    unsigned width = (frame >> 16) & 63;
    unsigned psm = (frame >> 24) & 63;
    unsigned zpsm = 48 | ((zbuf >> 24) & 15);
    unsigned height = psm & 2 ? 64 : 32;
    unsigned zheight = zpsm & 2 ? 64 : 32;
    if (x1 >= width * 64 || (y1 / height - y0 / height) * width + x1 / 64 - x0 / 64 >= 512) return 0;
    uint64_t color_pages[8] = {0}, depth_pages[8] = {0};
    gs_pages(color_pages, (frame & 511) * 32, width, psm, x0, y0, x1, y1);
    unsigned depth_read = (test & 0x10000) && ((test >> 17) & 3) >= 2;
    unsigned depth_write = (test & 0x10000) && !(zbuf & (UINT64_C(1) << 32));
    if (depth_read || depth_write) {
        if ((y1 / zheight - y0 / zheight) * width + x1 / 64 - x0 / 64 >= 512) return 0;
        gs_pages(depth_pages, (zbuf & 511) * 32, width, zpsm, x0, y0, x1, y1);
        if (!gs_disjoint(color_pages, depth_pages)) return 0;
    }
    uint64_t textures[8] = {0};
    if (prim & 16) {
    uint64_t tex = regs[6 + context], tex1 = regs[0x14 + context], clamp = regs[8 + context];
    unsigned tpsm = (tex >> 20) & 63;
    unsigned levels = ((tex1 >> 6) & 7) >= 2 ? (tex1 >> 2) & 7 : 0;
    for (unsigned level = 0; level <= levels; level++) {
        unsigned tw = (tex >> 26) & 15, th = (tex >> 30) & 15;
        unsigned right = (1u << (tw > level ? tw - level : 0)) - 1;
        unsigned bottom = (1u << (th > level ? th - level : 0)) - 1;
        unsigned base = tex & 16383, stride = (tex >> 14) & 63;
        if (level) {
            uint64_t mip = regs[(level <= 3 ? 0x34 : 0x36) + context];
            unsigned shift = ((level - 1) % 3) * 20;
            base = (mip >> shift) & 16383;
            stride = (mip >> (shift + 14)) & 63;
            if (!stride) stride = 1;
        }
        unsigned mx = clamp & 3, my = (clamp >> 2) & 3;
        if (mx >= 2) right = (((clamp >> 4) & 1023) | ((clamp >> 14) & 1023)) >> level;
        if (my >= 2) bottom = (((clamp >> 24) & 1023) | ((clamp >> 34) & 1023)) >> level;
        gs_pages(textures, base, stride, tpsm, 0, 0, right, bottom);
    }
    if (tpsm == 19 || tpsm == 20 || tpsm == 27 || tpsm == 36 || tpsm == 44) {
        unsigned base = (tex >> 37) & 16383;
        unsigned cpsm = (tex >> 51) & 15;
        if ((tex >> 55) & 1) {
            uint64_t clut = regs[0x1c];
            unsigned left = ((clut >> 6) & 63) * 16, top = (clut >> 12) & 1023;
            gs_pages(textures, base, clut & 63, cpsm, left, top, left + 255, top);
        } else gs_pages(textures, base, 1, cpsm, 0, 0, 15, 31);
    }
    }
    if (!gs_disjoint(textures, color_pages)) return 0;
    if (depth_write && !gs_disjoint(textures, depth_pages)) return 0;
    if (resources) {
        memcpy(resources[0], color_pages, sizeof(color_pages));
        memcpy(resources[1], depth_pages, sizeof(depth_pages));
        if (depth_write) memcpy(resources[2], depth_pages, sizeof(depth_pages));
        else memset(resources[2], 0, sizeof(depth_pages));
        memcpy(resources[3], textures, sizeof(textures));
    }
    return 1;
}

static unsigned gs_band_threshold(void)
{
    if (!band_minimum) {
        const char *setting = getenv("DC2TK_GS_BAND_PIXELS");
        band_minimum = setting ? (unsigned)strtoul(setting, NULL, 10) : 4096;
        if (band_minimum < 4096) band_minimum = 4096;
    }
    return band_minimum;
}

static unsigned gs_balancing(void)
{
    if (!band_balancing) {
        const char *setting = getenv("DC2TK_GS_BALANCE");
        band_balancing = !setting || (*setting && *setting != '0') ? 2 : 1;
    }
    return band_balancing == 2;
}

static void gs_batch_estimate(const uint64_t *regs, unsigned prim, const PortGsVertex *v,
                              unsigned x0, unsigned y0, unsigned x1, unsigned y1, double ox, double oy)
{
    if (!gs_balancing() || band_threads == 1) return;
    unsigned context = (prim >> 9) & 1;
    unsigned triangle = (prim & 7) == 3 && !(prim & 128);
    double area = triangle ? ((double)v[1].x - v[0].x) * ((double)v[2].y - v[0].y)
        - ((double)v[1].y - v[0].y) * ((double)v[2].x - v[0].x) : 0;
    double sign = area < 0 ? -1 : 1;
    double factor = 1;
    if (prim & 16) {
        uint64_t tex1 = regs[0x14 + context];
        factor += ((tex1 >> 5) & 1) || ((tex1 >> 6) & 7) == 1 || ((tex1 >> 6) & 7) >= 4 ? 4 : 1;
    }
    if (prim & 8) factor += 0.5;
    if (prim & 32) factor += 0.5;
    for (unsigned strip = y0 / 8; strip <= (y1 - 1) / 8; strip++) {
        unsigned top = strip * 8 > y0 ? strip * 8 : y0;
        unsigned bottom = strip * 8 + 8 < y1 ? strip * 8 + 8 : y1;
        double left = x0, right = x1;
        if (triangle) {
            if (!area) continue;
            double y = (top + bottom) * 0.5;
            for (unsigned i = 0; i < 3; i++) {
                unsigned a = (i + 1) % 3, b = (i + 2) % 3;
                double dx = (double)v[b].x - v[a].x, dy = (double)v[b].y - v[a].y;
                double edge = (dx * (y + oy - v[a].y) - dy * (x0 + ox - v[a].x)) * sign;
                double step = -dy * sign;
                if (step > 0) left = fmax(left, x0 - edge / step);
                else if (step < 0) right = fmin(right, x0 - edge / step);
                else if (edge < 0) right = left;
            }
        }
        if (right > left) batch_strip_work[strip] += (right - left) * (bottom - top) * factor;
    }
}

static void gs_batch_splits(unsigned threads, unsigned splits[13])
{
    unsigned height = batch_last_y - batch_first_y + 1;
    for (unsigned i = 0; i <= threads; i++) splits[i] = batch_first_y + height * i / threads;
    if (!gs_balancing()) return;
    double total = 0;
    unsigned first = batch_first_y / 8, last = batch_last_y / 8;
    for (unsigned i = first; i <= last; i++) total += batch_strip_work[i];
    if (total <= 0) return;
    unsigned strip = first;
    double accumulated = 0;
    for (unsigned i = 1; i < threads; i++) {
        double target = total * i / threads;
        while (strip < last && accumulated + batch_strip_work[strip] <= target) accumulated += batch_strip_work[strip++];
        unsigned top = strip * 8 > batch_first_y ? strip * 8 : batch_first_y;
        unsigned bottom = strip * 8 + 8 < batch_last_y + 1 ? strip * 8 + 8 : batch_last_y + 1;
        double fraction = batch_strip_work[strip] > 0 ? (target - accumulated) / batch_strip_work[strip] : 0;
        splits[i] = (unsigned)gs_limit((int)(top + (bottom - top) * fraction), splits[i - 1] + 1,
                                      batch_last_y + 1 - (threads - i));
    }
}

static int gs_parallel(uint64_t *regs, unsigned prim, const PortGsVertex *original,
                       const PortGsVertex *v, unsigned count, int *result)
{
    if (in_band || ((prim & 7) != 3 && (prim & 7) != 6)) return 0;
    unsigned context = (prim >> 9) & 1;
    uint64_t scissor = regs[0x40 + context];
    float left = v[0].x, right = v[0].x, top = v[0].y, bottom = v[0].y;
    for (unsigned i = 1; i < count; i++) {
        left = fminf(left, v[i].x); right = fmaxf(right, v[i].x);
        top = fminf(top, v[i].y); bottom = fmaxf(bottom, v[i].y);
    }
    float grow = prim & 128 ? 1 : 0;
    int x0 = gs_limit((int)ceilf(left - 0.5f - grow), scissor & 2047, ((scissor >> 16) & 2047) + 1);
    int x1 = gs_limit((int)ceilf(right - 0.5f + grow), scissor & 2047, ((scissor >> 16) & 2047) + 1);
    int y0 = gs_limit((int)ceilf(top - 0.5f - grow), (scissor >> 32) & 2047, ((scissor >> 48) & 2047) + 1);
    int y1 = gs_limit((int)ceilf(bottom - 0.5f + grow), (scissor >> 32) & 2047, ((scissor >> 48) & 2047) + 1);
    double work = (x1 - x0) * (y1 - y0);
    if ((prim & 7) == 3) {
        double area = fabs(((double)v[1].x - v[0].x) * ((double)v[2].y - v[0].y) -
                           ((double)v[1].y - v[0].y) * ((double)v[2].x - v[0].x)) * 0.5;
        if (area < work) work = area;
    }
    if (x0 >= x1 || y0 >= y1 || work < gs_band_threshold() || y1 - y0 < 32) return 0;
    gs_band_init();
    unsigned threads = band_threads < band_count + 1 ? band_threads : band_count + 1;
    if (threads < 2 || !gs_band_safe(regs, prim, x0, y0, x1 - 1, y1 - 1, NULL)) return 0;
    parallel_calls++;
    pthread_mutex_lock(&band_mutex);
    for (unsigned i = 0; i < band_count; i++) bands[i].active = i + 1 < threads;
    for (unsigned i = 1; i < threads; i++) {
        GsBand *band = &bands[i - 1];
        memcpy(band->regs, regs, sizeof(band->regs));
        memcpy(band->vertices, original, count * sizeof(*original));
        band->prim = prim;
        band->batch = 0;
        band->count = count;
        unsigned first = y0 + (y1 - y0) * i / threads;
        unsigned last = y0 + (y1 - y0) * (i + 1) / threads - 1;
        band->regs[0x40 + context] = (scissor & UINT64_C(0x00000000ffffffff)) | (uint64_t)first << 32 | (uint64_t)last << 48;
    }
    band_pending = threads - 1;
    band_epoch++;
    pthread_cond_broadcast(&band_work);
    pthread_mutex_unlock(&band_mutex);
    uint64_t local[128];
    memcpy(local, regs, sizeof(local));
    local[0x40 + context] = (scissor & UINT64_C(0x00000000ffffffff)) |
        (uint64_t)y0 << 32 | (uint64_t)(y0 + (y1 - y0) / threads - 1) << 48;
    in_band = 1;
    *result = port_gs_raster(local, prim, original, count);
    in_band = 0;
    pthread_mutex_lock(&band_mutex);
    while (band_pending) pthread_cond_wait(&band_complete, &band_mutex);
    for (unsigned i = 0; i + 1 < threads; i++) if (bands[i].result) *result = bands[i].result;
    pthread_mutex_unlock(&band_mutex);
    return 1;
}

void port_gs_raster_reset(void)
{
    draw_count = 0;
    batch_work = 0;
    memset(batch_resources, 0, sizeof(batch_resources));
    memset(&raster_stats, 0, sizeof(raster_stats));
}

int port_gs_raster_flush(void)
{
    if (!draw_count) return 0;
    raster_stats.flushes++;
    if (draw_count > raster_stats.largest_batch) raster_stats.largest_batch = draw_count;
    if (draw_count == 1) {
        PortGsDraw *draw = &draw_queue[0];
        int result = port_gs_raster(draw->regs, draw->prim, draw->vertices, draw->count);
        draw_count = 0;
        return result;
    }
    gs_band_init();
    unsigned threads = band_threads < band_count + 1 ? band_threads : band_count + 1;
    if (batch_work < gs_band_threshold() || batch_last_y - batch_first_y + 1 < threads) threads = 1;
    int result;
    if (threads == 1) {
        in_band = 1;
        result = gs_batch_range(batch_first_y, batch_last_y);
        in_band = 0;
    } else {
        parallel_calls++;
        raster_stats.parallel_batches++;
        pthread_mutex_lock(&band_mutex);
        for (unsigned i = 0; i < band_count; i++) bands[i].active = i + 1 < threads;
        unsigned splits[13];
        gs_batch_splits(threads, splits);
        for (unsigned i = 1; i < threads; i++) {
            GsBand *band = &bands[i - 1];
            band->batch = 1;
            band->first_y = splits[i];
            band->last_y = splits[i + 1] - 1;
        }
        band_pending = threads - 1;
        band_epoch++;
        pthread_cond_broadcast(&band_work);
        pthread_mutex_unlock(&band_mutex);
        in_band = 1;
        result = gs_batch_range(splits[0], splits[1] - 1);
        in_band = 0;
        pthread_mutex_lock(&band_mutex);
        while (band_pending) pthread_cond_wait(&band_complete, &band_mutex);
        for (unsigned i = 0; i + 1 < threads; i++) if (bands[i].result) result = bands[i].result;
        pthread_mutex_unlock(&band_mutex);
    }
    draw_count = 0;
    return result;
}

int port_gs_raster_batching(unsigned enabled)
{
    int result = port_gs_raster_flush();
    batching = enabled ? 2 : 1;
    return result;
}

int port_gs_raster_enqueue(const uint64_t *regs, unsigned prim, const PortGsVertex *vertices, unsigned count)
{
    raster_stats.draws++;
    if (!batching) {
        const char *setting = getenv("DC2TK_GS_BATCH");
        batching = !setting || (*setting && *setting != '0') ? 2 : 1;
    }
    if (batching == 1) return port_gs_raster((uint64_t *)regs, prim, vertices, count);
    if (gs_raster_valid(regs, prim, vertices, count)) {
        port_gs_raster_flush();
        return -1;
    }
    unsigned context = (prim >> 9) & 1;
    uint64_t scissor = regs[0x40 + context];
    unsigned min_x = scissor & 2047, max_x = (scissor >> 16) & 2047;
    unsigned min_y = (scissor >> 32) & 2047, max_y = (scissor >> 48) & 2047;
    double ox = (regs[0x18 + context] & 65535) / 16.0;
    double oy = ((regs[0x18 + context] >> 32) & 65535) / 16.0;
    double left = vertices[0].x - ox, right = left;
    double top = vertices[0].y - oy, bottom = top;
    for (unsigned i = 1; i < count; i++) {
        left = fmin(left, vertices[i].x - ox); right = fmax(right, vertices[i].x - ox);
        top = fmin(top, vertices[i].y - oy); bottom = fmax(bottom, vertices[i].y - oy);
    }
    int x0 = gs_limit((int)floor(left - 1), min_x, max_x + 1);
    int x1 = gs_limit((int)ceil(right + 1), min_x, max_x + 1);
    int y0 = gs_limit((int)floor(top - 1), min_y, max_y + 1);
    int y1 = gs_limit((int)ceil(bottom + 1), min_y, max_y + 1);
    if (min_x > max_x || min_y > max_y || x0 >= x1 || y0 >= y1) {
        if (port_gs_raster_flush()) return -1;
        return port_gs_raster((uint64_t *)regs, prim, vertices, count);
    }
    uint64_t frame = regs[0x4c + context] & UINT64_C(0xffffffff);
    uint64_t zbuf = regs[0x4e + context] & UINT64_C(0xffffffff);
    if (draw_count && (draw_count == 512 || frame != batch_frame || zbuf != batch_zbuf)) {
        if (port_gs_raster_flush()) return -1;
    }
    uint64_t resources[4][8];
    for (;;) {
        unsigned first_x = draw_count && batch_first_x < (unsigned)x0 ? batch_first_x : (unsigned)x0;
        unsigned last_x = draw_count && batch_last_x >= (unsigned)x1 ? batch_last_x : (unsigned)x1 - 1;
        unsigned first_y = draw_count && batch_first_y < (unsigned)y0 ? batch_first_y : (unsigned)y0;
        unsigned last_y = draw_count && batch_last_y >= (unsigned)y1 ? batch_last_y : (unsigned)y1 - 1;
        unsigned safe = gs_band_safe(regs, prim, first_x, first_y, last_x, last_y, resources);
        uint64_t merged[4][8];
        if (safe) {
            for (unsigned r = 0; r < 4; r++) for (unsigned i = 0; i < 8; i++)
                merged[r][i] = resources[r][i] | (draw_count ? batch_resources[r][i] : 0);
            safe = gs_disjoint(merged[0], merged[1]) && gs_disjoint(merged[0], merged[3]) && gs_disjoint(merged[2], merged[3]);
        }
        if (safe) {
            memcpy(batch_resources, merged, sizeof(batch_resources));
            batch_first_x = first_x; batch_last_x = last_x;
            batch_first_y = first_y; batch_last_y = last_y;
            break;
        }
        if (!draw_count) {
            raster_stats.serial_aliases++;
            return port_gs_raster((uint64_t *)regs, prim, vertices, count);
        }
        if (port_gs_raster_flush()) return -1;
    }
    if (!draw_count) {
        batch_work = 0;
        memset(batch_strip_work, 0, sizeof(batch_strip_work));
    }
    gs_batch_estimate(regs, prim, vertices, x0, y0, x1, y1, ox, oy);
    batch_work += (x1 - x0) * (y1 - y0);
    draw_y_bounds[draw_count][0] = y0;
    draw_y_bounds[draw_count][1] = y1 - 1;
    PortGsDraw *draw = &draw_queue[draw_count++];
    raster_stats.queued++;
    memcpy(draw->regs, regs, sizeof(draw->regs));
    memcpy(draw->vertices, vertices, count * sizeof(*vertices));
    draw->prim = prim;
    draw->count = count;
    batch_frame = frame;
    batch_zbuf = zbuf;
    return 0;
}

static int gs_limit(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static unsigned gs_channel(uint32_t value, unsigned channel)
{
    return (value >> (channel * 8)) & 255;
}

static uint32_t gs_color(unsigned r, unsigned g, unsigned b, unsigned a)
{
    return (r & 255) | ((g & 255) << 8) | ((b & 255) << 16) | ((a & 255) << 24);
}

static uint32_t gs_expand(uint32_t value, unsigned psm, uint64_t texa, int texture)
{
    if (psm == 2 || psm == 10) {
        unsigned alpha = value & 0x8000 ? (texa >> 32) & 255 : texa & 255;
        if (!texture) alpha = value & 0x8000 ? 128 : 0;
        if (texture && (texa & 0x8000) && !(value & 0x7fff)) alpha = 0;
        return gs_color((value & 31) << 3, ((value >> 5) & 31) << 3,
                        ((value >> 10) & 31) << 3, alpha);
    }
    if (psm == 1) {
        unsigned alpha = texture ? texa & 255 : 128;
        if (texture && (texa & 0x8000) && !(value & 0xffffff)) alpha = 0;
        return (value & 0xffffff) | (alpha << 24);
    }
    return value;
}

static void gs_palette(const uint64_t *regs, unsigned context)
{
    uint64_t generation = port_gs_clut_generation();
    uint64_t tex = regs[6 + context];
    uint64_t texa = regs[0x3b];
    uint64_t key = tex & UINT64_C(0x1fffffe003f00000);
    if (palette_generation[context] == generation && palette_tex[context] == key && palette_texa[context] == texa) return;
    unsigned psm = (tex >> 20) & 63;
    unsigned count = psm == 20 || psm == 36 || psm == 44 ? 16 : 256;
    for (unsigned i = 0; i < count; i++)
        palette_colors[context][i] = gs_expand(port_gs_clut_read(regs, context, i), (tex >> 51) & 15, texa, 1);
    palette_generation[context] = generation;
    palette_tex[context] = key;
    palette_texa[context] = texa;
}

static int gs_texture_coord(int value, unsigned size, unsigned mode, unsigned low, unsigned high)
{
    if (mode == 1) return gs_limit(value, 0, (int)size - 1);
    if (mode == 2) return gs_limit(value, (int)low, (int)high);
    if (mode == 3) return (value & (int)low) | (int)high;
    return value & ((int)size - 1);
}

static uint32_t gs_texel(const uint64_t *regs, unsigned context, unsigned level, int x, int y)
{
    (void)regs;
    const GsTextureState *state = &texture_states[context];
    const GsTextureLevel *sample = &state->levels[level];
    unsigned psm = state->psm;
    x = gs_texture_coord(x, sample->width, sample->xmode, sample->xmin, sample->xmax);
    y = gs_texture_coord(y, sample->height, sample->ymode, sample->ymin, sample->ymax);
    uint32_t pixel = port_gs_memory_read(sample->base, sample->stride, psm, (unsigned)x, (unsigned)y);
    if (psm == 19 || psm == 20 || psm == 27 || psm == 36 || psm == 44) {
        return palette_colors[context][pixel];
    }
    return gs_expand(pixel, psm, state->texa, 1);
}

static float gs_safe_coordinate(float value, unsigned mode, unsigned level)
{
    if (mode == 0 || mode == 3) {
        if (value < 0 || value >= 65536.0f) {
            value = fmodf(value, 65536.0f);
            if (value < 0) value += 65536.0f;
        }
    } else {
        if (value < -65536.0f) value = -65536.0f;
        if (value > 65536.0f) value = 65536.0f;
    }
    return value / (float)(1u << level);
}

static uint32_t gs_texture_level(const uint64_t *regs, unsigned context, unsigned level, float u, float v, int linear)
{
    uint64_t clamp = regs[8 + context];
    u = gs_safe_coordinate(u, clamp & 3, level);
    v = gs_safe_coordinate(v, (clamp >> 2) & 3, level);
    if (!linear) return gs_texel(regs, context, level, (int)floorf(u), (int)floorf(v));
    u -= 0.5f;
    v -= 0.5f;
    int x = (int)floorf(u), y = (int)floorf(v);
    unsigned fx = (unsigned)((u - floorf(u)) * 16.0f);
    unsigned fy = (unsigned)((v - floorf(v)) * 16.0f);
    uint32_t a = gs_texel(regs, context, level, x, y);
    if (!fx && !fy) return a;
    uint32_t b = fx ? gs_texel(regs, context, level, x + 1, y) : 0;
    uint32_t c = fy ? gs_texel(regs, context, level, x, y + 1) : 0;
    uint32_t d = fx && fy ? gs_texel(regs, context, level, x + 1, y + 1) : 0;
    uint32_t result = 0;
    for (unsigned channel = 0; channel < 4; channel++) {
        unsigned top = gs_channel(a, channel) * (16 - fx) + gs_channel(b, channel) * fx;
        unsigned bottom = gs_channel(c, channel) * (16 - fx) + gs_channel(d, channel) * fx;
        unsigned value = (top * (16 - fy) + bottom * fy) >> 8;
        result |= value << (channel * 8);
    }
    return result;
}

static uint32_t gs_texture(const uint64_t *regs, unsigned context, float u, float v, float q)
{
    uint64_t tex1 = regs[0x14 + context];
    unsigned filter = (tex1 >> 6) & 7;
    unsigned maximum = (tex1 >> 2) & 7;
    int magnify_linear = (tex1 >> 5) & 1;
    int minify_linear = filter < 2 ? filter == 1 : filter >= 4;
    if ((filter < 2 || !maximum) && magnify_linear == minify_linear)
        return gs_texture_level(regs, context, 0, u, v, magnify_linear);
    int k = (tex1 >> 32) & 4095;
    if (k & 2048) k -= 4096;
    float lod = (float)k / 16.0f;
    if (!(tex1 & 1) && q != 0.0f && isfinite(q)) lod -= log2f(fabsf(q)) * (float)(1u << ((tex1 >> 19) & 3));
    if (lod <= 0.0f) return gs_texture_level(regs, context, 0, u, v, (tex1 >> 5) & 1);
    if (filter < 2) return gs_texture_level(regs, context, 0, u, v, filter == 1);
    if (lod > (float)maximum) lod = (float)maximum;
    int linear = filter >= 4;
    if (!(filter & 1)) return gs_texture_level(regs, context, (unsigned)floorf(lod + 0.5f), u, v, linear);
    unsigned level = (unsigned)floorf(lod);
    unsigned fraction = (unsigned)((lod - (float)level) * 16.0f);
    uint32_t a = gs_texture_level(regs, context, level, u, v, linear);
    if (!fraction || level == maximum) return a;
    uint32_t b = gs_texture_level(regs, context, level + 1, u, v, linear);
    uint32_t result = 0;
    for (unsigned channel = 0; channel < 4; channel++)
        result |= ((gs_channel(a, channel) * (16 - fraction) + gs_channel(b, channel) * fraction) >> 4) << (channel * 8);
    return result;
}

static uint32_t gs_texture_function(uint32_t color, uint32_t texture, uint64_t tex)
{
    unsigned function = (tex >> 35) & 3;
    unsigned tcc = (tex >> 34) & 1;
    if (function == 1 || (function == 0 && color == UINT32_C(0x80808080)))
        return tcc ? texture : (texture & 0xffffff) | (color & 0xff000000);
    uint32_t result = 0;
    for (unsigned channel = 0; channel < 4; channel++) {
        int a = (int)gs_channel(color, channel);
        int b = (int)gs_channel(texture, channel);
        int value;
        if (channel == 3) {
            if (!tcc) value = a;
            else if (function == 0) value = (a * b) >> 7;
            else if (function == 2) value = a + b;
            else value = b;
        } else {
            if (function == 1) value = b;
            else value = (a * b) >> 7;
            if (function >= 2) value += (int)gs_channel(color, 3);
        }
        result |= (uint32_t)gs_limit(value, 0, 255) << (channel * 8);
    }
    return result;
}

static int gs_alpha_test(unsigned alpha, unsigned reference, unsigned function)
{
    if (function == 0) return 0;
    if (function == 1) return 1;
    if (function == 2) return alpha < reference;
    if (function == 3) return alpha <= reference;
    if (function == 4) return alpha == reference;
    if (function == 5) return alpha >= reference;
    if (function == 6) return alpha > reference;
    return alpha != reference;
}

static int gs_blend_value(unsigned selector, int source, int destination)
{
    if (selector == 0) return source;
    if (selector == 1) return destination;
    return 0;
}

static uint32_t gs_blend(uint32_t source, uint32_t destination, uint64_t alpha, int clamp)
{
    unsigned c = (alpha >> 4) & 3;
    int factor = c == 0 ? (int)(source >> 24) : c == 1 ? (int)(destination >> 24) : (int)((alpha >> 32) & 255);
    uint32_t result = source & 0xff000000;
    unsigned operation = alpha & 255;
    if (factor == 128 && (operation == 0x62 || operation == 0x68)) {
        for (unsigned channel = 0; channel < 3; channel++) {
            int s = (int)gs_channel(source, channel), d = (int)gs_channel(destination, channel);
            int value = operation == 0x62 ? d - s : d + s;
            if (clamp) value = gs_limit(value, 0, 255);
            result |= (uint32_t)(value & 255) << (channel * 8);
        }
        return result;
    }
    for (unsigned channel = 0; channel < 3; channel++) {
        int s = (int)gs_channel(source, channel), d = (int)gs_channel(destination, channel);
        int a = gs_blend_value(alpha & 3, s, d);
        int b = gs_blend_value((alpha >> 2) & 3, s, d);
        int add = gs_blend_value((alpha >> 6) & 3, s, d);
        int value = ((a - b) * factor >> 7) + add;
        if (clamp) value = gs_limit(value, 0, 255);
        result |= (uint32_t)(value & 255) << (channel * 8);
    }
    return result;
}

static void gs_pixel(const uint64_t *regs, unsigned prim, int x, int y, double z, uint32_t color, float u, float v, float q, unsigned fog, unsigned coverage)
{
    unsigned context = (prim >> 9) & 1;
    uint64_t scissor = regs[0x40 + context];
    if (x < 0 || y < 0 || x < (int)(scissor & 2047) || x > (int)((scissor >> 16) & 2047)
        || y < (int)((scissor >> 32) & 2047) || y > (int)((scissor >> 48) & 2047)) return;
    unsigned scan = regs[0x22] & 3;
    if ((scan == 2 && !(y & 1)) || (scan == 3 && (y & 1))) return;
    uint64_t frame = regs[0x4c + context];
    uint64_t zbuf = regs[0x4e + context];
    uint64_t test = regs[0x47 + context];
    unsigned width = (frame >> 16) & 63;
    unsigned psm = (frame >> 24) & 63;
    unsigned zpsm = 0x30 | ((zbuf >> 24) & 15);
    unsigned fbase = (frame & 511) * 32;
    unsigned zbase = (zbuf & 511) * 32;
    uint32_t depth = z <= 0.0f ? 0 : z >= 4294967295.0 ? UINT32_MAX : (uint32_t)z;
    if (zpsm == 0x31) depth &= 0xffffff;
    if (zpsm == 0x32 || zpsm == 0x3a) depth &= 65535;
    if ((test >> 16) & 1) {
        unsigned function = (test >> 17) & 3;
        if (function == 0) return;
        if (function >= 2) {
            uint32_t previous = port_gs_memory_read(zbase, width, zpsm, (unsigned)x, (unsigned)y);
            if ((function == 2 && depth < previous) || (function == 3 && depth <= previous)) return;
        }
    }
    if (prim & 16) color = gs_texture_function(color, gs_texture(regs, context, u, v, q), regs[6 + context]);
    if (prim & 32) {
        uint32_t fogcolor = (uint32_t)regs[0x3d];
        uint32_t result = color & 0xff000000;
        fog = fog > 255 ? 255 : fog;
        for (unsigned channel = 0; channel < 3; channel++) {
            unsigned value = (gs_channel(color, channel) * fog + gs_channel(fogcolor, channel) * (255 - fog)) / 255;
            result |= value << (channel * 8);
        }
        color = result;
    }
    if ((prim & 128) && (!(prim & 64) || (color >> 24) == 128))
        color = (color & 0xffffff) | (coverage << 24);
    int frame_write = 1, depth_write = !((zbuf >> 32) & 1) && coverage == 128, rgb_only = 0;
    if ((test & 1) && !gs_alpha_test(color >> 24, (test >> 4) & 255, (test >> 1) & 7)) {
        unsigned fail = (test >> 12) & 3;
        if (fail == 0) return;
        if (fail == 1) depth_write = 0;
        if (fail == 2) frame_write = 0;
        if (fail == 3) { depth_write = 0; rgb_only = 1; }
    }
    uint64_t alpha_register = regs[0x42 + context];
    unsigned blend = (prim & (64 | 128)) && (!(regs[0x49] & 1) || (color & 0x80000000));
    if ((alpha_register & 255) == 0x44 && (color >> 24) == 128) blend = 0;
    unsigned need_destination = blend || ((test >> 14) & 1) || rgb_only || (frame >> 32);
    uint32_t packed = need_destination ? port_gs_memory_read(fbase, width, psm, (unsigned)x, (unsigned)y) : 0;
    uint32_t destination = gs_expand(packed, psm, 0, 0);
    if (((test >> 14) & 1) && psm != 1) {
        unsigned alpha = psm == 2 || psm == 10 ? (packed >> 15) & 1 : packed >> 31;
        if (alpha != ((test >> 15) & 1)) return;
    }
    if (!((test >> 16) & 1)) depth_write = 0;
    if (frame_write) {
        if (blend) color = gs_blend(color, destination, alpha_register, regs[0x46] & 1);
        if (rgb_only) color = (color & 0xffffff) | (destination & 0xff000000);
        if (regs[0x4a + context] & 1) color |= 0x80000000;
        uint32_t output = color;
        if (psm == 2 || psm == 10) {
            int dither = 0;
            if (regs[0x45] & 1) {
                unsigned shift = ((unsigned)(y & 3) * 4 + (unsigned)(x & 3)) * 4;
                dither = (int)((regs[0x44] >> shift) & 7);
                if (dither & 4) dither -= 8;
            }
            unsigned r = (unsigned)gs_limit((int)gs_channel(color, 0) + dither, 0, 255) >> 3;
            unsigned g = (unsigned)gs_limit((int)gs_channel(color, 1) + dither, 0, 255) >> 3;
            unsigned b = (unsigned)gs_limit((int)gs_channel(color, 2) + dither, 0, 255) >> 3;
            output = r | (g << 5) | (b << 10) | ((color >> 16) & 0x8000);
            uint32_t mask = (uint32_t)(frame >> 32);
            mask = ((mask >> 3) & 31) | ((mask >> 6) & 0x3e0) | ((mask >> 9) & 0x7c00) | ((mask >> 16) & 0x8000);
            if (rgb_only) mask |= 0x8000;
            output = (output & ~mask) | (packed & mask);
        } else {
            uint32_t mask = (uint32_t)(frame >> 32);
            if (rgb_only) mask |= 0xff000000;
            output = (output & ~mask) | (packed & mask);
        }
        port_gs_memory_write(fbase, width, psm, (unsigned)x, (unsigned)y, output);
    }
    if (depth_write) port_gs_memory_write(zbase, width, zpsm, (unsigned)x, (unsigned)y, depth);
}

static uint32_t gs_interpolate_color(const PortGsVertex *v, unsigned count, const double *weights)
{
    uint32_t color = 0;
    for (unsigned channel = 0; channel < 4; channel++) {
        double value = gs_channel(v[0].rgba, channel);
        for (unsigned i = 1; i < count; i++)
            value += ((double)gs_channel(v[i].rgba, channel) - (double)gs_channel(v[0].rgba, channel)) * weights[i];
        color |= (uint32_t)gs_limit((int)value, 0, 255) << (channel * 8);
    }
    return color;
}

static void gs_fragment(const uint64_t *regs, unsigned prim, const PortGsVertex *vertices, unsigned count, const double *weights, int x, int y, unsigned coverage)
{
    double z = vertices[0].z;
    float u = (float)vertices[0].u / 16.0f, v = (float)vertices[0].v / 16.0f;
    float s = vertices[0].s, t = vertices[0].t, q = vertices[0].q, fog = (float)vertices[0].fog;
    for (unsigned i = 1; i < count; i++) z += (vertices[i].z - vertices[0].z) * weights[i];
    if (prim & 16) {
        for (unsigned i = 1; i < count; i++) {
            if (prim & 256) {
                u += ((float)vertices[i].u - (float)vertices[0].u) * weights[i] / 16.0f;
                v += ((float)vertices[i].v - (float)vertices[0].v) * weights[i] / 16.0f;
            } else {
                s += (vertices[i].s - vertices[0].s) * weights[i];
                t += (vertices[i].t - vertices[0].t) * weights[i];
            }
            q += (vertices[i].q - vertices[0].q) * weights[i];
        }
    }
    if (prim & 32) {
        for (unsigned i = 1; i < count; i++)
            fog += ((float)vertices[i].fog - (float)vertices[0].fog) * weights[i];
    }
    if (!(prim & 256) && (prim & 16)) {
        uint64_t tex = regs[6 + ((prim >> 9) & 1)];
        if (q == 0.0f || !isfinite(q)) return;
        u = s / q * (float)(1u << ((tex >> 26) & 15));
        v = t / q * (float)(1u << ((tex >> 30) & 15));
    }
    if (!isfinite(u) || !isfinite(v) || !isfinite(z)) return;
    uint32_t color = prim & 8 ? gs_interpolate_color(vertices, count, weights) : vertices[count - 1].rgba;
    gs_pixel(regs, prim, x, y, z, color, u, v, q, (unsigned)gs_limit((int)fog, 0, 255), coverage);
}

static double gs_edge(double ax, double ay, double bx, double by, double x, double y)
{
    return (bx - ax) * (y - ay) - (by - ay) * (x - ax);
}

static int gs_top_left(float ax, float ay, float bx, float by)
{
    return by < ay || (by == ay && bx > ax);
}

static unsigned gs_coverage(double value)
{
    if (value <= 0) return 0;
    if (value >= 1) return 127;
    return ((unsigned)(value * 4096)) >> 5;
}

typedef struct {
    unsigned frame, depth, width, subtract;
} GsOffsetBlend;

static unsigned gs_offset_blend_state(const uint64_t *regs, unsigned prim, const PortGsVertex *v, GsOffsetBlend *state)
{
    if ((prim & ~512u) != 67 || v[2].rgba != UINT32_C(0x40010101)) return 0;
    unsigned context = (prim >> 9) & 1;
    uint64_t frame = regs[0x4c + context], depth = regs[0x4e + context];
    uint64_t alpha = regs[0x42 + context];
    unsigned operation = alpha & 255;
    if ((frame >> 24) || ((depth >> 24) & 15) != 1 || !((depth >> 32) & 1)
        || (regs[0x47 + context] & 0x74001) != 0x50000 || !(regs[0x46] & 1)
        || (regs[0x49] & 1) || (regs[0x4a + context] & 1) || (regs[0x22] & 2)
        || ((alpha >> 32) & 255) != 128 || (operation != 0x62 && operation != 0x68)) return 0;
    state->frame = (frame & 511) * 32;
    state->depth = (depth & 511) * 32;
    state->width = (frame >> 16) & 63;
    state->subtract = operation == 0x62;
    return 1;
}

static void gs_offset_blend_pixel(const GsOffsetBlend *state, int x, int y, double z)
{
    if (!isfinite(z)) return;
    uint32_t depth = z <= 0 ? 0 : z >= 4294967295.0 ? UINT32_MAX : (uint32_t)z;
    depth &= 0xffffff;
    if (depth < port_gs_memory_read(state->depth, state->width, 49, (unsigned)x, (unsigned)y)) return;
    uint32_t destination = port_gs_memory_read(state->frame, state->width, 0, (unsigned)x, (unsigned)y);
    uint32_t result = UINT32_C(0x40000000);
    for (unsigned channel = 0; channel < 3; channel++) {
        unsigned value = gs_channel(destination, channel);
        value = state->subtract ? value - (value > 0) : value + (value < 255);
        result |= value << (channel * 8);
    }
    port_gs_memory_write(state->frame, state->width, 0, (unsigned)x, (unsigned)y, result);
}

static void gs_aa_line(const uint64_t *regs, unsigned prim, const PortGsVertex *v)
{
    double dx = (double)v[1].x - v[0].x, dy = (double)v[1].y - v[0].y;
    unsigned vertical = fabs(dy) > fabs(dx);
    double start = vertical ? v[0].y : v[0].x;
    double end = vertical ? v[1].y : v[1].x;
    double delta = end - start;
    if (!delta) return;
    uint64_t scissor = regs[0x40 + ((prim >> 9) & 1)];
    int low = vertical ? (scissor >> 32) & 2047 : scissor & 2047;
    int high = vertical ? (scissor >> 48) & 2047 : (scissor >> 16) & 2047;
    int begin = gs_limit((int)ceil(fmin(start, end) - 0.5), low, high + 1);
    int finish = gs_limit((int)ceil(fmax(start, end) - 0.5), low, high + 1);
    for (int major = begin; major < finish; major++) {
        double fraction = (major + 0.5 - start) / delta;
        if (fraction < 0 || fraction >= 1) continue;
        double minor = vertical ? v[0].x + dx * fraction : v[0].y + dy * fraction;
        int first = (int)floor(minor - 0.5);
        double weights[2] = {1 - fraction, fraction};
        for (int adjacent = first; adjacent <= first + 1; adjacent++) {
            unsigned coverage = gs_coverage(1 - fabs(adjacent + 0.5 - minor));
            if (!coverage) continue;
            gs_fragment(regs, prim, v, 2, weights, vertical ? adjacent : major,
                        vertical ? major : adjacent, coverage);
        }
    }
}

static int gs_raster_valid(const uint64_t *regs, unsigned prim, const PortGsVertex *vertices, unsigned count)
{
    if (!regs || !vertices) return -1;
    unsigned type = prim & 7;
    if ((type == 0 && count != 1) || (type == 1 && count != 2)
        || (type == 3 && count != 3) || (type == 6 && count != 2)) return -1;
    if (type != 0 && type != 1 && type != 3 && type != 6) return -1;
    if (type == 0 || type == 6) prim &= ~128u;
    unsigned context = (prim >> 9) & 1;
    uint64_t frame = regs[0x4c + context];
    unsigned psm = (frame >> 24) & 63;
    unsigned zpsm = (regs[0x4e + context] >> 24) & 15;
    if (!((frame >> 16) & 63) || (psm != 0 && psm != 1 && psm != 2 && psm != 10)) return -1;
    if (zpsm != 0 && zpsm != 1 && zpsm != 2 && zpsm != 10) return -1;
    if (prim & 16) {
        uint64_t tex1 = regs[0x14 + context];
        if (((tex1 >> 2) & 7) > 6 || ((tex1 >> 6) & 7) > 5) return -1;
        uint64_t tex = regs[6 + context];
        unsigned tpsm = (tex >> 20) & 63;
        if (((tex >> 26) & 15) > 10 || ((tex >> 30) & 15) > 10) return -1;
        if (tpsm != 0 && tpsm != 1 && tpsm != 2 && tpsm != 10 && tpsm != 19
            && tpsm != 20 && tpsm != 27 && tpsm != 36 && tpsm != 44) return -1;
        if (tpsm == 19 || tpsm == 20 || tpsm == 27 || tpsm == 36 || tpsm == 44) {
            unsigned cpsm = (tex >> 51) & 15;
            if (cpsm != 0 && cpsm != 2 && cpsm != 10) return -1;
        }
    }
    float ox = (float)(regs[0x18 + context] & 65535) / 16.0f;
    float oy = (float)((regs[0x18 + context] >> 32) & 65535) / 16.0f;
    for (unsigned i = 0; i < count; i++) {
        float x = vertices[i].x - ox, y = vertices[i].y - oy;
        if (!isfinite(x) || !isfinite(y) || fabsf(x) > 65536 || fabsf(y) > 65536) return -1;
    }
    return 0;
}

int port_gs_raster(uint64_t *regs, unsigned prim, const PortGsVertex *vertices, unsigned count)
{
    if (!in_band && gs_raster_valid(regs, prim, vertices, count)) return -1;
    unsigned type = prim & 7;
    if (type == 0 || type == 6) prim &= ~128u;
    unsigned context = (prim >> 9) & 1;
    if (prim & 16) {
        unsigned tpsm = (regs[6 + context] >> 20) & 63;
        if (tpsm == 19 || tpsm == 20 || tpsm == 27 || tpsm == 36 || tpsm == 44) gs_palette(regs, context);
        gs_texture_state(regs, context);
    }
    PortGsVertex v[3];
    float ox = (float)(regs[0x18 + context] & 65535) / 16.0f;
    float oy = (float)((regs[0x18 + context] >> 32) & 65535) / 16.0f;
    for (unsigned i = 0; i < count; i++) {
        v[i] = vertices[i];
        v[i].x -= ox;
        v[i].y -= oy;
    }
    uint64_t scissor = regs[0x40 + context];
    int min_x = (int)(scissor & 2047), max_x = (int)((scissor >> 16) & 2047);
    int min_y = (int)((scissor >> 32) & 2047), max_y = (int)((scissor >> 48) & 2047);
    if (min_x > max_x || min_y > max_y) return 0;
    int parallel_result;
    if (gs_parallel(regs, prim, vertices, v, count, &parallel_result)) return parallel_result;
    if (type == 0) {
        double weights[1] = {1};
        gs_fragment(regs, prim, v, 1, weights, (int)floorf(v[0].x), (int)floorf(v[0].y), 128);
        return 0;
    }
    if (type == 1) {
        if (prim & 128) {
            gs_aa_line(regs, prim, v);
            return 0;
        }
        float dx = v[1].x - v[0].x, dy = v[1].y - v[0].y;
        unsigned steps = (unsigned)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
        if (!steps) return 0;
        for (unsigned i = 0; i < steps; i++) {
            float fraction = (float)i / (float)steps;
            double weights[2] = {1 - fraction, fraction};
            int x = (int)floorf(v[0].x + dx * fraction);
            int y = (int)floorf(v[0].y + dy * fraction);
            gs_fragment(regs, prim, v, 2, weights, x, y, 128);
        }
        return 0;
    }
    if (type == 6) {
        if (v[0].x == v[1].x || v[0].y == v[1].y) return 0;
        int x0 = gs_limit((int)ceilf(fminf(v[0].x, v[1].x) - 0.5f), min_x, max_x + 1);
        int x1 = gs_limit((int)ceilf(fmaxf(v[0].x, v[1].x) - 0.5f), min_x, max_x + 1);
        int y0 = gs_limit((int)ceilf(fminf(v[0].y, v[1].y) - 0.5f), min_y, max_y + 1);
        int y1 = gs_limit((int)ceilf(fmaxf(v[0].y, v[1].y) - 0.5f), min_y, max_y + 1);
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            float fx = ((float)x + 0.5f - v[0].x) / (v[1].x - v[0].x);
            float fy = ((float)y + 0.5f - v[0].y) / (v[1].y - v[0].y);
            float u = ((float)v[0].u + ((float)v[1].u - (float)v[0].u) * fx) / 16.0f;
            float tv = ((float)v[0].v + ((float)v[1].v - (float)v[0].v) * fy) / 16.0f;
            if (!(prim & 256) && (prim & 16)) {
                uint64_t tex = regs[6 + context];
                if (v[1].q == 0 || !isfinite(v[1].q)) continue;
                u = (v[0].s + (v[1].s - v[0].s) * fx) / v[1].q * (float)(1u << ((tex >> 26) & 15));
                tv = (v[0].t + (v[1].t - v[0].t) * fy) / v[1].q * (float)(1u << ((tex >> 30) & 15));
            }
            if (isfinite(u) && isfinite(tv) && isfinite(v[1].z))
                gs_pixel(regs, prim, x, y, v[1].z, v[1].rgba, u, tv, v[1].q, v[1].fog, 128);
        }
        return 0;
    }
    double area = gs_edge(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    if (area == 0) return 0;
    double sign = area < 0 ? -1.0 : 1.0;
    GsOffsetBlend offset_blend;
    unsigned offset_blending = gs_offset_blend_state(regs, prim, v, &offset_blend);
    double edge_step[3];
    unsigned edge_inclusive[3];
    unsigned fixed_coordinates = 1;
    for (unsigned i = 0; i < 3; i++) {
        unsigned a = (i + 1) % 3, b = (i + 2) % 3;
        edge_step[i] = ((double)v[a].y - v[b].y) * sign;
        if (sign < 0) { unsigned swap = a; a = b; b = swap; }
        edge_inclusive[i] = gs_top_left(v[a].x, v[a].y, v[b].x, v[b].y);
        if (v[i].x * 16 != floorf(v[i].x * 16) || v[i].y * 16 != floorf(v[i].y * 16)) fixed_coordinates = 0;
    }
    float grow = prim & 128 ? 1.0f : 0;
    int x0 = gs_limit((int)ceilf(fminf(v[0].x, fminf(v[1].x, v[2].x)) - 0.5f - grow), min_x, max_x + 1);
    int x1 = gs_limit((int)ceilf(fmaxf(v[0].x, fmaxf(v[1].x, v[2].x)) - 0.5f + grow), min_x, max_x + 1);
    int y0 = gs_limit((int)ceilf(fminf(v[0].y, fminf(v[1].y, v[2].y)) - 0.5f - grow), min_y, max_y + 1);
    int y1 = gs_limit((int)ceilf(fmaxf(v[0].y, fmaxf(v[1].y, v[2].y)) - 0.5f + grow), min_y, max_y + 1);
    for (int y = y0; y < y1; y++) {
        double incremental_edges[3] = {
            gs_edge(v[1].x, v[1].y, v[2].x, v[2].y, x0 + 0.5, y + 0.5) * sign,
            gs_edge(v[2].x, v[2].y, v[0].x, v[0].y, x0 + 0.5, y + 0.5) * sign,
            gs_edge(v[0].x, v[0].y, v[1].x, v[1].y, x0 + 0.5, y + 0.5) * sign
        };
        int first_x = x0, last_x = x1;
        if (!(prim & 128)) {
            for (unsigned i = 0; i < 3; i++) {
                if (edge_step[i] > 0) {
                    double bound = floor(x0 - incremental_edges[i] / edge_step[i]) - 1;
                    if (bound > first_x) first_x = bound < last_x ? (int)bound : last_x;
                } else if (edge_step[i] < 0) {
                    double bound = ceil(x0 - incremental_edges[i] / edge_step[i]) + 2;
                    if (bound < last_x) last_x = bound > first_x ? (int)bound : first_x;
                } else if (incremental_edges[i] < 0) last_x = first_x;
            }
        }
        for (unsigned i = 0; i < 3; i++) incremental_edges[i] += edge_step[i] * (first_x - x0);
        for (int x = first_x; x < last_x; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            double edges[3];
            if (fixed_coordinates) {
                for (unsigned i = 0; i < 3; i++) {
                    edges[i] = incremental_edges[i];
                    incremental_edges[i] += edge_step[i];
                }
            } else {
                edges[0] = gs_edge(v[1].x, v[1].y, v[2].x, v[2].y, px, py) * sign;
                edges[1] = gs_edge(v[2].x, v[2].y, v[0].x, v[0].y, px, py) * sign;
                edges[2] = gs_edge(v[0].x, v[0].y, v[1].x, v[1].y, px, py) * sign;
            }
            unsigned inside = 1;
            for (unsigned i = 0; i < 3; i++) {
                if (edges[i] < 0 || (edges[i] == 0 && !edge_inclusive[i])) { inside = 0; break; }
            }
            if (!inside && !(prim & 128)) continue;
            double weights[3] = {edges[0] / fabs(area), edges[1] / fabs(area), edges[2] / fabs(area)};
            if (inside && offset_blending) {
                double z = v[0].z;
                for (unsigned i = 1; i < 3; i++) z += (v[i].z - v[0].z) * weights[i];
                gs_offset_blend_pixel(&offset_blend, x, y, z);
            } else if (inside) gs_fragment(regs, prim, v, 3, weights, x, y, 128);
            else if (prim & 128) {
                for (unsigned i = 0; i < 3; i++) {
                    if (edges[i] > 0) continue;
                    unsigned a = (i + 1) % 3, b = (i + 2) % 3;
                    double dx = (double)v[b].x - v[a].x, dy = (double)v[b].y - v[a].y;
                    double scale = fmax(fabs(dx), fabs(dy));
                    if (!scale) continue;
                    double fraction = fabs(dx) >= fabs(dy) ? (px - v[a].x) / dx : (py - v[a].y) / dy;
                    if (fraction < 0 || fraction >= 1) continue;
                    unsigned coverage = gs_coverage(1 + edges[i] / scale);
                    if (coverage) gs_fragment(regs, prim, v, 3, weights, x, y, coverage);
                }
            }
        }
    }
    return 0;
}
