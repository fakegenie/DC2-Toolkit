#define _POSIX_C_SOURCE 200809L
#include "gs_raster.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static uint64_t regs[128];

static double seconds(clockid_t clock) {
    struct timespec value;
    clock_gettime(clock, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}

static void scene(unsigned indexed) {
    port_gs_memory_reset();
    port_gs_clut_reset();
    regs[0x4c] = UINT64_C(8) << 16;
    regs[0x4e] = 128;
    regs[0x40] = UINT64_C(511) << 16 | UINT64_C(479) << 48;
    regs[0x46] = 1;
    regs[0x47] = UINT64_C(7) << 16;
    regs[0x42] = UINT64_C(1) << 2 | UINT64_C(1) << 6;
    unsigned psm = indexed == 3 ? 27 : indexed == 2 ? 20 : indexed ? 19 : 0;
    regs[6] = 8192 | UINT64_C(4) << 14 | (uint64_t)psm << 20 |
              UINT64_C(8) << 26 | UINT64_C(8) << 30 | UINT64_C(1) << 34 |
              UINT64_C(12288) << 37 | UINT64_C(1) << 61;
    regs[0x14] = 1 | 32 | 64;
    regs[8] = 0;
    for (unsigned y = 0; y < 256; y++) for (unsigned x = 0; x < 256; x++) {
        uint32_t color = UINT32_C(0x80000000) | x | y << 8 | (x ^ y) << 16;
        port_gs_memory_write(8192, 4, psm, x, y, indexed ? (x ^ y) : color);
    }
    for (unsigned i = 0; i < 256; i++) {
        unsigned j = (i & ~24u) | (i & 8) << 1 | (i & 16) >> 1;
        port_gs_memory_write(12288, 1, 0, j & 15, j >> 4, UINT32_C(0x80000000) | i * 65793);
    }
    assert(port_gs_clut_load(regs, 0) >= 0);
}

static void benchmark(unsigned indexed, unsigned sprite, unsigned rejected, unsigned thin) {
    scene(indexed);
    PortGsVertex a[3] = {
        {.x=0,.y=0,.z=100,.q=1,.rgba=0x40808080,.u=0,.v=0},
        {.x=512,.y=0,.z=100,.q=1,.rgba=0x40808080,.u=4096,.v=0},
        {.x=512,.y=480,.z=100,.q=1,.rgba=0x40808080,.u=4096,.v=4096}
    };
    PortGsVertex b[3] = {a[0],a[2],a[2]};
    b[2].x = 0;
    b[2].u = 0;
    if (thin) {
        a[1].y = 479;
        a[2].x = 511;
    }
    if (rejected) for (unsigned y = 0; y < 480; y++) for (unsigned x = 0; x < 512; x++)
        port_gs_memory_write(128 * 32, 8, 48, x, y, 1000);
    double start = seconds(CLOCK_PROCESS_CPUTIME_ID);
    double wall = seconds(CLOCK_MONOTONIC);
    unsigned frames = 12;
    for (unsigned frame = 0; frame < frames; frame++) {
        for (unsigned i = 0; i < 3; i++) a[i].z = b[i].z = frame + 100;
        if (sprite) {
            PortGsVertex v[2] = {a[0],a[2]};
            assert(!port_gs_raster(regs, 6 | 16 | 64 | 256, v, 2));
        } else {
            assert(!port_gs_raster(regs, 3 | 8 | 16 | 64 | 256, a, 3));
            if (!thin) assert(!port_gs_raster(regs, 3 | 8 | 16 | 64 | 256, b, 3));
        }
    }
    double duration = seconds(CLOCK_PROCESS_CPUTIME_ID) - start;
    wall = seconds(CLOCK_MONOTONIC) - wall;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 4 * 1024 * 1024; i++) hash = (hash ^ port_gs_memory_read_byte(i)) * UINT64_C(1099511628211);
    printf("%s %s%s cpu=%.3f wall=%.3f ms/frame hash=%016" PRIx64 "\n", indexed == 3 ? "indexed8h" : indexed == 2 ? "indexed4" : indexed ? "indexed8" : "rgba32", sprite ? "sprite" : thin ? "thin triangle" : "triangles", rejected ? " occluded" : "", duration * 1000 / frames, wall * 1000 / frames, hash);
}

int main(void) {
    benchmark(0, 0, 0, 0);
    benchmark(1, 0, 0, 0);
    benchmark(2, 0, 0, 0);
    benchmark(3, 0, 0, 0);
    benchmark(0, 1, 0, 0);
    benchmark(1, 1, 0, 0);
    benchmark(2, 1, 0, 0);
    benchmark(3, 1, 0, 0);
    benchmark(0, 0, 1, 0);
    benchmark(0, 0, 0, 1);
    benchmark(0, 0, 1, 1);
    return 0;
}
