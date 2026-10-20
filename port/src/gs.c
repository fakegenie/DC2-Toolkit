#include "gs.h"
#include "gs_memory.h"
#include "gs_clut.h"
#include "gs_raster.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint64_t registers[128];
static PortGsVertex vertex;
static PortGsVertex vertices[3];
static unsigned vertex_count;
static unsigned strip_count;
static char error_text[320];
static uint64_t transfer_bits;
static unsigned transfer_bit_count;
static unsigned transfer_pixel;

typedef struct {
    uint64_t descriptors;
    unsigned remaining;
    unsigned nreg;
    unsigned reg;
    unsigned mode;
    unsigned char pending[16];
    unsigned pending_size;
} GifPath;

static GifPath paths[3];

static int fail(const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(error_text)) length = sizeof(error_text) - 1;
    memcpy(error_text, message, length);
    error_text[length] = 0;
    return -1;
}

const char *port_gs_error(void) {
    return error_text;
}

static uint64_t read64(const void *source) {
    uint64_t value;
    memcpy(&value, source, sizeof(value));
    return value;
}

static float read_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static unsigned pixel_bits(unsigned psm) {
    switch (psm) {
        case 0: case 48: return 32;
        case 1: case 49: return 24;
        case 2: case 10: case 50: case 58: return 16;
        case 19: case 27: return 8;
        case 20: case 36: case 44: return 4;
        default: return 0;
    }
}

void port_gs_reset(void) {
    port_gs_raster_reset();
    memset(registers, 0, sizeof(registers));
    memset(paths, 0, sizeof(paths));
    memset(&vertex, 0, sizeof(vertex));
    memset(vertices, 0, sizeof(vertices));
    registers[0x1a] = 1;
    vertex.q = 1;
    vertex.rgba = UINT32_C(0x80808080);
    vertex_count = 0;
    strip_count = 0;
    transfer_bits = 0;
    transfer_bit_count = 0;
    transfer_pixel = 0;
    error_text[0] = 0;
    port_gs_memory_reset();
    port_gs_clut_reset();
}

int port_gs_sync(void) {
    return port_gs_raster_flush() ? fail("GS pending raster draw failed") : 0;
}

uint64_t port_gs_read_register(unsigned address) {
    return address < 128 ? registers[address] : 0;
}

static unsigned primitive(void) {
    unsigned value = (unsigned)registers[0];
    if (!(registers[0x1a] & 1)) value = (value & 7) | ((unsigned)registers[0x1b] & 0x7f8);
    return value;
}

static int mipmap_addresses(unsigned context) {
    if (!(registers[0x14 + context] & 512)) return 0;
    uint64_t tex = registers[6 + context];
    unsigned power = (tex >> 26) & 15;
    unsigned psm = (tex >> 20) & 63;
    unsigned bits = pixel_bits(psm);
    if (psm == 1 || psm == 27 || psm == 36 || psm == 44) bits = 32;
    if (power < 5 || power > (bits == 32 ? 9u : 10u) || psm >= 48) return 0;
    if (!bits) {
        snprintf(error_text, sizeof(error_text),
                 "GS automatic mipmap pixel format context=%u TEX0=%016" PRIx64 " TEX1=%016" PRIx64 " PSM=%u TW=%u",
                 context, tex, registers[0x14 + context], psm, power);
        return -1;
    }
    unsigned base = tex & 0x3fff;
    unsigned side = 1u << power;
    uint64_t mip = 0;
    for (unsigned level = 0; level < 3; level++) {
        unsigned blocks = (side * side * bits + 2047) / 2048;
        base = (base + blocks) & 0x3fff;
        side /= 2;
        unsigned width = side / 64;
        if (!width) width = 1;
        mip |= ((uint64_t)base | (uint64_t)width << 14) << (level * 20);
    }
    registers[0x34 + context] = mip;
    return 0;
}

