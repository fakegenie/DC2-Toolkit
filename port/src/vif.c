#include "vif.h"
#include "vu1.h"
#include <stdint.h>
#include <string.h>

extern int port_gif_feed(const void *data, size_t bytes, unsigned path);

PortVif port_vif = {.cl = 1, .wl = 1};
static unsigned char pending[1024 * 1024 + 16];
static size_t pending_size;
static const char *error;

static uint32_t word(const unsigned char *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static int fail(const char *message) {
    error = message;
    return -1;
}

const char *port_vif_error(void) {
    return error ? error : "no error";
}

void port_vif_reset(void) {
    memset(&port_vif, 0, sizeof(port_vif));
    port_vif.cl = 1;
    port_vif.wl = 1;
    pending_size = 0;
    error = 0;
}

static size_t unpack_bytes(unsigned command, unsigned count) {
    unsigned components = ((command >> 2) & 3) + 1;
    unsigned format = command & 3;
    unsigned consumed = count;
    if (port_vif.wl > port_vif.cl) {
        consumed = count / port_vif.wl * port_vif.cl;
        unsigned tail = count % port_vif.wl;
        consumed += tail < port_vif.cl ? tail : port_vif.cl;
    }
    if (format == 3) return consumed * 2;
    return (size_t)consumed * components * (4 >> format);
}

static int unpack(uint32_t code, const unsigned char *data) {
    unsigned command = (code >> 24) & 0x7f;
    unsigned components = ((command >> 2) & 3) + 1;
    unsigned format = command & 3;
    unsigned count = (code >> 16) & 255;
    if (!count) count = 256;
    unsigned address = code & 1023;
    if (code & 0x8000) address += port_vif.tops;
    unsigned source = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned cycle = i % port_vif.wl;
        int fill = port_vif.wl > port_vif.cl && cycle >= port_vif.cl;
        uint32_t values[4];
        memcpy(values, port_vif.row, sizeof(values));
        if (!fill) {
            if (format == 3) {
                unsigned packed = data[source] | (unsigned)data[source + 1] << 8;
                source += 2;
                values[0] = (packed & 31) << 3;
                values[1] = ((packed >> 5) & 31) << 3;
                values[2] = ((packed >> 10) & 31) << 3;
                values[3] = (packed & 0x8000) >> 8;
            } else {
                unsigned bytes = 4 >> format;
                for (unsigned j = 0; j < components; j++) {
                    uint32_t value = 0;
                    for (unsigned k = 0; k < bytes; k++) value |= (uint32_t)data[source++] << (k * 8);
                    if (!(code & 0x4000) && bytes < 4) {
                        unsigned shift = 32 - bytes * 8;
                        value = (uint32_t)((int32_t)(value << shift) >> shift);
                    }
                    values[j] = value;
                }
                if (components == 1) values[1] = values[2] = values[3] = values[0];
                if (components == 2) { values[2] = values[0]; values[3] = values[1]; }
                if (components == 3) values[3] = 0;
            }
        }
        unsigned target = address + i;
        if (port_vif.wl <= port_vif.cl) target += i / port_vif.wl * (port_vif.cl - port_vif.wl);
        target &= 1023;
        unsigned mask_cycle = cycle < 3 ? cycle : 3;
        for (unsigned j = 0; j < 4; j++) {
            unsigned mask = command & 0x10 ? (port_vif.mask >> (mask_cycle * 8 + j * 2)) & 3 : 0;
            uint32_t value = values[j];
            if (mask == 3) continue;
            if (mask == 1) value = port_vif.row[j];
            else if (mask == 2) value = port_vif.column[mask_cycle];
            else if (!fill && format != 3 && port_vif.mode) {
                value += port_vif.row[j];
                if (port_vif.mode == 2) port_vif.row[j] = value;
            }
            memcpy(port_vu1_data + target * 16 + j * 4, &value, 4);
        }
    }
    return 0;
}

