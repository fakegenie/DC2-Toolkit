#include "spu2.h"
#include "spu2_gaussian.h"
#include "spu2_reverb_presets.h"
#include <string.h>

static unsigned char ram[2 * 1024 * 1024];
static struct {
    unsigned address, start, loop, pitch, phase, index, flags;
    unsigned adsr1, adsr2, stage, counter;
    int left, right, history1, history2, envelope;
    int16_t samples[28], interpolation[4];
    unsigned route;
} voices[48];
static int master[2][2];
static unsigned mixer[2];
static int input_gain[2][2], external_gain[2][2];
static int32_t input_pcm[2][2];
static struct {
    uint16_t params[32];
    unsigned end, begin, length, cursor, mode, enabled, phase;
    int depth[2];
    int16_t down[2][39], up[2][39];
    unsigned filter_cursor;
} effects[2];

static int clamp(int value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

void spu_reset(void) {
    memset(ram, 0, sizeof(ram));
    memset(voices, 0, sizeof(voices));
    memset(effects, 0, sizeof(effects));
    for (unsigned i = 0; i < 2; ++i) effects[i].end = 0x1fffff - i * 0x20000;
    for (unsigned i = 0; i < 2; ++i) master[i][0] = master[i][1] = 0x3fff;
    for (unsigned i = 0; i < 48; ++i) voices[i].route = 15;
    memset(input_pcm, 0, sizeof(input_pcm));
    for (unsigned core = 0; core < 2; ++core) {
        mixer[core] = core ? 0xffc : 0xff0;
        for (unsigned channel = 0; channel < 2; ++channel) input_gain[core][channel] = external_gain[core][channel] = 32767;
    }
}

int spu_ram_write(unsigned address, const void *data, size_t size) {
    if (!data || address > sizeof(ram) || size > sizeof(ram) - address) return 0;
    memcpy(ram + address, data, size);
    return 1;
}

void spu_voice_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, unsigned adsr1, unsigned adsr2) {
    if (voice >= 48) return;
    memset(voices[voice].interpolation, 0, sizeof(voices[voice].interpolation));
    voices[voice].start = address;
    voices[voice].address = address;
    voices[voice].loop = address;
    voices[voice].pitch = pitch;
    voices[voice].left = left;
    voices[voice].right = right;
    voices[voice].adsr1 = adsr1;
    voices[voice].adsr2 = adsr2;
    voices[voice].index = 28;
    voices[voice].phase = 0;
    voices[voice].flags = 0;
    voices[voice].history1 = voices[voice].history2 = 0;
    voices[voice].envelope = 0;
    voices[voice].counter = 0;
    voices[voice].stage = 1;
}

void spu_voice_route(unsigned voice, unsigned route) { if (voice < 48) voices[voice].route = route & 15; }

void spu_voice_stop(unsigned voice) {
    if (voice < 48 && voices[voice].stage) { voices[voice].stage = 4; voices[voice].counter = 0; }
}

int spu_voice_active(unsigned voice) { return voice < 48 && voices[voice].stage != 0; }

int spu_voice_level(unsigned voice) { return voice < 48 ? voices[voice].envelope : 0; }

static int direct_volume(unsigned value) { return (value & 0x4000) ? (int)(value & 0x7fff) - 0x8000 : (int)(value & 0x7fff); }

void spu_voice_volume(unsigned voice, int left, int right) {
    if (voice >= 48) return;
    voices[voice].left = left;
    voices[voice].right = right;
}

void spu_voice_pitch(unsigned voice, unsigned pitch) { if (voice < 48) voices[voice].pitch = pitch & 0x3fff; }

void spu_voice_adsr2(unsigned voice, unsigned adsr2) { if (voice < 48) voices[voice].adsr2 = adsr2; }

int spu_core_master_volume(unsigned core, unsigned volume) {
    if (core >= 2 || (volume & 0x8000)) return -1;
    master[core][0] = master[core][1] = direct_volume(volume);
    return 0;
}

void spu_core_effect_volume(unsigned core, int volume) {
    if (core < 2) effects[core].depth[0] = effects[core].depth[1] = (int16_t)volume;
}

void spu_core_mixer(unsigned core, unsigned value) { if (core < 2) mixer[core] = value & (core ? 0xfff : 0xff0); }

