#include "gs_raster.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

static uint64_t regs[128];

static void sprite(unsigned queued, unsigned x, unsigned y, unsigned width, unsigned height, unsigned textured) {
    PortGsVertex v[2] = {
        {.x=x,.y=y,.z=100,.q=1,.rgba=0x80808080,.u=0,.v=0},
        {.x=x+width,.y=y+height,.z=100,.q=1,.rgba=0x80808080,.u=width*16,.v=height*16}
    };
    unsigned prim = 6 | 256 | (textured ? 16 : 0);
    assert(!(queued ? port_gs_raster_enqueue(regs, prim, v, 2) : port_gs_raster(regs, prim, v, 2)));
}

static uint64_t run(unsigned queued, unsigned scene) {
    port_gs_memory_reset();
    port_gs_clut_reset();
    port_gs_raster_reset();
    assert(!port_gs_raster_batching(1));
    for (unsigned i = 0; i < 128; i++) regs[i] = 0;
    regs[0x4c] = UINT64_C(2) << 16;
    regs[0x4e] = UINT64_C(1) << 32;
    regs[0x40] = UINT64_C(127) << 16 | UINT64_C(127) << 48;
    regs[0x46] = 1;
    regs[6] = 8192 | UINT64_C(2) << 14 | UINT64_C(6) << 26 | UINT64_C(6) << 30 |
        UINT64_C(1) << 34 | UINT64_C(1) << 35;
    regs[8] = 5;
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++)
        port_gs_memory_write(8192, 2, 0, x, y, UINT32_C(0x80604020));
    if (scene == 0) {
        sprite(queued, 0, 0, 64, 64, 1);
        regs[6] &= ~UINT64_C(16383);
        sprite(queued, 64, 64, 64, 64, 1);
        if (queued) assert(port_gs_memory_read(0, 2, 0, 10, 10) == UINT32_C(0x80604020));
    } else if (scene == 1) {
        sprite(queued, 0, 0, 64, 64, 1);
        regs[0x4c] |= 32;
        sprite(queued, 64, 64, 64, 64, 1);
        if (queued) assert(port_gs_memory_read(0, 2, 0, 10, 10) == UINT32_C(0x80604020));
    } else if (scene == 2) {
        for (unsigned i = 0; i < 1100; i++) sprite(queued, i % 128, i / 128, 1, 1, 0);
    } else if (scene == 3) {
        regs[0x4e] = 128;
        regs[0x47] = UINT64_C(5) << 16;
        for (unsigned i = 0; i < 300; i++) {
            regs[0x42] = 0x44;
            regs[0x47] = (UINT64_C(5) << 16) | (i & 1 ? 1 | UINT64_C(2) << 1 | UINT64_C(129) << 4 : 0);
            sprite(queued, i % 64, (i * 17) % 64, 64, 64, 1);
        }
    } else {
        regs[6] = (scene == 4 ? 8192 : 0) | UINT64_C(6) << 26 | UINT64_C(7) << 30 |
            UINT64_C(1) << 34 | UINT64_C(1) << 35;
        if (scene == 4) {
            sprite(queued, 0, 0, 64, 128, 1);
        } else if (scene == 5) {
            sprite(queued, 0, 0, 64, 32, 0);
            sprite(queued, 64, 64, 64, 64, 1);
            if (queued) assert(port_gs_memory_read(0, 2, 0, 0, 0) == UINT32_C(0x80808080));
        } else {
            for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; ++x)
                port_gs_memory_write(0, 0, 0, x, y, UINT32_C(0x80000000) | y << 8 | x);
            sprite(queued, 0, 0, 64, 128, 1);
            if (queued) {
                PortGsRasterStats stats;
                port_gs_raster_stats(&stats);
                assert(stats.serial_aliases > 0);
            }
        }
    }
    assert(!port_gs_raster_flush());
    if (scene == 0) assert(port_gs_memory_read(0, 2, 0, 80, 80) == UINT32_C(0x80604020));
    if (scene == 4) assert(port_gs_memory_read(0, 2, 0, 16, 96) == UINT32_C(0x80604020));
    if (scene == 5) assert(port_gs_memory_read(0, 2, 0, 80, 112) == UINT32_C(0x80808080));
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 4 * 1024 * 1024; i++) hash = (hash ^ port_gs_memory_read_byte(i)) * UINT64_C(1099511628211);
    return hash;
}

int main(void) {
    for (unsigned scene = 0; scene < 7; scene++) {
        port_gs_raster_threads(1);
        uint64_t expected = run(0, scene);
        const unsigned counts[] = {2, 4, 6, 8, 12, 1, 12};
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
            port_gs_raster_threads(counts[i]);
            assert(run(1, scene) == expected);
        }
        printf("GS batch scene %u: %016" PRIx64 "\n", scene, expected);
    }
    return 0;
}
