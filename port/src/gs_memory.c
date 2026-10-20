#include "gs_memory.h"
#include <string.h>

static uint8_t memory[4 * 1024 * 1024];

static const uint16_t address32_x[64] = {
    0, 4, 16, 20, 32, 36, 48, 52,
    256, 260, 272, 276, 288, 292, 304, 308,
    1024, 1028, 1040, 1044, 1056, 1060, 1072, 1076,
    1280, 1284, 1296, 1300, 1312, 1316, 1328, 1332,
    4096, 4100, 4112, 4116, 4128, 4132, 4144, 4148,
    4352, 4356, 4368, 4372, 4384, 4388, 4400, 4404,
    5120, 5124, 5136, 5140, 5152, 5156, 5168, 5172,
    5376, 5380, 5392, 5396, 5408, 5412, 5424, 5428
};

static const uint16_t address32_y[32] = {
    0, 8, 64, 72, 128, 136, 192, 200,
    512, 520, 576, 584, 640, 648, 704, 712,
    2048, 2056, 2112, 2120, 2176, 2184, 2240, 2248,
    2560, 2568, 2624, 2632, 2688, 2696, 2752, 2760
};

static unsigned address32(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y) {
    unsigned page = base256 + ((x >> 6) + (y >> 5) * width64) * 32;
    unsigned address = (page << 8) + address32_x[x & 63] + address32_y[y & 31];
    if (psm & 0x30) address ^= 0x1800;
    return address & 0x3fffff;
}

static unsigned address8(unsigned base256, unsigned width64, unsigned x, unsigned y) {
    unsigned bx = (x >> 4) & 7;
    unsigned by = (y >> 4) & 3;
    unsigned block = (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2);
    block += base256 + ((x >> 7) + (y >> 6) * (width64 >> 1)) * 32;
    unsigned column = ((y & 2) >> 1) | ((x & 8) >> 2) | ((x & 1) << 2) | ((y & 1) << 3) | ((x & 2) << 3)
        | (((x ^ y ^ (y << 1)) & 4) << 3) | ((y & 12) << 4);
    return ((block << 8) + column) & 0x3fffff;
}

static unsigned address4(unsigned base256, unsigned width64, unsigned x, unsigned y) {
    unsigned bx = (x >> 5) & 3;
    unsigned by = (y >> 4) & 7;
    unsigned block = (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2);
    block += base256 + ((x >> 7) + (y >> 7) * (width64 >> 1)) * 32;
    unsigned column = ((y & 2) >> 1) | ((x & 24) >> 2) | ((x & 1) << 3) | ((y & 1) << 4) | ((x & 2) << 4)
        | (((x ^ y ^ (y << 1)) & 4) << 4) | ((y & 12) << 5);
    return ((block << 9) + column) & 0x7fffff;
}

typedef struct {
    unsigned address;
    unsigned shift;
    unsigned bits;
} Pixel;

static Pixel pixel(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y) {
    unsigned bits = 32;
    unsigned storage = 32;
    unsigned shift = 0;
    unsigned page_width = 64;
    unsigned page_height = 32;
    unsigned block_width = 8;
    unsigned block_height = 8;
    switch (psm) {
        case 0x00: case 0x30: break;
        case 0x01: case 0x31: bits = 24; break;
        case 0x02: case 0x0a: case 0x32: case 0x3a:
            bits = storage = 16;
            page_height = 64;
            block_width = 16;
            break;
        case 0x13:
            bits = storage = 8;
            page_width = 128;
            page_height = 64;
            block_width = block_height = 16;
            break;
        case 0x14:
            bits = storage = 4;
            page_width = page_height = 128;
            block_width = 32;
            block_height = 16;
            break;
        case 0x1b: bits = 8; shift = 24; break;
        case 0x24: bits = 4; shift = 24; break;
        case 0x2c: bits = 4; shift = 28; break;
        default: return (Pixel){0};
    }
    unsigned page = x / page_width + y / page_height * (width64 / (page_width / 64));
    unsigned bx = x % page_width / block_width;
    unsigned by = y % page_height / block_height;
    unsigned block;
    if (storage == 32 || storage == 8) {
        block = (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2);
    } else if (psm == 0x0a || psm == 0x3a) {
        block = (by & 1) | ((bx & 1) << 1) | (by & 4) | ((by & 2) << 2) | ((bx & 2) << 3);
    } else {
        block = (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2);
    }
    block += base256 + page * 32;
    if (psm == 0x30 || psm == 0x31 || psm == 0x32 || psm == 0x3a) block ^= 0x18;
    unsigned column;
    if (storage == 32) {
        column = (x & 1) | ((y & 1) << 1) | ((x & 6) << 1) | ((y & 6) << 3);
    } else if (storage == 16) {
        column = ((x & 8) >> 3) | ((x & 1) << 1) | ((y & 1) << 2) | ((x & 6) << 2) | ((y & 6) << 4);
    } else if (storage == 8) {
        column = ((y & 2) >> 1) | ((x & 8) >> 2) | ((x & 1) << 2) | ((y & 1) << 3) | ((x & 2) << 3)
            | (((x ^ y ^ (y << 1)) & 4) << 3) | ((y & 12) << 4);
    } else {
        column = ((y & 2) >> 1) | ((x & 24) >> 2) | ((x & 1) << 3) | ((y & 1) << 4) | ((x & 2) << 4)
            | (((x ^ y ^ (y << 1)) & 4) << 4) | ((y & 12) << 5);
    }
    unsigned address = (block * 2048 + column * storage + shift) & 0x1ffffff;
    return (Pixel){address >> 3, address & 7, bits};
}

