#define _POSIX_C_SOURCE 200809L
#include "iop_midi.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned char memory[65536];
static uint32_t cursor = 256;
static struct { uint32_t base; size_t size; } allocations[32];
static unsigned allocation_count;
static unsigned starts, stops, last_pitch, last_left, last_right, last_adsr1, last_adsr2, last_route, last_update_adsr2;
static int active[48];
void *iop_memory_resolve(uint32_t address, size_t size) {
    for (unsigned i = 0; i < allocation_count; i++) if (address >= allocations[i].base && address - allocations[i].base <= allocations[i].size && size <= allocations[i].size - (address - allocations[i].base)) return memory + address;
    return NULL;
}
size_t iop_memory_size(uint32_t address) {
    for (unsigned i = 0; i < allocation_count; i++) if (address == allocations[i].base) return allocations[i].size;
    return 0;
}
uint32_t iop_memory_alloc(size_t size) {
    assert(allocation_count < 32 && cursor + size <= sizeof(memory));
    uint32_t result = cursor;
    allocations[allocation_count].base = cursor;
    allocations[allocation_count++].size = size;
    cursor += (uint32_t)size;
    return result;
}
void port_spu_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, unsigned adsr1, unsigned adsr2) {
    assert(voice < 48 && address == 0x1000);
    last_adsr1 = adsr1; last_adsr2 = adsr2;
    (void)pitch; (void)left; (void)right;
    active[voice] = 1;
    starts++;
}
void port_spu_route(unsigned voice, unsigned route) { assert(voice < 48); last_route = route; }
void port_spu_stop(unsigned voice) { assert(voice < 48); active[voice] = 0; stops++; }
unsigned port_spu_level(unsigned voice) { return active[voice] ? 32767 : 0; }
int port_spu_active(unsigned voice) { assert(voice < 48); return active[voice]; }
void port_spu_param(unsigned entry, unsigned value) {
    unsigned reg = entry >> 8;
    if (reg == 0) last_left = value;
    if (reg == 1) last_right = value;
    if (reg == 2) last_pitch = value;
    if (reg == 4) last_update_adsr2 = value;
}
static void put32(unsigned char *p, uint32_t value) {
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}
static void put16(unsigned char *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void chunk(unsigned char *p, const char *tag, unsigned size) { memcpy(p, tag, 8); put32(p + 8, size); }
static void table(unsigned char *p, const char *tag, unsigned size) { chunk(p, tag, size); put32(p + 12, 0); put32(p + 16, 20); }
static uint32_t bank(void) {
    uint32_t address = iop_memory_alloc(288);
    unsigned char *p = memory + address;
    chunk(p, "IECSsreV", 16); chunk(p + 16, "IECSdaeH", 64);
    put32(p + 28, 288); put32(p + 32, 16);
    put32(p + 36, 208); put32(p + 40, 176); put32(p + 44, 112); put32(p + 48, 80);
    table(p + 80, "IECSigaV", 32); put32(p + 100, 0); put16(p + 104, 48000);
    table(p + 112, "IECSlpmS", 64);
    unsigned char *sample = p + 132;
    sample[2] = 1; sample[4] = 127; sample[11] = 60; sample[13] = 64; sample[15] = 10; sample[16] = 128;
    put16(sample + 18, 0x80ff); put16(sample + 20, 0x5fec); sample[41] = 0x2f;
    table(p + 176, "IECStesS", 32); p[197] = 1; p[198] = 127; p[199] = 1;
    table(p + 208, "IECSgorP", 80);
    unsigned char *program = p + 228;
    put32(program, 36); program[4] = 1; program[5] = 20; program[6] = 128; program[7] = 64;
    unsigned char *split = program + 36;
    split[4] = 127; split[16] = 128; split[17] = 64;
    put16(split + 6, 256); put16(split + 8, 256);
    return address;
}
static uint32_t sequence(const unsigned char *events, size_t count) {
    uint32_t address = iop_memory_alloc(0x4a + count);
    unsigned char *p = memory + address;
    chunk(p, "IECSsreV", 16); chunk(p + 16, "IECSuqeS", 32); chunk(p + 48, "IECSidiM", (unsigned)(0x1a + count));
    put32(p + 64, 20); put32(p + 68, 6); put16(p + 72, 480); memcpy(p + 74, events, count);
    return address;
}
static int rpc(unsigned command, uint32_t argument) { int out = -2; assert(iop_midi_rpc(command, &argument, 4, &out, 4) == 0); return out; }
static void attach(unsigned port, uint32_t hd) {
    unsigned char data[64] = {0};
    put32(data + 4, hd); put32(data + 12, 16); put32(data + 16, 0x1000);
    assert(iop_midi_rpc(0x9050 + port, data, sizeof(data), NULL, 0) == 0);
}
int main(void) {
    iop_midi_reset();
    uint32_t hd = bank();
    attach(0, hd); attach(7, hd);
    const unsigned char events[] = {0, 0xc0, 0x80, 0x90, 60, 127, 0x83, 0x60, 0x80, 60, 0, 0xff, 0x2f, 0};
    uint32_t sq = sequence(events, sizeof(events));
    rpc(0x40, sq); rpc(0, 0);
    iop_midi_tick();
    assert(last_route == 15);
    assert(starts == 1 && last_pitch == 4096 && last_left == 16383 && last_right == 16383);
    for (unsigned i = 0; i < 23999; i++) iop_midi_tick();
    assert(stops == 0 && rpc(0x8090, 0) == 1);
    iop_midi_tick();
    assert(stops == 1 && rpc(0x8090, 0) == 0);
    rpc(0x30, 0); rpc(0, 0); iop_midi_tick(); assert(starts == 2);
    rpc(0x20, 0); assert(stops == 2);
    unsigned char effect[] = {0xfd, 16, 0, 60, 3, 127, 0};
    assert(iop_midi_hs_message(0, effect) == 0); assert(starts == 3);
    effect[5] = 0; assert(iop_midi_hs_message(0, effect) == 0); assert(stops == 3);
    uint32_t buffers = (uint32_t)rpc(0x8010, 0x4000);
    unsigned char *stream = iop_memory_resolve(buffers, 512);
    put32(stream + 4, 3); stream[8] = 0x90; stream[9] = 60; stream[10] = 127;
    iop_midi_tick(); assert(starts == 4 && !stream[4]);
    rpc(0x27, 0);
    effect[5] = 127;
    assert(iop_midi_hs_message(0, effect) == 0);
    effect[1] = 0; effect[5] = 64;
    assert(iop_midi_hs_message(0, effect) == 0);
    unsigned gain = last_left;
    assert(iop_midi_hs_message(0, effect) == 0 && gain == last_left);
    effect[1] = 1; effect[5] = 127;
    assert(iop_midi_hs_message(0, effect) == 0 && last_left == 0);
    assert(iop_midi_hs_message(0, effect) == 0 && last_left == 0);
    effect[1] = 2; effect[5] = 0; effect[6] = 80;
    assert(iop_midi_hs_message(0, effect) == 0);
    unsigned pitch = last_pitch;
    assert(iop_midi_hs_message(0, effect) == 0 && pitch == last_pitch);
    unsigned char defaults[] = {0xf9, 0, 0, 128, 0};
    assert(iop_midi_hs_message(0, defaults) == 0 && last_left == 0);
    rpc(0x27, 0);
    unsigned char *sample = memory + hd + 132;
    sample[22] = 12; sample[23] = 60; sample[24] = 244; sample[25] = 60;
    sample[26] = 244; sample[27] = 60; sample[28] = 12; sample[29] = 60; sample[30] = 232; sample[31] = 60;
    effect[1] = 16; effect[3] = 72; effect[5] = 127;
    assert(iop_midi_hs_message(0, effect) == 0 && last_adsr1 == 0x8c30 && last_adsr2 == 0x5cf8);
    effect[3] = 0;
    assert(iop_midi_hs_message(0, effect) == 0 && last_adsr1 == 0x80ff && last_adsr2 == 0x5fe0);
    memset(sample + 22, 0, 10);
    rpc(0x27, 0);
    sample[14] = 1;
    effect[3] = 60;
    assert(iop_midi_hs_message(0, effect) == 0);
    unsigned before_group = stops;
    effect[3] = 72;
    assert(iop_midi_hs_message(0, effect) == 0 && stops == before_group + 1 && last_update_adsr2 == 0xc021);
    effect[2] = 1;
    assert(iop_midi_hs_message(0, effect) == 0 && stops == before_group + 1);
    effect[2] = 0;
    sample[14] = 0;
    rpc(0x27, 0);
    unsigned char *program = memory + hd + 228;
    program[10] = 12; program[11] = 60;
    assert(iop_midi_message(0, 0x90 | 72 << 8 | 127 << 16) == 0 && last_left < last_right);
    assert(iop_midi_message(0, 0x90 | 48 << 8 | 127 << 16) == 0 && last_left > last_right);
    program[10] = 0;
    program[12] = 1; program[7] = 192;
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && last_left == 0x4001 && last_right == 0x4001);
    program[7] = 64;
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && last_left == 0x3fff && last_right == 0x3fff);
    program[12] = 0;
    rpc(0x27, 0);
    const unsigned char loop_events[] = {0, 0xb0, 99, 0x80, 6, 0, 0, 0x90, 60, 127, 1, 0x80, 60, 0, 0xb0, 99, 0x81, 6, 0, 0, 38, 0};
    uint32_t loop_sq = sequence(loop_events, sizeof(loop_events));
    rpc(0x40, loop_sq); rpc(0, 0);
    unsigned before = starts;
    for (unsigned i = 0; i < 101; i++) iop_midi_tick();
    assert(starts >= before + 3 && rpc(0x8090, 0));
    rpc(0x20, 0);
    rpc(0x27, 0);
    rpc(0xa7, 0x3064);
    for (unsigned i = 0; i < 24; i++) assert(iop_midi_message(0, 0x90 | (40 + i) << 8 | 127 << 16) == 0);
    before = starts;
    rpc(0xa7, 0x3010);
    assert(iop_midi_message(0, 0x90 | 80 << 8 | 127 << 16) == 0 && starts == before);
    rpc(0xa7, 0x307f);
    assert(iop_midi_message(0, 0x90 | 80 << 8 | 127 << 16) == 0 && starts == before + 1);
    rpc(0x27, 0);
    rpc(0x30, 0); rpc(0, 0); rpc(0x80f0, 0);
    before = starts;
    for (unsigned i = 0; i < 100; i++) iop_midi_tick();
    assert(starts == before);
    rpc(0x8010, 0x4000); iop_midi_tick(); assert(starts == before && !rpc(0x8090, 0));
    for (unsigned wave = 1; wave <= 6; wave++) {
        iop_midi_reset(); attach(7, hd);
        sample[40] = 1; program[14] = (unsigned char)wave; put16(program + 20, 100);
        put16(program + 24, 512); put16(program + 26, 512);
        assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && last_pitch == 4096);
        for (unsigned i = 0; i < 201; i++) iop_midi_tick();
        if (wave == 1 || wave == 4) assert(last_pitch < 4096);
        if (wave == 2 || wave == 3 || wave == 6) assert(last_pitch > 4096);
        if (wave == 5) {
            assert(last_pitch == 4096);
            for (unsigned i = 201; i < 5001; i++) iop_midi_tick();
            assert(last_pitch != 4096);
        }
    }
    iop_midi_reset(); attach(7, hd);
    program[14] = 4; put16(sample + 32, 100);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0);
    for (unsigned i = 0; i < 4801; i++) iop_midi_tick();
    assert(last_pitch == 4096);
    for (unsigned i = 0; i < 201; i++) iop_midi_tick();
    assert(last_pitch < 4096);
    put16(sample + 32, 0);
    iop_midi_reset(); attach(7, hd); rpc(0xb7, 128);
    sample[40] = 16; program[15] = 4; put16(program + 22, 100); program[32] = 64; program[33] = 64;
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && last_left == 8191);
    for (unsigned i = 0; i < 201; i++) iop_midi_tick();
    assert(last_left == 4095);
    for (unsigned i = 201; i < 2601; i++) iop_midi_tick();
    assert(last_left == 12287);
    iop_midi_reset(); attach(7, hd);
    sample[40] = 1; program[14] = 4; put16(program + 24, 0); put16(program + 26, 0); put16(program + 28, 512); put16(program + 30, 512);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0);
    for (unsigned i = 0; i < 201; i++) iop_midi_tick();
    assert(last_pitch == 4096);
    assert(iop_midi_message(0, 0xb0 | 1 << 8 | 127 << 16) == 0 && last_pitch < 4096);
    iop_midi_reset(); attach(7, hd);
    sample[40] = 0;
    unsigned char *split = program + 36;
    put16(split + 6, 0); put16(split + 8, 0);
    assert(iop_midi_message(0, 0xe0 | 127 << 8 | 127 << 16) == 0);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && last_pitch == 4096);
    program[8] = 127; split[18] = 127;
    assert(iop_midi_message(0, 0x90 | 127 << 8 | 127 << 16) == 0 && last_pitch == 16383);
    iop_midi_reset(); attach(7, hd);
    unsigned before_missing = starts;
    FILE *diagnostic_file = tmpfile();
    assert(diagnostic_file);
    int original_stderr = dup(STDERR_FILENO);
    assert(original_stderr >= 0 && dup2(fileno(diagnostic_file), STDERR_FILENO) >= 0);
    put32(memory + hd + 224, UINT32_MAX);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && starts == before_missing);
    assert(iop_midi_message(0, 0x90 | 61 << 8 | 127 << 16) == 0 && starts == before_missing);
    fflush(stderr);
    assert(ftell(diagnostic_file) == 0);
    put32(memory + hd + 224, UINT32_MAX - 1);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && starts == before_missing);
    assert(iop_midi_message(0, 0x90 | 61 << 8 | 127 << 16) == 0 && starts == before_missing);
    assert(iop_midi_message(0, 0xc0 | 1 << 8) == 0);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && starts == before_missing);
    assert(iop_midi_message(0, 0xb0 | 1 << 16) == 0);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && starts == before_missing);
    fflush(stderr);
    assert(dup2(original_stderr, STDERR_FILENO) >= 0);
    close(original_stderr);
    rewind(diagnostic_file);
    char diagnostic_text[4096] = {0};
    assert(fread(diagnostic_text, 1, sizeof(diagnostic_text) - 1, diagnostic_file) > 0);
    fclose(diagnostic_file);
    assert(!strstr(diagnostic_text, "silent"));
    const char *invalid_line = strstr(diagnostic_text, "invalid program record port=7 bank=0 channel=0 program=0");
    assert(invalid_line && !strstr(invalid_line + 1, "invalid program record"));
    assert(strstr(diagnostic_text, "table_index=0 table_count=1 entry=0xfffffffe"));
    assert(strstr(diagnostic_text, "program outside table port=7 bank=0 channel=0 program=1"));
    assert(strstr(diagnostic_text, "unloaded bank port=7 bank=1"));
    put32(memory + hd + 224, 20);
    assert(iop_midi_message(0, 0xb0) == 0 && iop_midi_message(0, 0xc0) == 0);
    assert(iop_midi_message(0, 0x90 | 60 << 8 | 127 << 16) == 0 && starts == before_missing + 1);
    uint32_t invalid = 0;
    int result = 0;
    assert(iop_midi_rpc(0x40, &invalid, 4, &result, 4) == -1 && result == -1);
    assert(iop_midi_rpc(0x70, &invalid, 4, &result, 4) == -1);
    puts("iop_midi tests passed");
    return 0;
}
