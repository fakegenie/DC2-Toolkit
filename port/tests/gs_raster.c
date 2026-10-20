#include "gs_raster.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t regs[128];

static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    port_gs_memory_reset();
    port_gs_clut_reset();
    regs[0x4c] = UINT64_C(1) << 16;
    regs[0x4e] = 128 | (UINT64_C(1) << 32);
    regs[0x40] = (UINT64_C(63) << 16) | (UINT64_C(63) << 48);
    regs[0x46] = 1;
}

static uint32_t pixel(unsigned x, unsigned y)
{
    return port_gs_memory_read(0, 1, 0, x, y);
}

static void zero_stride_textures(void)
{
    reset();
    port_gs_memory_write(0, 0, 0, 0, 0, UINT32_C(0x80305070));
    PortGsVertex vertices[2] = {
        {.x=2,.y=3,.q=1,.rgba=0x80808080},
        {.x=6,.y=7,.q=1,.rgba=0x80808080,.u=16,.v=16}
    };
    assert(!port_gs_raster(regs, 0x156, vertices, 2));
    for (unsigned y = 3; y < 7; ++y) for (unsigned x = 2; x < 6; ++x)
        assert(pixel(x, y) == UINT32_C(0x80305070));

    reset();
    regs[0x40] = UINT64_C(63) << 16 | UINT64_C(127) << 48;
    regs[6] = 8192 | UINT64_C(6) << 26 | UINT64_C(7) << 30 | UINT64_C(1) << 34 | UINT64_C(1) << 35;
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; ++x)
        port_gs_memory_write(8192, 0, 0, x, y, UINT32_C(0x80000000) | y << 8 | x);
    vertices[0].x = vertices[0].y = 0;
    vertices[1].x = 64;
    vertices[1].y = 128;
    vertices[1].u = 64 * 16;
    vertices[1].v = 128 * 16;
    assert(!port_gs_raster(regs, 0x116, vertices, 2));
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 64; ++x)
        assert(pixel(x, y) == (UINT32_C(0x80000000) | (y % 32) << 8 | x));
}

static void additive_blending(void)
{
    reset();
    PortGsVertex v = {.x=0,.y=0,.q=1};
    for (unsigned subtract = 0; subtract < 2; subtract++) for (unsigned clamp = 0; clamp < 2; clamp++) {
        regs[0x42] = UINT64_C(128) << 32 | (subtract ? 0x62 : 0x68);
        regs[0x46] = clamp;
        for (unsigned source = 0; source < 256; source++) for (unsigned destination = 0; destination < 256; destination++) {
            v.rgba = UINT32_C(0x40000000) | source * UINT32_C(0x10101);
            port_gs_memory_write(0, 1, 0, 0, 0, UINT32_C(0x80000000) | destination * UINT32_C(0x10101));
            int value = (int)destination + (subtract ? -(int)source : (int)source);
            if (clamp && value < 0) value = 0;
            if (clamp && value > 255) value = 255;
            assert(!port_gs_raster(regs, 64, &v, 1));
            assert(pixel(0, 0) == (UINT32_C(0x40000000) | ((unsigned)value & 255) * UINT32_C(0x10101)));
        }
    }
}

