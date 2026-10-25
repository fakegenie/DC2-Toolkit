#include "spu2.h"
#include "spu2_gaussian.h"
#include "spu2_reverb_presets.h"
#include <string.h>

enum { SPU_ADPCM_SHIFT_LIMIT = 12, SPU_ADPCM_SHIFT_FALLBACK = 9, SPU_ADPCM_FILTERS = 5 };
enum { SPU_ADSR_OFF, SPU_ADSR_ATTACK, SPU_ADSR_DECAY, SPU_ADSR_SUSTAIN, SPU_ADSR_RELEASE };
enum { SPU_ENVELOPE_MAX = 0x7fff, SPU_ENVELOPE_COUNTER_CARRY = 0x8000, SPU_RATE_DISABLED = SPU_ATTACK_RATE_MAX, SPU_RELEASE_RATE_DISABLED = SPU_RELEASE_RATE_MAX };
enum { SPU_EFFECT_CORE_SPACING = 0x20000, REVERB_ADDRESS_SAMPLES = 4, REVERB_FILTER_TAPS = 39, REVERB_DEPTH_SCALE = 256 };
enum { ECHO_DELAY_STEP = 32, ECHO_FEEDBACK_SCALE = 258 };
enum { SPU_LEFT, SPU_RIGHT, SPU_CHANNELS };

struct spu_mix_gates {
    unsigned voice_dry, voice_wet, input_dry, input_wet, external_dry, external_wet;
};

static const struct spu_mix_gates mix_gates[SPU_CHANNELS] = {
    [SPU_LEFT] = {SPU_MIX_VOICE_DRY_LEFT, SPU_MIX_VOICE_WET_LEFT, SPU_MIX_INPUT_DRY_LEFT, SPU_MIX_INPUT_WET_LEFT,
                  SPU_MIX_EXTERNAL_DRY_LEFT, SPU_MIX_EXTERNAL_WET_LEFT},
    [SPU_RIGHT] = {SPU_MIX_VOICE_DRY_RIGHT, SPU_MIX_VOICE_WET_RIGHT, SPU_MIX_INPUT_DRY_RIGHT, SPU_MIX_INPUT_WET_RIGHT,
                   SPU_MIX_EXTERNAL_DRY_RIGHT, SPU_MIX_EXTERNAL_WET_RIGHT},
};
static const unsigned route_dry[SPU_CHANNELS] = {SPU_ROUTE_DRY_LEFT, SPU_ROUTE_DRY_RIGHT};
static const unsigned route_wet[SPU_CHANNELS] = {SPU_ROUTE_WET_LEFT, SPU_ROUTE_WET_RIGHT};
static const unsigned core_mixer_bits[SPU_CORES] = {SPU_MIX_ALL & ~SPU_MIX_EXTERNAL, SPU_MIX_ALL};

static unsigned char ram[SPU_RAM_SIZE];
static struct {
    unsigned address, start, loop, pitch, phase, index, flags;
    struct spu_adsr1 adsr1;
    struct spu_adsr2 adsr2;
    unsigned stage, counter;
    int left, right, history1, history2, envelope;
    int16_t samples[SPU_ADPCM_SAMPLES], interpolation[4];
    unsigned route;
} voices[SPU_VOICES];
static int master[SPU_CORES][SPU_CHANNELS];
static unsigned mixer[SPU_CORES];
static int input_gain[SPU_CORES][SPU_CHANNELS], external_gain[SPU_CORES][SPU_CHANNELS];
static int32_t input_pcm[SPU_CORES][SPU_CHANNELS];
static struct {
    struct spu_reverb_params params;
    unsigned end, begin, length, cursor, mode, enabled, phase;
    int depth[SPU_CHANNELS];
    int16_t down[SPU_CHANNELS][2 * REVERB_FILTER_TAPS], up[SPU_CHANNELS][2 * REVERB_FILTER_TAPS];
    unsigned filter_cursor;
} effects[SPU_CORES];

static int clamp(int value) {
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return value;
}

static unsigned effect_area_end(unsigned core) {
    return SPU_RAM_SIZE - 1 - core * SPU_EFFECT_CORE_SPACING;
}

