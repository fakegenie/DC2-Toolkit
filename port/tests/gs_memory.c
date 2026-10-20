#include "gs_memory.h"
#include <assert.h>
#include <stdint.h>

static void addresses(void) {
    const unsigned cases[][6] = {
        {0x00, 1, 0, 4, 0, 32},
        {0x00, 0, 1, 8, 0, 32},
        {0x00, 2, 0, 16, 0, 32},
        {0x00, 0, 2, 64, 0, 32},
        {0x00, 8, 0, 256, 0, 32},
        {0x00, 0, 8, 512, 0, 32},
        {0x00, 32, 16, 6144, 0, 32},
        {0x00, 64, 0, 8192, 0, 32},
        {0x00, 0, 32, 32768, 0, 32},
        {0x01, 1, 0, 4, 0, 24},
        {0x02, 8, 0, 2, 0, 16},
        {0x02, 1, 0, 4, 0, 16},
        {0x02, 16, 0, 512, 0, 16},
        {0x02, 0, 8, 256, 0, 16},
        {0x02, 0, 32, 4096, 0, 16},
        {0x0a, 32, 0, 4096, 0, 16},
        {0x0a, 0, 16, 2048, 0, 16},
        {0x0a, 0, 32, 1024, 0, 16},
        {0x13, 1, 0, 4, 0, 8},
        {0x13, 0, 2, 33, 0, 8},
        {0x13, 0, 4, 96, 0, 8},
        {0x13, 4, 2, 1, 0, 8},
        {0x13, 16, 0, 256, 0, 8},
        {0x13, 0, 16, 512, 0, 8},
        {0x13, 128, 0, 8192, 0, 8},
        {0x13, 0, 64, 16384, 0, 8},
        {0x14, 1, 0, 4, 0, 4},
        {0x14, 0, 2, 32, 4, 4},
        {0x14, 4, 2, 0, 4, 4},
        {0x14, 8, 0, 1, 0, 4},
        {0x14, 16, 0, 2, 0, 4},
        {0x14, 32, 0, 512, 0, 4},
        {0x14, 0, 16, 256, 0, 4},
        {0x14, 0, 128, 16384, 0, 4},
        {0x1b, 1, 0, 7, 0, 8},
        {0x24, 1, 0, 7, 0, 4},
        {0x2c, 1, 0, 7, 4, 4},
        {0x30, 0, 0, 6144, 0, 32},
        {0x31, 0, 0, 6144, 0, 24},
        {0x32, 0, 0, 6144, 0, 16},
        {0x3a, 0, 0, 6144, 0, 16},
        {0x3a, 32, 0, 2048, 0, 16}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const unsigned *c = cases[i];
        uint32_t value = c[5] == 32 ? UINT32_C(0x87654321) : (UINT32_C(1) << c[5]) - 1;
        port_gs_memory_reset();
        port_gs_memory_write(0, 4, c[0], c[1], c[2], value);
        for (unsigned byte = 0; byte < (c[5] + 7) / 8; byte++) {
            assert(port_gs_memory_read_byte(c[3] + byte) == (uint8_t)((value << c[4]) >> (byte * 8)));
        }
        assert(port_gs_memory_read(0, 4, c[0], c[1], c[2]) == value);
    }
}

static void preservation(void) {
    port_gs_memory_reset();
    port_gs_memory_write(0, 1, 0, 0, 0, UINT32_C(0x12345678));
    port_gs_memory_write(0, 1, 1, 0, 0, UINT32_C(0xffabcdef));
    assert(port_gs_memory_read(0, 1, 0, 0, 0) == UINT32_C(0x12abcdef));
    port_gs_memory_write(0, 1, 0x24, 0, 0, 0xf5);
    assert(port_gs_memory_read(0, 1, 0, 0, 0) == UINT32_C(0x15abcdef));
    port_gs_memory_write(0, 1, 0x2c, 0, 0, 0xfa);
    assert(port_gs_memory_read(0, 1, 0, 0, 0) == UINT32_C(0xa5abcdef));
    assert(port_gs_memory_read(0, 1, 0x1b, 0, 0) == 0xa5);
    assert(port_gs_memory_read(0, 1, 0x24, 0, 0) == 5);
    assert(port_gs_memory_read(0, 1, 0x2c, 0, 0) == 10);
    port_gs_memory_write(0, 1, 0x1b, 0, 0, 0x876);
    assert(port_gs_memory_read(0, 1, 0, 0, 0) == UINT32_C(0x76abcdef));
    port_gs_memory_write(0, 2, 0x14, 0, 0, 3);
    port_gs_memory_write(0, 2, 0x14, 4, 2, 9);
    assert(port_gs_memory_read_byte(0) == 0x93);
}