void port_gs_memory_reset(void) {
    memset(memory, 0, sizeof(memory));
}

uint8_t port_gs_memory_read_byte(unsigned address) {
    return memory[address & 0x3fffff];
}

void port_gs_memory_write_byte(unsigned address, uint8_t value) {
    memory[address & 0x3fffff] = value;
}

uint32_t port_gs_memory_read(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y) {
    if (psm == 27 || psm == 36 || psm == 44) {
        unsigned value = memory[address32(base256, width64, 0, x, y) + 3];
        return psm == 27 ? value : (value >> (psm == 44 ? 4 : 0)) & 15;
    }
    if (psm == 19) return memory[address8(base256, width64, x, y)];
    if (psm == 20) {
        unsigned address = address4(base256, width64, x, y);
        return (memory[address >> 1] >> ((address & 1) * 4)) & 15;
    }
    if (psm == 0 || psm == 1 || psm == 48 || psm == 49) {
        uint32_t value;
        memcpy(&value, memory + address32(base256, width64, psm, x, y), sizeof(value));
        return psm & 1 ? value & 0xffffff : value;
    }
    Pixel location = pixel(base256, width64, psm, x, y);
    uint32_t value = 0;
    for (unsigned i = 0; i < location.bits; i += 8) {
        value |= (uint32_t)port_gs_memory_read_byte(location.address + i / 8) << i;
    }
    value >>= location.shift;
    if (location.bits < 32) value &= (UINT32_C(1) << location.bits) - 1;
    return value;
}

void port_gs_memory_write(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y, uint32_t value) {
    if (psm == 27 || psm == 36 || psm == 44) {
        unsigned address = address32(base256, width64, 0, x, y) + 3;
        unsigned shift = psm == 44 ? 4 : 0;
        unsigned mask = psm == 27 ? 255 : 15;
        memory[address] = (uint8_t)((memory[address] & ~(mask << shift)) | ((value & mask) << shift));
        return;
    }
    if (psm == 19) {
        memory[address8(base256, width64, x, y)] = (uint8_t)value;
        return;
    }
    if (psm == 20) {
        unsigned address = address4(base256, width64, x, y);
        unsigned shift = (address & 1) * 4;
        memory[address >> 1] = (memory[address >> 1] & ~(15u << shift)) | ((value & 15) << shift);
        return;
    }
    if (psm == 0 || psm == 1 || psm == 48 || psm == 49) {
        unsigned address = address32(base256, width64, psm, x, y);
        if (psm & 1) {
            memcpy(memory + address, &value, 3);
        } else memcpy(memory + address, &value, sizeof(value));
        return;
    }
    Pixel location = pixel(base256, width64, psm, x, y);
    if (location.bits == 4) {
        uint8_t old = port_gs_memory_read_byte(location.address);
        port_gs_memory_write_byte(location.address, (old & ~(15u << location.shift)) | ((value & 15) << location.shift));
        return;
    }
    for (unsigned i = 0; i < location.bits; i += 8) {
        port_gs_memory_write_byte(location.address + i / 8, (uint8_t)(value >> i));
    }
}