static int kick(unsigned draw) {
    unsigned prim = primitive();
    unsigned type = prim & 7;
    unsigned needed = type == 0 ? 1 : type == 1 || type == 2 || type == 6 ? 2 : 3;
    if (type == 7) return fail("GS primitive type 7 is reserved");
    vertices[vertex_count++] = vertex;
    if (vertex_count < needed) return 0;
    PortGsVertex render[3];
    memcpy(render, vertices, sizeof(render));
    if (type == 4 && (strip_count & 1)) {
        render[0] = vertices[1];
        render[1] = vertices[0];
    }
    unsigned raster_type = type == 2 ? 1 : type == 4 || type == 5 ? 3 : type;
    int result = draw ? port_gs_raster_enqueue(registers, (prim & ~7u) | raster_type, render, needed) : 0;
    if (type == 2) {
        vertices[0] = vertices[1];
        vertex_count = 1;
    } else if (type == 4) {
        vertices[0] = vertices[1];
        vertices[1] = vertices[2];
        vertex_count = 2;
        strip_count++;
    } else if (type == 5) {
        vertices[1] = vertices[2];
        vertex_count = 2;
    } else vertex_count = 0;
    if (result) {
        unsigned context = (prim >> 9) & 1;
        float left = render[0].x, right = left, top = render[0].y, bottom = top;
        for (unsigned i = 1; i < needed; ++i) {
            if (render[i].x < left) left = render[i].x;
            if (render[i].x > right) right = render[i].x;
            if (render[i].y < top) top = render[i].y;
            if (render[i].y > bottom) bottom = render[i].y;
        }
        float ox = (registers[0x18 + context] & 65535) / 16.0f;
        float oy = ((registers[0x18 + context] >> 32) & 65535) / 16.0f;
        snprintf(error_text, sizeof(error_text),
                 "GS raster prim=%03x FRAME=%016" PRIx64 " ZBUF=%016" PRIx64 " TEX0=%016" PRIx64 " TEX1=%016" PRIx64
                 " bounds=%.2f,%.2f..%.2f,%.2f SCISSOR=%016" PRIx64,
                 prim, registers[0x4c + context], registers[0x4e + context],
                 registers[6 + context], registers[0x14 + context], left - ox, top - oy, right - ox, bottom - oy,
                 registers[0x40 + context]);
        return -1;
    }
    return 0;
}

static unsigned transfer_total(void) {
    return (registers[0x52] & 0xfff) * ((registers[0x52] >> 32) & 0xfff);
}

static void transfer_position(unsigned index, unsigned source, unsigned *x, unsigned *y) {
    uint64_t position = registers[0x51];
    unsigned width = registers[0x52] & 0xfff;
    unsigned dx = width ? index % width : 0;
    unsigned dy = width ? index / width : 0;
    unsigned height = (registers[0x52] >> 32) & 0xfff;
    if ((registers[0x53] & 3) == 2) {
        if ((position >> 60) & 1) dx = width - 1 - dx;
        if ((position >> 59) & 1) dy = height - 1 - dy;
    }
    unsigned shift = source ? 0 : 32;
    *x = (((unsigned)(position >> shift) & 0x7ff) + dx) & 0x7ff;
    *y = (((unsigned)(position >> (shift + 16)) & 0x7ff) + dy) & 0x7ff;
}

static int transfer_local(void) {
    uint64_t bitblt = registers[0x50];
    unsigned spsm = (bitblt >> 24) & 63;
    unsigned dpsm = (bitblt >> 56) & 63;
    if (!pixel_bits(spsm) || !pixel_bits(dpsm) || pixel_bits(spsm) != pixel_bits(dpsm))
        return fail("GS local transfer requires compatible pixel formats");
    unsigned total = transfer_total();
    for (unsigned i = 0; i < total; i++) {
        unsigned sx, sy, dx, dy;
        transfer_position(i, 1, &sx, &sy);
        transfer_position(i, 0, &dx, &dy);
        uint32_t pixel = port_gs_memory_read(bitblt & 0x3fff, (bitblt >> 16) & 63, spsm, sx, sy);
        port_gs_memory_write((bitblt >> 32) & 0x3fff, (bitblt >> 48) & 63, dpsm, dx, dy, pixel);
    }
    transfer_pixel = total;
    return 0;
}

static int transfer_input(const unsigned char *data, size_t size) {
    if (port_gs_sync()) return -1;
    uint64_t bitblt = registers[0x50];
    unsigned psm = (bitblt >> 56) & 63;
    unsigned bits = pixel_bits(psm);
    if ((registers[0x53] & 3) != 0) return fail("GS IMAGE data requires host-to-local TRXDIR");
    if (!bits) return fail("GS IMAGE transfer has unsupported pixel format");
    unsigned total = transfer_total();
    for (size_t i = 0; i < size && transfer_pixel < total; i++) {
        transfer_bits |= (uint64_t)data[i] << transfer_bit_count;
        transfer_bit_count += 8;
        while (transfer_bit_count >= bits && transfer_pixel < total) {
            unsigned x, y;
            transfer_position(transfer_pixel++, 0, &x, &y);
            uint32_t value = (uint32_t)transfer_bits;
            if (bits < 32) value &= (UINT32_C(1) << bits) - 1;
            port_gs_memory_write((bitblt >> 32) & 0x3fff, (bitblt >> 48) & 63, psm, x, y, value);
            transfer_bits >>= bits;
            transfer_bit_count -= bits;
        }
    }
    return 0;
}

