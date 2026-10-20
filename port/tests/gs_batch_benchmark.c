#define _POSIX_C_SOURCE 200809L
#include "gs_raster.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static double seconds(clockid_t clock) {
    struct timespec value;
    clock_gettime(clock, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}

static uint64_t run(unsigned threads, unsigned queued) {
    assert(!port_gs_raster_flush());
    port_gs_raster_threads(threads);
    port_gs_memory_reset();
    port_gs_clut_reset();
    assert(!port_gs_raster_batching(queued));
    uint64_t regs[128] = {0};
    regs[0x4c] = UINT64_C(8) << 16;
    regs[0x4e] = 128;
    regs[0x40] = UINT64_C(511) << 16 | UINT64_C(479) << 48;
    regs[0x46] = 1;
    regs[0x47] = UINT64_C(5) << 16;
    regs[0x42] = 0x44;
    regs[0x3d] = 0x802010;
    regs[6] = 8192 | UINT64_C(1) << 14 | UINT64_C(6) << 26 | UINT64_C(6) << 30 | UINT64_C(1) << 34;
    regs[0x14] = 97;
    regs[8] = 5;
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++)
        port_gs_memory_write(8192, 1, 0, x, y, UINT32_C(0x80000000) | x * 3 | y * 3 << 8);
    double wall = seconds(CLOCK_MONOTONIC), cpu = seconds(CLOCK_PROCESS_CPUTIME_ID);
    for (unsigned frame = 0; frame < 12; frame++) {
        for (unsigned y = 0; y < 480; y += 15) for (unsigned x = 0; x < 512; x += 16) {
            PortGsVertex a[3] = {
                {.x=x,.y=y,.z=frame+100,.q=1,.rgba=0x40808080,.s=0,.t=0,.fog=255},
                {.x=x+16,.y=y,.z=frame+100,.q=1,.rgba=0x40808080,.s=1,.t=0,.fog=255},
                {.x=x+16,.y=y+15,.z=frame+100,.q=1,.rgba=0x40808080,.s=1,.t=1,.fog=255}
            };
            PortGsVertex b[3] = {a[0],a[2],a[2]};
            b[2].x = x;
            b[2].s = 0;
            unsigned prim = 3 | 8 | 16 | 32 | 64;
            assert(!(queued ? port_gs_raster_enqueue(regs, prim, a, 3) : port_gs_raster(regs, prim, a, 3)));
            assert(!(queued ? port_gs_raster_enqueue(regs, prim, b, 3) : port_gs_raster(regs, prim, b, 3)));
        }
        assert(!port_gs_raster_flush());
    }
    wall = seconds(CLOCK_MONOTONIC) - wall;
    cpu = seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 4 * 1024 * 1024; i++) hash = (hash ^ port_gs_memory_read_byte(i)) * UINT64_C(1099511628211);
    printf("GS 2048 triangles threads=%u batch=%u cpu=%.3f wall=%.3f ms/frame hash=%016" PRIx64 "\n",
           threads, queued, cpu * 1000 / 12, wall * 1000 / 12, hash);
    return hash;
}

int main(void) {
    uint64_t expected = run(1, 0);
    assert(run(4, 0) == expected);
    assert(run(4, 1) == expected);
    return 0;
}