void spu_reset(void) {
    memset(ram, 0, sizeof(ram));
    memset(voices, 0, sizeof(voices));
    memset(effects, 0, sizeof(effects));
    for (unsigned i = 0; i < SPU_CORES; ++i) effects[i].end = effect_area_end(i);
    for (unsigned i = 0; i < SPU_CORES; ++i) master[i][SPU_LEFT] = master[i][SPU_RIGHT] = SPU_VOLUME_MAX;
    for (unsigned i = 0; i < SPU_VOICES; ++i) voices[i].route = SPU_ROUTE_ALL;
    memset(input_pcm, 0, sizeof(input_pcm));
    for (unsigned core = 0; core < SPU_CORES; ++core) {
        mixer[core] = core_mixer_bits[core] & ~SPU_MIX_EXTERNAL_WET;
        for (unsigned channel = 0; channel < SPU_CHANNELS; ++channel) input_gain[core][channel] = external_gain[core][channel] = INT16_MAX;
    }
}

int spu_ram_write(unsigned address, const void *data, size_t size) {
    if (!data || address > sizeof(ram) || size > sizeof(ram) - address) return 0;
    memcpy(ram + address, data, size);
    return 1;
}

void spu_voice_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, struct spu_adsr1 adsr1, struct spu_adsr2 adsr2) {
    if (voice >= SPU_VOICES) return;
    memset(voices[voice].interpolation, 0, sizeof(voices[voice].interpolation));
    voices[voice].start = address;
    voices[voice].address = address;
    voices[voice].loop = address;
    voices[voice].pitch = pitch;
    voices[voice].left = left;
    voices[voice].right = right;
    voices[voice].adsr1 = adsr1;
    voices[voice].adsr2 = adsr2;
    voices[voice].index = SPU_ADPCM_SAMPLES;
    voices[voice].phase = 0;
    voices[voice].flags = 0;
    voices[voice].history1 = voices[voice].history2 = 0;
    voices[voice].envelope = 0;
    voices[voice].counter = 0;
    voices[voice].stage = SPU_ADSR_ATTACK;
}

void spu_voice_route(unsigned voice, unsigned route) { if (voice < SPU_VOICES) voices[voice].route = route & SPU_ROUTE_ALL; }

void spu_voice_stop(unsigned voice) {
    if (voice < SPU_VOICES && voices[voice].stage) { voices[voice].stage = SPU_ADSR_RELEASE; voices[voice].counter = 0; }
}

int spu_voice_active(unsigned voice) { return voice < SPU_VOICES && voices[voice].stage != SPU_ADSR_OFF; }

int spu_voice_level(unsigned voice) { return voice < SPU_VOICES ? voices[voice].envelope : 0; }

static int direct_volume(unsigned value) {
    int level = (int)(value & SPU_VOLUME_LEVEL);
    return value & SPU_VOLUME_NEGATIVE ? level - (SPU_VOLUME_LEVEL + 1) : level;
}

void spu_voice_volume(unsigned voice, int left, int right) {
    if (voice >= SPU_VOICES) return;
    voices[voice].left = left;
    voices[voice].right = right;
}

void spu_voice_pitch(unsigned voice, unsigned pitch) { if (voice < SPU_VOICES) voices[voice].pitch = pitch & SPU_PITCH_MAX; }

void spu_voice_adsr2(unsigned voice, struct spu_adsr2 adsr2) { if (voice < SPU_VOICES) voices[voice].adsr2 = adsr2; }

int spu_core_master_volume(unsigned core, unsigned volume) {
    if (core >= SPU_CORES || (volume & SPU_VOLUME_SWEEP)) return -1;
    master[core][SPU_LEFT] = master[core][SPU_RIGHT] = direct_volume(volume);
    return 0;
}

void spu_core_effect_volume(unsigned core, int volume) {
    if (core < SPU_CORES) effects[core].depth[SPU_LEFT] = effects[core].depth[SPU_RIGHT] = (int16_t)volume;
}

void spu_core_mixer(unsigned core, unsigned value) { if (core < SPU_CORES) mixer[core] = value & core_mixer_bits[core]; }

void spu_core_input_gain(unsigned core, int left, int right) {
    if (core >= SPU_CORES) return;
    input_gain[core][SPU_LEFT] = (int16_t)left;
    input_gain[core][SPU_RIGHT] = (int16_t)right;
}