static void bilinear_fractions(void)
{
    const uint32_t colors[2][2] = {
        {UINT32_C(0x0211feff), UINT32_C(0xfd7d0301)},
        {UINT32_C(0x81ff8007), UINT32_C(0x4001f7fc)}
    };
    const unsigned positions[4][3][2] = {
        {{1, 0}, {0, 1}, {1, 0}},
        {{0, 0}, {0, 1}, {1, 1}},
        {{1, 1}, {1, 1}, {1, 1}},
        {{1, 0}, {0, 1}, {1, 0}}
    };
    for (unsigned indexed = 0; indexed < 2; indexed++) {
        reset();
        regs[6] = 256 | UINT64_C(2) << 14 | (uint64_t)(indexed ? 20 : 0) << 20
            | UINT64_C(1) << 26 | UINT64_C(1) << 30 | UINT64_C(1) << 34 | UINT64_C(1) << 35;
        regs[0x14] = 32 | UINT64_C(1) << 6;
        for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 2; x++) {
            port_gs_memory_write(256, 2, indexed ? 20 : 0, x, y, indexed ? y * 2 + x : colors[y][x]);
            if (indexed) port_gs_memory_write(512, 1, 0, y * 2 + x, 0, colors[y][x]);
        }
        if (indexed) {
            regs[6] |= UINT64_C(512) << 37 | UINT64_C(1) << 61;
            assert(port_gs_clut_load(regs, 0) == 1);
        }
        for (unsigned mode = 0; mode < 4; mode++) {
            regs[8] = mode | (uint64_t)mode << 2;
            if (mode >= 2) regs[8] |= UINT64_C(1) << 4 | UINT64_C(1) << 24;
            if (mode == 2) regs[8] |= UINT64_C(1) << 14 | UINT64_C(1) << 34;
            for (unsigned ix = 0; ix < 3; ix++) for (unsigned iy = 0; iy < 3; iy++) {
                uint32_t a = colors[positions[mode][iy][0]][positions[mode][ix][0]];
                uint32_t b = colors[positions[mode][iy][0]][positions[mode][ix][1]];
                uint32_t c = colors[positions[mode][iy][1]][positions[mode][ix][0]];
                uint32_t d = colors[positions[mode][iy][1]][positions[mode][ix][1]];
                for (unsigned fy = 0; fy < 16; fy++) for (unsigned fx = 0; fx < 16; fx++) {
                    PortGsVertex v = {.x = 2, .y = 3, .q = 1, .rgba = UINT32_MAX};
                    v.s = ((float)ix - 0.5f + (float)fx / 16.0f) / 2.0f;
                    v.t = ((float)iy - 0.5f + (float)fy / 16.0f) / 2.0f;
                    uint32_t expected = 0;
                    for (unsigned channel = 0; channel < 4; channel++) {
                        unsigned shift = channel * 8;
                        unsigned value = ((a >> shift) & 255) * (16 - fx) * (16 - fy)
                            + ((b >> shift) & 255) * fx * (16 - fy)
                            + ((c >> shift) & 255) * (16 - fx) * fy
                            + ((d >> shift) & 255) * fx * fy;
                        expected |= (value >> 8) << shift;
                    }
                    assert(!port_gs_raster(regs, 16, &v, 1));
                    assert(pixel(2, 3) == expected);
                }
            }
        }
    }
}

static uint64_t offset_blending(unsigned generic, unsigned subtract, unsigned variation)
{
    reset();
    regs[0x4e] |= UINT64_C(1) << 24;
    regs[0x47] = 0x5000a;
    regs[0x42] = UINT64_C(128) << 32 | (subtract ? 0x62 : 0x68);
    PortGsVertex v[3] = {
        {.x=-5,.y=2.25,.z=10,.q=1,.rgba=0x40010101},
        {.x=65,.y=4.5,.z=16777217,.q=1,.rgba=0x40010101},
        {.x=5,.y=65,.z=200,.q=1,.rgba=0x40010101}
    };
    switch (variation) {
        case 1: regs[0x46] = 0; break;
        case 2: regs[0x49] = 1; break;
        case 3: regs[0x4a] = 1; break;
        case 4: regs[0x4c] |= UINT64_C(2) << 24; break;
        case 5: regs[0x4c] |= UINT64_C(1) << 24; break;
        case 6: regs[0x4c] |= UINT64_C(0xff00) << 32; break;
        case 7: regs[0x22] = 2; break;
        case 8: regs[0x22] = 3; break;
        case 9: regs[0x47] |= 1; break;
        case 10: regs[0x47] |= UINT64_C(1) << 14; break;
        case 11: regs[0x4e] &= ~(UINT64_C(1) << 32); break;
        case 12: regs[0x47] = UINT64_C(7) << 16; break;
        case 13: regs[0x4e] &= ~(UINT64_C(15) << 24); break;
        case 14: regs[0x4e] = (regs[0x4e] & ~(UINT64_C(15) << 24)) | UINT64_C(2) << 24; break;
        case 15: v[2].rgba = 0x40020100; break;
        case 16: regs[0x42] ^= UINT64_C(1) << 32; break;
        case 17: regs[0x47] = 0; break;
        case 18: v[0].z = v[1].z = v[2].z = -1; break;
        case 19: v[0].z = v[1].z = v[2].z = 4294967296.0; break;
        case 20: regs[0x4e] = UINT64_C(0x101000000); break;
    }
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
        port_gs_memory_write(0, 1, 0, x, y, UINT32_C(0x80000000) | (x * 37 + y * 53) * UINT32_C(0x10101));
        port_gs_memory_write(128 * 32, 1, 49, x, y, x * 21111 + y * 17777);
    }
    assert(!port_gs_raster(regs, 67 | (generic ? 256 : 0), v, 3));
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 4 * 1024 * 1024; i++)
        hash = (hash ^ port_gs_memory_read_byte(i)) * UINT64_C(1099511628211);
    return hash;
}

