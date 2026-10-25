#include "audio.h"
#include "spu2.h"
#include "midi.h"
#include "cdvd.h"
#include "port_mod.h"
#include "port_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PORT_GRAPHICS
#include <SDL3/SDL.h>
#define AUDIO_TARGET 3.0f
#define AUDIO_LIMIT 12.0f
#define AUDIO_PULL 0.004f
#define AUDIO_SWAY 0.01f

static SDL_AudioStream *audio;
#endif

enum { MOVIE_PCM_FRAMES = 4 * AUDIO_RATE, STREAM_SAMPLE_BITS = 16 };
enum { SOUND_INDEX_LIMIT = 1024 * 1024, STREAM_SIZE_LIMIT = 64 * 1024 * 1024 };

struct stereo_frame {
    int16_t left, right;
};

static struct {
    unsigned char *data;
    size_t size, position;
    unsigned channels, state, end_frames;
} streams[AUDIO_STREAMS];
static struct stereo_frame movie_pcm[MOVIE_PCM_FRAMES];
static size_t movie_head, movie_count;

void audio_movie_clear(void) { movie_head = movie_count = 0; }

size_t audio_movie_queued(void) { return movie_count; }

void audio_movie_pcm(const int16_t *samples, size_t frames, unsigned rate, unsigned channels) {
    if (!samples || !rate || (channels != 1 && channels != 2)) return;
    size_t count = frames * AUDIO_RATE / rate;
    for (size_t i = 0; i < count && movie_count < MOVIE_PCM_FRAMES; ++i) {
        const int16_t *source = samples + i * rate / AUDIO_RATE * channels;
        struct stereo_frame *target = &movie_pcm[(movie_head + movie_count) % MOVIE_PCM_FRAMES];
        target->left = source[0];
        target->right = source[channels - 1];
        ++movie_count;
    }
}

void audio_reset(void) {
    audio_movie_clear();
    for (unsigned i = 0; i < AUDIO_STREAMS; ++i) {
        free(streams[i].data);
        memset(&streams[i], 0, sizeof(streams[i]));
    }
    midi_reset();
    spu_reset();
}

static int sound_index_find(const char *name, uint64_t *offset, size_t *size) {
    struct sceCdlFILE file;
    if (!port_cdvd_find("cdrom0:\\SOUND.HD3;1", &file) || file.size < sizeof(struct sound_index_entry) || file.size > SOUND_INDEX_LIMIT) return 0;
    char *index = malloc(file.size);
    if (!index) return 0;
    if (!port_cdvd_read_file(&file, 0, index, file.size)) { free(index); return 0; }
    const struct sound_index_entry *entries = (const struct sound_index_entry *)index;
    uint32_t names = entries[0].name_offset;
    *size = 0;
    if (names && !(names % sizeof(struct sound_index_entry)) && names <= file.size) {
        for (uint32_t i = 0; i < names / sizeof(struct sound_index_entry); i++) {
            uint32_t name_at = entries[i].name_offset;
            if (name_at < names || name_at >= file.size || !memchr(index + name_at, 0, file.size - name_at)) continue;
            if (strcmp(index + name_at, name)) continue;
            *size = entries[i].byte_size;
            *offset = (uint64_t)entries[i].start_sector * CDVD_SECTOR_SIZE;
            break;
        }
    }
    free(index);
    return *size != 0;
}

