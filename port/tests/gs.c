#include "gs.h"
#include "gs_memory.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void set_transfer(unsigned psm, unsigned width, unsigned height) {
    assert(!port_gs_write_register(0x50, UINT64_C(1) << 48 | (uint64_t)psm << 56));
    assert(!port_gs_write_register(0x51, 0));
    assert(!port_gs_write_register(0x52, width | (uint64_t)height << 32));
    assert(!port_gs_write_register(0x53, 0));
}

static void image_roundtrip(unsigned psm, unsigned bits) {
    uint64_t load[12] = {0};
    uint64_t store[14] = {0};
    unsigned char source[128];
    unsigned char result[128];
    unsigned bytes = 32 * bits / 8;
    for (unsigned i = 0; i < bytes; i++) source[i] = (unsigned char)(i * 37 + 19);
    load[2] = UINT64_C(1) << 48 | (uint64_t)psm << 56;
    load[4] = UINT64_C(7) << 32 | UINT64_C(3) << 48;
    load[6] = 8 | UINT64_C(4) << 32;
    store[4] = UINT64_C(1) << 16 | (uint64_t)psm << 24;
    store[6] = 7 | UINT64_C(3) << 16;
    store[8] = load[6];
    store[12] = 1;
    assert(!port_gs_image_load(load, source, bytes));
    assert(!port_gs_image_store(store, result, bytes));
    assert(!memcmp(source, result, bytes));
    assert(port_gs_image_load(load, source, bytes - 1) == -1);
    assert(port_gs_image_store(store, result, bytes - 1) == -1);
}

static void gif_formats(void) {
    port_gs_reset();
    uint64_t packed[] = {
        UINT64_C(1) | UINT64_C(1) << 60, 14,
        UINT64_C(0x123456789abcdef0), 0x40,
        UINT64_C(3) | UINT64_C(1) << 58 | UINT64_C(1) << 60, 0x46,
        UINT64_C(0x11223344), UINT64_C(0x88776655),
        UINT64_C(0x55667788), UINT64_C(0xfedcba98),
        UINT64_C(1) | UINT64_C(1) << 60, 14,
        UINT64_C(0x99), 0x3d
    };
    assert(!port_gif_feed(packed, 7, 2));
    assert(!port_gif_feed((unsigned char *)packed + 7, 25, 2));
    assert(!port_gif_feed((unsigned char *)packed + 32, sizeof(packed) - 32, 2));
    assert(port_gs_read_register(0x40) == UINT64_C(0x123456789abcdef0));
    assert(port_gs_read_register(6) == UINT64_C(0x55667788));
    assert(port_gs_read_register(0x3d) == 0x99);
    uint64_t sixteen[18] = {1, UINT64_C(0xffffffffffffffff)};
    assert(!port_gif_submit(sixteen, sizeof(sixteen)));
    assert(port_gif_feed(NULL, 1, 3) == -1);
    assert(port_gif_feed(NULL, 0, 4) == -1);
    assert(port_gs_write_register(128, 0) == -1);
    assert(strlen(port_gs_error()));
}

static void gif_image(void) {
    port_gs_reset();
    set_transfer(1, 3, 3);
    uint64_t stream[6] = {2 | UINT64_C(2) << 58, 0, 0, 0, 0, 0};
    unsigned char *pixels = (unsigned char *)(stream + 2);
    for (unsigned i = 0; i < 27; i++) pixels[i] = (unsigned char)(i + 1);
    assert(!port_gif_submit(stream, 23));
    assert(!port_gif_submit((unsigned char *)stream + 23, sizeof(stream) - 23));
    for (unsigned y = 0; y < 3; y++) {
        for (unsigned x = 0; x < 3; x++) {
            unsigned i = (y * 3 + x) * 3;
            assert(port_gs_memory_read(0, 1, 1, x, y) ==
                   ((uint32_t)pixels[i] | (uint32_t)pixels[i + 1] << 8 | (uint32_t)pixels[i + 2] << 16));
        }
    }
}

static void local_transfer(void) {
    port_gs_reset();
    for (unsigned x = 0; x < 8; x++) port_gs_memory_write(0, 1, 0, x, 0, x + 10);
    assert(!port_gs_write_register(0x50, UINT64_C(1) << 16 | UINT64_C(1) << 48));
    assert(!port_gs_write_register(0x51, UINT64_C(1) << 32 | UINT64_C(2) << 59));
    assert(!port_gs_write_register(0x52, 7 | UINT64_C(1) << 32));
    assert(!port_gs_write_register(0x53, 2));
    for (unsigned x = 1; x < 8; x++) assert(port_gs_memory_read(0, 1, 0, x, 0) == x + 9);
}

