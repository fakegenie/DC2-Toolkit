#define _POSIX_C_SOURCE 200809L
#include "midi.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned starts, stops, last_pitch, last_left, last_right, last_adsr1, last_adsr2, last_route, last_update_adsr2;
static int active[48];
static unsigned char hd[288];
static unsigned char body[16];

int spu_ram_write(unsigned address, const void *data, size_t size) { (void)address; (void)data; (void)size; return 1; }
void spu_voice_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, unsigned adsr1, unsigned adsr2) {
    assert(voice < 48 && (address == 0x5210 || address == 0x18ae20 || address == 0x7d210));
    last_adsr1 = adsr1; last_adsr2 = adsr2;
    (void)pitch; (void)left; (void)right;
    active[voice] = 1;
    starts++;
}
void spu_voice_route(unsigned voice, unsigned route) { assert(voice < 48); last_route = route; }
void spu_voice_stop(unsigned voice) {
    assert(voice < 48);
    if (active[voice]) stops++;
    active[voice] = 0;
}
int spu_voice_level(unsigned voice) { return active[voice] ? 32767 : 0; }
int spu_voice_active(unsigned voice) { assert(voice < 48); return active[voice]; }
void spu_voice_volume(unsigned voice, int left, int right) {
    (void)voice;
    last_left = (unsigned)left & 0x7fff;
    last_right = (unsigned)right & 0x7fff;
}
void spu_voice_pitch(unsigned voice, unsigned pitch) { (void)voice; last_pitch = pitch; }
void spu_voice_adsr2(unsigned voice, unsigned adsr2) { (void)voice; last_update_adsr2 = adsr2; }