int port_gs_write_register(unsigned address, uint64_t value) {
    if (address >= 128) return fail("GS register address exceeds 0x7f");
    if (address >= 0x50 || ((address == 6 || address == 7 || address == 0x16 || address == 0x17) && (value >> 61))) {
        if (port_gs_sync()) return -1;
    }
    registers[address] = value;
    switch (address) {
        case 0:
            vertex_count = 0;
            strip_count = 0;
            break;
        case 1:
            vertex.rgba = (uint32_t)value;
            vertex.q = read_float((uint32_t)(value >> 32));
            break;
        case 2:
            vertex.s = read_float((uint32_t)value);
            vertex.t = read_float((uint32_t)(value >> 32));
            break;
        case 3:
            vertex.u = value & 0x3fff;
            vertex.v = (value >> 16) & 0x3fff;
            break;
        case 4: case 12:
            vertex.x = (float)(value & 0xffff) / 16;
            vertex.y = (float)((value >> 16) & 0xffff) / 16;
            vertex.z = (double)((value >> 32) & 0xffffff);
            vertex.fog = value >> 56;
            return kick(address == 4);
        case 5: case 13:
            vertex.x = (float)(value & 0xffff) / 16;
            vertex.y = (float)((value >> 16) & 0xffff) / 16;
            vertex.z = (double)(uint32_t)(value >> 32);
            return kick(address == 5);
        case 10:
            vertex.fog = value >> 56;
            break;
        case 0x16: case 0x17: {
            uint64_t mask = UINT64_C(0xffffffe003f00000);
            unsigned tex0 = address == 0x16 ? 6 : 7;
            registers[tex0] = (registers[tex0] & ~mask) | (value & mask);
            if (port_gs_clut_load(registers, tex0 - 6) < 0) return fail("GS palette load has unsupported CLUT mode");
            break;
        }
        case 6: case 7:
            if (mipmap_addresses(address - 6)) return -1;
            if (port_gs_clut_load(registers, address - 6) < 0) return fail("GS palette load has unsupported CLUT mode");
            break;
        case 0x53:
            transfer_pixel = 0;
            transfer_bits = 0;
            transfer_bit_count = 0;
            if ((value & 3) == 2) return transfer_local();
            break;
        case 0x54: {
            unsigned char bytes[8];
            memcpy(bytes, &value, 8);
            return transfer_input(bytes, 8);
        }
        default: break;
    }
    return 0;
}

static int packed(unsigned address, const unsigned char *data) {
    uint64_t lo = read64(data);
    uint64_t hi = read64(data + 8);
    uint32_t words[4];
    memcpy(words, data, 16);
    switch (address) {
        case 0: return port_gs_write_register(0, lo & 0x7ff);
        case 1:
            vertex.rgba = (words[0] & 255) | (words[1] & 255) << 8 |
                          (words[2] & 255) << 16 | (words[3] & 255) << 24;
            registers[1] = vertex.rgba;
            return 0;
        case 2:
            vertex.s = read_float(words[0]);
            vertex.t = read_float(words[1]);
            vertex.q = read_float(words[2]);
            return 0;
        case 3:
            vertex.u = words[0] & 0x3fff;
            vertex.v = words[1] & 0x3fff;
            return 0;
        case 4: case 5: case 12: case 13:
            vertex.x = (float)(words[0] & 0xffff) / 16;
            vertex.y = (float)(words[1] & 0xffff) / 16;
            vertex.z = (double)(address == 4 || address == 12 ? (words[2] >> 4) & 0xffffff : words[2]);
            if (address == 4 || address == 12) vertex.fog = (words[3] >> 4) & 255;
            return kick(address < 8 && !(words[3] & 0x8000));
        case 10:
            vertex.fog = (words[3] >> 4) & 255;
            return 0;
        case 14: return port_gs_write_register(hi & 255, lo);
        case 15: return 0;
        default: return port_gs_write_register(address, lo);
    }
}

static int gif_qword(GifPath *path, const unsigned char *data) {
    if (!path->remaining) {
        uint64_t tag = read64(data);
        path->descriptors = read64(data + 8);
        path->nreg = (tag >> 60) & 15;
        if (!path->nreg) path->nreg = 16;
        path->reg = 0;
        path->mode = (tag >> 58) & 3;
        path->remaining = (tag & 0x7fff) * (path->mode < 2 ? path->nreg : 1);
        vertex.q = 1;
        if (path->mode == 0 && ((tag >> 46) & 1)) return port_gs_write_register(0, (tag >> 47) & 0x7ff);
        return 0;
    }
    if (path->mode >= 2) {
        path->remaining--;
        return transfer_input(data, 16);
    }
    unsigned count = path->mode == 1 ? 2 : 1;
    for (unsigned i = 0; i < count && path->remaining; i++) {
        unsigned address = (path->descriptors >> (path->reg * 4)) & 15;
        int result = path->mode == 1 ? address == 15 ? 0 : port_gs_write_register(address, read64(data + i * 8)) : packed(address, data);
        if (result) return result;
        path->remaining--;
        path->reg = (path->reg + 1) % path->nreg;
    }
    return 0;
}