static void automatic_mipmaps(void) {
    port_gs_reset();
    assert(!port_gs_write_register(0x14, 512 | 8));
    assert(!port_gs_write_register(6, UINT64_C(7) << 26));
    uint64_t mip = port_gs_read_register(0x34);
    assert((mip & 16383) == 256);
    assert(((mip >> 14) & 63) == 1);
    assert(((mip >> 20) & 16383) == 320);
    assert(((mip >> 40) & 16383) == 336);
    assert(!port_gs_write_register(6, UINT64_C(5) << 26 | UINT64_C(2) << 20));
    mip = port_gs_read_register(0x34);
    assert((mip & 16383) == 8);
    assert(((mip >> 20) & 16383) == 10);
    assert(((mip >> 40) & 16383) == 11);
    assert(!port_gs_write_register(6, UINT64_C(4) << 26));
    assert(port_gs_read_register(0x34) == mip);
    assert(!port_gs_write_register(6, UINT64_C(10) << 26));
    assert(port_gs_read_register(0x34) == mip);
    assert(!port_gs_write_register(6, UINT64_C(5) << 26 | UINT64_C(48) << 20));
    assert(port_gs_read_register(0x34) == mip);
    assert(!port_gs_write_register(6, UINT64_C(5) << 26 | UINT64_C(10) << 20));
    assert((port_gs_read_register(0x34) & 16383) == 8);
}

static void sprite(void) {
    port_gs_reset();
    assert(!port_gs_write_register(0x4c, UINT64_C(1) << 16));
    assert(!port_gs_write_register(0x4e, UINT64_C(1) << 32));
    assert(!port_gs_write_register(0x40, UINT64_C(15) << 16 | UINT64_C(15) << 48));
    assert(!port_gs_write_register(0x46, 1));
    assert(!port_gs_write_register(0, 6));
    assert(!port_gs_write_register(1, UINT64_C(0x3f80000080402010)));
    assert(!port_gs_write_register(5, 16 | UINT64_C(32) << 16));
    assert(!port_gs_write_register(5, 64 | UINT64_C(80) << 16));
    assert(!port_gs_sync());
    assert(port_gs_memory_read(0, 1, 0, 1, 2) == UINT32_C(0x80402010));
    assert(port_gs_memory_read(0, 1, 0, 3, 4) == UINT32_C(0x80402010));
    assert(!port_gs_memory_read(0, 1, 0, 4, 4));
    uint32_t rgba[16 * 16];
    assert(!port_gs_read_rgba(0, 1, 0, 0, 0, 16, 16, rgba, 16));
    assert(rgba[2 * 16 + 1] == UINT32_C(0xff402010));
    assert(!port_gs_write_register(0x4e, 128));
    assert(!port_gs_write_register(0x47, UINT64_C(3) << 16));
    assert(!port_gs_write_register(0, 0));
    assert(!port_gs_write_register(5, UINT64_C(0xfffffffe) << 32));
    assert(!port_gs_sync());
    assert(port_gs_memory_read(128 * 32, 1, 48, 0, 0) == UINT32_C(0xfffffffe));
}

static void antialiased_strip(void) {
    port_gs_reset();
    assert(!port_gs_write_register(0x4c, UINT64_C(1) << 16));
    assert(!port_gs_write_register(0x4e, 128 | UINT64_C(1) << 32));
    assert(!port_gs_write_register(0x40, UINT64_C(15) << 16 | UINT64_C(15) << 48));
    assert(!port_gs_write_register(0x46, 1));
    assert(!port_gs_write_register(0x42, 0x44));
    assert(!port_gs_write_register(0, 0x1cc));
    assert(!port_gs_write_register(1, UINT64_C(0x3f80000080000080)));
    assert(!port_gs_write_register(5, 32 | UINT64_C(32) << 16));
    assert(!port_gs_write_register(5, 160 | UINT64_C(32) << 16));
    assert(!port_gs_write_register(5, 32 | UINT64_C(160) << 16));
    assert(!port_gs_sync());
    assert(port_gs_memory_read(0, 1, 0, 4, 1) == UINT32_C(0x40000040));
    assert(port_gs_memory_read(0, 1, 0, 3, 3) == UINT32_C(0x80000080));
}

int main(void) {
    port_gs_reset();
    image_roundtrip(0, 32);
    image_roundtrip(1, 24);
    image_roundtrip(2, 16);
    image_roundtrip(10, 16);
    image_roundtrip(19, 8);
    image_roundtrip(20, 4);
    image_roundtrip(27, 8);
    image_roundtrip(36, 4);
    image_roundtrip(44, 4);
    image_roundtrip(48, 32);
    image_roundtrip(49, 24);
    image_roundtrip(50, 16);
    image_roundtrip(58, 16);
    gif_formats();
    gif_image();
    local_transfer();
    automatic_mipmaps();
    sprite();
    antialiased_strip();
    puts("gs: passed");
    return 0;
}