static void put32(unsigned char *p, uint32_t value) {
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}
static void put16(unsigned char *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void chunk(unsigned char *p, const char *tag, unsigned size) { memcpy(p, tag, 8); put32(p + 8, size); }
static void table(unsigned char *p, const char *tag, unsigned size) { chunk(p, tag, size); put32(p + 12, 0); put32(p + 16, 20); }
static void build_bank(void) {
    unsigned char *p = hd;
    memset(hd, 0, sizeof(hd));
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
}
static size_t build_sequence(unsigned char *p, const unsigned char *events, size_t count) {
    chunk(p, "IECSsreV", 16); chunk(p + 16, "IECSuqeS", 32); chunk(p + 48, "IECSidiM", (unsigned)(0x1a + count));
    put32(p + 64, 20); put32(p + 68, 6); put16(p + 72, 480); memcpy(p + 74, events, count);
    return 0x4a + count;
}
static void attach(unsigned port) { assert(midi_bank_first(port, hd, sizeof(hd), body, sizeof(body)) == 0); }
static void note(unsigned port, unsigned key) { assert(midi_message(port, 0x90, key, 127) == 0); }
static void ticks(unsigned count) { for (unsigned i = 0; i < count; i++) midi_tick(); }
static void fresh(void) {
    midi_reset();
    attach(0);
    attach(7);
}

int main(void) {
    midi_reset();
    build_bank();
    unsigned char *sample = hd + 132;
    unsigned char *program = hd + 228;
    unsigned char *split = program + 36;
    attach(0); attach(7);
    const unsigned char events[] = {0, 0xc0, 0x80, 0x90, 60, 127, 0x83, 0x60, 0x80, 60, 0, 0xff, 0x2f, 0};
    unsigned char sq[256];
    size_t sq_size = build_sequence(sq, events, sizeof(events));
    assert(midi_sequence_load(0, sq, (int)sq_size) == 0);
    midi_sequence_play(0, 0, 256);
    midi_tick();
    assert(last_route == 15);
    assert(starts == 1 && last_pitch == 4096 && last_left == 16383 && last_right == 16383);
    ticks(23999);
    assert(stops == 0 && midi_playing(0));
    midi_tick();
    assert(stops == 1 && !midi_playing(0));
    midi_sequence_play(0, 0, 256);
    midi_tick();
    assert(starts == 2);
    midi_stop(0);
    assert(stops == 2);
    assert(midi_sequence_load(0, NULL, 4) == -1);
    midi_effect_play(7, 0, 0, 60, 64, 127, 128, 3);
    assert(starts == 3);
    midi_effect_stop(7, 0, 0, 60, 3);
    assert(stops == 3);
    note(7, 60);
    assert(starts == 4);
    midi_stop(7);
    midi_effect_play(7, 0, 0, 60, 64, 127, 128, 3);
    midi_effect_volume(7, 0, 0, 60, 64, 3);
    unsigned gain = last_left;
    midi_effect_volume(7, 0, 0, 60, 64, 3);
    assert(gain == last_left);
    midi_effect_pan(7, 0, 0, 60, 127, 3);
    assert(last_left == 0);
    midi_effect_pan(7, 0, 0, 60, 127, 3);
    assert(last_left == 0);
    midi_effect_pitch(7, 0, 0, 60, 80 << 7, 3);
    unsigned pitch = last_pitch;
    midi_effect_pitch(7, 0, 0, 60, 80 << 7, 3);
    assert(pitch == last_pitch);
    midi_effect_play(7, 0, 0, 60, 64, 127, 128, 200);
    assert(starts == 5);
    midi_effect_play(1, 0, 0, 60, 64, 127, 128, 3);
    assert(starts == 5);
    midi_stop(7);
    sample[22] = 12; sample[23] = 60; sample[24] = 244; sample[25] = 60;
    sample[26] = 244; sample[27] = 60; sample[28] = 12; sample[29] = 60; sample[30] = 232; sample[31] = 60;
    attach(7);
    midi_effect_play(7, 0, 0, 72, 64, 127, 128, 3);
    assert(last_adsr1 == 0x8c30 && last_adsr2 == 0x5cf8);
    midi_effect_play(7, 0, 0, 0, 64, 127, 128, 3);
    assert(last_adsr1 == 0x80ff && last_adsr2 == 0x5fe0);
    memset(sample + 22, 0, 10);
    sample[14] = 1;
    attach(7);
    midi_effect_play(7, 0, 0, 60, 64, 127, 128, 3);
    unsigned before_group = stops;
    midi_effect_play(7, 0, 0, 72, 64, 127, 128, 3);
    assert(stops == before_group + 1 && last_update_adsr2 == 0xc021);
    assert(midi_message(7, 0x91, 72, 127) == 0 && stops == before_group + 1);
    sample[14] = 0;
    program[10] = 12; program[11] = 60;
    attach(7);
    note(7, 72);
    assert(last_left < last_right);
    note(7, 48);
    assert(last_left > last_right);
    program[10] = 0;
    program[12] = 1; program[7] = 192;
    attach(7);
    note(7, 60);
    assert(last_left == 0x4001 && last_right == 0x4001);
    program[7] = 64;
    attach(7);
    note(7, 60);
    assert(last_left == 0x3fff && last_right == 0x3fff);
    program[12] = 0;
    attach(0); attach(7);
    const unsigned char loop_events[] = {0, 0xb0, 99, 0x80, 6, 0, 0, 0x90, 60, 127, 1, 0x80, 60, 0, 0xb0, 99, 0x81, 6, 0, 0, 38, 0};
    sq_size = build_sequence(sq, loop_events, sizeof(loop_events));
    assert(midi_sequence_load(0, sq, (int)sq_size) == 0);
    midi_sequence_play(0, 0, 256);
    unsigned before = starts;
    ticks(101);
    assert(starts >= before + 3 && midi_playing(0));
    midi_stop(0);
    fresh();
    attach(10);
    for (unsigned i = 0; i < 24; i++) note(10, 40 + i);
    before = starts;
    note(7, 80);
    assert(starts == before);
    note(0, 80);
    assert(starts == before + 1);
    midi_stop(7);
    midi_sequence_play(0, 0, 256);
    midi_close();
    before = starts;
    ticks(100);
    assert(starts == before);
    midi_open();
    midi_tick();
    assert(starts == before && !midi_playing(0));
    for (unsigned wave = 1; wave <= 6; wave++) {
        sample[40] = 1; program[14] = (unsigned char)wave; put16(program + 20, 100);
        put16(program + 24, 512); put16(program + 26, 512);
        midi_reset(); attach(7);
        note(7, 60);
        assert(last_pitch == 4096);
        ticks(201);
        if (wave == 1 || wave == 4) assert(last_pitch < 4096);
        if (wave == 2 || wave == 3 || wave == 6) assert(last_pitch > 4096);
        if (wave == 5) {
            assert(last_pitch == 4096);
            ticks(4800);
            assert(last_pitch != 4096);
        }
    }
    program[14] = 4; put16(sample + 32, 100);
    midi_reset(); attach(7);
    note(7, 60);
    ticks(4801);
    assert(last_pitch == 4096);
    ticks(201);
    assert(last_pitch < 4096);
    put16(sample + 32, 0);
    sample[40] = 16; program[15] = 4; put16(program + 22, 100); program[32] = 64; program[33] = 64;
    midi_reset(); attach(7);
    midi_volume(7, 64);
    note(7, 60);
    assert(last_left == (unsigned)(16383.0 * (129 / 256.0)));
    ticks(201);
    assert(last_left == (unsigned)(16383.0 * (129 / 256.0 * 0.5)));
    ticks(2400);
    assert(last_left == (unsigned)(16383.0 * (129 / 256.0 * 1.5)));
    sample[40] = 1; program[14] = 4; put16(program + 24, 0); put16(program + 26, 0); put16(program + 28, 512); put16(program + 30, 512);
    midi_reset(); attach(7);
    note(7, 60);
    ticks(201);
    assert(last_pitch == 4096);
    assert(midi_message(7, 0xb0, 1, 127) == 0 && last_pitch < 4096);
    sample[40] = 0;
    put16(split + 6, 0); put16(split + 8, 0);
    midi_reset(); attach(7);
    assert(midi_message(7, 0xe0, 127, 127) == 0);
    note(7, 60);
    assert(last_pitch == 4096);
    program[8] = 127; split[18] = 127;
    attach(7);
    note(7, 127);
    assert(last_pitch == 16383);
    midi_reset();
    put32(hd + 224, UINT32_MAX);
    attach(7);
    unsigned before_missing = starts;
    FILE *diagnostic_file = tmpfile();
    assert(diagnostic_file);
    int original_stderr = dup(STDERR_FILENO);
    assert(original_stderr >= 0 && dup2(fileno(diagnostic_file), STDERR_FILENO) >= 0);
    note(7, 60);
    note(7, 61);
    assert(starts == before_missing);
    fflush(stderr);
    assert(ftell(diagnostic_file) == 0);
    put32(hd + 224, UINT32_MAX - 1);
    attach(7);
    note(7, 60);
    note(7, 61);
    assert(midi_message(7, 0xc0, 1, 0) == 0);
    note(7, 60);
    assert(midi_message(7, 0xb0, 0, 1) == 0);
    note(7, 60);
    assert(starts == before_missing);
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
    put32(hd + 224, 20);
    attach(7);
    assert(midi_message(7, 0xb0, 0, 0) == 0 && midi_message(7, 0xc0, 0, 0) == 0);
    note(7, 60);
    assert(starts == before_missing + 1);
    puts("midi tests passed");
    return 0;
}