void spu_core_input_gain(unsigned core, int left, int right) {
    if (core >= 2) return;
    input_gain[core][0] = (int16_t)left;
    input_gain[core][1] = (int16_t)right;
}

void spu_core_effect_end(unsigned core, unsigned address) { if (core < 2) effects[core].end = address & 0x1fffff; }

void spu_voices_stop(unsigned core, unsigned mask) {
    if (core >= 2) return;
    for (unsigned i = 0; i < 24; ++i) if (mask & (1u << i)) spu_voice_stop(core * 24 + i);
}

static int block(unsigned v) {
    static const int filters[5][2] = {{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
    if (voices[v].flags & 1) {
        if (voices[v].flags & 2) voices[v].address = voices[v].loop;
        else { voices[v].stage = 0; return 0; }
    }
    unsigned address = voices[v].address;
    if (address > sizeof(ram) - 16) { voices[v].stage = 0; return 0; }
    unsigned shift = ram[address] & 15;
    unsigned filter = ram[address] >> 4;
    if (filter > 4) filter = 0;
    if (shift > 12) shift = 9;
    voices[v].flags = ram[address + 1];
    if (voices[v].flags & 4) voices[v].loop = address;
    for (unsigned i = 0; i < 28; ++i) {
        int nibble = (ram[address + 2 + i / 2] >> ((i & 1) * 4)) & 15;
        if (nibble >= 8) nibble -= 16;
        int value = (nibble * 4096) >> shift;
        value += (voices[v].history1 * filters[filter][0] + voices[v].history2 * filters[filter][1] + 32) >> 6;
        value = clamp(value);
        voices[v].samples[i] = (int16_t)value;
        voices[v].history2 = voices[v].history1;
        voices[v].history1 = value;
    }
    voices[v].address += 16;
    voices[v].index = 0;
    return 1;
}

static void envelope(unsigned v) {
    unsigned stage = voices[v].stage;
    unsigned a = voices[v].adsr1, b = voices[v].adsr2;
    unsigned rate = stage == 1 ? (a >> 8) & 127 : stage == 2 ? ((a >> 4) & 15) * 4 : stage == 3 ? (b >> 6) & 127 : (b & 31) * 4;
    unsigned shift = rate >> 2;
    unsigned increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    int decreasing = stage == 2 || stage == 4 || (stage == 3 && (b & 0x4000));
    int exponential = stage == 2 || (stage == 1 && (a & 0x8000)) || (stage == 3 && (b & 0x8000)) || (stage == 4 && (b & 0x20));
    int step = decreasing ? -8 + (int)(rate & 3) : 7 - (int)(rate & 3);
    if (shift < 11) step *= 1 << (11 - shift);
    if (exponential && !decreasing && voices[v].envelope > 0x6000) {
        if (shift < 10) step >>= 2;
        else if (shift >= 11) increment >>= 2;
        else { step >>= 1; increment >>= 1; }
    }
    if ((stage == 4 && (b & 31) == 31) || ((stage == 1 || stage == 3) && rate == 127)) increment = 0;
    else if (!increment) increment = 1;
    voices[v].counter += increment;
    if (!(voices[v].counter & 0x8000)) return;
    voices[v].counter &= 0x7fff;
    if (exponential && decreasing) step = (step * voices[v].envelope) >> 15;
    voices[v].envelope += step;
    if (voices[v].envelope > 32767) voices[v].envelope = 32767;
    if (voices[v].envelope < 0) voices[v].envelope = 0;
    if (stage == 1 && voices[v].envelope == 32767) voices[v].stage = 2;
    if (stage == 2 && voices[v].envelope <= (int)((a & 15) + 1) * 2048) voices[v].stage = 3;
    if (stage == 4 && voices[v].envelope == 0) voices[v].stage = 0;
}

static int multiply(int value, int coefficient) { return (int)(((int64_t)value * coefficient) >> 15); }

static unsigned effect_address(unsigned core, int offset) {
    int position = (int)effects[core].cursor + offset;
    position %= (int)effects[core].length;
    if (position < 0) position += (int)effects[core].length;
    return effects[core].begin + (unsigned)position * 2;
}

static int effect_read(unsigned core, int offset) {
    unsigned address = effect_address(core, offset);
    return (int16_t)(ram[address] | (unsigned)ram[address + 1] << 8);
}

static void effect_write(unsigned core, int offset, int value) {
    if (!effects[core].enabled) return;
    unsigned address = effect_address(core, offset);
    unsigned sample = (uint16_t)clamp(value);
    ram[address] = sample;
    ram[address + 1] = sample >> 8;
}

int spu_core_effect(unsigned core, unsigned mode, int left, int right, int delay, int feedback) {
    if (core >= 2 || (mode & ~0x100u) > 9) return -1;
    unsigned preset = mode & ~0x100u;
    unsigned bytes = spu_reverb_sizes[preset] * 16;
    unsigned end = (effects[core].end & ~1u) + 2;
    if (end < bytes || end > sizeof(ram)) return -1;
    for (unsigned i = 0; i < 32; ++i) effects[core].params[i] = (uint16_t)(spu_reverb_presets[preset][i / 2] >> ((i & 1) * 16));
    effects[core].begin = end - bytes;
    effects[core].length = bytes / 2;
    effects[core].mode = preset;
    effects[core].depth[0] = left;
    effects[core].depth[1] = right;
    effects[core].cursor = 0;
    if (preset == 7 || preset == 8) {
        unsigned distance = ((unsigned)delay + 1) * 32;
        uint16_t *p = effects[core].params;
        p[7] = (uint16_t)((unsigned)feedback * 258);
        p[10] = (uint16_t)(distance * 2 - p[0]);
        p[11] = (uint16_t)(distance - p[1]);
        p[16] = (uint16_t)(p[17] + distance);
        p[12] = (uint16_t)(p[13] + distance);
        p[27] = (uint16_t)(p[29] + distance);
        p[26] = (uint16_t)(p[28] + distance);
    }
    if (mode & 0x100) {
        memset(ram + effects[core].begin, 0, bytes);
        memset(effects[core].down, 0, sizeof(effects[core].down));
        memset(effects[core].up, 0, sizeof(effects[core].up));
        effects[core].filter_cursor = effects[core].phase = 0;
    }
    return 0;
}

void spu_core_effect_enable(unsigned core, int enabled) { if (core < 2) effects[core].enabled = !!enabled; }

void spu_core_reverb(unsigned core, unsigned mode, int depth) {
    spu_core_effect_end(core, 0x1fffff - (core << 17));
    spu_core_effect(core, mode | 0x100, 0, 0, 0, 0);
    spu_core_effect_enable(core, 1);
    spu_core_effect_volume(core, (depth << 8) & 0xffff);
    spu_core_master_volume(core, 0x3fff);
}

static int effect_filter(const int16_t *samples, unsigned cursor, int scale) {
    static const int taps[39] = {-1,0,2,0,-10,0,35,0,-103,0,266,0,-616,0,1332,0,-2960,0,10246,16384,10246,0,-2960,0,1332,0,-616,0,266,0,-103,0,35,0,-10,0,2,0,-1};
    int64_t sum = 0;
    for (unsigned i = 0; i < 39; ++i) sum += (int64_t)samples[(cursor + 39 - i) % 39] * taps[i];
    return clamp((int)((sum * scale) >> 15));
}

static int effect_channel(unsigned core, unsigned channel, int input) {
    uint16_t *p = effects[core].params;
    int same = p[10 + channel] * 4;
    int different = p[18 + channel] * 4;
    int previous_same = effect_read(core, same - 1);
    int previous_different = effect_read(core, different - 1);
    int direct = multiply(input, (int16_t)p[30 + channel]);
    int reflection_same = previous_same + multiply(direct + multiply(effect_read(core, p[16 + channel] * 4), (int16_t)p[7]) - previous_same, (int16_t)p[2]);
    int reflection_different = previous_different + multiply(direct + multiply(effect_read(core, p[24 + (channel ^ 1)] * 4), (int16_t)p[7]) - previous_different, (int16_t)p[2]);
    static const unsigned comb[4] = {12,14,20,22};
    int value = 0;
    for (unsigned i = 0; i < 4; ++i) value += multiply(effect_read(core, p[comb[i] + channel] * 4), (int16_t)p[3 + i]);
    int writes[2];
    for (unsigned i = 0; i < 2; ++i) {
        int source = effect_read(core, ((int)p[26 + i * 2 + channel] - p[i]) * 4);
        writes[i] = value - multiply(source, (int16_t)p[8 + i]);
        value = source + multiply(writes[i], (int16_t)p[8 + i]);
    }
    effect_write(core, same, reflection_same);
    effect_write(core, different, reflection_different);
    for (unsigned i = 0; i < 2; ++i) effect_write(core, p[26 + i * 2 + channel] * 4, writes[i]);
    return clamp(value);
}

static void effect_sample(unsigned core, const int32_t *input, int32_t *output) {
    if (!effects[core].mode || !effects[core].length) return;
    unsigned cursor = effects[core].filter_cursor;
    unsigned channel = effects[core].phase;
    for (unsigned c = 0; c < 2; ++c) {
        effects[core].down[c][cursor] = (int16_t)clamp(input[c]);
        effects[core].up[c][cursor] = 0;
    }
    int filtered = effect_filter(effects[core].down[channel], cursor, 1);
    effects[core].up[channel][cursor] = (int16_t)effect_channel(core, channel, filtered);
    for (unsigned c = 0; c < 2; ++c) output[c] += multiply(effect_filter(effects[core].up[c], cursor, 2), effects[core].depth[c]);
    if (channel) effects[core].cursor = (effects[core].cursor + 1) % effects[core].length;
    effects[core].phase ^= 1;
    effects[core].filter_cursor = (cursor + 1) % 39;
}

void spu_core_input(unsigned core, int left, int right) {
    if (core < 2) { input_pcm[core][0] += left; input_pcm[core][1] += right; }
}

void spu_mix(int32_t *left, int32_t *right) {
    int32_t dry[2][2] = {{0}}, wet[2][2] = {{0}};
    for (unsigned v = 0; v < 48; ++v) {
        if (!voices[v].stage) continue;
        if (voices[v].index >= 28) {
            if (!block(v)) continue;
            voices[v].interpolation[3] = voices[v].samples[voices[v].index];
        }
        envelope(v);
        unsigned phase = voices[v].phase >> 4;
        int interpolated = multiply(voices[v].interpolation[0], spu_gaussian[255 - phase]) + multiply(voices[v].interpolation[1], spu_gaussian[511 - phase]) + multiply(voices[v].interpolation[2], spu_gaussian[256 + phase]) + multiply(voices[v].interpolation[3], spu_gaussian[phase]);
        int value = multiply(clamp(interpolated), voices[v].envelope);
        int levels[2] = {multiply(value, voices[v].left * 2), multiply(value, voices[v].right * 2)};
        for (unsigned c = 0; c < 2; ++c) {
            if (voices[v].route & (1u << c)) dry[v / 24][c] += levels[c];
            if (voices[v].route & (4u << c)) wet[v / 24][c] += levels[c];
        }
        voices[v].phase += voices[v].pitch;
        while (voices[v].phase >= 4096) {
            voices[v].phase -= 4096;
            for (unsigned i = 0; i < 3; ++i) voices[v].interpolation[i] = voices[v].interpolation[i + 1];
            if (++voices[v].index >= 28 && !block(v)) break;
            voices[v].interpolation[3] = voices[v].samples[voices[v].index];
        }
    }
    int32_t external[2] = {0};
    for (unsigned core = 0; core < 2; ++core) {
        int32_t mixed[2] = {0}, effect_input[2] = {0};
        for (unsigned c = 0; c < 2; ++c) {
            unsigned right_shift = c ^ 1;
            int incoming = multiply(clamp(input_pcm[core][c]), input_gain[core][c]);
            int forwarded = multiply(external[c], external_gain[core][c]);
            if (mixer[core] & (0x400u << right_shift)) mixed[c] += clamp(dry[core][c]);
            if (mixer[core] & (0x100u << right_shift)) effect_input[c] += clamp(wet[core][c]);
            if (mixer[core] & (0x40u << right_shift)) mixed[c] += incoming;
            if (mixer[core] & (0x10u << right_shift)) effect_input[c] += incoming;
            if (mixer[core] & (4u << right_shift)) mixed[c] += forwarded;
            if (mixer[core] & (1u << right_shift)) effect_input[c] += forwarded;
        }
        effect_sample(core, effect_input, mixed);
        for (unsigned c = 0; c < 2; ++c) external[c] = multiply(clamp(mixed[c]), master[core][c] * 2);
    }
    memset(input_pcm, 0, sizeof(input_pcm));
    *left += external[0];
    *right += external[1];
}