int audio_stream_open(unsigned channel, const char *name) {
    if (channel >= AUDIO_STREAMS) return -1;
    struct sceCdlFILE file;
    uint64_t offset = 0;
    size_t size;
    if (strchr(name, ':')) {
        if (!port_cdvd_find(name, &file)) return -1;
        size = file.size;
    } else if (!sound_index_find(name, &offset, &size) || !port_cdvd_find("cdrom0:\\SOUND.DAT;1", &file)) {
        return -1;
    }
    if (size > STREAM_SIZE_LIMIT || offset > file.size || size > file.size - offset || size < sizeof(struct riff_header)) return -1;
    unsigned char *wave = malloc(size);
    if (!wave) return -1;
    if (!port_cdvd_read_file(&file, offset, wave, size)) { free(wave); return -1; }
    struct riff_header header;
    memcpy(&header, wave, sizeof header);
    if (memcmp(header.chunk.id, "RIFF", sizeof header.chunk.id) || memcmp(header.form, "WAVE", sizeof header.form)) { free(wave); return -1; }
    unsigned channels = 0;
    for (size_t at = sizeof header; at <= size - sizeof(struct riff_chunk);) {
        struct riff_chunk chunk;
        memcpy(&chunk, wave + at, sizeof chunk);
        at += sizeof chunk;
        if (chunk.size > size - at) break;
        if (!memcmp(chunk.id, "fmt ", sizeof chunk.id) && chunk.size >= sizeof(struct wav_fmt)) {
            struct wav_fmt format;
            memcpy(&format, wave + at, sizeof format);
            if (format.format != WAVE_FORMAT_PCM || format.bits_per_sample != STREAM_SAMPLE_BITS || format.sample_rate != AUDIO_RATE) break;
            channels = format.channels;
            if (channels != 1 && channels != 2) break;
        }
        if (!memcmp(chunk.id, "data", sizeof chunk.id) && channels) {
            size_t frame_size = channels * sizeof(int16_t);
            memmove(wave, wave + at, chunk.size);
            free(streams[channel].data);
            streams[channel].data = wave;
            streams[channel].size = chunk.size - chunk.size % frame_size;
            streams[channel].position = 0;
            streams[channel].channels = channels;
            streams[channel].state = AUDIO_STREAM_STOPPED;
            return channels == 2;
        }
        at += chunk.size;
    }
    free(wave);
    return -1;
}


int audio_stream_open_pack(unsigned channel, const char *name, const char *pack) {
    static unsigned warnings;
    if (channel >= AUDIO_STREAMS) return -1;
    if (warnings++ < 24) fprintf(stderr, "sound: streams from packs are not supported (%s in %s)\n", name, pack);
    return -1;
}

void audio_stream_close(unsigned channel) {
    if (channel >= AUDIO_STREAMS) return;
    free(streams[channel].data);
    streams[channel].data = NULL;
    streams[channel].size = 0;
    streams[channel].state = AUDIO_STREAM_STOPPED;
}

int audio_stream_play(unsigned channel) {
    if (channel >= AUDIO_STREAMS || !streams[channel].data) return -1;
    streams[channel].state = AUDIO_STREAM_PLAYING;
    return 0;
}

void audio_stream_stop(unsigned channel) {
    if (channel < AUDIO_STREAMS && streams[channel].state == AUDIO_STREAM_PLAYING) streams[channel].state = AUDIO_STREAM_STOPPED;
}

void audio_stream_rewind(unsigned channel) { if (channel < AUDIO_STREAMS) streams[channel].position = 0; }

void audio_stream_volume(unsigned channel, int left, int right) { spu_core_input_gain(channel, left, right); }

unsigned audio_stream_state(unsigned channel) { return channel < AUDIO_STREAMS ? streams[channel].state : AUDIO_STREAM_STOPPED; }

int audio_stream_stereo(unsigned channel) { return channel < AUDIO_STREAMS && streams[channel].channels == 2; }

unsigned audio_stream_level(unsigned channel) {
    if (channel >= AUDIO_STREAMS || streams[channel].state != AUDIO_STREAM_PLAYING || !streams[channel].data ||
        streams[channel].position >= streams[channel].size) return 0;
    size_t available = streams[channel].size - streams[channel].position;
    const unsigned char *sample = streams[channel].data + streams[channel].position;
    uint32_t level = 0;
    if (available >= sizeof(uint32_t)) memcpy(&level, sample, sizeof(uint32_t));
    else if (available >= sizeof(uint16_t)) memcpy(&level, sample, sizeof(uint16_t));
    return level;
}

static int16_t clamp_sample(int32_t value) {
    return (int16_t)(value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : value);
}

