#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SPU_RAM_SIZE = 2 * 1024 * 1024, SPU_CORES = 2, SPU_CORE_VOICES = 24, SPU_VOICES = SPU_CORES * SPU_CORE_VOICES };
enum { SPU_CORE_MOVIE = 1, SPU_SAMPLE_RATE = 48000, SPU_PITCH_UNITY = 0x1000, SPU_PITCH_MAX = 0x3fff, SPU_VOLUME_MAX = 0x3fff };
enum { SPU_VOLUME_SWEEP = 0x8000, SPU_VOLUME_NEGATIVE = 0x4000, SPU_VOLUME_LEVEL = 0x7fff };
enum { SPU_ATTACK_RATE_MAX = 0x7f, SPU_DECAY_RATE_MAX = 0xf, SPU_SUSTAIN_LEVEL_MAX = 0xf, SPU_SUSTAIN_RATE_MAX = 0x7f, SPU_RELEASE_RATE_MAX = 0x1f };

enum {
    SPU_ROUTE_DRY_LEFT = 0x1,
    SPU_ROUTE_DRY_RIGHT = 0x2,
    SPU_ROUTE_WET_LEFT = 0x4,
    SPU_ROUTE_WET_RIGHT = 0x8,
    SPU_ROUTE_ALL = 0xf
};

enum {
    SPU_MIX_EXTERNAL_WET_RIGHT = 0x001,
    SPU_MIX_EXTERNAL_WET_LEFT = 0x002,
    SPU_MIX_EXTERNAL_DRY_RIGHT = 0x004,
    SPU_MIX_EXTERNAL_DRY_LEFT = 0x008,
    SPU_MIX_INPUT_WET_RIGHT = 0x010,
    SPU_MIX_INPUT_WET_LEFT = 0x020,
    SPU_MIX_INPUT_DRY_RIGHT = 0x040,
    SPU_MIX_INPUT_DRY_LEFT = 0x080,
    SPU_MIX_VOICE_WET_RIGHT = 0x100,
    SPU_MIX_VOICE_WET_LEFT = 0x200,
    SPU_MIX_VOICE_DRY_RIGHT = 0x400,
    SPU_MIX_VOICE_DRY_LEFT = 0x800,
    SPU_MIX_EXTERNAL_WET = SPU_MIX_EXTERNAL_WET_LEFT | SPU_MIX_EXTERNAL_WET_RIGHT,
    SPU_MIX_EXTERNAL = SPU_MIX_EXTERNAL_WET | SPU_MIX_EXTERNAL_DRY_LEFT | SPU_MIX_EXTERNAL_DRY_RIGHT,
    SPU_MIX_INPUT_WET = SPU_MIX_INPUT_WET_LEFT | SPU_MIX_INPUT_WET_RIGHT,
    SPU_MIX_ALL = 0xfff
};

enum { SPU_ADPCM_SAMPLES = 28 };
enum { SPU_ADPCM_LOOP_END = 0x1, SPU_ADPCM_LOOP_REPEAT = 0x2, SPU_ADPCM_LOOP_START = 0x4 };

struct spu_adpcm_block {
    uint8_t shift : 4;
    uint8_t filter : 4;
    uint8_t flags;
    uint8_t data[SPU_ADPCM_SAMPLES / 2];
};
_Static_assert(sizeof(struct spu_adpcm_block) == 16, "SPU2 ADPCM block");

struct spu_adsr1 {
    uint16_t sustain_level : 4;
    uint16_t decay_rate : 4;
    uint16_t attack_rate : 7;
    uint16_t attack_exponential : 1;
};
_Static_assert(sizeof(struct spu_adsr1) == 2, "SPU2 ADSR1 register");

struct spu_adsr2 {
    uint16_t release_rate : 5;
    uint16_t release_exponential : 1;
    uint16_t sustain_rate : 7;
    uint16_t unused : 1;
    uint16_t sustain_decrease : 1;
    uint16_t sustain_exponential : 1;
};
_Static_assert(sizeof(struct spu_adsr2) == 2, "SPU2 ADSR2 register");

void spu_reset(void);
int spu_ram_write(unsigned address, const void *data, size_t size);
void spu_voice_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, struct spu_adsr1 adsr1, struct spu_adsr2 adsr2);
void spu_voice_stop(unsigned voice);
void spu_voice_route(unsigned voice, unsigned route);
void spu_voice_volume(unsigned voice, int left, int right);
void spu_voice_pitch(unsigned voice, unsigned pitch);
void spu_voice_adsr2(unsigned voice, struct spu_adsr2 adsr2);
int spu_voice_active(unsigned voice);
int spu_voice_level(unsigned voice);
void spu_voices_stop(unsigned core, unsigned mask);
int spu_core_master_volume(unsigned core, unsigned volume);
void spu_core_effect_volume(unsigned core, int volume);
void spu_core_mixer(unsigned core, unsigned value);
void spu_core_input_gain(unsigned core, int left, int right);
void spu_core_input(unsigned core, int left, int right);
int spu_core_effect(unsigned core, unsigned mode, int left, int right, int delay, int feedback);
void spu_core_effect_end(unsigned core, unsigned address);
void spu_core_effect_enable(unsigned core, int enabled);
void spu_core_reverb(unsigned core, unsigned mode, int depth);
void spu_mix(int32_t *left, int32_t *right);
