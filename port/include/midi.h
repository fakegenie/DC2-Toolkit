#pragma once
#include <stddef.h>
#include <stdint.h>
void midi_reset(void);
void midi_tick(void);
int midi_opened(void);
void midi_open(void);
void midi_close(void);
void midi_port_delete(unsigned port);
int midi_bank_first(unsigned port, const void *hd, int hd_size, const void *bd, int bd_size);
int midi_bank_add(unsigned port, const void *hd, int hd_size, const void *bd, int bd_size);
int midi_sequence_load(unsigned port, const void *data, int size);
void midi_sequence_play(unsigned port, int index, int volume);
void midi_sequence_resume(unsigned port);
void midi_stop(unsigned port);
int midi_playing(unsigned port);
void midi_volume(unsigned port, int volume);
void midi_stereo(int mode);
int midi_message(unsigned port, unsigned status, unsigned a, unsigned b);
void midi_effect_play(unsigned port, int bank, int program, int key, int pan, int velocity, int volume, int id);
void midi_effect_stop(unsigned port, int bank, int program, int key, int id);
void midi_effect_volume(unsigned port, int bank, int program, int key, int volume, int id);
void midi_effect_pan(unsigned port, int bank, int program, int key, int pan, int id);
void midi_effect_pitch(unsigned port, int bank, int program, int key, int pitch, int id);
size_t midi_save(unsigned char *out);
size_t midi_load(const unsigned char *in);