int port_gif_feed(const void *data, size_t bytes, unsigned path) {
    if (path < 1 || path > 3) return fail("GIF path must be 1, 2 or 3");
    if (bytes && !data) return fail("GIF stream has null data");
    GifPath *state = &paths[path - 1];
    const unsigned char *source = data;
    while (bytes) {
        unsigned take = 16 - state->pending_size;
        if (take > bytes) take = (unsigned)bytes;
        memcpy(state->pending + state->pending_size, source, take);
        state->pending_size += take;
        source += take;
        bytes -= take;
        if (state->pending_size == 16) {
            state->pending_size = 0;
            if (gif_qword(state, state->pending)) return -1;
        }
    }
    return 0;
}

int port_gif_submit(const void *data, size_t bytes) {
    return port_gif_feed(data, bytes, 3) ? -1 : port_gs_sync();
}

int port_gs_image_load(const uint64_t *packet, const void *source, size_t bytes) {
    if (!packet || (bytes && !source)) return fail("GS load image has null buffer");
    if (port_gs_write_register(0x50, packet[2]) ||
        port_gs_write_register(0x51, packet[4]) ||
        port_gs_write_register(0x52, packet[6]) ||
        port_gs_write_register(0x53, packet[8])) return -1;
    unsigned bits = pixel_bits((packet[2] >> 56) & 63);
    if (!bits || (uint64_t)bytes * 8 < (uint64_t)transfer_total() * bits)
        return fail("GS load image source is too small or has unsupported pixel format");
    return transfer_input(source, bytes);
}

int port_gs_image_store(const uint64_t *packet, void *destination, size_t bytes) {
    if (!packet || (bytes && !destination)) return fail("GS store image has null buffer");
    if (port_gs_write_register(0x50, packet[4]) ||
        port_gs_write_register(0x51, packet[6]) ||
        port_gs_write_register(0x52, packet[8]) ||
        port_gs_write_register(0x53, packet[12])) return -1;
    uint64_t bitblt = registers[0x50];
    unsigned psm = (bitblt >> 24) & 63;
    unsigned bits = pixel_bits(psm);
    unsigned total = transfer_total();
    size_t required = ((uint64_t)total * bits + 7) / 8;
    if (!bits || bytes < required) return fail("GS store image destination is too small or has unsupported pixel format");
    unsigned char *output = destination;
    memset(output, 0, required);
    uint64_t accumulator = 0;
    unsigned available = 0;
    size_t cursor = 0;
    for (unsigned i = 0; i < total; i++) {
        unsigned x, y;
        transfer_position(i, 1, &x, &y);
        uint32_t pixel = port_gs_memory_read(bitblt & 0x3fff, (bitblt >> 16) & 63, psm, x, y);
        accumulator |= (uint64_t)pixel << available;
        available += bits;
        while (available >= 8) {
            output[cursor++] = (unsigned char)accumulator;
            accumulator >>= 8;
            available -= 8;
        }
    }
    if (available) output[cursor] = (unsigned char)accumulator;
    return 0;
}

int port_gs_read_rgba(unsigned base256, unsigned width64, unsigned psm,
                      unsigned x, unsigned y, unsigned width, unsigned height,
                      uint32_t *rgba, size_t stride_pixels) {
    if (port_gs_sync()) return -1;
    if (!rgba || !width64 || width64 > 63 || stride_pixels < width || width > 2048 || height > 2048)
        return fail("GS display buffer has invalid dimensions");
    if (psm != 0 && psm != 1 && psm != 2 && psm != 10)
        return fail("GS display buffer has unsupported pixel format");
    for (unsigned row = 0; row < height; row++) {
        for (unsigned column = 0; column < width; column++) {
            uint32_t color = port_gs_memory_read(base256, width64, psm, x + column, y + row);
            if (psm == 2 || psm == 10)
                color = (color & 31) << 3 | (color & 0x3e0) << 6 | (color & 0x7c00) << 9;
            rgba[row * stride_pixels + column] = color | UINT32_C(0xff000000);
        }
    }
    return 0;
}
