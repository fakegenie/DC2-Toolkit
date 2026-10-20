#include "gs_clut.h"
#include "gs_memory.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint64_t texture(unsigned psm, unsigned cpsm, unsigned base, unsigned csa, unsigned csm, unsigned cld) {
    return (uint64_t)psm << 20 | (uint64_t)cpsm << 51 | (uint64_t)base << 37
        | (uint64_t)csa << 56 | (uint64_t)csm << 55 | (uint64_t)cld << 61;
}

static void raw(unsigned address, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) port_gs_memory_write_byte(address + i, (uint8_t)(value >> (8 * i)));
}

static void csm1(void) {
    uint64_t regs[128] = {0};
    const unsigned indices[] = {1, 8, 16, 32, 128, 255};
    const unsigned words[] = {1, 2, 64, 16, 128, 255};
    const unsigned halves[] = {2, 4, 1, 32, 128, 255};
    const unsigned formats[] = {0, 2, 10};
    for (unsigned f = 0; f < 3; f++) {
        port_gs_memory_reset();
        port_gs_clut_reset();
        unsigned cpsm = formats[f];
        regs[6] = texture(0x13, cpsm, 0, 0, 0, 1);
        for (unsigned i = 0; i < 6; i++) {
            if (cpsm == 0) raw(words[i] * 4, UINT32_C(0x80400000) + indices[i], 4);
            else raw(halves[i] * 2, 0x8000 + indices[i], 2);
        }
        assert(port_gs_clut_load(regs, 0) == 1);
        for (unsigned i = 0; i < 6; i++) {
            assert(port_gs_clut_read(regs, 0, indices[i]) == (cpsm == 0 ? UINT32_C(0x80400000) : 0x8000) + indices[i]);
        }
        port_gs_memory_reset();
        assert(port_gs_clut_read(regs, 0, 128) == (cpsm == 0 ? UINT32_C(0x80400000) : 0x8000) + 128);
    }
}

static void offsets(void) {
    uint64_t regs[128] = {0};
    port_gs_memory_reset();
    port_gs_clut_reset();
    raw(4, UINT32_C(0x81436587), 4);
    regs[6] = texture(0x14, 0, 0, 3, 0, 1);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_read(regs, 0, 1) == UINT32_C(0x81436587));
    raw(4, UINT32_C(0x826486a8), 4);
    regs[6] = texture(0x14, 0, 0, 5, 0, 1);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_read(regs, 0, 1) == UINT32_C(0x826486a8));
    regs[6] = texture(0x14, 0, 0, 3, 0, 0);
    assert(port_gs_clut_load(regs, 0) == 0);
    assert(port_gs_clut_read(regs, 0, 1) == UINT32_C(0x81436587));
    port_gs_clut_reset();
    raw(4, 0x8421, 2);
    regs[6] = texture(0x13, 2, 0, 31, 0, 1);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_read(regs, 0, 1) == 0x8421);
    port_gs_clut_reset();
    raw(64 * 4, UINT32_C(0x87654321), 4);
    raw(240 * 4, UINT32_C(0xabcdef12), 4);
    regs[6] = texture(0x13, 0, 0, 1, 0, 1);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_read(regs, 0, 0) == UINT32_C(0x87654321));
    assert(port_gs_clut_read(regs, 0, 240) == UINT32_C(0xabcdef12));
    regs[6] = texture(0x14, 0, 0, 0, 1, 0);
    assert(port_gs_clut_load(regs, 0) == 0);
    assert(port_gs_clut_read(regs, 0, 8) == UINT32_C(0x87654321));
}

static void csm2(void) {
    uint64_t regs[128] = {0};
    const unsigned formats[] = {0, 2, 10};
    for (unsigned f = 0; f < 3; f++) {
        unsigned cpsm = formats[f];
        port_gs_clut_reset();
        port_gs_memory_reset();
        regs[0x1c] = UINT64_C(5) | UINT64_C(3) << 6 | UINT64_C(17) << 12;
        regs[7] = texture(0x13, cpsm, 16, 0, 1, 1);
        for (unsigned i = 0; i < 256; i++) {
            port_gs_memory_write(16, 5, cpsm, 48 + i, 17, 0x8000 + i);
        }
        assert(port_gs_clut_load(regs, 1) == 1);
        for (unsigned i = 0; i < 256; i++) assert(port_gs_clut_read(regs, 1, i) == 0x8000 + i);
        regs[7] = texture(0x14, cpsm, 16, 7, 1, 1);
        assert(port_gs_clut_load(regs, 1) == 1);
        for (unsigned i = 0; i < 16; i++) assert(port_gs_clut_read(regs, 1, i) == 0x8000 + i);
    }
}

static void commands(void) {
    uint64_t regs[128] = {0};
    port_gs_clut_reset();
    port_gs_memory_reset();
    regs[6] = texture(0x14, 0, 0, 0, 0, 4);
    assert(port_gs_clut_load(regs, 0) == 0);
    regs[6] = texture(0x14, 0, 1, 0, 0, 4);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_load(regs, 0) == 0);
    regs[6] = texture(0x14, 0, 2, 0, 0, 5);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_load(regs, 0) == 0);
    regs[6] = texture(0x14, 0, 3, 0, 0, 2);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_load(regs, 0) == 1);
    regs[6] = texture(0x14, 5, 0, 0, 0, 1);
    assert(port_gs_clut_load(regs, 0) == -1);
    regs[6] = texture(0x14, 0, 3, 0, 0, 4);
    assert(port_gs_clut_load(regs, 0) == 0);
    regs[7] = texture(0x14, 0, 3, 0, 0, 4);
    assert(port_gs_clut_load(regs, 1) == 0);
    regs[6] = texture(0x14, 0, 4, 0, 0, 3);
    assert(port_gs_clut_load(regs, 0) == 1);
    regs[6] = texture(0x14, 0, 4, 0, 0, 5);
    assert(port_gs_clut_load(regs, 0) == 0);
    regs[6] = texture(0x00, 0, 5, 0, 0, 2);
    assert(port_gs_clut_load(regs, 0) == 0);
    regs[6] = texture(0x14, 0, 5, 0, 0, 4);
    assert(port_gs_clut_load(regs, 0) == 1);
    for (unsigned cld = 6; cld < 8; cld++) {
        regs[6] = texture(0x14, 0, 0, 0, 0, cld);
        assert(port_gs_clut_load(regs, 0) == -1);
    }
    regs[6] = texture(0x14, 0, 0, 0, 0, 1);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_load(regs, 0) == 1);
    assert(port_gs_clut_load(regs, 2) == -1);
    assert(port_gs_clut_read(regs, 2, 0) == 0);
}

int main(void) {
    csm1();
    offsets();
    csm2();
    commands();
    return 0;
}
