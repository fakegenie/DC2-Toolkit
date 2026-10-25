#define _POSIX_C_SOURCE 200809L
#include "midi.h"
#include "sound_bank.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { CHUNK_ALIGNMENT = 16, TEST_DIVISION = 480 };

struct program_record {
    struct sce_program program;
    struct sce_split split;
};
_Static_assert(sizeof(struct program_record) == sizeof(struct sce_program) + sizeof(struct sce_split), "program with one split");

static unsigned starts, stops, last_pitch, last_left, last_right, last_route;
static struct spu_adsr1 last_adsr1;
static struct spu_adsr2 last_adsr2, last_update_adsr2;
static int active[SPU_VOICES];
static unsigned char hd[288];
static unsigned char body[16];
static struct sce_vag_info vag;
static struct sce_sample sample;
static struct sce_sample_set set;
static struct program_record program;
static uint32_t program_entry;

int spu_ram_write(unsigned address, const void *data, size_t size) { (void)address; (void)data; (void)size; return 1; }
void spu_voice_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, struct spu_adsr1 adsr1, struct spu_adsr2 adsr2) {
    assert(voice < SPU_VOICES && (address == 0x5210 || address == 0x18ae20 || address == 0x7d210));
    last_adsr1 = adsr1; last_adsr2 = adsr2;
    (void)pitch; (void)left; (void)right;
    active[voice] = 1;
    starts++;
}
void spu_voice_route(unsigned voice, unsigned route) { assert(voice < SPU_VOICES); last_route = route; }
void spu_voice_stop(unsigned voice) {
    assert(voice < SPU_VOICES);
    if (active[voice]) stops++;
    active[voice] = 0;
}
int spu_voice_level(unsigned voice) { return active[voice] ? INT16_MAX : 0; }
int spu_voice_active(unsigned voice) { assert(voice < SPU_VOICES); return active[voice]; }
void spu_voice_volume(unsigned voice, int left, int right) {
    (void)voice;
    last_left = (unsigned)left & SPU_VOLUME_LEVEL;
    last_right = (unsigned)right & SPU_VOLUME_LEVEL;
}
void spu_voice_pitch(unsigned voice, unsigned pitch) { (void)voice; last_pitch = pitch; }
void spu_voice_adsr2(unsigned voice, struct spu_adsr2 adsr2) { (void)voice; last_update_adsr2 = adsr2; }

static int same_adsr1(struct spu_adsr1 a, struct spu_adsr1 b) { return !memcmp(&a, &b, sizeof a); }
static int same_adsr2(struct spu_adsr2 a, struct spu_adsr2 b) { return !memcmp(&a, &b, sizeof a); }
static uint32_t padded(size_t size) { return (uint32_t)((size + CHUNK_ALIGNMENT - 1) / CHUNK_ALIGNMENT * CHUNK_ALIGNMENT); }
static void chunk(struct sce_chunk *chunk, const char *type, size_t size) {
    memcpy(chunk->creator, SCE_CREATOR, sizeof chunk->creator);
    memcpy(chunk->type, type, sizeof chunk->type);
    chunk->size = (uint32_t)size;
}
static uint32_t table(uint32_t at, const char *type, uint32_t entry, const void *record, size_t size) {
    struct sce_table head = {.max_index = 0};
    uint32_t first = sizeof head + sizeof entry;
    chunk(&head.chunk, type, padded(first + size));
    memcpy(hd + at, &head, sizeof head);
    memcpy(hd + at + sizeof head, &entry, sizeof entry);
    memcpy(hd + at + first, record, size);
    return at + head.chunk.size;
}
static uint32_t first_entry(void) { return sizeof(struct sce_table) + sizeof(uint32_t); }
static void default_bank(void) {
    vag = (struct sce_vag_info){.offset = 0, .rate = SPU_SAMPLE_RATE};
    sample = (struct sce_sample){
        .vel_range_low = 1, .vel_range_high = 127, .base_note = 60, .panpot = 64, .priority = 10, .volume = 128,
        .adsr1 = {.sustain_level = 15, .decay_rate = 15, .attack_rate = 0, .attack_exponential = 1},
        .adsr2 = {.release_rate = 12, .release_exponential = 1, .sustain_rate = 127, .sustain_decrease = 1},
        .spu_attribute = {.route = SPU_ROUTE_ALL, .core = SCE_SAMPLE_CORE_1},
    };
    set = (struct sce_sample_set){.vel_curve = SCE_VELOCITY_CURVE_LINEAR, .vel_limit_low = 1, .vel_limit_high = 127, .sample_count = 1, .samples = {0}};
    program = (struct program_record){
        .program = {.split_block_addr = sizeof(struct sce_program), .n_split = 1, .split_block_size = sizeof(struct sce_split), .volume = 128, .panpot = 64},
        .split = {.range_high = 127, .bend_range_low = 256, .bend_range_high = 256, .volume = 128, .panpot = 64},
    };
    program_entry = first_entry();
}
static void build_bank(void) {
    memset(hd, 0, sizeof(hd));
    struct sce_hd_header header = {0};
    chunk(&header.vers.chunk, "sreV", sizeof header.vers);
    chunk(&header.head.chunk, "daeH", sizeof header.head);
    header.head.header_size = sizeof hd;
    header.head.body_size = sizeof body;
    header.head.vag_info_chunk = sizeof header;
    header.head.sample_chunk = table(header.head.vag_info_chunk, "igaV", first_entry(), &vag, sizeof vag);
    header.head.sample_set_chunk = table(header.head.sample_chunk, "lpmS", first_entry(), &sample, sizeof sample);
    header.head.program_chunk = table(header.head.sample_set_chunk, "tesS", first_entry(), &set, offsetof(struct sce_sample_set, samples) + set.sample_count * sizeof set.samples[0]);
    assert(table(header.head.program_chunk, "gorP", program_entry, &program, sizeof program) == sizeof hd);
    memcpy(hd, &header, sizeof header);
}
static size_t build_sequence(unsigned char *p, const unsigned char *events, size_t count) {
    struct sce_sq_head head = {0};
    struct sce_sq_midi_block block = {.sequence_data_offset = sizeof block, .division = TEST_DIVISION};
    chunk(&head.vers.chunk, "sreV", sizeof head.vers);
    chunk(&head.sequ.chunk, "uqeS", sizeof head.sequ);
    chunk(&head.midi.chunk, "idiM", sizeof head.midi + sizeof block + count);
    head.midi.data_offset = sizeof head.midi;
    memcpy(p, &head, sizeof head);
    memcpy(p + sizeof head, &block, sizeof block);
    memcpy(p + sizeof head + sizeof block, events, count);
    return sizeof head + sizeof block + count;
}
static void attach(unsigned port) { build_bank(); assert(midi_bank_first(port, hd, sizeof(hd), body, sizeof(body)) == 0); }
static void note(unsigned port, unsigned key) { assert(midi_message(port, 0x90, key, 127) == 0); }
static void ticks(unsigned count) { for (unsigned i = 0; i < count; i++) midi_tick(); }
static void fresh(void) {
    midi_reset();
    attach(0);
    attach(7);
}