static void antialiasing(void)
{
    PortGsVertex v[3] = {
        {.x=2,.y=2,.z=10,.q=1,.rgba=0x80000080},
        {.x=10,.y=2,.z=10,.q=1,.rgba=0x80000080},
        {.x=2,.y=10,.z=10,.q=1,.rgba=0x80000080}
    };
    reset();
    regs[0x42] = 0x44;
    regs[0x4e] = 128;
    regs[0x47] = UINT64_C(3) << 16;
    assert(!port_gs_raster(regs, 3 | 128, v, 3));
    assert(pixel(3, 3) == 0x80000080);
    assert(pixel(4, 1) == 0x40000040);
    assert(pixel(1, 4) == 0x40000040);
    assert(pixel(5, 6) == 0x7f00007f);
    assert(!pixel(6, 6));
    assert(port_gs_memory_read(128 * 32, 1, 48, 3, 3) == 10);
    assert(!port_gs_memory_read(128 * 32, 1, 48, 4, 1));
    reset();
    regs[0x42] = 0x44;
    regs[0x47] = 1 | UINT64_C(6) << 1 | UINT64_C(100) << 4;
    assert(!port_gs_raster(regs, 3 | 128, v, 3));
    assert(pixel(3, 3) == 0x80000080);
    assert(!pixel(4, 1));
    reset();
    regs[0x42] = 0x44;
    for (unsigned i = 0; i < 3; i++) v[i].rgba = 0x40000080;
    assert(!port_gs_raster(regs, 3 | 128 | 64, v, 3));
    assert(pixel(3, 3) == 0x40000040);
    assert(pixel(4, 1) == 0x40000040);
    reset();
    regs[0x42] = 0x44;
    assert(!port_gs_raster(regs, 3 | 128, v, 3));
    assert(pixel(3, 3) == 0x80000080);
    assert(pixel(4, 1) == 0x40000040);
    reset();
    regs[0x42] = 0x44;
    regs[0x4e] = 128;
    regs[0x47] = UINT64_C(3) << 16;
    v[0].rgba = v[1].rgba = 0x80000080;
    v[0].y = v[1].y = 2.25f;
    assert(!port_gs_raster(regs, 1 | 128, v, 2));
    assert(pixel(4, 1) == 0x20000020);
    assert(pixel(4, 2) == 0x60000060);
    assert(!pixel(4, 3));
    assert(!port_gs_memory_read(128 * 32, 1, 48, 4, 2));
    reset();
    regs[0x42] = 0x44;
    v[0].x = v[1].x = 2.25f;
    v[0].y = 2;
    v[1].y = 10;
    assert(!port_gs_raster(regs, 1 | 128, v, 2));
    assert(pixel(1, 4) == 0x20000020);
    assert(pixel(2, 4) == 0x60000060);
    reset();
    regs[0x42] = 0x44;
    v[0].x = v[0].y = 2;
    v[1].x = v[1].y = 10;
    assert(!port_gs_raster(regs, 1 | 128, v, 2));
    assert(pixel(4, 4) == 0x7f00007f);
    assert(!pixel(4, 5));
    reset();
    regs[0x42] = 0x44;
    v[0] = (PortGsVertex){.x=2,.y=2,.z=10,.q=1,.rgba=0x80000010};
    v[1] = (PortGsVertex){.x=10,.y=2,.z=10,.q=1,.rgba=0x80000010};
    v[2] = (PortGsVertex){.x=2,.y=10,.z=90,.q=1,.rgba=0x80000090};
    assert(!port_gs_raster(regs, 3 | 8 | 128, v, 3));
    assert(pixel(4, 1) == 0x40000004);
    reset();
    regs[0x42] = 0x44;
    regs[0x4e] = 128;
    regs[0x47] = UINT64_C(5) << 16;
    port_gs_memory_write(128 * 32, 1, 48, 4, 1, 6);
    assert(!port_gs_raster(regs, 3 | 8 | 128, v, 3));
    assert(!pixel(4, 1));
    assert(port_gs_memory_read(128 * 32, 1, 48, 4, 1) == 6);
}

