#include "midi.h"
#include "spu2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *data; size_t size; } Buffer;
typedef struct { unsigned char *hd; uint32_t allocated, size, spu, body; uint32_t tables[4]; } Bank;
typedef struct { int bank, program, volume, expression, pan, bend, sustain, nrpn, data, pitch_modulation, amp_modulation; } Channel;
typedef struct { uint32_t at; unsigned running, flagged; int remaining, valid; } Loop;
typedef struct {
    Bank banks[16];
    Channel channels[16];
    Loop loops[128];
    Buffer sequences[16], resident;
    unsigned char *sequence;
    unsigned sequence_count;
    uint32_t sequence_size, begin, at, tempo;
    unsigned running, flagged, division, attribute;
    int playing, volume, effect_volume, effect_pan, effect_pitch;
    int spu_base, spu_next, bank_count;
    double due;
} Port;
typedef struct { int spu_base, descending, attribute, linked, dependents[5]; } Layout;
static const Layout layouts[16] = {
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
    Lfo lfo[2];
    int lfo_depth[4], midi_lfo_depth[4];
} MidiVoice;
static Port ports[16];
static MidiVoice voices[48];
static uint32_t serial, random_seed, control_clock;
static int16_t sine_wave[200];
static struct { const unsigned char *hd; int port, bank, program; } missing_programs[32];
static unsigned missing_program_count;
static unsigned diagnostics, features;
static int opened, stereo, enabled;

