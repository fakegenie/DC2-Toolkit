#include "gs_raster.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

static uint64_t draw(unsigned scene, unsigned threads) {
    port_gs_raster_threads(threads);
    port_gs_memory_reset();
    port_gs_clut_reset();
    uint64_t regs[128] = {0};
    unsigned base = scene == 2 || scene == 11 ? 0 : scene == 3 ? 4096 : 8192;
    unsigned psm = scene == 1 || scene == 4 ? 20 : scene == 11 || scene == 12 ? 27 : 0;
    regs[0x4c] = (uint64_t)(scene == 14 ? 1 : scene == 15 ? 63 : 4) << 16;
    if (scene == 7 || scene == 8) regs[0x4c] |= (uint64_t)(scene == 7 ? 2 : 10) << 24;
    regs[0x4e] = scene == 5 ? 0 : 128;
    if (scene == 13) regs[0x4e] |= UINT64_C(2) << 24;
    regs[0x40] = UINT64_C(255) << 16 | UINT64_C(255) << 48;
    if (scene == 15) regs[0x40] = UINT64_C(255) << 16 | UINT64_C(511) << 48;
    regs[0x46] = 1;
    regs[0x47] = UINT64_C(5) << 16;
    regs[0x42] = 0x44;
    regs[6] = base | UINT64_C(4) << 14 | (uint64_t)psm << 20 |
        UINT64_C(8) << 26 | UINT64_C(8) << 30 | UINT64_C(1) << 34 |
        (uint64_t)(scene == 4 ? 0 : 12288) << 37 | UINT64_C(1) << 61;
    regs[0x14] = 97;
    if (scene == 9 || scene == 10) {
        regs[0x14] = 1 | 4 | 32 | UINT64_C(4) << 6 | UINT64_C(16) << 32;
        regs[0x34] = (scene == 9 ? 0 : 9216) | UINT64_C(4) << 14;
    }
    regs[8] = 5;
    for (unsigned y = 0; y < 256; y++) for (unsigned x = 0; x < 256; x++) {
        uint32_t color = 0x80000000 | ((x * 73 + y * 35) & 0xffffff);
        port_gs_memory_write(base, 4, psm, x, y, color);
    }
    for (unsigned i = 0; i < 256; i++)
        port_gs_memory_write(scene == 4 ? 0 : 12288, 1, 0, i & 15, i >> 4, 0x80000000 | i * 65793);
    assert(port_gs_clut_load(regs, 0) >= 0);
    PortGsVertex v[3] = {
        {.x=0,.y=0,.z=100,.q=1,.rgba=0x80808080,.u=0,.v=0},
        {.x=256,.y=0,.z=100,.q=1,.rgba=0x80808080,.u=4096,.v=0},
        {.x=256,.y=256,.z=100,.q=1,.rgba=0x80808080,.u=4096,.v=4096}
    };
    if (scene == 15) v[2].y = 512;
    uint64_t jobs = port_gs_raster_parallel_calls();
    unsigned prim = 3 | 16 | 64 | 256;
    if (scene == 6) prim |= 128;
    assert(!port_gs_raster(regs, prim, v, 3));
    unsigned safe = scene < 2 || scene == 6 || scene == 7 || scene == 8 || scene == 10 || scene == 12 || scene == 13;
    if (threads > 1) assert((port_gs_raster_parallel_calls() > jobs) == safe);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 4 * 1024 * 1024; i++) hash = (hash ^ port_gs_memory_read_byte(i)) * UINT64_C(1099511628211);
    return hash;
}

int main(void) {
    for (unsigned scene = 0; scene < 16; scene++) {
        uint64_t expected = draw(scene, 1);
        const unsigned counts[] = {2, 4, 6, 8, 12, 1, 12};
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) assert(draw(scene, counts[i]) == expected);
        printf("GS bands scene %u: %016" PRIx64 "\n", scene, expected);
    }
    return 0;
}