static void pages(void) {
    const unsigned cases[][5] = {
        {0x00, 64, 32, 32, UINT32_MAX},
        {0x01, 64, 32, 24, 0xffffff},
        {0x02, 64, 64, 16, 0xffff},
        {0x0a, 64, 64, 16, 0xffff},
        {0x13, 128, 64, 8, 255},
        {0x14, 128, 128, 4, 15},
        {0x30, 64, 32, 32, UINT32_MAX},
        {0x32, 64, 64, 16, 0xffff},
        {0x3a, 64, 64, 16, 0xffff}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const unsigned *c = cases[i];
        port_gs_memory_reset();
        for (unsigned y = 0; y < c[2]; y++) {
            for (unsigned x = 0; x < c[1]; x++) port_gs_memory_write(0, 2, c[0], x, y, c[4]);
        }
        for (unsigned byte = 0; byte < 8192; byte++) {
            assert(port_gs_memory_read_byte(byte) == (c[3] == 24 && byte % 4 == 3 ? 0 : 255));
        }
        for (unsigned y = 0; y < c[2]; y++) {
            for (unsigned x = 0; x < c[1]; x++) {
                uint32_t value = (x * 23 + y * 43) & c[4];
                port_gs_memory_write(0, 2, c[0], x, y, value);
            }
        }
        for (unsigned y = 0; y < c[2]; y++) {
            for (unsigned x = 0; x < c[1]; x++) assert(port_gs_memory_read(0, 2, c[0], x, y) == ((x * 23 + y * 43) & c[4]));
        }
    }
}

static void high_formats(void) {
    const unsigned bases[] = {0, 31, 16383};
    const unsigned widths[] = {1, 4, 63};
    const unsigned formats[] = {27, 36, 44};
    for (unsigned b = 0; b < 3; b++) for (unsigned w = 0; w < 3; w++) {
        for (unsigned f = 0; f < 3; f++) for (unsigned y = 0; y < 130; y++) {
            for (unsigned x = 0; x < 130; x++) {
                unsigned format = formats[f], shift = format == 44 ? 28 : 24;
                uint32_t mask = format == 27 ? 255 : 15;
                uint32_t packed = UINT32_C(0xabcdef01) ^ x * 53 ^ y * 71;
                uint32_t value = x * 13 + y * 29;
                port_gs_memory_write(bases[b], widths[w], 0, x, y, packed);
                assert(port_gs_memory_read(bases[b], widths[w], format, x, y) == ((packed >> shift) & mask));
                port_gs_memory_write(bases[b], widths[w], format, x, y, value);
                assert(port_gs_memory_read(bases[b], widths[w], format, x, y) == (value & mask));
                assert(port_gs_memory_read(bases[b], widths[w], 0, x, y) ==
                       ((packed & ~(mask << shift)) | ((value & mask) << shift)));
            }
        }
    }
}

int main(void) {
    addresses();
    preservation();
    pages();
    high_formats();
    port_gs_memory_reset();
    port_gs_memory_write(8, 1, 0x30, 0, 0, UINT32_C(0x12345678));
    assert(port_gs_memory_read_byte(4096) == 0x78);
    port_gs_memory_write(16383, 1, 0, 8, 0, UINT32_C(0x76543210));
    assert(port_gs_memory_read_byte(0) == 0x10);
    port_gs_memory_write_byte(0x400000, 0xab);
    assert(port_gs_memory_read_byte(0) == 0xab);
    port_gs_memory_write(0, 1, 0x3f, 0, 0, UINT32_MAX);
    assert(port_gs_memory_read(0, 1, 0x3f, 0, 0) == 0);
    assert(port_gs_memory_read_byte(0) == 0xab);
    port_gs_memory_reset();
    assert(port_gs_memory_read_byte(0) == 0);
    return 0;
}