void audio_render(int16_t *output, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        int32_t left = 0, right = 0;
        if (movie_count) {
            spu_core_input(SPU_CORE_MOVIE, movie_pcm[movie_head].left, movie_pcm[movie_head].right);
            movie_head = (movie_head + 1) % MOVIE_PCM_FRAMES;
            --movie_count;
        }
        midi_tick();
        for (unsigned s = 0; s < AUDIO_STREAMS; ++s) {
            if (streams[s].state == AUDIO_STREAM_ENDED && streams[s].end_frames && !--streams[s].end_frames) streams[s].state = AUDIO_STREAM_STOPPED;
            if (streams[s].state != AUDIO_STREAM_PLAYING || !streams[s].data) continue;
            if (streams[s].position >= streams[s].size) {
                streams[s].state = AUDIO_STREAM_ENDED;
                streams[s].end_frames = AUDIO_RATE;
                continue;
            }
            int16_t frame[2];
            memcpy(frame, streams[s].data + streams[s].position, streams[s].channels * sizeof(int16_t));
            int l = frame[0];
            int r = streams[s].channels == 2 ? frame[1] : l;
            streams[s].position += streams[s].channels * sizeof(int16_t);
            spu_core_input(s, l, r);
        }
        spu_mix(&left, &right);
        output[i * 2] = clamp_sample(left);
        output[i * 2 + 1] = clamp_sample(right);
    }
}

int audio_init(int enabled) {
    audio_reset();
#ifdef PORT_GRAPHICS
    SDL_AudioSpec spec = {.format = SDL_AUDIO_S16, .channels = 2, .freq = AUDIO_RATE};
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio && (!SDL_SetAudioStreamGain(audio, enabled ? 1.0f : 0.0f) || !SDL_ResumeAudioStreamDevice(audio))) {
        SDL_DestroyAudioStream(audio);
        audio = NULL;
    }
    if (!audio && enabled) { fprintf(stderr, "sound: %s\n", SDL_GetError()); return 0; }
#else
    (void)enabled;
#endif
    return 1;
}

void audio_frame(void) {
    int16_t samples[AUDIO_FRAMES_PER_FIELD * 2];
    audio_render(samples, AUDIO_FRAMES_PER_FIELD);
    port_mods_audio(samples, AUDIO_FRAMES_PER_FIELD);
#ifdef PORT_GRAPHICS
    if (!audio) return;
    float fill = (float)SDL_GetAudioStreamQueued(audio) / (float)sizeof(samples);
    port_stats.audio_ms = (uint32_t)(fill * AUDIO_FRAMES_PER_FIELD * 1000 / AUDIO_RATE);
    if (fill >= AUDIO_LIMIT) return;
    float ratio = 1.0f + (fill - AUDIO_TARGET) * AUDIO_PULL;
    SDL_SetAudioStreamFrequencyRatio(audio, ratio < 1.0f - AUDIO_SWAY ? 1.0f - AUDIO_SWAY : ratio > 1.0f + AUDIO_SWAY ? 1.0f + AUDIO_SWAY : ratio);
    if (!SDL_PutAudioStreamData(audio, samples, sizeof(samples))) fprintf(stderr, "sound: %s\n", SDL_GetError());
#endif
}

void audio_shutdown(void) {
#ifdef PORT_GRAPHICS
    SDL_DestroyAudioStream(audio);
    audio = NULL;
#endif
    audio_reset();
}

size_t audio_save(unsigned char *out) {
    size_t at = sizeof(streams);
    if (out) memcpy(out, streams, sizeof(streams));
    for (unsigned i = 0; i < AUDIO_STREAMS; ++i) {
        if (!streams[i].data) continue;
        if (out) memcpy(out + at, streams[i].data, streams[i].size);
        at += streams[i].size;
    }
    return at + midi_save(out ? out + at : NULL);
}

size_t audio_load(const unsigned char *in) {
    for (unsigned i = 0; i < AUDIO_STREAMS; ++i) free(streams[i].data);
    memcpy(streams, in, sizeof(streams));
    size_t at = sizeof(streams);
    for (unsigned i = 0; i < AUDIO_STREAMS; ++i) {
        if (!streams[i].data) continue;
        streams[i].data = malloc(streams[i].size);
        memcpy(streams[i].data, in + at, streams[i].size);
        at += streams[i].size;
    }
    audio_movie_clear();
#ifdef PORT_GRAPHICS
    if (audio) SDL_ClearAudioStream(audio);
#endif
    return at + midi_load(in + at);
}
