#include "gs_clut.h"
#include "gs_memory.h"
#include <string.h>

static uint16_t cache[512];
static unsigned previous_base[2];
static unsigned loaded_csm;
static uint64_t generation;

uint64_t port_gs_clut_generation(void) {
    return generation;
}

void port_gs_clut_reset(void) {
    memset(cache, 0, sizeof(cache));
    memset(previous_base, 0, sizeof(previous_base));
    loaded_csm = 0;
    generation++;
}

int port_gs_clut_load(const uint64_t regs[128], unsigned context) {
    if (context > 1) return -1;
    uint64_t tex = regs[6 + context];
    unsigned psm = (tex >> 20) & 63;
    unsigned cpsm = (tex >> 51) & 15;
    unsigned base = (tex >> 37) & 16383;
    unsigned command = (tex >> 61) & 7;
    unsigned eight = psm == 0x13 || psm == 0x1b;
    unsigned four = psm == 0x14 || psm == 0x24 || psm == 0x2c;
    if (!eight && !four) return 0;
    if (command == 0) return 0;
    if (command >= 6) return -1;
    if (cpsm != 0 && cpsm != 1 && cpsm != 2 && cpsm != 10) return -1;
    if (command >= 4 && previous_base[command - 4] == base) return 0;
    if (command >= 2) previous_base[(command - 2) & 1] = base;
    unsigned sixteen = cpsm == 2 || cpsm == 10;
    unsigned offset = ((tex >> 56) & (sixteen ? 31 : 15)) * 16;
    unsigned csm2 = (tex >> 55) & 1;
    unsigned count = eight ? 256 : 16;
    unsigned start = !csm2 && !sixteen && eight ? offset : 0;
    for (unsigned i = start; i < count; i++) {
        unsigned x, y, width;
        unsigned destination = offset + i;
        if (csm2) {
            uint64_t clut = regs[0x1c];
            x = ((clut >> 6) & 63) * 16 + i;
            y = (clut >> 12) & 1023;
            width = clut & 63;
        } else {
            unsigned swizzled = (i & ~24u) | ((i & 8) << 1) | ((i & 16) >> 1);
            x = swizzled & 15;
            y = swizzled >> 4;
            width = 1;
            if (!sixteen && eight) destination = i;
        }
        uint32_t value = port_gs_memory_read(base, width, cpsm == 1 ? 0 : cpsm, x, y);
        cache[destination & 511] = (uint16_t)value;
        if (!sixteen) cache[(destination + 256) & 511] = (uint16_t)(value >> 16);
    }
    loaded_csm = csm2;
    generation++;
    return 1;
}

uint32_t port_gs_clut_read(const uint64_t regs[128], unsigned context, unsigned index) {
    if (context > 1) return 0;
    uint64_t tex = regs[6 + context];
    unsigned cpsm = (tex >> 51) & 15;
    unsigned psm = (tex >> 20) & 63;
    unsigned sixteen = cpsm == 2 || cpsm == 10;
    unsigned offset = ((tex >> 56) & (sixteen ? 31 : 15)) * 16;
    unsigned four = psm == 0x14 || psm == 0x24 || psm == 0x2c;
    if (four) index &= 15;
    else index &= 255;
    if (sixteen) return cache[(offset + index) & 511];
    if (four && ((tex >> 55) & 1) && !loaded_csm) {
        unsigned address = offset + (index & 7) + ((index & 8) << 1);
        return cache[address & 511] | (uint32_t)cache[(address + 256) & 511] << 16;
    }
    unsigned group = offset + (index & ~15u);
    if (group > 240) group = 240;
    unsigned address = group + (index & 15);
    return cache[address] | (uint32_t)cache[address + 256] << 16;
}