static int execute(uint32_t code, const unsigned char *data) {
    unsigned command = (code >> 24) & 0x7f;
    unsigned immediate = code & 65535;
    unsigned count = (code >> 16) & 255;
    if (!count) count = 256;
    if ((command & 0x60) == 0x60) return unpack(code, data);
    switch (command) {
        case 0: case 6: case 7: case 0x10: case 0x11: case 0x13: return 0;
        case 1:
            port_vif.cl = immediate & 255;
            port_vif.wl = immediate >> 8;
            if (!port_vif.cl) port_vif.cl = 256;
            if (!port_vif.wl) port_vif.wl = 256;
            return 0;
        case 2:
            port_vif.offset = immediate & 1023;
            port_vif.dbf = 0;
            port_vif.tops = port_vif.base;
            return 0;
        case 3: port_vif.base = immediate & 1023; return 0;
        case 4: port_vif.itops = immediate & 1023; return 0;
        case 5:
            if ((immediate & 3) == 3) return fail("invalid VIF STMOD mode");
            port_vif.mode = immediate & 3;
            return 0;
        case 0x14: case 0x15: case 0x17:
            port_vif.top = port_vif.tops;
            port_vif.itop = port_vif.itops;
            port_vu1_top = port_vif.top;
            port_vu1_itop = port_vif.itop;
            port_vif.dbf ^= 1;
            port_vif.tops = (port_vif.base + (port_vif.dbf ? port_vif.offset : 0)) & 1023;
            if (command == 0x17) return port_vu1_continue() ? fail("VU1 continuation failed") : 0;
            return port_vu1_start(immediate & 2047) ? fail("VU1 program failed") : 0;
        case 0x20: port_vif.mask = word(data); return 0;
        case 0x30: case 0x31:
            for (unsigned i = 0; i < 4; i++) {
                if (command == 0x30) port_vif.row[i] = word(data + i * 4);
                else port_vif.column[i] = word(data + i * 4);
            }
            return 0;
        case 0x4a:
            for (unsigned i = 0; i < count; i++) memcpy(port_vu1_code + ((immediate + i) & 2047) * 8, data + i * 8, 8);
            return 0;
        case 0x50: case 0x51:
            return port_gif_feed(data, (size_t)(immediate ? immediate : 65536) * 16, 2) ? fail("VIF DIRECT GIF failed") : 0;
        default: return fail("unsupported VIF command");
    }
}

int port_vif_feed(const void *data, size_t bytes) {
    if (error) return -1;
    if ((!data && bytes) || bytes > sizeof(pending) - pending_size) return fail("VIF buffer exceeded");
    if (bytes) memcpy(pending + pending_size, data, bytes);
    pending_size += bytes;
    size_t position = 0;
    while (pending_size - position >= 4) {
        uint32_t code = word(pending + position);
        unsigned command = (code >> 24) & 0x7f;
        unsigned count = (code >> 16) & 255;
        if (!count) count = 256;
        size_t payload = 0;
        if ((command & 0x60) == 0x60) {
            if ((command & 3) == 3 && ((command >> 2) & 3) != 3) return fail("invalid VIF UNPACK format");
            payload = unpack_bytes(command, count);
            payload = (payload + 3) & ~(size_t)3;
        } else if (command == 0x20) payload = 4;
        else if (command == 0x30 || command == 0x31) payload = 16;
        else if (command == 0x4a) payload = count * 8;
        else if (command == 0x50 || command == 0x51) payload = (size_t)((code & 65535) ? code & 65535 : 65536) * 16;
        if (pending_size - position < payload + 4) break;
        if (execute(code, pending + position + 4)) return -1;
        position += payload + 4;
    }
    if (position) {
        pending_size -= position;
        memmove(pending, pending + position, pending_size);
    }
    return 0;
}

int port_vif_finish(void) {
    if (error) return -1;
    return pending_size ? fail("truncated VIF command") : 0;
}
