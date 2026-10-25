#pragma once
#include <stddef.h>
#include <stdint.h>

enum { AUDIO_RATE = 48000, AUDIO_FRAMES_PER_FIELD = 960, AUDIO_STREAMS = 2 };
enum { AUDIO_STREAM_STOPPED, AUDIO_STREAM_PLAYING, AUDIO_STREAM_ENDED };
enum { WAVE_FORMAT_PCM = 1 };

struct sound_index_entry {
    uint32_t name_offset;
    uint32_t byte_size;
    uint32_t start_sector;
    uint32_t sector_count;
};
_Static_assert(sizeof(struct sound_index_entry) == 16, "SOUND.HD3 entry");

struct riff_chunk {
    char id[4];
    uint32_t size;
};
_Static_assert(sizeof(struct riff_chunk) == 8, "RIFF chunk header");

struct riff_header {
    struct riff_chunk chunk;
    char form[4];
};
_Static_assert(sizeof(struct riff_header) == 12, "RIFF file header");

struct wav_fmt {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};
_Static_assert(sizeof(struct wav_fmt) == 16, "WAVE fmt chunk");

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