void spu_core_effect_end(unsigned core, unsigned address) { if (core < SPU_CORES) effects[core].end = address & (SPU_RAM_SIZE - 1); }

void spu_voices_stop(unsigned core, unsigned mask) {
    if (core >= SPU_CORES) return;
    for (unsigned i = 0; i < SPU_CORE_VOICES; ++i) if (mask & (1u << i)) spu_voice_stop(core * SPU_CORE_VOICES + i);
}

static int block(unsigned v) {
    static const int filters[SPU_ADPCM_FILTERS][2] = {{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
    if (voices[v].flags & SPU_ADPCM_LOOP_END) {
        if (voices[v].flags & SPU_ADPCM_LOOP_REPEAT) voices[v].address = voices[v].loop;
        else { voices[v].stage = SPU_ADSR_OFF; return 0; }
    }
    unsigned address = voices[v].address;
    if (address > sizeof(ram) - sizeof(struct spu_adpcm_block)) { voices[v].stage = SPU_ADSR_OFF; return 0; }
    const struct spu_adpcm_block *adpcm = (const struct spu_adpcm_block *)(ram + address);
    unsigned shift = adpcm->shift;
    unsigned filter = adpcm->filter;
    if (filter >= SPU_ADPCM_FILTERS) filter = 0;
    if (shift > SPU_ADPCM_SHIFT_LIMIT) shift = SPU_ADPCM_SHIFT_FALLBACK;
    voices[v].flags = adpcm->flags;
    if (voices[v].flags & SPU_ADPCM_LOOP_START) voices[v].loop = address;
    for (unsigned i = 0; i < SPU_ADPCM_SAMPLES; ++i) {
        int nibble = (adpcm->data[i / 2] >> ((i & 1) * 4)) & 15;
        if (nibble >= 8) nibble -= 16;
        int value = (nibble * 4096) >> shift;
        value += (voices[v].history1 * filters[filter][0] + voices[v].history2 * filters[filter][1] + 32) >> 6;
        value = clamp(value);
        voices[v].samples[i] = (int16_t)value;
        voices[v].history2 = voices[v].history1;
        voices[v].history1 = value;
    }
    voices[v].address += sizeof(struct spu_adpcm_block);
    voices[v].index = 0;
    return 1;
}

static unsigned envelope_rate(unsigned v) {
    struct spu_adsr1 a = voices[v].adsr1;
    struct spu_adsr2 b = voices[v].adsr2;
    switch (voices[v].stage) {
        case SPU_ADSR_ATTACK: return a.attack_rate;
        case SPU_ADSR_DECAY: return a.decay_rate * 4u;
        case SPU_ADSR_SUSTAIN: return b.sustain_rate;
        default: return b.release_rate * 4u;
    }
}

static void envelope(unsigned v) {
    unsigned stage = voices[v].stage;
    struct spu_adsr1 a = voices[v].adsr1;
    struct spu_adsr2 b = voices[v].adsr2;
    unsigned rate = envelope_rate(v);
    unsigned shift = rate >> 2;
    unsigned increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    int decreasing = stage == SPU_ADSR_DECAY || stage == SPU_ADSR_RELEASE || (stage == SPU_ADSR_SUSTAIN && b.sustain_decrease);
    int exponential = stage == SPU_ADSR_DECAY || (stage == SPU_ADSR_ATTACK && a.attack_exponential) ||
                      (stage == SPU_ADSR_SUSTAIN && b.sustain_exponential) || (stage == SPU_ADSR_RELEASE && b.release_exponential);
    int step = decreasing ? -8 + (int)(rate & 3) : 7 - (int)(rate & 3);
    if (shift < 11) step *= 1 << (11 - shift);
    if (exponential && !decreasing && voices[v].envelope > 0x6000) {
        if (shift < 10) step >>= 2;
        else if (shift >= 11) increment >>= 2;
        else { step >>= 1; increment >>= 1; }
    }
    if ((stage == SPU_ADSR_RELEASE && b.release_rate == SPU_RELEASE_RATE_DISABLED) ||
        ((stage == SPU_ADSR_ATTACK || stage == SPU_ADSR_SUSTAIN) && rate == SPU_RATE_DISABLED)) increment = 0;
    else if (!increment) increment = 1;
    voices[v].counter += increment;
    if (!(voices[v].counter & SPU_ENVELOPE_COUNTER_CARRY)) return;
    voices[v].counter &= SPU_ENVELOPE_COUNTER_CARRY - 1;
    if (exponential && decreasing) step = (step * voices[v].envelope) >> 15;
    voices[v].envelope += step;
    if (voices[v].envelope > SPU_ENVELOPE_MAX) voices[v].envelope = SPU_ENVELOPE_MAX;
    if (voices[v].envelope < 0) voices[v].envelope = 0;
    if (stage == SPU_ADSR_ATTACK && voices[v].envelope == SPU_ENVELOPE_MAX) voices[v].stage = SPU_ADSR_DECAY;
    if (stage == SPU_ADSR_DECAY && voices[v].envelope <= (int)(a.sustain_level + 1) * 2048) voices[v].stage = SPU_ADSR_SUSTAIN;
    if (stage == SPU_ADSR_RELEASE && voices[v].envelope == 0) voices[v].stage = SPU_ADSR_OFF;
}

static int multiply(int value, int coefficient) { return (int)(((int64_t)value * coefficient) >> 15); }

static unsigned effect_address(unsigned core, int offset) {
    int position = (int)effects[core].cursor + offset;
    position %= (int)effects[core].length;
    if (position < 0) position += (int)effects[core].length;
    return effects[core].begin + (unsigned)position * sizeof(int16_t);
}

static int effect_read(unsigned core, int offset) {
    int16_t sample;
    memcpy(&sample, ram + effect_address(core, offset), sizeof sample);
    return sample;
}

static void effect_write(unsigned core, int offset, int value) {
    if (!effects[core].enabled) return;
    int16_t sample = (int16_t)clamp(value);
    memcpy(ram + effect_address(core, offset), &sample, sizeof sample);
}

int spu_core_effect(unsigned core, unsigned mode, int left, int right, int delay, int feedback) {
    unsigned preset = mode & ~(unsigned)SD_REV_MODE_CLEAR_WA;
    if (core >= SPU_CORES || preset >= SD_REV_MODE_MAX) return -1;
    unsigned bytes = spu_reverb_bytes[preset];
    unsigned end = (effects[core].end & ~1u) + sizeof(int16_t);
    if (end < bytes || end > sizeof(ram)) return -1;
    struct spu_reverb_params *p = &effects[core].params;
    *p = spu_reverb_presets[preset];
    effects[core].begin = end - bytes;
    effects[core].length = bytes / sizeof(int16_t);
    effects[core].mode = preset;
    effects[core].depth[SPU_LEFT] = left;
    effects[core].depth[SPU_RIGHT] = right;
    effects[core].cursor = 0;
    if (preset == SD_REV_MODE_ECHO || preset == SD_REV_MODE_DELAY) {
        unsigned distance = ((unsigned)delay + 1) * ECHO_DELAY_STEP;
        p->vWALL = (uint16_t)((unsigned)feedback * ECHO_FEEDBACK_SCALE);
        p->mSAME[SPU_LEFT] = (uint16_t)(distance * 2 - p->dAPF[0]);
        p->mSAME[SPU_RIGHT] = (uint16_t)(distance - p->dAPF[1]);
        p->dSAME[SPU_LEFT] = (uint16_t)(p->dSAME[SPU_RIGHT] + distance);
        p->mCOMB1[SPU_LEFT] = (uint16_t)(p->mCOMB1[SPU_RIGHT] + distance);
        p->mAPF[0][SPU_RIGHT] = (uint16_t)(p->mAPF[1][SPU_RIGHT] + distance);
        p->mAPF[0][SPU_LEFT] = (uint16_t)(p->mAPF[1][SPU_LEFT] + distance);
    }
    if (mode & SD_REV_MODE_CLEAR_WA) {
        memset(ram + effects[core].begin, 0, bytes);
        memset(effects[core].down, 0, sizeof(effects[core].down));
        memset(effects[core].up, 0, sizeof(effects[core].up));
        effects[core].filter_cursor = effects[core].phase = 0;
    }
    return 0;
}

void spu_core_effect_enable(unsigned core, int enabled) { if (core < SPU_CORES) effects[core].enabled = !!enabled; }

void spu_core_reverb(unsigned core, unsigned mode, int depth) {
    spu_core_effect_end(core, effect_area_end(core));
    spu_core_effect(core, mode | SD_REV_MODE_CLEAR_WA, 0, 0, 0, 0);
    spu_core_effect_enable(core, 1);
    spu_core_effect_volume(core, (uint16_t)(depth * REVERB_DEPTH_SCALE));
    spu_core_master_volume(core, SPU_VOLUME_MAX);
}

static int effect_filter(const int16_t *samples, unsigned cursor, int scale) {
    static const int taps[10] = {-1, 2, -10, 35, -103, 266, -616, 1332, -2960, 10246};
    static const int center_tap = 16384;
    const int16_t *newest = samples + cursor + REVERB_FILTER_TAPS;
    int64_t sum = (int64_t)newest[-(REVERB_FILTER_TAPS - 1) / 2] * center_tap;
    for (unsigned i = 0; i < 10; ++i) sum += (int64_t)(newest[-2 * (int)i] + newest[-(REVERB_FILTER_TAPS - 1) + 2 * (int)i]) * taps[i];
    return clamp((int)((sum * scale) >> 15));
}

static int effect_tap(unsigned core, unsigned address) {
    return effect_read(core, (int)address * REVERB_ADDRESS_SAMPLES);
}

static int effect_channel(unsigned core, unsigned channel, int input) {
    const struct spu_reverb_params *p = &effects[core].params;
    unsigned other = channel ^ 1;
    int same = p->mSAME[channel] * REVERB_ADDRESS_SAMPLES;
    int different = p->mDIFF[channel] * REVERB_ADDRESS_SAMPLES;
    int previous_same = effect_read(core, same - 1);
    int previous_different = effect_read(core, different - 1);
    int direct = multiply(input, (int16_t)p->vIN[channel]);
    int reflection_same = previous_same + multiply(direct + multiply(effect_tap(core, p->dSAME[channel]), (int16_t)p->vWALL) - previous_same, (int16_t)p->vIIR);
    int reflection_different = previous_different + multiply(direct + multiply(effect_tap(core, p->dDIFF[other]), (int16_t)p->vWALL) - previous_different, (int16_t)p->vIIR);
    int value = multiply(effect_tap(core, p->mCOMB1[channel]), (int16_t)p->vCOMB1) +
                multiply(effect_tap(core, p->mCOMB2[channel]), (int16_t)p->vCOMB2) +
                multiply(effect_tap(core, p->mCOMB3[channel]), (int16_t)p->vCOMB3) +
                multiply(effect_tap(core, p->mCOMB4[channel]), (int16_t)p->vCOMB4);
    int writes[2];
    for (unsigned stage = 0; stage < 2; ++stage) {
        int source = effect_read(core, ((int)p->mAPF[stage][channel] - p->dAPF[stage]) * REVERB_ADDRESS_SAMPLES);
        writes[stage] = value - multiply(source, (int16_t)p->vAPF[stage]);
        value = source + multiply(writes[stage], (int16_t)p->vAPF[stage]);
    }
    effect_write(core, same, reflection_same);
    effect_write(core, different, reflection_different);
    for (unsigned stage = 0; stage < 2; ++stage) effect_write(core, p->mAPF[stage][channel] * REVERB_ADDRESS_SAMPLES, writes[stage]);
    return clamp(value);
}

static void effect_sample(unsigned core, const int32_t *input, int32_t *output) {
    if (!effects[core].mode || !effects[core].length) return;
    unsigned cursor = effects[core].filter_cursor;
    unsigned channel = effects[core].phase;
    for (unsigned c = 0; c < SPU_CHANNELS; ++c) {
        effects[core].down[c][cursor] = effects[core].down[c][cursor + REVERB_FILTER_TAPS] = (int16_t)clamp(input[c]);
        effects[core].up[c][cursor] = effects[core].up[c][cursor + REVERB_FILTER_TAPS] = 0;
    }
    int filtered = effect_filter(effects[core].down[channel], cursor, 1);
    effects[core].up[channel][cursor] = effects[core].up[channel][cursor + REVERB_FILTER_TAPS] = (int16_t)effect_channel(core, channel, filtered);
    for (unsigned c = 0; c < SPU_CHANNELS; ++c) output[c] += multiply(effect_filter(effects[core].up[c], cursor, 2), effects[core].depth[c]);
    if (channel) effects[core].cursor = (effects[core].cursor + 1) % effects[core].length;
    effects[core].phase ^= 1;
    effects[core].filter_cursor = (cursor + 1) % REVERB_FILTER_TAPS;
}

void spu_core_input(unsigned core, int left, int right) {
    if (core < SPU_CORES) { input_pcm[core][SPU_LEFT] += left; input_pcm[core][SPU_RIGHT] += right; }
}

void spu_mix(int32_t *left, int32_t *right) {
    int32_t dry[SPU_CORES][SPU_CHANNELS] = {{0}}, wet[SPU_CORES][SPU_CHANNELS] = {{0}};
    for (unsigned v = 0; v < SPU_VOICES; ++v) {
        if (!voices[v].stage) continue;
        if (voices[v].index >= SPU_ADPCM_SAMPLES) {
            if (!block(v)) continue;
            voices[v].interpolation[3] = voices[v].samples[voices[v].index];
        }
        envelope(v);
        unsigned phase = voices[v].phase >> 4;
        int interpolated = multiply(voices[v].interpolation[0], spu_gaussian[255 - phase]) + multiply(voices[v].interpolation[1], spu_gaussian[511 - phase]) + multiply(voices[v].interpolation[2], spu_gaussian[256 + phase]) + multiply(voices[v].interpolation[3], spu_gaussian[phase]);
        int value = multiply(clamp(interpolated), voices[v].envelope);
        int levels[SPU_CHANNELS] = {multiply(value, voices[v].left * 2), multiply(value, voices[v].right * 2)};
        unsigned core = v / SPU_CORE_VOICES;
        for (unsigned c = 0; c < SPU_CHANNELS; ++c) {
            if (voices[v].route & route_dry[c]) dry[core][c] += levels[c];
            if (voices[v].route & route_wet[c]) wet[core][c] += levels[c];
        }
        voices[v].phase += voices[v].pitch;
        while (voices[v].phase >= SPU_PITCH_UNITY) {
            voices[v].phase -= SPU_PITCH_UNITY;
            for (unsigned i = 0; i < 3; ++i) voices[v].interpolation[i] = voices[v].interpolation[i + 1];
            if (++voices[v].index >= SPU_ADPCM_SAMPLES && !block(v)) break;
            voices[v].interpolation[3] = voices[v].samples[voices[v].index];
        }
    }
    int32_t external[SPU_CHANNELS] = {0};
    for (unsigned core = 0; core < SPU_CORES; ++core) {
        int32_t mixed[SPU_CHANNELS] = {0}, effect_input[SPU_CHANNELS] = {0};
        for (unsigned c = 0; c < SPU_CHANNELS; ++c) {
            const struct spu_mix_gates *gates = &mix_gates[c];
            int incoming = multiply(clamp(input_pcm[core][c]), input_gain[core][c]);
            int forwarded = multiply(external[c], external_gain[core][c]);
            if (mixer[core] & gates->voice_dry) mixed[c] += clamp(dry[core][c]);
            if (mixer[core] & gates->voice_wet) effect_input[c] += clamp(wet[core][c]);
            if (mixer[core] & gates->input_dry) mixed[c] += incoming;
            if (mixer[core] & gates->input_wet) effect_input[c] += incoming;
            if (mixer[core] & gates->external_dry) mixed[c] += forwarded;
            if (mixer[core] & gates->external_wet) effect_input[c] += forwarded;
        }
        effect_sample(core, effect_input, mixed);
        for (unsigned c = 0; c < SPU_CHANNELS; ++c) external[c] = multiply(clamp(mixed[c]), master[core][c] * 2);
    }
    memset(input_pcm, 0, sizeof(input_pcm));
    *left += external[SPU_LEFT];
    *right += external[SPU_RIGHT];
}
