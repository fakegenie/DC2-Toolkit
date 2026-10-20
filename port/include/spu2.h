#pragma once
#include <stddef.h>
#include <stdint.h>
void spu_reset(void);
int spu_ram_write(unsigned address, const void *data, size_t size);
void spu_voice_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, unsigned adsr1, unsigned adsr2);
void spu_voice_stop(unsigned voice);
void spu_voice_route(unsigned voice, unsigned route);
void spu_voice_volume(unsigned voice, int left, int right);
void spu_voice_pitch(unsigned voice, unsigned pitch);
void spu_voice_adsr2(unsigned voice, unsigned adsr2);
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