static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }
static int fail(const char *message, unsigned value) {
    if (diagnostics++ < 24) fprintf(stderr, "EZMIDI: %s (0x%x)\n", message, value);
    return -1;
}
static void feature(unsigned flag, const char *message) {
    if (features & flag) return;
    features |= flag;
    fail(message, flag);
}
static int random_value(void) {
    random_seed = random_seed * 0x41c64e6d + 0x3039;
    return (int16_t)(random_seed >> 16);
}
static void lfo_begin(Lfo *lfo) {
    lfo->remaining = lfo->delay;
    lfo->fade = 0;
    lfo->stage = lfo->delay ? 1 : lfo->fade_step ? 2 : 3;
}
static void lfo_init(Lfo *lfo, unsigned wave, unsigned phase, unsigned random, unsigned period, unsigned delay, unsigned fade, unsigned attr) {
    memset(lfo, 0, sizeof(*lfo));
    if (!wave || !attr) return;
    if (wave > 6 || !period) { feature(2, "unsupported LFO waveform or zero period"); return; }
    unsigned ticks = (period * 256 + 533) / 1066;
    if (!ticks) { feature(2, "LFO period shorter than synth tick"); return; }
    lfo->wave = wave;
    lfo->step = 65536 / ticks;
    lfo->delay = (delay * 256 + 533) / 1066;
    ticks = (fade * 256 + 533) / 1066;
    lfo->fade_step = ticks ? 65536 / ticks : 0;
    if (ticks && !lfo->fade_step) lfo->fade_step = 1;
    int start = (int)(phase * 256) + (int)(random * 256) * random_value() / 65536;
    lfo->phase = (unsigned)start & 65535;
    lfo->release = !!(attr & 6);
    if (attr & 5) lfo_begin(lfo);
}
static int lfo_wave(Lfo *lfo, int wrapped) {
    int phase = (int)lfo->phase;
    switch (lfo->wave) {
        case 1: return (phase - 32768) >> 1;
        case 2: return (32767 - phase) >> 1;
        case 3: return phase > 49151 ? phase - 65536 : phase <= 16384 ? phase : 32768 - phase;
        case 4: return phase > 32767 ? 16384 : -16384;
        case 5: return wrapped ? random_value() >> 2 : lfo->value;
        case 6: {
            unsigned position = lfo->phase * 200;
            unsigned index = position >> 16;
            unsigned fraction = position & 65535;
            int a = sine_wave[index], b = sine_wave[(index + 1) % 200];
            return (a + (((b - a) * (int)fraction) >> 16)) / 2;
        }
        default: return 0;
    }
}
static void lfo_tick(Lfo *lfo) {
    if (!lfo->stage || lfo->stage >= 6) { lfo->value = 0; return; }
    int value = 0;
    if (lfo->stage != 1) {
        lfo->phase += lfo->step;
        int wrapped = lfo->phase >= 65536;
        lfo->phase &= 65535;
        value = lfo_wave(lfo, wrapped);
    }
    switch (lfo->stage) {
        case 1:
            if (!--lfo->remaining) { lfo->fade = 0; lfo->stage = lfo->fade_step ? 2 : 3; }
            break;
        case 2:
            value = value * (int)lfo->fade / 65536;
            lfo->fade += lfo->fade_step;
            if (lfo->fade >= 65536) lfo->stage = 3;
            break;
        case 4:
            if (!--lfo->remaining) { lfo->fade = 65536; lfo->stage = lfo->fade_step ? 5 : 6; }
            break;
        case 5:
            value = value * (int)lfo->fade / 65536;
            if (lfo->fade <= lfo->fade_step) lfo->stage = 6;
            else lfo->fade -= lfo->fade_step;
            break;
        default: break;
    }
    lfo->value = value;
}
static void lfo_release(Lfo *lfo) {
    if (!lfo->release) return;
    if (!lfo->stage) lfo_begin(lfo);
    else if (lfo->stage == 1) lfo->stage = 6;
    else if (lfo->stage == 2 || lfo->stage == 3) {
        lfo->remaining = lfo->delay;
        lfo->fade = 65536;
        lfo->stage = lfo->delay ? 4 : lfo->fade_step ? 5 : 6;
    }
}
static void init_channel(Channel *c) {
    memset(c, 0, sizeof(*c));
    c->volume = 127;
    c->expression = 127;
    c->pan = 64;
    c->bend = 8192;
    c->nrpn = -1;
    c->data = -1;
}
static void release_banks(Port *p) {
    for (unsigned i = 0; i < 16; i++) {
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
    for (unsigned i = 0; i < 48; i++) if (voices[i].used) spu_voice_stop(i);
    for (unsigned p = 0; p < 16; p++) release_port(&ports[p]);
    memset(ports, 0, sizeof(ports));
    memset(voices, 0, sizeof(voices));
    for (unsigned p = 0; p < 16; p++) {
        ports[p].volume = 256;
        ports[p].effect_volume = 128;
        ports[p].effect_pan = 64;
        ports[p].effect_pitch = 8192;
        ports[p].tempo = 500000;
        layout_port(p);
        for (unsigned c = 0; c < 16; c++) init_channel(&ports[p].channels[c]);
    }
    serial = 0;
    random_seed = 0x02cee9d7;
    control_clock = 0;
    for (unsigned i = 0; i < 200; i++) sine_wave[i] = (int16_t)(32767.0 * sin(6.2831853071795864769 * i / 200.0));
    diagnostics = 0;
    features = 0;
    missing_program_count = 0;
    stereo = 1;
    enabled = 1;
    opened = 0;
}
static const unsigned char *bank_bytes(const Bank *b, uint32_t at, size_t size) {
    if (!b->hd || at > b->size || size > b->size - at) return NULL;
    return b->hd + at;
}
static const unsigned char *record(const Bank *b, unsigned table, unsigned index, size_t size) {
    uint32_t base = b->tables[table];
    const unsigned char *head = bank_bytes(b, base, 16);
    if (!head || index > u32(head + 12)) return NULL;
    const unsigned char *entry = bank_bytes(b, base + 16 + index * 4, 4);
    if (!entry || u32(entry) == UINT32_MAX || u32(entry) > b->size - base) return NULL;
    return bank_bytes(b, base + u32(entry), size);
}
static void update_voice(unsigned i) {
    MidiVoice *v = &voices[i];
    if (!v->used || !spu_voice_active(i)) { v->used = 0; return; }
    Port *p = &ports[v->port];
    Channel *c = &p->channels[v->channel];
    int bend = (v->effect ? v->effect_pitch : c->bend) - 8192;
    int range = bend < 0 ? v->low : v->high;
    unsigned pitch_side = v->lfo[0].value < 0;
    int pitch_depth = clamp(v->lfo_depth[pitch_side] + c->pitch_modulation * v->midi_lfo_depth[pitch_side] / 128, -32768, 32767);
    int pitch_lfo = pitch_depth * v->lfo[0].value / 16384;
    int bend_fine = bend * range / 8192;
    double pitch = 4096.0 * exp2((v->pitch + (bend_fine + pitch_lfo) / 128.0) / 12.0);
    double gain = v->gain * clamp(p->volume, 0, 256) / 256.0;
    unsigned amp_side = 2 + (v->lfo[1].value < 0);
    int amp_depth = clamp(v->lfo_depth[amp_side] + c->amp_modulation * v->midi_lfo_depth[amp_side] / 128, -128, 127);
    int amp_factor = 16384 + v->lfo[1].value * amp_depth / 128;
    gain *= amp_factor > 0 ? amp_factor / 16384.0 : 0;
    gain *= v->effect ? v->effect_volume / 128.0 : c->volume / 127.0 * c->expression / 127.0;
    int channel_pan = clamp((v->effect ? v->effect_pan : c->pan) - 64, -63, 63);
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
    int left = clamp((int)(16383.0 * gain * (pan > 0 && stereo ? (63 - pan) / 63.0 : 1.0)), 0, 16383);
    int right = clamp((int)(16383.0 * gain * (pan < 0 && stereo ? (63 + pan) / 63.0 : 1.0)), 0, 16383);
    if (!phase && stereo) { left = -left; right = -right; }
    spu_voice_volume(i, left, right);
    spu_voice_pitch(i, pitch < 1 ? 1 : pitch > 16383 ? 16383 : (unsigned)pitch);
}
static void update_port(unsigned port) { for (unsigned i = 0; i < 48; i++) if (voices[i].port == (int)port) update_voice(i); }
static void stop_port(unsigned port) {
    ports[port].playing = 0;
    for (unsigned i = 0; i < 48; i++) if (voices[i].used && voices[i].port == (int)port) {
        spu_voice_stop(i);
        voices[i].released = 1;
    }
}
static void note_off(unsigned port, unsigned channel, unsigned key, int id, int effect) {
    for (unsigned i = 0; i < 48; i++) {
        MidiVoice *v = &voices[i];
        if (!v->used || v->port != (int)port || v->channel != (int)channel || v->key != (int)key || v->effect != effect) continue;
        if (effect && (v->id != id || v->bank != ports[port].channels[channel].bank || v->program != ports[port].channels[channel].program)) continue;
        if (!effect && ports[port].channels[channel].sustain) v->held = 1;
        else { spu_voice_stop(i); v->released = 1; lfo_release(&v->lfo[0]); lfo_release(&v->lfo[1]); }
    }
}
static void stop_group(unsigned port, unsigned channel, unsigned group) {
    if (!group) return;
    for (unsigned i = 0; i < 48; i++) {
        MidiVoice *v = &voices[i];
        if (!v->used || v->released || v->port != (int)port || v->channel != (int)channel || v->group != (int)group) continue;
        spu_voice_adsr2(i, 0xc021);
        spu_voice_stop(i);
        v->released = 1;
        v->held = 0;
    }
}
static unsigned follow_adsr(unsigned value, unsigned key, const unsigned char *sample, unsigned offset, unsigned mask, unsigned shift) {
    int amount = ((int)key - sample[offset + 1]) * (int8_t)sample[offset] / 12;
    unsigned field = (unsigned)clamp((int)(value >> shift & mask) + amount, 0, (int)mask);
    return (value & ~(mask << shift)) | field << shift;
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
static int allocate_voice(unsigned attr, unsigned priority) {
    unsigned begin = (attr & 0x30) == 0x20 ? 24 : 0;
    unsigned end = (attr & 0x30) == 0x10 ? 24 : 48;
    if ((attr & 0x30) == 0 || (attr & 0x30) == 0x30) {
        unsigned free[2] = {0, 0};
        for (unsigned i = 0; i < 48; i++) if (!voices[i].used || !spu_voice_active(i)) free[i / 24]++;
        if (free[0] || free[1]) { begin = free[1] > free[0] ? 24 : 0; end = begin + 24; }
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
    Bank *b = c->bank < 16 ? &ports[port].banks[c->bank] : NULL;
    const unsigned char *hd = b ? b->hd : NULL;
    unsigned count = 0;
    uint32_t entry = UINT32_MAX;
    const char *reason = "unloaded bank";
    if (b && hd) {
        const unsigned char *table = bank_bytes(b, b->tables[3], 16);
        reason = "invalid program table";
        if (table) {
            count = u32(table + 12) + 1;
            reason = "program outside table";
            if ((unsigned)c->program < count) {
                const unsigned char *offset = bank_bytes(b, b->tables[3] + 16 + (unsigned)c->program * 4, 4);
                if (offset) {
                    entry = u32(offset);
                    if (entry == UINT32_MAX) return;
                    reason = "invalid program record";
                }
            }
        }
    }
    for (unsigned i = 0; i < missing_program_count; i++) {
        if (missing_programs[i].hd == hd && missing_programs[i].port == (int)port && missing_programs[i].bank == c->bank && missing_programs[i].program == c->program) return;
    }
    if (missing_program_count == 32) return;
    missing_programs[missing_program_count].hd = hd;
    missing_programs[missing_program_count].port = (int)port;
    missing_programs[missing_program_count].bank = c->bank;
    missing_programs[missing_program_count++].program = c->program;
    fprintf(stderr, "EZMIDI: %s port=%u bank=%d channel=%u program=%d key=%u velocity=%u effect=%d id=%d hd=%p table=0x%08x table_index=%d table_count=%u entry=0x%08x\n", reason, port, c->bank, channel, c->program, key, velocity, effect, id, (const void *)hd, b ? b->tables[3] : 0, c->program, count, entry);
}
static void note_on(unsigned port, unsigned channel, unsigned key, unsigned velocity, int id, int effect) {
    if (!velocity) { note_off(port, channel, key, id, effect); return; }
    Port *p = &ports[port];
    Channel *c = &p->channels[channel];
    if (c->bank > 15) { missing_program(port, channel, key, velocity, effect, id); return; }
    Bank *b = &p->banks[c->bank];
    const unsigned char *prog = record(b, 3, (unsigned)c->program, 36);
    if (!prog) { missing_program(port, channel, key, velocity, effect, id); return; }
    uint32_t prog_at = (uint32_t)(prog - b->hd);
    uint32_t split_at = u32(prog);
    if (split_at > b->size - prog_at || prog[5] < 20) { fail("invalid split table", split_at); return; }
    for (unsigned j = 0; j < prog[4]; j++) {
        const unsigned char *split = bank_bytes(b, prog_at + split_at + j * prog[5], 20);
        if (!split || key < split[2] || key > split[4]) continue;
        const unsigned char *set = record(b, 2, u16(split), 4);
        if (!set || velocity < set[1] || velocity > set[2]) continue;
        uint32_t set_at = (uint32_t)(set - b->hd);
        const unsigned char *indices = bank_bytes(b, set_at + 4, set[3] * 2u);
        if (!indices) continue;
        for (unsigned k = 0; k < set[3]; k++) {
            const unsigned char *sample = record(b, 1, u16(indices + k * 2), 42);
            if (!sample || velocity < sample[2] || velocity > sample[4]) continue;
            const unsigned char *vag = record(b, 0, u16(sample), 8);
            if (!vag || u32(vag) >= b->body || !u16(vag + 4)) continue;
            if (split[10] || split[12] || sample[5] || sample[8]) feature(1, "key/velocity follow modulation is approximated");
            if (set[0] != 0 && set[0] != 2) feature(16, "sample velocity curve is approximated");
            stop_group(port, channel, sample[14]);
            unsigned priority = sample[15] + (p->attribute & 255);
            int selected = allocate_voice(sample[41], priority);
            if (selected < 0) continue;
            unsigned i = (unsigned)selected;
            MidiVoice *v = &voices[i];
            memset(v, 0, sizeof(*v));
            v->used = 1; v->port = (int)port; v->channel = (int)channel; v->key = (int)key; v->id = id; v->effect = effect;
            v->group = sample[14]; v->priority = (int)priority;
            v->bank = c->bank; v->program = c->program; v->serial = ++serial;
            v->effect_volume = p->effect_volume; v->effect_pan = p->effect_pan; v->effect_pitch = p->effect_pitch;
            v->pitch = (int)key - sample[11] + (int8_t)prog[8] + (int8_t)split[18] + ((int8_t)prog[9] + (int8_t)split[19] + (int8_t)sample[12]) / 128.0 + 12 * log2(u16(vag + 4) / 48000.0);
            v->gain = (set[0] == 2 ? clamp((int)(velocity * velocity / 127), 1, 127) : (int)velocity) / 127.0 * prog[6] / 128.0 * split[16] / 128.0 * sample[16] / 128.0;
            int key_pan = (((int)key - prog[11]) * (int8_t)prog[10] + ((int)key - split[15]) * (int8_t)split[14]) / 12;
            int program_pan = (int8_t)prog[7], split_pan = (int8_t)split[17], sample_pan = (int8_t)sample[13];
            if (!prog[12]) {
                if (program_pan < 0) program_pan = -program_pan;
                if (split_pan < 0) split_pan = -split_pan;
                if (sample_pan < 0) sample_pan = -sample_pan;
            }
            v->circular = prog[12] & 1;
            v->pan = program_pan + split_pan + sample_pan - 128 + key_pan;
            if (v->circular) {
                while (v->pan >= 128) v->pan -= 255;
                while (v->pan < -128) v->pan += 255;
            } else v->pan = clamp(v->pan, 0, 127) - 64;
            v->low = (int)u16(split + 6); v->high = (int)u16(split + 8);
            v->lfo_depth[0] = (int16_t)u16(prog + 24); v->lfo_depth[1] = (int16_t)u16(prog + 26);
            v->lfo_depth[2] = (int8_t)prog[32]; v->lfo_depth[3] = (int8_t)prog[33];
            v->midi_lfo_depth[0] = (int16_t)u16(prog + 28); v->midi_lfo_depth[1] = (int16_t)u16(prog + 30);
            v->midi_lfo_depth[2] = (int8_t)prog[34]; v->midi_lfo_depth[3] = (int8_t)prog[35];
            lfo_init(&v->lfo[0], prog[14], prog[16], prog[18], u16(prog + 20), u16(sample + 32), u16(sample + 34), sample[40] & 7);
            lfo_init(&v->lfo[1], prog[15], prog[17], prog[19], u16(prog + 22), u16(sample + 36), u16(sample + 38), sample[40] >> 4 & 7);
            unsigned adsr1 = follow_adsr(u16(sample + 18), key, sample, 22, 127, 8);
            adsr1 = follow_adsr(adsr1, key, sample, 24, 15, 4);
            adsr1 = follow_adsr(adsr1, key, sample, 30, 15, 0);
            unsigned adsr2 = follow_adsr(u16(sample + 20), key, sample, 26, 127, 6);
            adsr2 = follow_adsr(adsr2, key, sample, 28, 31, 0);
            spu_voice_start(i, b->spu + u32(vag), 4096, 0, 0, adsr1, adsr2);
            spu_voice_route(i, sample[41] & 15);
            update_voice(i);
        }
    }
}
static int channel_message(unsigned port, unsigned status, unsigned a, unsigned b) {
    unsigned channel = status & 15;
    Port *p = &ports[port];
    Channel *c = &p->channels[channel];
    switch (status & 0xf0) {
        case 0x80: note_off(port, channel, a, -1, 0); break;
        case 0x90: note_on(port, channel, a, b, -1, 0); break;
        case 0xc0: c->program = (int)a; break;
        case 0xe0: c->bend = (int)(a | b << 7); update_port(port); break;
        case 0xb0:
            switch (a) {
                case 0: c->bank = (int)b; break;
                case 1: c->pitch_modulation = (int)(b * 128 / 127); update_port(port); break;
                case 2: c->amp_modulation = (int)(b * 128 / 127); update_port(port); break;
                case 7: c->volume = (int)b; update_port(port); break;
                case 10: c->pan = (int)b; update_port(port); break;
                case 11: c->expression = (int)b; update_port(port); break;
                case 64:
                    c->sustain = b >= 64;
                    if (!c->sustain) for (unsigned i = 0; i < 48; i++) if (voices[i].used && voices[i].port == (int)port && voices[i].channel == (int)channel && voices[i].held) {
                        voices[i].held = 0; voices[i].released = 1; spu_voice_stop(i); lfo_release(&voices[i].lfo[0]); lfo_release(&voices[i].lfo[1]);
                    }
                    break;
                case 120: case 123:
                    for (unsigned key = 0; key < 128; key++) note_off(port, channel, key, -1, 0);
                    break;
                case 121: init_channel(c); update_port(port); break;
                case 99: c->nrpn = (int)b; c->data = -1; break;
                case 6:
                    c->data = (int)b;
                    if (c->nrpn == 0 && p->sequence) {
                        Loop *loop = &p->loops[b];
                        loop->at = p->at; loop->running = p->running; loop->flagged = p->flagged;
                        loop->remaining = -1; loop->valid = 1;
                    }
                    break;
                case 38:
                    if (c->nrpn == 1 && c->data >= 0) {
                        Loop *loop = &p->loops[c->data];
                        if (!loop->valid) return fail("loop without start", (unsigned)c->data);
                        if (loop->remaining < 0) loop->remaining = (int)b;
                        if (b && !loop->remaining) { loop->remaining = -1; break; }
                        p->at = loop->at; p->running = loop->running; p->flagged = loop->flagged;
                        if (b) loop->remaining--;
                    }
                    break;
                case 32: case 91: case 93: break;
                default: return fail("unsupported controller", a);
            }
            break;
        case 0xa0: case 0xd0: return fail("unsupported pressure", status);
        default: return fail("unsupported MIDI status", status);
    }
    return 0;
}
static void select_program(unsigned port, int bank, int program) {
    channel_message(port, 0xb0, 0, (unsigned)bank & 127);
    channel_message(port, 0xc0, (unsigned)program & 127, 0);
}
static int effect_ready(unsigned port, int id, int needs_bank) {
    if (id > 0x7e || port < 7 || port >= 16) return 0;
    return !needs_bank || ports[port].bank_count > 0;
}
static void effect_update(unsigned port, unsigned kind, unsigned key, unsigned id, unsigned a, unsigned b) {
    Port *p = &ports[port];
    for (unsigned i = 0; i < 48; i++) {
        MidiVoice *v = &voices[i];
        if (!v->used || !v->effect || v->port != (int)port || v->channel != 0 || v->key != (int)key || v->id != (int)id || v->bank != p->channels[0].bank || v->program != p->channels[0].program) continue;
        if (kind == 0) v->effect_volume = (int)a;
        else if (kind == 1) v->effect_pan = (int)a;
        else v->effect_pitch = (int)(a | b << 7);
        update_voice(i);
    }
}
void midi_effect_play(unsigned port, int bank, int program, int key, int pan, int velocity, int volume, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    ports[port].effect_volume = (unsigned char)volume;
    ports[port].effect_pan = (unsigned char)pan;
    note_on(port, 0, (unsigned char)key & 127, (unsigned char)velocity & 127, (unsigned char)id, 1);
}
void midi_effect_stop(unsigned port, int bank, int program, int key, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    note_on(port, 0, (unsigned char)key & 127, 0, (unsigned char)id, 1);
}
void midi_effect_volume(unsigned port, int bank, int program, int key, int volume, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    effect_update(port, 0, (unsigned char)key, (unsigned char)id, (unsigned char)volume, 0);
}
void midi_effect_pan(unsigned port, int bank, int program, int key, int pan, int id) {
    if (!effect_ready(port, id, 1)) return;
    select_program(port, bank, program);
    effect_update(port, 1, (unsigned char)key, (unsigned char)id, (unsigned char)pan, 0);
}
void midi_effect_pitch(unsigned port, int bank, int program, int key, int pitch, int id) {
    if (!effect_ready(port, id, 0)) return;
    select_program(port, bank, program);
    effect_update(port, 2, (unsigned char)key, (unsigned char)id, (unsigned)pitch & 0x7f, (unsigned)(pitch >> 7) & 0x7f);
}
int midi_message(unsigned port, unsigned status, unsigned a, unsigned b) {
    if (port >= 16) return -1;
    return channel_message(port, status & 255, a & 127, b & 127);
}
static int read_byte(Port *p, unsigned *value) {
    if (p->at >= p->sequence_size) return -1;
    *value = p->sequence[p->at];
    p->at++;
    return 0;
}
static int read_vlq(Port *p, uint32_t *value) {
    *value = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned b;
        if (read_byte(p, &b)) return -1;
        *value = *value << 7 | (b & 127);
        if (!(b & 128)) return 0;
    }
    return -1;
}
static int next_delta(Port *p) {
    uint32_t delta = 0;
    if (!p->flagged && read_vlq(p, &delta)) return -1;
    p->due += delta * (double)p->tempo * 0.048 / p->division;
    return 0;
}
static int event(unsigned port) {
    Port *p = &ports[port];
    unsigned status, a, b = 0;
    if (read_byte(p, &status)) return -1;
    int written = status >= 128;
    if (!written) { p->at--; status = p->running; }
    p->flagged = 0;
    if (status == 255) {
        uint32_t length;
        if (read_byte(p, &a) || read_vlq(p, &length) || length > p->sequence_size - p->at) return -1;
        const unsigned char *data = p->sequence + p->at;
        if (a == 0x51 && length == 3) {
            uint32_t tempo = (uint32_t)data[0] << 16 | (uint32_t)data[1] << 8 | data[2];
            if (!tempo) return -1;
            p->tempo = tempo;
        }
        p->at += length;
        p->running = 0;
        if (a == 0x2f) { p->playing = 0; return 0; }
    } else {
        if (status < 0x80 || status >= 0xf0 || read_byte(p, &a)) return -1;
        unsigned kind = status & 0xf0;
        if (kind != 0x80 && kind != 0xc0 && kind != 0xd0) {
            if (a & 128 || read_byte(p, &b)) return -1;
            p->flagged = !!(b & 128);
        } else p->flagged = !!(a & 128);
        if (p->flagged && !written) return -1;
        p->running = status;
        channel_message(port, status, a & 127, b & 127);
    }
    if (p->playing && next_delta(p)) return -1;
    return 0;
}
static int rewind_port(unsigned port) {
    Port *p = &ports[port];
    p->playing = 0;
    p->at = p->begin;
    p->tempo = 500000;
    p->running = p->flagged = 0;
    p->due = 0;
    memset(p->loops, 0, sizeof(p->loops));
    for (unsigned c = 0; c < 16; c++) init_channel(&p->channels[c]);
    return p->sequence && p->division ? next_delta(p) : -1;
}
static int set_sequence(unsigned port, const Buffer *buffer) {
    Port *p = &ports[port];
    stop_port(port);
    p->sequence = NULL;
    const unsigned char *data = buffer->data;
    size_t size = buffer->size;
    if (!data || size < 0x4e || size > UINT32_MAX || memcmp(data, "IECSsreV", 8) || memcmp(data + 0x30, "IECSidiM", 8)) return fail("invalid SQ header", (unsigned)size);
    uint32_t block = u32(data + 0x40);
    if (block > size - 0x36) return -1;
    block += 0x30;
    uint32_t offset = u32(data + block);
    if (offset < 6 || offset > size - block) return -1;
    unsigned division = u16(data + block + 4);
    if (!division || division >= 0x8000) return -1;
    p->sequence = buffer->data; p->sequence_size = (uint32_t)size; p->division = division; p->begin = block + offset;
    return rewind_port(port);
}
static Buffer padded_copy(const void *data, int size) {
    Buffer buffer = {NULL, 0};
    if (!data || size <= 0 || size > 16 * 1024 * 1024) return buffer;
    size_t allocated = size <= 256 ? 256 : (size_t)size + 256;
    buffer.data = calloc(1, allocated);
    if (buffer.data) {
        memcpy(buffer.data, data, (size_t)size);
        buffer.size = allocated;
    }
    return buffer;
}
static int attach_bank(unsigned port, unsigned index, const Buffer *hd, unsigned spu, unsigned body) {
    if (index >= 16) return -1;
    Bank b;
    memset(&b, 0, sizeof(b));
    b.hd = hd->data; b.body = body; b.spu = spu;
    if (b.spu > 0x200000 || b.body > 0x200000 - b.spu) return fail("bank body exceeds SPU memory", b.spu);
    if (!hd->data || hd->size < 80 || hd->size > UINT32_MAX) return fail("missing HD allocation", (unsigned)hd->size);
    b.allocated = b.size = (uint32_t)hd->size;
    const unsigned char *head = bank_bytes(&b, 16, 40);
    if (!head || memcmp(head, "IECSdaeH", 8)) return fail("invalid HD header", index);
    b.size = u32(head + 12);
    if (b.size > b.allocated || b.size < 80 || u32(head + 16) > b.body) return -1;
    b.tables[0] = u32(head + 32); b.tables[1] = u32(head + 28); b.tables[2] = u32(head + 24); b.tables[3] = u32(head + 20);
    static const char tags[4][9] = {"IECSigaV", "IECSlpmS", "IECStesS", "IECSgorP"};
    for (unsigned i = 0; i < 4; i++) {
        const unsigned char *table = bank_bytes(&b, b.tables[i], 16);
        if (!table || memcmp(table, tags[i], 8) || u32(table + 12) >= (b.size - b.tables[i] - 16) / 4) return fail("invalid HD table", i);
    }
    b.hd = malloc(b.allocated);
    if (!b.hd) return -1;
    memcpy(b.hd, hd->data, b.allocated);
    free(ports[port].banks[index].hd);
    ports[port].banks[index] = b;
    return 0;
}
static void drop_stale_banks(unsigned port, int first_failed) {
    for (unsigned i = first_failed ? 0 : 1; i < 16; i++) {
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
    for (unsigned i = 0; i < 5 && layout->dependents[i] >= 0; i++)
        ports[layout->dependents[i]].spu_base = ports[layout->dependents[i]].spu_next = address;
}
int midi_bank_first(unsigned port, const void *hd, int hd_size, const void *bd, int bd_size) {
    if (port >= 16) return -1;
    Port *p = &ports[port];
    const Layout *layout = &layouts[port];
    int length = bd_size + 0x10;
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
    for (unsigned i = 0; i < 5 && layout->dependents[i] >= 0; i++) stop_port((unsigned)layout->dependents[i]);
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
    if (port >= 16) return -1;
    Port *p = &ports[port];
    const Layout *layout = &layouts[port];
    if (p->bank_count > 15) return 0;
    unsigned index = (unsigned)p->bank_count;
    int length = bd_size + 0x10;
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
    if (port >= 16) return -1;
    Port *p = &ports[port];
    stop_port(port);
    Buffer fresh = padded_copy(data, size);
    if (!fresh.data) return -1;
    if (p->sequence_count >= 16) { free(fresh.data); return -1; }
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
    if (port >= 16 || index < 0 || (unsigned)index >= ports[port].sequence_count) return;
    stop_port(port);
    set_sequence(port, sequence_at(&ports[port], (unsigned)index));
    midi_volume(port, volume);
    rewind_port(port);
    play_port(port);
}
void midi_sequence_resume(unsigned port) {
    if (port < 16 && ports[port].sequence_count > 0) play_port(port);
}
void midi_stop(unsigned port) { if (port < 16) stop_port(port); }
int midi_playing(unsigned port) { return port < 16 && ports[port].playing; }
void midi_volume(unsigned port, int volume) {
    if (port >= 16) return;
    if (volume != 256) volume = (int)(volume * volume_scale);
    ports[port].volume = clamp(volume, 0, 256);
    update_port(port);
}
void midi_stereo(int mode) {
    stereo = !!mode;
    for (unsigned p = 0; p < 16; p++) update_port(p);
}
static void reset_dependents(unsigned port) {
    const Layout *layout = &layouts[port];
    for (unsigned i = 0; i < 5 && layout->dependents[i] >= 0; i++) {
        unsigned dependent = (unsigned)layout->dependents[i];
        stop_port(dependent);
        ports[dependent].spu_base = ports[dependent].spu_next = ports[port].spu_base;
        restart_sequence(dependent);
    }
}
void midi_port_delete(unsigned port) {
    if (port >= 16) return;
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
    for (unsigned port = 0; port < 16; port++) {
        stop_port(port);
        release_port(&ports[port]);
        layout_port(port);
    }
    enabled = 0;
    opened = 0;
}
void midi_tick(void) {
    if (!enabled) return;
    control_clock += 1000000;
    if (control_clock >= 200016000) {
        control_clock -= 200016000;
        for (unsigned i = 0; i < 48; i++) if (voices[i].used) {
            lfo_tick(&voices[i].lfo[0]);
            lfo_tick(&voices[i].lfo[1]);
            if (voices[i].lfo[0].wave || voices[i].lfo[1].wave) update_voice(i);
        }
    }
    for (unsigned port = 0; port < 16; port++) {
        Port *p = &ports[port];
        if (!p->playing) continue;
        unsigned budget = 4096;
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
    for (int i = 1; i < 16; i++)
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
    for (unsigned port = 0; port < 16; port++) {
        const Port *p = &ports[port];
        int owner = sequence_owner(p);
        const unsigned char *base = owner < 0 ? NULL : sequence_at((Port *)p, (unsigned)owner)->data;
        uint64_t offset = base ? (uint64_t)(p->sequence - base) : 0;
        save_bytes(out, &at, &owner, sizeof(owner));
        save_bytes(out, &at, &offset, sizeof(offset));
        for (unsigned i = 0; i < 16; i++) if (p->banks[i].hd) save_bytes(out, &at, p->banks[i].hd, p->banks[i].allocated);
        if (p->resident.data) save_bytes(out, &at, p->resident.data, p->resident.size);
        for (unsigned i = 1; i < 16; i++) if (p->sequences[i].data) save_bytes(out, &at, p->sequences[i].data, p->sequences[i].size);
    }
    return at;
}
size_t midi_load(const unsigned char *in) {
    size_t at = 0;
    uint32_t scalars[3];
    int flags[3];
    for (unsigned port = 0; port < 16; port++) release_port(&ports[port]);
    load_bytes(in, &at, ports, sizeof(ports));
    load_bytes(in, &at, voices, sizeof(voices));
    load_bytes(in, &at, scalars, sizeof(scalars));
    load_bytes(in, &at, flags, sizeof(flags));
    serial = scalars[0]; random_seed = scalars[1]; control_clock = scalars[2];
    stereo = flags[0]; enabled = flags[1]; opened = flags[2];
    missing_program_count = 0;
    for (unsigned port = 0; port < 16; port++) {
        Port *p = &ports[port];
        int owner;
        uint64_t offset;
        load_bytes(in, &at, &owner, sizeof(owner));
        load_bytes(in, &at, &offset, sizeof(offset));
        for (unsigned i = 0; i < 16; i++) if (p->banks[i].hd) p->banks[i].hd = load_copy(in, &at, p->banks[i].allocated);
        if (p->resident.data) p->resident.data = load_copy(in, &at, p->resident.size);
        for (unsigned i = 1; i < 16; i++) if (p->sequences[i].data) p->sequences[i].data = load_copy(in, &at, p->sequences[i].size);
        p->sequence = owner < 0 ? NULL : sequence_at(p, (unsigned)owner)->data + offset;
    }
    return at;
}
