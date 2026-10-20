#include "dma_chain.h"
#include "vif.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern int port_gif_feed(const void *data, size_t bytes, unsigned path);

static struct {
    uint32_t address;
    const unsigned char *data;
    size_t size;
} regions[128];
static size_t region_count;
static struct {
    uintptr_t start;
    uintptr_t end;
} readable[1024];
static size_t readable_count;
static const char *error;

static int fail(const char *message) {
    error = message;
    return -1;
}

const char *port_dma_error(void) {
    return error ? error : "no error";
}

int port_dma_map(uint32_t address, const void *data, size_t size) {
    if (!data || !size || region_count == sizeof(regions) / sizeof(regions[0]) || size > UINT32_MAX - address) return fail("invalid DMA mapping");
    regions[region_count].address = address;
    regions[region_count].data = data;
    regions[region_count].size = size;
    region_count++;
    return 0;
}

void port_dma_unmap(void) {
    region_count = 0;
}

static int load_maps(void) {
    FILE *file = fopen("/proc/self/maps", "r");
    if (!file) return fail("cannot read DMA memory map");
    char line[512];
    readable_count = 0;
    while (fgets(line, sizeof(line), file)) {
        unsigned long start, end;
        char permissions[5];
        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) != 3 || permissions[0] != 'r') continue;
        if (readable_count == sizeof(readable) / sizeof(readable[0])) {
            fclose(file);
            return fail("DMA memory map exceeded");
        }
        if (readable_count && readable[readable_count - 1].end == start) readable[readable_count - 1].end = end;
        else {
            readable[readable_count].start = start;
            readable[readable_count].end = end;
            readable_count++;
        }
    }
    fclose(file);
    return 0;
}

static const unsigned char *resolve(uintptr_t address, size_t bytes) {
    if (address <= UINT32_MAX) {
        uint32_t physical = (uint32_t)address;
        if (physical & 0x80000000) physical = 0x70000000 | (physical & 0x3fff);
        else if (physical < 0x70000000) physical &= 0x0fffffff;
        for (size_t i = 0; i < region_count; i++) {
            if (physical >= regions[i].address && physical - regions[i].address <= regions[i].size && bytes <= regions[i].size - (physical - regions[i].address)) return regions[i].data + physical - regions[i].address;
        }
        address = physical;
    }
    for (size_t i = 0; i < readable_count; i++) {
        if (address >= readable[i].start && address <= readable[i].end && bytes <= readable[i].end - address) return (const unsigned char *)address;
    }
    fail("DMA address outside readable memory");
    return 0;
}

static uint32_t word(const unsigned char *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static int feed(unsigned channel, const void *data, size_t bytes) {
    if (!bytes) return 0;
    if (channel == 1) return port_vif_feed(data, bytes) ? fail(port_vif_error()) : 0;
    if (channel == 2) return port_gif_feed(data, bytes, 3) ? fail("DMA GIF stream failed") : 0;
    return fail("unsupported DMA channel");
}

int port_dma_send(unsigned channel, const void *data, size_t qwords) {
    error = 0;
    if (channel != 1 && channel != 2) return fail("unsupported DMA channel");
    if (qwords > SIZE_MAX / 16 || load_maps()) return -1;
    const unsigned char *source = resolve((uintptr_t)data, qwords * 16);
    if (!source) return -1;
    if (feed(channel, source, qwords * 16)) return -1;
    return channel == 1 && port_vif_finish() ? fail(port_vif_error()) : 0;
}

int port_dma_chain(unsigned channel, const void *tag, int transfer_tag) {
    error = 0;
    if (channel != 1 && channel != 2) return fail("unsupported DMA channel");
    if (load_maps()) return -1;
    uintptr_t position = (uintptr_t)tag;
    uintptr_t stack[2];
    unsigned depth = 0;
    size_t total = 0;
    for (unsigned step = 0; step < 1048576; step++) {
        if (position & 15) return fail("unaligned DMA tag");
        const unsigned char *header = resolve(position, 16);
        if (!header) return -1;
        uint32_t control = word(header);
        uint32_t address = word(header + 4);
        unsigned id = (control >> 28) & 7;
        size_t bytes = (control & 65535) * 16;
        if (bytes > 256 * 1024 * 1024 - total) return fail("DMA transfer budget exceeded");
        total += bytes;
        if (transfer_tag && channel == 1 && feed(channel, header + 8, 8)) return -1;
        uintptr_t source = id == 0 || id == 3 || id == 4 ? address : position + 16;
        const unsigned char *payload = bytes ? resolve(source, bytes) : header;
        if (!payload || feed(channel, payload, bytes)) return -1;
        uintptr_t next = position + 16 + bytes;
        switch (id) {
            case 0: case 7:
                return channel == 1 && port_vif_finish() ? fail(port_vif_error()) : 0;
            case 1: position = next; break;
            case 2: position = address; break;
            case 3: case 4: position += 16; break;
            case 5:
                if (depth == 2) return fail("DMA CALL stack overflow");
                stack[depth++] = next;
                position = address;
                break;
            case 6:
                if (!depth) return channel == 1 && port_vif_finish() ? fail(port_vif_error()) : 0;
                position = stack[--depth];
                break;
        }
    }
    return fail("DMA chain budget exceeded");
}
