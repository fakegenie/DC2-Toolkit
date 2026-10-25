#include "midi.h"
#include "sound_bank.h"
#include "spu2.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MIDI_PORTS = 16, MIDI_CHANNELS = 16, PORT_BANKS = 16, PORT_SEQUENCES = 16, MIDI_LOOPS = 128, LAYOUT_DEPENDENTS = 5, MISSING_LIMIT = 32 };
enum {
    MIDI_NOTE_OFF = 0x80,
    MIDI_NOTE_ON = 0x90,
    MIDI_KEY_PRESSURE = 0xa0,
    MIDI_CONTROL = 0xb0,
    MIDI_PROGRAM = 0xc0,
    MIDI_CHANNEL_PRESSURE = 0xd0,
    MIDI_PITCH_BEND = 0xe0,
    MIDI_SYSTEM = 0xf0,
    MIDI_META = 0xff
};
enum { MIDI_STATUS_KIND = 0xf0, MIDI_STATUS_CHANNEL = 0x0f, MIDI_STATUS_BIT = 0x80, MIDI_DATA = 0x7f, MIDI_DATA_BITS = 7 };
enum { MIDI_META_END_OF_TRACK = 0x2f, MIDI_META_TEMPO = 0x51, MIDI_TEMPO_BYTES = 3, MIDI_DEFAULT_TEMPO = 500000 };
enum {
    MIDI_CONTROL_BANK = 0,
    MIDI_CONTROL_MODULATION = 1,
    MIDI_CONTROL_BREATH = 2,
    MIDI_CONTROL_DATA_ENTRY = 6,
    MIDI_CONTROL_VOLUME = 7,
    MIDI_CONTROL_PAN = 10,
    MIDI_CONTROL_EXPRESSION = 11,
    MIDI_CONTROL_BANK_LSB = 32,
    MIDI_CONTROL_DATA_ENTRY_LSB = 38,
    MIDI_CONTROL_SUSTAIN = 64,
    MIDI_CONTROL_REVERB = 91,
    MIDI_CONTROL_CHORUS = 93,
    MIDI_CONTROL_NRPN = 99,
    MIDI_CONTROL_ALL_SOUND_OFF = 120,
    MIDI_CONTROL_RESET = 121,
    MIDI_CONTROL_ALL_NOTES_OFF = 123
};
enum { MIDI_NRPN_LOOP_START = 0, MIDI_NRPN_LOOP_END = 1 };
enum { MIDI_VALUE_MAX = 127, MIDI_SWITCH_ON = 64, MIDI_PAN_CENTER = 64, MIDI_BEND_CENTER = 8192, MIDI_KEYS = 128 };
enum { SQ_SKIP_DELTA = 0x80, VLQ_CONTINUE = 0x80, VLQ_BYTES = 4, EVENT_BUDGET = 4096 };
enum { PORT_VOLUME_FULL = 256, EFFECT_VOLUME_FULL = 128, FIRST_EFFECT_PORT = 7, EFFECT_ID_MAX = 0x7e };
enum { EFFECT_VOLUME, EFFECT_PAN, EFFECT_PITCH };
enum { FEATURE_FOLLOW = 0x1, FEATURE_LFO = 0x2, FEATURE_VELOCITY_CURVE = 0x10 };
enum { LFO_IDLE, LFO_DELAY, LFO_FADE_IN, LFO_RUNNING, LFO_RELEASE_DELAY, LFO_FADE_OUT, LFO_DONE };
enum { LFO_TICK_SCALE = 256, LFO_TICK_LENGTH = 1066, LFO_CYCLE = 65536, LFO_SINE_STEPS = 200 };
enum { CONTROL_CLOCK_STEP = 1000000, CONTROL_CLOCK_LFO_TICK = 200016000 };
enum { RANDOM_MULTIPLIER = 0x41c64e6d, RANDOM_INCREMENT = 0x3039, RANDOM_SEED = 0x02cee9d7 };
enum { BANK_BODY_GAP = 16, SEQUENCE_PADDING = 256, SEQUENCE_LIMIT = 16 * 1024 * 1024, DIAGNOSTIC_LIMIT = 24 };
static const double ticks_per_microsecond = SPU_SAMPLE_RATE / 1e6;
static const struct spu_adsr2 group_cut = {.release_rate = 1, .release_exponential = 1, .sustain_decrease = 1, .sustain_exponential = 1};

