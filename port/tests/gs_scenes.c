#include "gs_raster.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint32_t random_state = 0x138a52f1;

static unsigned random_value(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        port_gs_raster_threads(12);
        assert(!port_gs_raster_batching(1));
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned scene = 0; scene < 384; scene++) {
        uint64_t regs[128] = {0};
        port_gs_memory_reset();
        port_gs_clut_reset();
        unsigned psm = scene & 1 ? 0 : 2;
        regs[0x4c] = UINT64_C(1) << 16 | (uint64_t)psm << 24;
        if (!(scene % 7)) regs[0x4c] |= (uint64_t)random_value() << 32;
        regs[0x4e] = 128;
        regs[0x40] = UINT64_C(63) << 16 | UINT64_C(63) << 48;
        regs[0x46] = 1;
        regs[0x47] = UINT64_C(5) << 16;
        if (!(scene % 3)) regs[0x47] |= 1 | (uint64_t)(random_value() & 7) << 1 |
            (uint64_t)(random_value() & 255) << 4 | (uint64_t)(random_value() & 3) << 12;
        regs[0x42] = 0x44;
        regs[0x44] = UINT64_C(0x2354235123521351);
        regs[0x45] = scene & 1;
        regs[0x3d] = random_value() & 0xffffff;
        regs[0x3b] = 0x8000000080;
        unsigned tpsm = scene % 3 ? 0 : 20;
        regs[6] = 256 | UINT64_C(1) << 14 | (uint64_t)tpsm << 20 | UINT64_C(4) << 26 |
            UINT64_C(4) << 30 | UINT64_C(1) << 34 | UINT64_C(1024) << 37 | UINT64_C(1) << 61;
        regs[0x14] = 1 | ((scene & 1) ? 32 : 0);
        regs[8] = scene % 4 == 0 ? 5 : 0;
        for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
            port_gs_memory_write(0, 1, psm, x, y, random_value());
            port_gs_memory_write(128 * 32, 1, 48, x, y, random_value() & 255);
        }
        for (unsigned y = 0; y < 16; y++) for (unsigned x = 0; x < 16; x++)
            port_gs_memory_write(256, 1, tpsm, x, y, random_value());
        for (unsigned i = 0; i < 256; i++)
            port_gs_memory_write(1024, 1, 0, i & 15, i >> 4, random_value());
        assert(port_gs_clut_load(regs, 0) >= 0);
        for (unsigned draw = 0; draw < 8; draw++) {
            PortGsVertex v[3];
            memset(v, 0, sizeof(v));
            for (unsigned i = 0; i < 3; i++) {
                v[i].x = (int)(random_value() % 288) / 4.0f - 4;
                v[i].y = (int)(random_value() % 288) / 4.0f - 4;
                v[i].z = random_value() & 1023;
                v[i].rgba = random_value();
                if (!(scene % 5)) v[i].rgba = (v[i].rgba & 0xffffff) | 0x80000000;
                v[i].u = random_value() & 511;
                v[i].v = random_value() & 511;
                v[i].s = (random_value() & 255) / 256.0f;
                v[i].t = (random_value() & 255) / 256.0f;
                v[i].q = 0.5f + (random_value() & 255) / 128.0f;
                v[i].fog = random_value() & 255;
            }
            unsigned types[] = {0, 1, 3, 6};
            unsigned type = types[scene % 4];
            unsigned prim = type | (random_value() & (8 | 16 | 32 | 64 | 128 | 256));
            unsigned count = type == 0 ? 1 : type == 3 ? 3 : 2;
            assert(!(argc > 1 ? port_gs_raster_enqueue(regs, prim, v, count) : port_gs_raster(regs, prim, v, count)));
        }
        assert(!port_gs_raster_flush());
        for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
            hash = (hash ^ port_gs_memory_read(0, 1, psm, x, y)) * UINT64_C(1099511628211);
            hash = (hash ^ port_gs_memory_read(128 * 32, 1, 48, x, y)) * UINT64_C(1099511628211);
        }
    }
    assert(hash == UINT64_C(0xa65be69641c5097a));
    printf("gs scenes: %016" PRIx64 "\n", hash);
    return 0;
}