int main(void)
{
    zero_stride_textures();
    bilinear_fractions();
    additive_blending();
    for (unsigned subtract = 0; subtract < 2; subtract++) for (unsigned variation = 0; variation <= 20; variation++)
        assert(offset_blending(0, subtract, variation) == offset_blending(1, subtract, variation));
    PortGsVertex vertices[3] = {
        {.x = 2, .y = 3, .z = 10, .q = 1, .rgba = 0x80112233, .fog = 255},
        {.x = 6, .y = 7, .z = 10, .q = 1, .rgba = 0x80112233, .fog = 255},
        {.x = 2, .y = 7, .z = 10, .q = 1, .rgba = 0x80112233, .fog = 255}
    };
    reset();
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80112233);
    assert(pixel(3, 3) == 0);
    reset();
    regs[0x18] = 16 | (UINT64_C(32) << 32);
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(pixel(1, 1) == 0x80112233);
    reset();
    assert(port_gs_raster(regs, 6, vertices, 2) == 0);
    for (unsigned y = 0; y < 10; y++) for (unsigned x = 0; x < 10; x++)
        assert(pixel(x, y) == (x >= 2 && x < 6 && y >= 3 && y < 7 ? 0x80112233u : 0));
    reset();
    assert(port_gs_raster(regs, 3, vertices, 3) == 0);
    assert(pixel(2, 4) == 0x80112233);
    assert(pixel(5, 3) == 0);
    reset();
    assert(port_gs_raster(regs, 1, vertices, 2) == 0);
    assert(pixel(2, 3) == 0x80112233);
    assert(pixel(5, 6) == 0x80112233);
    assert(pixel(6, 7) == 0);
    reset();
    regs[0x47] = 1;
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(pixel(2, 3) == 0);
    regs[0x47] = 1 | (UINT64_C(6) << 1) | (UINT64_C(127) << 4);
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80112233);
    reset();
    regs[0x4e] = 128;
    regs[0x47] = (UINT64_C(1) << 16) | (UINT64_C(3) << 17);
    port_gs_memory_write(128 * 32, 1, 0x30, 2, 3, 20);
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(pixel(2, 3) == 0);
    vertices[0].z = 30;
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80112233);
    assert(port_gs_memory_read(128 * 32, 1, 0x30, 2, 3) == 30);
    reset();
    vertices[0].rgba = 0x40000080;
    port_gs_memory_write(0, 1, 0, 2, 3, 0x80000020);
    regs[0x42] = 0 | (UINT64_C(1) << 2) | (UINT64_C(1) << 6);
    assert(port_gs_raster(regs, 64, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x40000050);
    reset();
    vertices[0].rgba = 0x80808080;
    vertices[0].u = 16;
    vertices[0].v = 0;
    regs[6] = 256 | (UINT64_C(1) << 14) | (UINT64_C(1) << 26) | (UINT64_C(1) << 30) | (UINT64_C(1) << 34);
    port_gs_memory_write(256, 1, 0, 1, 0, 0x80402010);
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80402010);
    reset();
    regs[6] = 256 | (UINT64_C(1) << 14) | (UINT64_C(1) << 26) | (UINT64_C(1) << 30) | (UINT64_C(1) << 34);
    regs[0x14] = 32;
    vertices[0].u = 16;
    vertices[0].v = 8;
    port_gs_memory_write(256, 1, 0, 0, 0, 0x80000000);
    port_gs_memory_write(256, 1, 0, 1, 0, 0x80000080);
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80000040);
    reset();
    regs[6] = 256 | (UINT64_C(1) << 14) | (UINT64_C(1) << 26) | (UINT64_C(1) << 30) | (UINT64_C(1) << 34);
    regs[0x14] = 1 | (UINT64_C(1) << 2) | (UINT64_C(2) << 6) | (UINT64_C(16) << 32);
    regs[0x34] = 768 | (UINT64_C(1) << 14);
    port_gs_memory_write(768, 1, 0, 0, 0, 0x80402060);
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80402060);
    vertices[0].u = 0;
    vertices[0].v = 0;
    regs[0x14] = 1 | (UINT64_C(1) << 2) | (UINT64_C(3) << 6) | (UINT64_C(8) << 32);
    port_gs_memory_write(256, 1, 0, 0, 0, 0x80000020);
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80201040);
    vertices[0].s = 1e30f;
    vertices[0].t = -1e30f;
    vertices[0].q = 1e-5f;
    assert(port_gs_raster(regs, 16, vertices, 1) == 0);
    reset();
    regs[0x4e] = 128;
    regs[0x47] = (UINT64_C(1) << 16) | (UINT64_C(3) << 17);
    vertices[0].z = 4294967294.0;
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(port_gs_memory_read(128 * 32, 1, 0x30, 2, 3) == 4294967294u);
    reset();
    regs[6] = 256 | (UINT64_C(1) << 14) | (UINT64_C(19) << 20) | (UINT64_C(1) << 26)
        | (UINT64_C(1) << 30) | (UINT64_C(1) << 34) | (UINT64_C(512) << 37);
    regs[0x14] = 0;
    vertices[0].u = 0;
    vertices[0].v = 0;
    port_gs_memory_write(256, 1, 19, 0, 0, 8);
    port_gs_memory_write(512, 1, 0, 0, 1, 0x80705030);
    regs[6] |= UINT64_C(1) << 61;
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80705030);
    port_gs_memory_write(512, 1, 0, 0, 1, 0x80604020);
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80705030);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80604020);
    regs[6] &= ~(UINT64_C(15) << 51);
    regs[6] |= UINT64_C(2) << 51;
    port_gs_memory_write(512, 1, 2, 0, 1, 31);
    assert(port_gs_clut_load(regs, 0) == 1);
    regs[0x3b] = 0x40;
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x400000f8);
    regs[0x3b] = 0x20;
    assert(port_gs_raster(regs, 16 | 256, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x200000f8);
    reset();
    regs[0x4c] |= UINT64_C(0xff) << 32;
    assert(port_gs_raster(regs, 0, vertices, 1) == 0);
    assert(pixel(2, 3) == 0x80808000);
    assert(port_gs_raster(regs, 2, vertices, 2) == -1);
    assert(port_gs_raster(regs, 128, vertices, 1) == 0);
    assert(port_gs_raster(regs, 128 | 6, vertices, 2) == 0);
    assert(port_gs_raster(regs, 128 | 1, vertices, 2) == 0);
    antialiasing();
    puts("GS raster tests passed");
    return 0;
}