typedef struct { unsigned char *data; size_t size; } Buffer;
typedef struct { unsigned char *hd; uint32_t allocated, size, spu, body; uint32_t vag_infos, samples, sample_sets, programs; } Bank;
typedef struct { int bank, program, volume, expression, pan, bend, sustain, nrpn, data, pitch_modulation, amp_modulation; } Channel;
typedef struct { uint32_t at; unsigned running, flagged; int remaining, valid; } Loop;
typedef struct {
    Bank banks[PORT_BANKS];
    Channel channels[MIDI_CHANNELS];
    Loop loops[MIDI_LOOPS];
    Buffer sequences[PORT_SEQUENCES], resident;
    unsigned char *sequence;
    unsigned sequence_count;
    uint32_t sequence_size, begin, at, tempo;
    unsigned running, flagged, division, attribute;
    int playing, volume, effect_volume, effect_pan, effect_pitch;
    int spu_base, spu_next, bank_count;
    double due;
} Port;
typedef struct { int spu_base, descending, attribute, linked, dependents[LAYOUT_DEPENDENTS]; } Layout;
static const Layout layouts[MIDI_PORTS] = {
    {0x5210, 0, 0x3040, -1, {-1}},
    {0x7d210, 0, 0x3032, -1, {15, 2, 14, -1}},
    {0x7d210, 0, 0x3039, 14, {-1}},
    {0x5210, 0, 0x3040, -1, {-1}},
    {0, 0, 0, -1, {-1}},
    {0, 0, 0, -1, {-1}},
    {0, 0, 0, -1, {-1}},
    {0x18ae20, 0, 0x3010, -1, {-1}},
    {0x7d210, 0, 0x3035, -1, {1, 15, 2, 14, -1}},
    {0x1e0000, 0, 0x3038, -1, {-1}},
    {0x7d210, 0, 0x3037, -1, {8, 1, 15, 2, 14}},
    {0x1a82e0, 0, 0x3033, -1, {-1}},
    {0x18ae20, 0, 0x3034, -1, {-1}},
    {0x18ae20, 1, 0x3036, -1, {-1}},
    {0x7d210, 0, 0x3039, 2, {-1}},
    {0x7d210, 0, 0x3031, -1, {2, 14, -1}},
};
static const float volume_scale = 2.015748f;
typedef struct {
    unsigned stage, release, wave, phase, step, delay, remaining, fade_step, fade;
    int value;
} Lfo;
typedef struct {
    int used, released, held, port, channel, key, id, effect, bank, program, pan, low, high, effect_volume, effect_pan, effect_pitch, group, circular, priority;
    double pitch, gain;
    uint32_t serial;
    Lfo lfo[SCE_LFOS];
    int pitch_depth[SCE_LFO_SIDES], amp_depth[SCE_LFO_SIDES], midi_pitch_depth[SCE_LFO_SIDES], midi_amp_depth[SCE_LFO_SIDES];
} MidiVoice;
static Port ports[MIDI_PORTS];
static MidiVoice voices[SPU_VOICES];
static uint32_t serial, random_seed, control_clock;
static int16_t sine_wave[LFO_SINE_STEPS];
static struct { const unsigned char *hd; int port, bank, program; } missing_programs[MISSING_LIMIT];
static unsigned missing_program_count;
static unsigned diagnostics, features;
static int opened, stereo, enabled;

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }
static int fail(const char *message, unsigned value) {
    if (diagnostics++ < DIAGNOSTIC_LIMIT) fprintf(stderr, "EZMIDI: %s (0x%x)\n", message, value);
    return -1;
}
static void feature(unsigned flag, const char *message) {
    if (features & flag) return;
    features |= flag;
    fail(message, flag);
}
static int random_value(void) {
    random_seed = random_seed * RANDOM_MULTIPLIER + RANDOM_INCREMENT;
    return (int16_t)(random_seed >> 16);
}
static unsigned lfo_ticks(unsigned milliseconds) { return (milliseconds * LFO_TICK_SCALE + LFO_TICK_LENGTH / 2) / LFO_TICK_LENGTH; }
static void lfo_begin(Lfo *lfo) {
    lfo->remaining = lfo->delay;
    lfo->fade = 0;
    lfo->stage = lfo->delay ? LFO_DELAY : lfo->fade_step ? LFO_FADE_IN : LFO_RUNNING;
}
static void lfo_init(Lfo *lfo, unsigned wave, unsigned phase, unsigned random, unsigned period, unsigned delay, unsigned fade, unsigned trigger) {
    memset(lfo, 0, sizeof(*lfo));
    if (wave == SCE_LFO_WAVE_OFF || !trigger) return;
    if (wave > SCE_LFO_WAVE_SINE || !period) { feature(FEATURE_LFO, "unsupported LFO waveform or zero period"); return; }
    unsigned ticks = lfo_ticks(period);
    if (!ticks) { feature(FEATURE_LFO, "LFO period shorter than synth tick"); return; }
    lfo->wave = wave;
    lfo->step = LFO_CYCLE / ticks;
    lfo->delay = lfo_ticks(delay);
    ticks = lfo_ticks(fade);
    lfo->fade_step = ticks ? LFO_CYCLE / ticks : 0;
    if (ticks && !lfo->fade_step) lfo->fade_step = 1;
    int start = (int)(phase * 256) + (int)(random * 256) * random_value() / LFO_CYCLE;
    lfo->phase = (unsigned)start & (LFO_CYCLE - 1);
    lfo->release = !!(trigger & (SCE_LFO_TRIGGER_KEY_OFF | SCE_LFO_TRIGGER_BOTH));
    if (trigger & (SCE_LFO_TRIGGER_KEY_ON | SCE_LFO_TRIGGER_BOTH)) lfo_begin(lfo);
}
static int lfo_wave(Lfo *lfo, int wrapped) {
    int phase = (int)lfo->phase;
    switch (lfo->wave) {
        case SCE_LFO_WAVE_SAW_UP: return (phase - 32768) >> 1;
        case SCE_LFO_WAVE_SAW_DOWN: return (32767 - phase) >> 1;
        case SCE_LFO_WAVE_TRIANGLE: return phase > 49151 ? phase - LFO_CYCLE : phase <= 16384 ? phase : 32768 - phase;
        case SCE_LFO_WAVE_SQUARE: return phase > 32767 ? 16384 : -16384;
        case SCE_LFO_WAVE_NOISE: return wrapped ? random_value() >> 2 : lfo->value;
        case SCE_LFO_WAVE_SINE: {
            unsigned position = lfo->phase * LFO_SINE_STEPS;
            unsigned index = position >> 16;
            unsigned fraction = position & (LFO_CYCLE - 1);
            int a = sine_wave[index], b = sine_wave[(index + 1) % LFO_SINE_STEPS];
            return (a + (((b - a) * (int)fraction) >> 16)) / 2;
        }
        default: return 0;
    }
}
static void lfo_tick(Lfo *lfo) {
    if (lfo->stage == LFO_IDLE || lfo->stage >= LFO_DONE) { lfo->value = 0; return; }
    int value = 0;
    if (lfo->stage != LFO_DELAY) {
        lfo->phase += lfo->step;
        int wrapped = lfo->phase >= LFO_CYCLE;
        lfo->phase &= LFO_CYCLE - 1;
        value = lfo_wave(lfo, wrapped);
    }
    switch (lfo->stage) {
        case LFO_DELAY:
            if (!--lfo->remaining) { lfo->fade = 0; lfo->stage = lfo->fade_step ? LFO_FADE_IN : LFO_RUNNING; }
            break;
        case LFO_FADE_IN:
            value = value * (int)lfo->fade / LFO_CYCLE;
            lfo->fade += lfo->fade_step;
            if (lfo->fade >= LFO_CYCLE) lfo->stage = LFO_RUNNING;
            break;
        case LFO_RELEASE_DELAY:
            if (!--lfo->remaining) { lfo->fade = LFO_CYCLE; lfo->stage = lfo->fade_step ? LFO_FADE_OUT : LFO_DONE; }
            break;
        case LFO_FADE_OUT:
            value = value * (int)lfo->fade / LFO_CYCLE;
            if (lfo->fade <= lfo->fade_step) lfo->stage = LFO_DONE;
            else lfo->fade -= lfo->fade_step;
            break;
        default: break;
    }
    lfo->value = value;
}
static void lfo_release(Lfo *lfo) {
    if (!lfo->release) return;
    if (lfo->stage == LFO_IDLE) lfo_begin(lfo);
    else if (lfo->stage == LFO_DELAY) lfo->stage = LFO_DONE;
    else if (lfo->stage == LFO_FADE_IN || lfo->stage == LFO_RUNNING) {
        lfo->remaining = lfo->delay;
        lfo->fade = LFO_CYCLE;
        lfo->stage = lfo->delay ? LFO_RELEASE_DELAY : lfo->fade_step ? LFO_FADE_OUT : LFO_DONE;
    }
}
static void init_channel(Channel *c) {
    memset(c, 0, sizeof(*c));
    c->volume = MIDI_VALUE_MAX;
    c->expression = MIDI_VALUE_MAX;
    c->pan = MIDI_PAN_CENTER;
    c->bend = MIDI_BEND_CENTER;
    c->nrpn = -1;
    c->data = -1;
}
static void release_banks(Port *p) {
    for (unsigned i = 0; i < PORT_BANKS; i++) {
        free(p->banks[i].hd);
        memset(&p->banks[i], 0, sizeof(p->banks[i]));
    }
    p->bank_count = 0;
}
static void release_sequences(Port *p) {
    for (unsigned i = 1; i < p->sequence_count; i++) {
        free(p->sequences[i].data);
        p->sequences[i].data = NULL;
    }
    p->sequence_count = 0;
}
static void release_port(Port *p) {
    release_banks(p);
    release_sequences(p);
    free(p->resident.data);
    p->resident.data = NULL;
    p->sequence = NULL;
}
static Buffer *sequence_at(Port *p, unsigned index) { return index ? &p->sequences[index] : &p->resident; }
static void layout_port(unsigned port) {
    ports[port].spu_base = ports[port].spu_next = layouts[port].spu_base;
}
void midi_reset(void) {
    for (unsigned i = 0; i < SPU_VOICES; i++) if (voices[i].used) spu_voice_stop(i);
    for (unsigned p = 0; p < MIDI_PORTS; p++) release_port(&ports[p]);
    memset(ports, 0, sizeof(ports));
    memset(voices, 0, sizeof(voices));
    for (unsigned p = 0; p < MIDI_PORTS; p++) {
        ports[p].volume = PORT_VOLUME_FULL;
        ports[p].effect_volume = EFFECT_VOLUME_FULL;
        ports[p].effect_pan = MIDI_PAN_CENTER;
        ports[p].effect_pitch = MIDI_BEND_CENTER;
        ports[p].tempo = MIDI_DEFAULT_TEMPO;
        layout_port(p);
        for (unsigned c = 0; c < MIDI_CHANNELS; c++) init_channel(&ports[p].channels[c]);
    }
    serial = 0;
    random_seed = RANDOM_SEED;
    control_clock = 0;
    for (unsigned i = 0; i < LFO_SINE_STEPS; i++) sine_wave[i] = (int16_t)(32767.0 * sin(6.2831853071795864769 * i / (double)LFO_SINE_STEPS));
    diagnostics = 0;
    features = 0;
    missing_program_count = 0;
    stereo = 1;
    enabled = 1;
    opened = 0;
}
static int chunk_is(const struct sce_chunk *chunk, const char *type) {
    return !memcmp(chunk->creator, SCE_CREATOR, sizeof chunk->creator) && !memcmp(chunk->type, type, sizeof chunk->type);
}
static int bank_read(const Bank *b, uint32_t at, void *out, size_t size) {
    if (!b->hd || at > b->size || size > b->size - at) return 0;
    memcpy(out, b->hd + at, size);
    return 1;
}
static const struct sce_table *bank_table(const Bank *b, uint32_t at) {
    if (!b->hd || at > b->size || sizeof(struct sce_table) > b->size - at || at % _Alignof(struct sce_table)) return NULL;
    return (const struct sce_table *)(b->hd + at);
}
static uint32_t record(const Bank *b, uint32_t table, unsigned index, void *out, size_t size) {
    const struct sce_table *t = bank_table(b, table);
    if (!t || index > t->max_index) return 0;
    uint32_t offset = t->offsets[index];
    if (offset == SCE_TABLE_NONE || offset > b->size - table) return 0;
    return bank_read(b, table + offset, out, size) ? table + offset : 0;
}
static void update_voice(unsigned i) {
    MidiVoice *v = &voices[i];
    if (!v->used || !spu_voice_active(i)) { v->used = 0; return; }
    Port *p = &ports[v->port];
    Channel *c = &p->channels[v->channel];
    int bend = (v->effect ? v->effect_pitch : c->bend) - MIDI_BEND_CENTER;
    int range = bend < 0 ? v->low : v->high;
    unsigned pitch_side = v->lfo[SCE_LFO_PITCH].value < 0 ? SCE_LFO_NEGATIVE : SCE_LFO_POSITIVE;
    int pitch_depth = clamp(v->pitch_depth[pitch_side] + c->pitch_modulation * v->midi_pitch_depth[pitch_side] / 128, INT16_MIN, INT16_MAX);
    int pitch_lfo = pitch_depth * v->lfo[SCE_LFO_PITCH].value / 16384;
    int bend_fine = bend * range / MIDI_BEND_CENTER;
    double pitch = SPU_PITCH_UNITY * exp2((v->pitch + (bend_fine + pitch_lfo) / 128.0) / 12.0);
    double gain = v->gain * clamp(p->volume, 0, PORT_VOLUME_FULL) / (double)PORT_VOLUME_FULL;
    unsigned amp_side = v->lfo[SCE_LFO_AMP].value < 0 ? SCE_LFO_NEGATIVE : SCE_LFO_POSITIVE;
    int amp_depth = clamp(v->amp_depth[amp_side] + c->amp_modulation * v->midi_amp_depth[amp_side] / 128, INT8_MIN, INT8_MAX);
    int amp_factor = 16384 + v->lfo[SCE_LFO_AMP].value * amp_depth / 128;
    gain *= amp_factor > 0 ? amp_factor / 16384.0 : 0;
    gain *= v->effect ? v->effect_volume / (double)EFFECT_VOLUME_FULL : c->volume / (double)MIDI_VALUE_MAX * c->expression / MIDI_VALUE_MAX;
    int channel_pan = clamp((v->effect ? v->effect_pan : c->pan) - MIDI_PAN_CENTER, -63, 63);
    int pan = v->pan + channel_pan;
    int phase = 1;
    if (v->circular) {
        phase = v->pan >= 0;
        pan = (v->pan < 0 ? -v->pan : v->pan) + channel_pan;
        if (pan < 0) { phase ^= 1; pan = -pan; }
        else if (pan >= 128) { phase ^= 1; pan = 256 - pan; }
        pan -= 64;
    }
    pan = clamp(pan, -63, 63);
    int left = clamp((int)((double)SPU_VOLUME_MAX * gain * (pan > 0 && stereo ? (63 - pan) / 63.0 : 1.0)), 0, SPU_VOLUME_MAX);
    int right = clamp((int)((double)SPU_VOLUME_MAX * gain * (pan < 0 && stereo ? (63 + pan) / 63.0 : 1.0)), 0, SPU_VOLUME_MAX);
    if (!phase && stereo) { left = -left; right = -right; }
    spu_voice_volume(i, left, right);
    spu_voice_pitch(i, pitch < 1 ? 1 : pitch > SPU_PITCH_MAX ? SPU_PITCH_MAX : (unsigned)pitch);
}
static void update_port(unsigned port) { for (unsigned i = 0; i < SPU_VOICES; i++) if (voices[i].port == (int)port) update_voice(i); }
static void stop_port(unsigned port) {
    ports[port].playing = 0;
    for (unsigned i = 0; i < SPU_VOICES; i++) if (voices[i].used && voices[i].port == (int)port) {
        spu_voice_stop(i);
        voices[i].released = 1;
    }
}
static void note_off(unsigned port, unsigned channel, unsigned key, int id, int effect) {
    for (unsigned i = 0; i < SPU_VOICES; i++) {
        MidiVoice *v = &voices[i];
        if (!v->used || v->port != (int)port || v->channel != (int)channel || v->key != (int)key || v->effect != effect) continue;
        if (effect && (v->id != id || v->bank != ports[port].channels[channel].bank || v->program != ports[port].channels[channel].program)) continue;
        if (!effect && ports[port].channels[channel].sustain) v->held = 1;
        else { spu_voice_stop(i); v->released = 1; lfo_release(&v->lfo[SCE_LFO_PITCH]); lfo_release(&v->lfo[SCE_LFO_AMP]); }
    }
}
static void stop_group(unsigned port, unsigned channel, unsigned group) {
    if (!group) return;
    for (unsigned i = 0; i < SPU_VOICES; i++) {
        MidiVoice *v = &voices[i];
        if (!v->used || v->released || v->port != (int)port || v->channel != (int)channel || v->group != (int)group) continue;
        spu_voice_adsr2(i, group_cut);
        spu_voice_stop(i);
        v->released = 1;
        v->held = 0;
    }
}
static unsigned follow_key(unsigned value, unsigned key, struct sce_key_follow follow, unsigned limit) {
    int amount = ((int)key - follow.center) * follow.amount / 12;
    return (unsigned)clamp((int)value + amount, 0, (int)limit);
}
static int prefer_voice(unsigned candidate, unsigned selected) {
    MidiVoice *v = &voices[candidate], *old = &voices[selected];
    unsigned category = v->released ? 0 : v->held ? 1 : 2;
    unsigned old_category = old->released ? 0 : old->held ? 1 : 2;
    if (category != old_category) return category < old_category;
    if (v->priority != old->priority) return v->priority < old->priority;
    unsigned level = spu_voice_level(candidate), old_level = spu_voice_level(selected);
    if (level != old_level) return level < old_level;
    return v->serial < old->serial;
}
static int allocate_voice(struct sce_spu_attribute attribute, unsigned priority) {
    unsigned core = attribute.core;
    unsigned begin = core == SCE_SAMPLE_CORE_1 ? SPU_CORE_VOICES : 0;
    unsigned end = core == SCE_SAMPLE_CORE_0 ? SPU_CORE_VOICES : SPU_VOICES;
    if (core == SCE_SAMPLE_CORE_ANY || core == SCE_SAMPLE_CORE_EITHER) {
        unsigned free[SPU_CORES] = {0, 0};
        for (unsigned i = 0; i < SPU_VOICES; i++) if (!voices[i].used || !spu_voice_active(i)) free[i / SPU_CORE_VOICES]++;
        if (free[0] || free[1]) { begin = free[1] > free[0] ? SPU_CORE_VOICES : 0; end = begin + SPU_CORE_VOICES; }
    }
    int selected = -1;
    for (unsigned i = begin; i < end; i++) {
        if (!voices[i].used || !spu_voice_active(i)) return (int)i;
        MidiVoice *v = &voices[i];
        if ((unsigned)v->priority > priority) continue;
        if (selected < 0) { selected = (int)i; continue; }
        if (prefer_voice(i, (unsigned)selected)) selected = (int)i;
    }
    if (selected >= 0) spu_voice_stop((unsigned)selected);
    return selected;
}
static void missing_program(unsigned port, unsigned channel, unsigned key, unsigned velocity, int effect, int id) {
    Channel *c = &ports[port].channels[channel];
    Bank *b = c->bank < PORT_BANKS ? &ports[port].banks[c->bank] : NULL;
    const unsigned char *hd = b ? b->hd : NULL;
    unsigned count = 0;
    uint32_t entry = SCE_TABLE_NONE;
    const char *reason = "unloaded bank";
    if (b && hd) {
        const struct sce_table *table = bank_table(b, b->programs);
        reason = "invalid program table";
        if (table) {
            count = table->max_index + 1;
            reason = "program outside table";
            if ((unsigned)c->program < count) {
                entry = table->offsets[c->program];
                if (entry == SCE_TABLE_NONE) return;
                reason = "invalid program record";
            }
        }
    }
    for (unsigned i = 0; i < missing_program_count; i++) {
        if (missing_programs[i].hd == hd && missing_programs[i].port == (int)port && missing_programs[i].bank == c->bank && missing_programs[i].program == c->program) return;
    }
    if (missing_program_count == MISSING_LIMIT) return;
    missing_programs[missing_program_count].hd = hd;
    missing_programs[missing_program_count].port = (int)port;
    missing_programs[missing_program_count].bank = c->bank;
    missing_programs[missing_program_count++].program = c->program;
    fprintf(stderr, "EZMIDI: %s port=%u bank=%d channel=%u program=%d key=%u velocity=%u effect=%d id=%d hd=%p table=0x%08x table_index=%d table_count=%u entry=0x%08x\n", reason, port, c->bank, channel, c->program, key, velocity, effect, id, (const void *)hd, b ? b->programs : 0, c->program, count, entry);
}
static void note_on(unsigned port, unsigned channel, unsigned key, unsigned velocity, int id, int effect) {
    if (!velocity) { note_off(port, channel, key, id, effect); return; }
    Port *p = &ports[port];
    Channel *c = &p->channels[channel];
    if (c->bank >= PORT_BANKS) { missing_program(port, channel, key, velocity, effect, id); return; }
    Bank *b = &p->banks[c->bank];
    struct sce_program prog;
    uint32_t prog_at = record(b, b->programs, (unsigned)c->program, &prog, sizeof prog);
    if (!prog_at) { missing_program(port, channel, key, velocity, effect, id); return; }
    if (prog.split_block_addr > b->size - prog_at || prog.split_block_size < sizeof(struct sce_split)) { fail("invalid split table", prog.split_block_addr); return; }
    for (unsigned j = 0; j < prog.n_split; j++) {
        struct sce_split split;
        if (!bank_read(b, prog_at + prog.split_block_addr + j * prog.split_block_size, &split, sizeof split) || key < split.range_low || key > split.range_high) continue;
        struct sce_sample_set set;
        uint32_t set_at = record(b, b->sample_sets, split.sample_set, &set, offsetof(struct sce_sample_set, samples));
        if (!set_at || velocity < set.vel_limit_low || velocity > set.vel_limit_high) continue;
        if (!bank_read(b, set_at, &set, offsetof(struct sce_sample_set, samples) + set.sample_count * sizeof set.samples[0])) continue;
        for (unsigned k = 0; k < set.sample_count; k++) {
            struct sce_sample sample;
            if (!record(b, b->samples, set.samples[k], &sample, sizeof sample) || velocity < sample.vel_range_low || velocity > sample.vel_range_high) continue;
            struct sce_vag_info vag;
            if (!record(b, b->vag_infos, sample.vag_index, &vag, sizeof vag) || vag.offset >= b->body || !vag.rate) continue;
            if (split.key_follow_pitch.amount || split.key_follow_amp.amount || sample.vel_follow_pitch || sample.vel_follow_amp) feature(FEATURE_FOLLOW, "key/velocity follow modulation is approximated");
            if (set.vel_curve != SCE_VELOCITY_CURVE_LINEAR && set.vel_curve != SCE_VELOCITY_CURVE_SQUARE) feature(FEATURE_VELOCITY_CURVE, "sample velocity curve is approximated");
            stop_group(port, channel, sample.group);
            unsigned priority = sample.priority + (p->attribute & UINT8_MAX);
            int selected = allocate_voice(sample.spu_attribute, priority);
            if (selected < 0) continue;
            unsigned i = (unsigned)selected;
            MidiVoice *v = &voices[i];
            memset(v, 0, sizeof(*v));
            v->used = 1; v->port = (int)port; v->channel = (int)channel; v->key = (int)key; v->id = id; v->effect = effect;
            v->group = sample.group; v->priority = (int)priority;
            v->bank = c->bank; v->program = c->program; v->serial = ++serial;
            v->effect_volume = p->effect_volume; v->effect_pan = p->effect_pan; v->effect_pitch = p->effect_pitch;
            v->pitch = (int)key - sample.base_note + prog.transpose + split.transpose + (prog.detune + split.detune + sample.detune) / 128.0 + 12 * log2(vag.rate / (double)SPU_SAMPLE_RATE);
            int curved = set.vel_curve == SCE_VELOCITY_CURVE_SQUARE ? clamp((int)(velocity * velocity / MIDI_VALUE_MAX), 1, MIDI_VALUE_MAX) : (int)velocity;
            v->gain = curved / (double)MIDI_VALUE_MAX * prog.volume / 128.0 * split.volume / 128.0 * sample.volume / 128.0;
            int key_pan = (((int)key - prog.key_follow_pan.center) * prog.key_follow_pan.amount + ((int)key - split.key_follow_pan.center) * split.key_follow_pan.amount) / 12;
            int program_pan = (int8_t)prog.panpot, split_pan = (int8_t)split.panpot, sample_pan = (int8_t)sample.panpot;
            if (!prog.attribute) {
                if (program_pan < 0) program_pan = -program_pan;
                if (split_pan < 0) split_pan = -split_pan;
                if (sample_pan < 0) sample_pan = -sample_pan;
            }
            v->circular = prog.attribute & SCE_PROGRAM_PAN_CIRCULAR;
            v->pan = program_pan + split_pan + sample_pan - 128 + key_pan;
            if (v->circular) {
                while (v->pan >= 128) v->pan -= 255;
                while (v->pan < -128) v->pan += 255;
            } else v->pan = clamp(v->pan, 0, MIDI_VALUE_MAX) - MIDI_PAN_CENTER;
            v->low = split.bend_range_low; v->high = split.bend_range_high;
            for (unsigned side = 0; side < SCE_LFO_SIDES; side++) {
                v->pitch_depth[side] = prog.lfo_pitch_depth[side];
                v->amp_depth[side] = prog.lfo_amp_depth[side];
                v->midi_pitch_depth[side] = prog.lfo_midi_pitch_depth[side];
                v->midi_amp_depth[side] = prog.lfo_midi_amp_depth[side];
            }
            lfo_init(&v->lfo[SCE_LFO_PITCH], prog.lfo_wave[SCE_LFO_PITCH], prog.lfo_start_phase[SCE_LFO_PITCH], prog.lfo_phase_random[SCE_LFO_PITCH],
                     prog.lfo_cycle[SCE_LFO_PITCH], sample.pitch_lfo_delay, sample.pitch_lfo_fade, sample.lfo_attribute.pitch);
            lfo_init(&v->lfo[SCE_LFO_AMP], prog.lfo_wave[SCE_LFO_AMP], prog.lfo_start_phase[SCE_LFO_AMP], prog.lfo_phase_random[SCE_LFO_AMP],
                     prog.lfo_cycle[SCE_LFO_AMP], sample.amp_lfo_delay, sample.amp_lfo_fade, sample.lfo_attribute.amp);
            struct spu_adsr1 adsr1 = sample.adsr1;
            adsr1.attack_rate = follow_key(adsr1.attack_rate, key, sample.key_follow_ar, SPU_ATTACK_RATE_MAX);
            adsr1.decay_rate = follow_key(adsr1.decay_rate, key, sample.key_follow_dr, SPU_DECAY_RATE_MAX);
            adsr1.sustain_level = follow_key(adsr1.sustain_level, key, sample.key_follow_sl, SPU_SUSTAIN_LEVEL_MAX);
            struct spu_adsr2 adsr2 = sample.adsr2;
            adsr2.sustain_rate = follow_key(adsr2.sustain_rate, key, sample.key_follow_sr, SPU_SUSTAIN_RATE_MAX);
            adsr2.release_rate = follow_key(adsr2.release_rate, key, sample.key_follow_rr, SPU_RELEASE_RATE_MAX);
            spu_voice_start(i, b->spu + vag.offset, SPU_PITCH_UNITY, 0, 0, adsr1, adsr2);
            spu_voice_route(i, sample.spu_attribute.route);
            update_voice(i);
        }
    }
}
static void release_held(unsigned port, unsigned channel) {
    for (unsigned i = 0; i < SPU_VOICES; i++) if (voices[i].used && voices[i].port == (int)port && voices[i].channel == (int)channel && voices[i].held) {
        voices[i].held = 0; voices[i].released = 1; spu_voice_stop(i); lfo_release(&voices[i].lfo[SCE_LFO_PITCH]); lfo_release(&voices[i].lfo[SCE_LFO_AMP]);
    }
}
static int channel_message(unsigned port, unsigned status, unsigned a, unsigned b) {
    unsigned channel = status & MIDI_STATUS_CHANNEL;
    Port *p = &ports[port];
    Channel *c = &p->channels[channel];
    switch (status & MIDI_STATUS_KIND) {
        case MIDI_NOTE_OFF: note_off(port, channel, a, -1, 0); break;
        case MIDI_NOTE_ON: note_on(port, channel, a, b, -1, 0); break;
        case MIDI_PROGRAM: c->program = (int)a; break;
        case MIDI_PITCH_BEND: c->bend = (int)(a | b << MIDI_DATA_BITS); update_port(port); break;
        case MIDI_CONTROL:
            switch (a) {
                case MIDI_CONTROL_BANK: c->bank = (int)b; break;
                case MIDI_CONTROL_MODULATION: c->pitch_modulation = (int)(b * 128 / MIDI_VALUE_MAX); update_port(port); break;
                case MIDI_CONTROL_BREATH: c->amp_modulation = (int)(b * 128 / MIDI_VALUE_MAX); update_port(port); break;
                case MIDI_CONTROL_VOLUME: c->volume = (int)b; update_port(port); break;
                case MIDI_CONTROL_PAN: c->pan = (int)b; update_port(port); break;
                case MIDI_CONTROL_EXPRESSION: c->expression = (int)b; update_port(port); break;
                case MIDI_CONTROL_SUSTAIN:
                    c->sustain = b >= MIDI_SWITCH_ON;
                    if (!c->sustain) release_held(port, channel);
                    break;
                case MIDI_CONTROL_ALL_SOUND_OFF: case MIDI_CONTROL_ALL_NOTES_OFF:
                    for (unsigned key = 0; key < MIDI_KEYS; key++) note_off(port, channel, key, -1, 0);
                    break;
                case MIDI_CONTROL_RESET: init_channel(c); update_port(port); break;
                case MIDI_CONTROL_NRPN: c->nrpn = (int)b; c->data = -1; break;
                case MIDI_CONTROL_DATA_ENTRY:
                    c->data = (int)b;
                    if (c->nrpn == MIDI_NRPN_LOOP_START && p->sequence) {
                        Loop *loop = &p->loops[b];
                        loop->at = p->at; loop->running = p->running; loop->flagged = p->flagged;
                        loop->remaining = -1; loop->valid = 1;
                    }
                    break;
                case MIDI_CONTROL_DATA_ENTRY_LSB:
                    if (c->nrpn == MIDI_NRPN_LOOP_END && c->data >= 0) {
                        Loop *loop = &p->loops[c->data];
                        if (!loop->valid) return fail("loop without start", (unsigned)c->data);
                        if (loop->remaining < 0) loop->remaining = (int)b;
                        if (b && !loop->remaining) { loop->remaining = -1; break; }
                        p->at = loop->at; p->running = loop->running; p->flagged = loop->flagged;
                        if (b) loop->remaining--;
                    }
                    break;
                case MIDI_CONTROL_BANK_LSB: case MIDI_CONTROL_REVERB: case MIDI_CONTROL_CHORUS: break;
                default: return fail("unsupported controller", a);
            }
            break;
        case MIDI_KEY_PRESSURE: case MIDI_CHANNEL_PRESSURE: return fail("unsupported pressure", status);
        default: return fail("unsupported MIDI status", status);
    }
    return 0;
}
static void select_program(unsigned port, int bank, int program) {
    channel_message(port, MIDI_CONTROL, MIDI_CONTROL_BANK, (unsigned)bank & MIDI_DATA);
    channel_message(port, MIDI_PROGRAM, (unsigned)program & MIDI_DATA, 0);
}
static int effect_ready(unsigned port, int id, int needs_bank) {
    if (id > EFFECT_ID_MAX || port < FIRST_EFFECT_PORT || port >= MIDI_PORTS) return 0;
    return !needs_bank || ports[port].bank_count > 0;
}
static void effect_update(unsigned port, unsigned kind, unsigned key, unsigned id, unsigned a, unsigned b) {
    Port *p = &ports[port];
    for (unsigned i = 0; i < SPU_VOICES; i++) {
        MidiVoice *v = &voices[i];
        if (!v->used || !v->effect || v->port != (int)port || v->channel != 0 || v->key != (int)key || v->id != (int)id || v->bank != p->channels[0].bank || v->program != p->channels[0].program) continue;
        if (kind == EFFECT_VOLUME) v->effect_volume = (int)a;
        else if (kind == EFFECT_PAN) v->effect_pan = (int)a;
        else v->effect_pitch = (int)(a | b << MIDI_DATA_BITS);
        update_voice(i);
    }
}
void midi_effect_play(unsigned port, int bank, int program, int key, int pan, int velocity, int volume, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    ports[port].effect_volume = (unsigned char)volume;
    ports[port].effect_pan = (unsigned char)pan;
    note_on(port, 0, (unsigned char)key & MIDI_DATA, (unsigned char)velocity & MIDI_DATA, (unsigned char)id, 1);
}
void midi_effect_stop(unsigned port, int bank, int program, int key, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    note_on(port, 0, (unsigned char)key & MIDI_DATA, 0, (unsigned char)id, 1);
}
void midi_effect_volume(unsigned port, int bank, int program, int key, int volume, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    effect_update(port, EFFECT_VOLUME, (unsigned char)key, (unsigned char)id, (unsigned char)volume, 0);
}
void midi_effect_pan(unsigned port, int bank, int program, int key, int pan, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    effect_update(port, EFFECT_PAN, (unsigned char)key, (unsigned char)id, (unsigned char)pan, 0);
}
void midi_effect_pitch(unsigned port, int bank, int program, int key, int pitch, int id) {
    if (!effect_ready(port, id, 0)) return;
    select_program(port, bank, program);
    effect_update(port, EFFECT_PITCH, (unsigned char)key, (unsigned char)id, (unsigned)pitch & MIDI_DATA, (unsigned)(pitch >> MIDI_DATA_BITS) & MIDI_DATA);
}
int midi_message(unsigned port, unsigned status, unsigned a, unsigned b) {
    if (port >= MIDI_PORTS) return -1;
    return channel_message(port, (uint8_t)status, a & MIDI_DATA, b & MIDI_DATA);
}
static int read_byte(Port *p, unsigned *value) {
    if (p->at >= p->sequence_size) return -1;
    *value = p->sequence[p->at];
    p->at++;
    return 0;
}
static int read_vlq(Port *p, uint32_t *value) {
    *value = 0;
    for (unsigned i = 0; i < VLQ_BYTES; i++) {
        unsigned b;
        if (read_byte(p, &b)) return -1;
        *value = *value << MIDI_DATA_BITS | (b & MIDI_DATA);
        if (!(b & VLQ_CONTINUE)) return 0;
    }
    return -1;
}
static int next_delta(Port *p) {
    uint32_t delta = 0;
    if (!p->flagged && read_vlq(p, &delta)) return -1;
    p->due += delta * (double)p->tempo * ticks_per_microsecond / p->division;
    return 0;
}
static int event(unsigned port) {
    Port *p = &ports[port];
    unsigned status, a, b = 0;
    if (read_byte(p, &status)) return -1;
    int written = status >= MIDI_STATUS_BIT;
    if (!written) { p->at--; status = p->running; }
    p->flagged = 0;
    if (status == MIDI_META) {
        uint32_t length;
        if (read_byte(p, &a) || read_vlq(p, &length) || length > p->sequence_size - p->at) return -1;
        const unsigned char *data = p->sequence + p->at;
        if (a == MIDI_META_TEMPO && length == MIDI_TEMPO_BYTES) {
            uint32_t tempo = (uint32_t)data[0] << 16 | (uint32_t)data[1] << 8 | data[2];
            if (!tempo) return -1;
            p->tempo = tempo;
        }
        p->at += length;
        p->running = 0;
        if (a == MIDI_META_END_OF_TRACK) { p->playing = 0; return 0; }
    } else {
        if (status < MIDI_STATUS_BIT || status >= MIDI_SYSTEM || read_byte(p, &a)) return -1;
        unsigned kind = status & MIDI_STATUS_KIND;
        if (kind != MIDI_NOTE_OFF && kind != MIDI_PROGRAM && kind != MIDI_CHANNEL_PRESSURE) {
            if (a & MIDI_STATUS_BIT || read_byte(p, &b)) return -1;
            p->flagged = !!(b & SQ_SKIP_DELTA);
        } else p->flagged = !!(a & SQ_SKIP_DELTA);
        if (p->flagged && !written) return -1;
        p->running = status;
        channel_message(port, status, a & MIDI_DATA, b & MIDI_DATA);
    }
    if (p->playing && next_delta(p)) return -1;
    return 0;
}
static int rewind_port(unsigned port) {
    Port *p = &ports[port];
    p->playing = 0;
    p->at = p->begin;
    p->tempo = MIDI_DEFAULT_TEMPO;
    p->running = p->flagged = 0;
    p->due = 0;
    memset(p->loops, 0, sizeof(p->loops));
    for (unsigned c = 0; c < MIDI_CHANNELS; c++) init_channel(&p->channels[c]);
    return p->sequence && p->division ? next_delta(p) : -1;
}
static int set_sequence(unsigned port, const Buffer *buffer) {
    Port *p = &ports[port];
    stop_port(port);
    p->sequence = NULL;
    const unsigned char *data = buffer->data;
    size_t size = buffer->size;
    struct sce_sq_head head;
    if (!data || size < sizeof head + sizeof(struct sce_sq_midi_block) || size > UINT32_MAX) return fail("invalid SQ header", (unsigned)size);
    memcpy(&head, data, sizeof head);
    if (!chunk_is(&head.vers.chunk, "sreV") || !chunk_is(&head.midi.chunk, "idiM")) return fail("invalid SQ header", (unsigned)size);
    uint32_t block = head.midi.data_offset;
    if (block > size - offsetof(struct sce_sq_head, midi) - sizeof(struct sce_sq_midi_block)) return -1;
    block += offsetof(struct sce_sq_head, midi);
    struct sce_sq_midi_block midi;
    memcpy(&midi, data + block, sizeof midi);
    if (midi.sequence_data_offset < sizeof midi || midi.sequence_data_offset > size - block) return -1;
    if (!midi.division || midi.division >= SCE_SQ_DIVISION_SMPTE) return -1;
    p->sequence = buffer->data; p->sequence_size = (uint32_t)size; p->division = midi.division; p->begin = block + midi.sequence_data_offset;
    return rewind_port(port);
}
static Buffer padded_copy(const void *data, int size) {
    Buffer buffer = {NULL, 0};
    if (!data || size <= 0 || size > SEQUENCE_LIMIT) return buffer;
    size_t allocated = size <= SEQUENCE_PADDING ? SEQUENCE_PADDING : (size_t)size + SEQUENCE_PADDING;
    buffer.data = calloc(1, allocated);
    if (buffer.data) {
        memcpy(buffer.data, data, (size_t)size);
        buffer.size = allocated;
    }
    return buffer;
}
static int attach_bank(unsigned port, unsigned index, const Buffer *hd, unsigned spu, unsigned body) {
    if (index >= PORT_BANKS) return -1;
    Bank b;
    memset(&b, 0, sizeof(b));
    b.hd = hd->data; b.body = body; b.spu = spu;
    if (b.spu > SPU_RAM_SIZE || b.body > SPU_RAM_SIZE - b.spu) return fail("bank body exceeds SPU memory", b.spu);
    if (!hd->data || hd->size < sizeof(struct sce_hd_header) || hd->size > UINT32_MAX) return fail("missing HD allocation", (unsigned)hd->size);
    b.allocated = b.size = (uint32_t)hd->size;
    struct sce_hd_header header;
    memcpy(&header, hd->data, sizeof header);
    if (!chunk_is(&header.head.chunk, "daeH")) return fail("invalid HD header", index);
    b.size = header.head.header_size;
    if (b.size > b.allocated || b.size < sizeof header || header.head.body_size > b.body) return -1;
    b.vag_infos = header.head.vag_info_chunk;
    b.samples = header.head.sample_chunk;
    b.sample_sets = header.head.sample_set_chunk;
    b.programs = header.head.program_chunk;
    const struct { uint32_t at; const char *type; } tables[] = {{b.vag_infos, "igaV"}, {b.samples, "lpmS"}, {b.sample_sets, "tesS"}, {b.programs, "gorP"}};
    for (unsigned i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        const struct sce_table *table = bank_table(&b, tables[i].at);
        if (!table || !chunk_is(&table->chunk, tables[i].type) || table->max_index >= (b.size - tables[i].at - sizeof *table) / sizeof table->offsets[0]) return fail("invalid HD table", i);
    }
    b.hd = malloc(b.allocated);
    if (!b.hd) return -1;
    memcpy(b.hd, hd->data, b.allocated);
    free(ports[port].banks[index].hd);
    ports[port].banks[index] = b;
    return 0;
}
static void drop_stale_banks(unsigned port, int first_failed) {
    for (unsigned i = first_failed ? 0 : 1; i < PORT_BANKS; i++) {
        free(ports[port].banks[i].hd);
        memset(&ports[port].banks[i], 0, sizeof(ports[port].banks[i]));
    }
}
static Buffer stage_bank(const void *hd, int hd_size, const void *bd, int bd_size, int spu) {
    Buffer staged = {NULL, 0};
    if (bd_size == 0) { fail("bank without body", (unsigned)spu); return staged; }
    if (hd_size == 0) { fail("bank without header", (unsigned)spu); return staged; }
    staged = padded_copy(hd, hd_size);
    spu_ram_write((unsigned)spu, bd, (size_t)bd_size);
    return staged;
}
static void restart_sequence(unsigned port) {
    Port *p = &ports[port];
    if (p->sequence_count) set_sequence(port, &p->resident);
    release_sequences(p);
}
static void share_address(const Layout *layout, int address) {
    for (unsigned i = 0; i < LAYOUT_DEPENDENTS && layout->dependents[i] >= 0; i++)
        ports[layout->dependents[i]].spu_base = ports[layout->dependents[i]].spu_next = address;
}
int midi_bank_first(unsigned port, const void *hd, int hd_size, const void *bd, int bd_size) {
    if (port >= MIDI_PORTS) return -1;
    Port *p = &ports[port];
    const Layout *layout = &layouts[port];
    int length = bd_size + BANK_BODY_GAP;
    int spu = layout->descending ? p->spu_base - length : p->spu_base;
    p->spu_next = layout->descending ? p->spu_base - length : p->spu_base + length;
    Buffer staged = stage_bank(hd, hd_size, bd, bd_size, spu);
    stop_port(port);
    int result = attach_bank(port, 0, &staged, (unsigned)spu, (unsigned)bd_size);
    int linked_result = 0;
    if (layout->linked >= 0) {
        stop_port((unsigned)layout->linked);
        linked_result = attach_bank((unsigned)layout->linked, 0, &staged, (unsigned)spu, (unsigned)bd_size);
    }
    free(staged.data);
    drop_stale_banks(port, result != 0);
    p->bank_count = 0;
    restart_sequence(port);
    if (layout->linked >= 0) {
        drop_stale_banks((unsigned)layout->linked, linked_result != 0);
        ports[layout->linked].bank_count = 0;
        restart_sequence((unsigned)layout->linked);
    }
    for (unsigned i = 0; i < LAYOUT_DEPENDENTS && layout->dependents[i] >= 0; i++) stop_port((unsigned)layout->dependents[i]);
    p->attribute = (unsigned)layout->attribute;
    if (layout->linked >= 0) ports[layout->linked].attribute = (unsigned)layouts[layout->linked].attribute;
    share_address(layout, p->spu_next);
    if (layout->linked >= 0) {
        ports[layout->linked].spu_next = p->spu_next;
        ports[layout->linked].bank_count++;
    }
    p->bank_count++;
    return result;
}
int midi_bank_add(unsigned port, const void *hd, int hd_size, const void *bd, int bd_size) {
    if (port >= MIDI_PORTS) return -1;
    Port *p = &ports[port];
    const Layout *layout = &layouts[port];
    if (p->bank_count >= PORT_BANKS) return 0;
    unsigned index = (unsigned)p->bank_count;
    int length = bd_size + BANK_BODY_GAP;
    int spu = layout->descending ? p->spu_next - length : p->spu_next;
    p->spu_next = layout->descending ? p->spu_next - length : p->spu_next + length;
    Buffer staged = stage_bank(hd, hd_size, bd, bd_size, spu);
    share_address(layout, p->spu_next);
    int result = attach_bank(port, index, &staged, (unsigned)spu, (unsigned)bd_size);
    if (layout->linked >= 0) {
        result = attach_bank((unsigned)layout->linked, index, &staged, (unsigned)spu, (unsigned)bd_size);
        ports[layout->linked].spu_next = p->spu_next;
        ports[layout->linked].bank_count++;
    }
    free(staged.data);
    p->bank_count++;
    return result;
}
int midi_sequence_load(unsigned port, const void *data, int size) {
    if (port >= MIDI_PORTS) return -1;
    Port *p = &ports[port];
    stop_port(port);
    Buffer fresh = padded_copy(data, size);
    if (!fresh.data) return -1;
    if (p->sequence_count >= PORT_SEQUENCES) { free(fresh.data); return -1; }
    if (!p->sequence_count) {
        set_sequence(port, &fresh);
        free(p->resident.data);
        p->resident = fresh;
    } else p->sequences[p->sequence_count] = fresh;
    p->sequence_count++;
    return 0;
}
static void play_port(unsigned port) {
    if (!ports[port].sequence) fail("play without sequence", port);
    else ports[port].playing = 1;
}
void midi_sequence_play(unsigned port, int index, int volume) {
    if (port >= MIDI_PORTS || index < 0 || (unsigned)index >= ports[port].sequence_count) return;
    stop_port(port);
    set_sequence(port, sequence_at(&ports[port], (unsigned)index));
    midi_volume(port, volume);
    rewind_port(port);
    play_port(port);
}
void midi_sequence_resume(unsigned port) {
    if (port < MIDI_PORTS && ports[port].sequence_count > 0) play_port(port);
}
void midi_stop(unsigned port) { if (port < MIDI_PORTS) stop_port(port); }
int midi_playing(unsigned port) { return port < MIDI_PORTS && ports[port].playing; }
void midi_volume(unsigned port, int volume) {
    if (port >= MIDI_PORTS) return;
    if (volume != PORT_VOLUME_FULL) volume = (int)(volume * volume_scale);
    ports[port].volume = clamp(volume, 0, PORT_VOLUME_FULL);
    update_port(port);
}
void midi_stereo(int mode) {
    stereo = !!mode;
    for (unsigned p = 0; p < MIDI_PORTS; p++) update_port(p);
}
static void reset_dependents(unsigned port) {
    const Layout *layout = &layouts[port];
    for (unsigned i = 0; i < LAYOUT_DEPENDENTS && layout->dependents[i] >= 0; i++) {
        unsigned dependent = (unsigned)layout->dependents[i];
        stop_port(dependent);
        ports[dependent].spu_base = ports[dependent].spu_next = ports[port].spu_base;
        restart_sequence(dependent);
    }
}
void midi_port_delete(unsigned port) {
    if (port >= MIDI_PORTS) return;
    stop_port(port);
    restart_sequence(port);
    if (layouts[port].linked >= 0) {
        stop_port((unsigned)layouts[port].linked);
        restart_sequence((unsigned)layouts[port].linked);
    }
    reset_dependents(port);
}
int midi_opened(void) { return opened; }
void midi_open(void) {
    if (opened) return;
    if (!enabled) midi_reset();
    enabled = 1;
    opened = 1;
}
void midi_close(void) {
    for (unsigned port = 0; port < MIDI_PORTS; port++) {
        stop_port(port);
        release_port(&ports[port]);
        layout_port(port);
    }
    enabled = 0;
    opened = 0;
}
void midi_tick(void) {
    if (!enabled) return;
    control_clock += CONTROL_CLOCK_STEP;
    if (control_clock >= CONTROL_CLOCK_LFO_TICK) {
        control_clock -= CONTROL_CLOCK_LFO_TICK;
        for (unsigned i = 0; i < SPU_VOICES; i++) if (voices[i].used) {
            lfo_tick(&voices[i].lfo[SCE_LFO_PITCH]);
            lfo_tick(&voices[i].lfo[SCE_LFO_AMP]);
            if (voices[i].lfo[SCE_LFO_PITCH].wave || voices[i].lfo[SCE_LFO_AMP].wave) update_voice(i);
        }
    }
    for (unsigned port = 0; port < MIDI_PORTS; port++) {
        Port *p = &ports[port];
        if (!p->playing) continue;
        unsigned budget = EVENT_BUDGET;
        while (p->playing && p->due <= 0 && budget) {
            budget--;
            if (event(port)) { fail("malformed SQ event", p->at); stop_port(port); }
        }
        if (!budget) { fail("SQ event budget exhausted", port); stop_port(port); }
        p->due -= 1;
    }
}
static void save_bytes(unsigned char *out, size_t *at, const void *data, size_t size) {
    if (out) memcpy(out + *at, data, size);
    *at += size;
}
static void load_bytes(const unsigned char *in, size_t *at, void *data, size_t size) {
    memcpy(data, in + *at, size);
    *at += size;
}
static unsigned char *load_copy(const unsigned char *in, size_t *at, size_t size) {
    unsigned char *copy = malloc(size);
    load_bytes(in, at, copy, size);
    return copy;
}
static int sequence_owner(const Port *p) {
    if (!p->sequence) return -1;
    if (p->resident.data && p->sequence >= p->resident.data && p->sequence < p->resident.data + p->resident.size) return 0;
    for (int i = 1; i < PORT_SEQUENCES; i++)
        if (p->sequences[i].data && p->sequence >= p->sequences[i].data && p->sequence < p->sequences[i].data + p->sequences[i].size) return i;
    return -1;
}
size_t midi_save(unsigned char *out) {
    size_t at = 0;
    uint32_t scalars[3] = {serial, random_seed, control_clock};
    int flags[3] = {stereo, enabled, opened};
    save_bytes(out, &at, ports, sizeof(ports));
    save_bytes(out, &at, voices, sizeof(voices));
    save_bytes(out, &at, scalars, sizeof(scalars));
    save_bytes(out, &at, flags, sizeof(flags));
    for (unsigned port = 0; port < MIDI_PORTS; port++) {
        const Port *p = &ports[port];
        int owner = sequence_owner(p);
        const unsigned char *base = owner < 0 ? NULL : sequence_at((Port *)p, (unsigned)owner)->data;
        uint64_t offset = base ? (uint64_t)(p->sequence - base) : 0;
        save_bytes(out, &at, &owner, sizeof(owner));
        save_bytes(out, &at, &offset, sizeof(offset));
        for (unsigned i = 0; i < PORT_BANKS; i++) if (p->banks[i].hd) save_bytes(out, &at, p->banks[i].hd, p->banks[i].allocated);
        if (p->resident.data) save_bytes(out, &at, p->resident.data, p->resident.size);
        for (unsigned i = 1; i < PORT_SEQUENCES; i++) if (p->sequences[i].data) save_bytes(out, &at, p->sequences[i].data, p->sequences[i].size);
    }
    return at;
}
size_t midi_load(const unsigned char *in) {
    size_t at = 0;
    uint32_t scalars[3];
    int flags[3];
    for (unsigned port = 0; port < MIDI_PORTS; port++) release_port(&ports[port]);
    load_bytes(in, &at, ports, sizeof(ports));
    load_bytes(in, &at, voices, sizeof(voices));
    load_bytes(in, &at, scalars, sizeof(scalars));
    load_bytes(in, &at, flags, sizeof(flags));
    serial = scalars[0]; random_seed = scalars[1]; control_clock = scalars[2];
    stereo = flags[0]; enabled = flags[1]; opened = flags[2];
    missing_program_count = 0;
    for (unsigned port = 0; port < MIDI_PORTS; port++) {
        Port *p = &ports[port];
        int owner;
        uint64_t offset;
        load_bytes(in, &at, &owner, sizeof(owner));
        load_bytes(in, &at, &offset, sizeof(offset));
        for (unsigned i = 0; i < PORT_BANKS; i++) if (p->banks[i].hd) p->banks[i].hd = load_copy(in, &at, p->banks[i].allocated);
        if (p->resident.data) p->resident.data = load_copy(in, &at, p->resident.size);
        for (unsigned i = 1; i < PORT_SEQUENCES; i++) if (p->sequences[i].data) p->sequences[i].data = load_copy(in, &at, p->sequences[i].size);
        p->sequence = owner < 0 ? NULL : sequence_at(p, (unsigned)owner)->data + offset;
    }
    return at;
}
