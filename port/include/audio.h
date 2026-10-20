#pragma once
#include <stddef.h>
#include <stdint.h>
int audio_init(int enabled);
void audio_shutdown(void);
void audio_frame(void);
void audio_render(int16_t *output, size_t frames);
void audio_reset(void);
void audio_movie_pcm(const int16_t *samples, size_t frames, unsigned rate, unsigned channels);
void audio_movie_clear(void);
size_t audio_movie_queued(void);
int audio_stream_open(unsigned channel, const char *name);
int audio_stream_open_pack(unsigned channel, const char *name, const char *pack);
void audio_stream_close(unsigned channel);
int audio_stream_play(unsigned channel);
void audio_stream_stop(unsigned channel);
void audio_stream_rewind(unsigned channel);
void audio_stream_volume(unsigned channel, int left, int right);
unsigned audio_stream_state(unsigned channel);
int audio_stream_stereo(unsigned channel);
unsigned audio_stream_level(unsigned channel);
size_t audio_save(unsigned char *out);
size_t audio_load(const unsigned char *in);