int main(void) {
    midi_reset();
    default_bank();
    attach(0); attach(7);
    const unsigned char events[] = {0, 0xc0, 0x80, 0x90, 60, 127, 0x83, 0x60, 0x80, 60, 0, 0xff, 0x2f, 0};
    unsigned char sq[256];
    size_t sq_size = build_sequence(sq, events, sizeof(events));
    assert(midi_sequence_load(0, sq, (int)sq_size) == 0);
    midi_sequence_play(0, 0, 256);
    midi_tick();
    assert(last_route == SPU_ROUTE_ALL);
    assert(starts == 1 && last_pitch == SPU_PITCH_UNITY && last_left == SPU_VOLUME_MAX && last_right == SPU_VOLUME_MAX);
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
    sample.key_follow_ar = (struct sce_key_follow){12, 60}; sample.key_follow_dr = (struct sce_key_follow){-12, 60};
    sample.key_follow_sr = (struct sce_key_follow){-12, 60}; sample.key_follow_rr = (struct sce_key_follow){12, 60};
    sample.key_follow_sl = (struct sce_key_follow){-24, 60};
    attach(7);
    midi_effect_play(7, 0, 0, 72, 64, 127, 128, 3);
    assert(same_adsr1(last_adsr1, (struct spu_adsr1){.sustain_level = 0, .decay_rate = 3, .attack_rate = 12, .attack_exponential = 1}));
    assert(same_adsr2(last_adsr2, (struct spu_adsr2){.release_rate = 24, .release_exponential = 1, .sustain_rate = 115, .sustain_decrease = 1}));
    midi_effect_play(7, 0, 0, 0, 64, 127, 128, 3);
    assert(same_adsr1(last_adsr1, (struct spu_adsr1){.sustain_level = 15, .decay_rate = 15, .attack_rate = 0, .attack_exponential = 1}));
    assert(same_adsr2(last_adsr2, (struct spu_adsr2){.release_rate = 0, .release_exponential = 1, .sustain_rate = 127, .sustain_decrease = 1}));
    sample.key_follow_ar = sample.key_follow_dr = sample.key_follow_sr = sample.key_follow_rr = sample.key_follow_sl = (struct sce_key_follow){0, 0};
    sample.group = 1;
    attach(7);
    midi_effect_play(7, 0, 0, 60, 64, 127, 128, 3);
    unsigned before_group = stops;
    midi_effect_play(7, 0, 0, 72, 64, 127, 128, 3);
    assert(stops == before_group + 1 && same_adsr2(last_update_adsr2, (struct spu_adsr2){.release_rate = 1, .release_exponential = 1, .sustain_decrease = 1, .sustain_exponential = 1}));
    assert(midi_message(7, 0x91, 72, 127) == 0 && stops == before_group + 1);
    sample.group = 0;
    program.program.key_follow_pan = (struct sce_key_follow){12, 60};
    attach(7);
    note(7, 72);
    assert(last_left < last_right);
    note(7, 48);
    assert(last_left > last_right);
    program.program.key_follow_pan.amount = 0;
    program.program.attribute = SCE_PROGRAM_PAN_CIRCULAR; program.program.panpot = 192;
    attach(7);
    note(7, 60);
    assert(last_left == 0x4001 && last_right == 0x4001);
    program.program.panpot = 64;
    attach(7);
    note(7, 60);
    assert(last_left == 0x3fff && last_right == 0x3fff);
    program.program.attribute = 0;
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
    for (unsigned wave = SCE_LFO_WAVE_SAW_UP; wave <= SCE_LFO_WAVE_SINE; wave++) {
        sample.lfo_attribute.pitch = SCE_LFO_TRIGGER_KEY_ON; program.program.lfo_wave[SCE_LFO_PITCH] = (uint8_t)wave; program.program.lfo_cycle[SCE_LFO_PITCH] = 100;
        program.program.lfo_pitch_depth[SCE_LFO_POSITIVE] = 512; program.program.lfo_pitch_depth[SCE_LFO_NEGATIVE] = 512;
        midi_reset(); attach(7);
        note(7, 60);
        assert(last_pitch == SPU_PITCH_UNITY);
        ticks(201);
        if (wave == SCE_LFO_WAVE_SAW_UP || wave == SCE_LFO_WAVE_SQUARE) assert(last_pitch < SPU_PITCH_UNITY);
        if (wave == SCE_LFO_WAVE_SAW_DOWN || wave == SCE_LFO_WAVE_TRIANGLE || wave == SCE_LFO_WAVE_SINE) assert(last_pitch > SPU_PITCH_UNITY);
        if (wave == SCE_LFO_WAVE_NOISE) {
            assert(last_pitch == SPU_PITCH_UNITY);
            ticks(4800);
            assert(last_pitch != SPU_PITCH_UNITY);
        }
    }
    program.program.lfo_wave[SCE_LFO_PITCH] = SCE_LFO_WAVE_SQUARE; sample.pitch_lfo_delay = 100;
    midi_reset(); attach(7);
    note(7, 60);
    ticks(4801);
    assert(last_pitch == SPU_PITCH_UNITY);
    ticks(201);
    assert(last_pitch < SPU_PITCH_UNITY);
    sample.pitch_lfo_delay = 0;
    sample.lfo_attribute = (struct sce_lfo_attribute){.amp = SCE_LFO_TRIGGER_KEY_ON};
    program.program.lfo_wave[SCE_LFO_AMP] = SCE_LFO_WAVE_SQUARE; program.program.lfo_cycle[SCE_LFO_AMP] = 100;
    program.program.lfo_amp_depth[SCE_LFO_POSITIVE] = 64; program.program.lfo_amp_depth[SCE_LFO_NEGATIVE] = 64;
    midi_reset(); attach(7);
    midi_volume(7, 64);
    note(7, 60);
    assert(last_left == (unsigned)(16383.0 * (129 / 256.0)));
    ticks(201);
    assert(last_left == (unsigned)(16383.0 * (129 / 256.0 * 0.5)));
    ticks(2400);
    assert(last_left == (unsigned)(16383.0 * (129 / 256.0 * 1.5)));
    sample.lfo_attribute = (struct sce_lfo_attribute){.pitch = SCE_LFO_TRIGGER_KEY_ON}; program.program.lfo_wave[SCE_LFO_PITCH] = SCE_LFO_WAVE_SQUARE;
    program.program.lfo_pitch_depth[SCE_LFO_POSITIVE] = program.program.lfo_pitch_depth[SCE_LFO_NEGATIVE] = 0;
    program.program.lfo_midi_pitch_depth[SCE_LFO_POSITIVE] = program.program.lfo_midi_pitch_depth[SCE_LFO_NEGATIVE] = 512;
    midi_reset(); attach(7);
    note(7, 60);
    ticks(201);
    assert(last_pitch == SPU_PITCH_UNITY);
    assert(midi_message(7, 0xb0, 1, 127) == 0 && last_pitch < SPU_PITCH_UNITY);
    sample.lfo_attribute = (struct sce_lfo_attribute){0};
    program.split.bend_range_low = program.split.bend_range_high = 0;
    midi_reset(); attach(7);
    assert(midi_message(7, 0xe0, 127, 127) == 0);
    note(7, 60);
    assert(last_pitch == SPU_PITCH_UNITY);
    program.program.transpose = 127; program.split.transpose = 127;
    attach(7);
    note(7, 127);
    assert(last_pitch == SPU_PITCH_MAX);
    midi_reset();
    program_entry = SCE_TABLE_NONE;
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
    program_entry = SCE_TABLE_NONE - 1;
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
    program_entry = first_entry();
    attach(7);
    assert(midi_message(7, 0xb0, 0, 0) == 0 && midi_message(7, 0xc0, 0, 0) == 0);
    note(7, 60);
    assert(starts == before_missing + 1);
    puts("midi tests passed");
    return 0;
}
