#include "audio.h"
#include "spu2.h"
#include "midi.h"
#include "cdvd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PORT_GRAPHICS
#include <SDL3/SDL.h>
static SDL_AudioStream *audio;
#endif

static struct {
    unsigned char *data;
    size_t size, position;
    unsigned channels, state, end_frames;
} streams[2];
static int16_t movie_pcm[192000 * 2];
static size_t movie_head, movie_count;
static FILE *dump;
static int dump_checked;

static uint32_t u32(const void *address) {
    const unsigned char *p = address;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }

void audio_movie_clear(void) { movie_head = movie_count = 0; }

size_t audio_movie_queued(void) { return movie_count; }

void audio_movie_pcm(const int16_t *samples, size_t frames, unsigned rate, unsigned channels) {
    if (!samples || !rate || (channels != 1 && channels != 2)) return;
    size_t count = frames * 48000 / rate;
    for (size_t i = 0; i < count && movie_count < 192000; ++i) {
        size_t source = i * rate / 48000;
        size_t target = (movie_head + movie_count) % 192000;
        movie_pcm[target * 2] = samples[source * channels];
        movie_pcm[target * 2 + 1] = samples[source * channels + channels - 1];
        ++movie_count;
    }
}

void audio_reset(void) {
    audio_movie_clear();
    for (unsigned i = 0; i < 2; ++i) {
        free(streams[i].data);
        memset(&streams[i], 0, sizeof(streams[i]));
    }
    midi_reset();
    spu_reset();
}

int audio_stream_open(unsigned channel, const char *name) {
    if (channel >= 2) return -1;
    struct sceCdlFILE file;
    uint64_t offset = 0;
    size_t size;
    unsigned char *index = NULL;
    if (strchr(name, ':')) {
        if (!port_cdvd_find(name, &file)) return -1;
        size = file.size;
    } else {
        if (!port_cdvd_find("cdrom0:\\SOUND.HD3;1", &file) || file.size < 16 || file.size > 1024 * 1024) return -1;
        index = malloc(file.size);
        if (!index) return -1;
        if (!port_cdvd_read_file(&file, 0, index, file.size)) { free(index); return -1; }
        unsigned table = u32(index);
        if (!table || table % 16 || table > file.size) { free(index); return -1; }
        size = 0;
        for (unsigned record = 0; record < table; record += 16) {
            unsigned entry = u32(index + record);
            if (entry < table || entry >= file.size || !memchr(index + entry, 0, file.size - entry)) continue;
            if (strcmp((char *)index + entry, name)) continue;
            size = u32(index + record + 4);
            offset = (uint64_t)u32(index + record + 8) * 2048;
            break;
        }
        free(index);
        if (!size || !port_cdvd_find("cdrom0:\\SOUND.DAT;1", &file)) return -1;
    }
    if (size > 64 * 1024 * 1024 || offset > file.size || size > file.size - offset || size < 12) return -1;
    unsigned char *wave = malloc(size);
    if (!wave) return -1;
    if (!port_cdvd_read_file(&file, offset, wave, size) || memcmp(wave, "RIFF", 4) || memcmp(wave + 8, "WAVE", 4)) { free(wave); return -1; }
    unsigned channels = 0;
    for (size_t chunk = 12; chunk <= size - 8;) {
        size_t length = u32(wave + chunk + 4);
        chunk += 8;
        if (length > size - chunk) break;
        if (!memcmp(wave + chunk - 8, "fmt ", 4) && length >= 16) {
            if (u16(wave + chunk) != 1 || u16(wave + chunk + 14) != 16 || u32(wave + chunk + 4) != 48000) break;
            channels = u16(wave + chunk + 2);
            if (channels != 1 && channels != 2) break;
        }
        if (!memcmp(wave + chunk - 8, "data", 4) && channels) {
            memmove(wave, wave + chunk, length);
            free(streams[channel].data);
            streams[channel].data = wave;
            streams[channel].size = length - length % (channels * 2);
            streams[channel].position = 0;
            streams[channel].channels = channels;
            streams[channel].state = 0;
            return channels == 2;
        }
        chunk += length;
    }
    free(wave);
    return -1;
}


int audio_stream_open_pack(unsigned channel, const char *name, const char *pack) {
    static unsigned warnings;
    if (channel >= 2) return -1;
    if (warnings++ < 24) fprintf(stderr, "sound: streams from packs are not supported (%s in %s)\n", name, pack);
    return -1;
}

void audio_stream_close(unsigned channel) {
    if (channel >= 2) return;
    free(streams[channel].data);
    streams[channel].data = NULL;
    streams[channel].size = 0;
    streams[channel].state = 0;
}

int audio_stream_play(unsigned channel) {
    if (channel >= 2 || !streams[channel].data) return -1;
    streams[channel].state = 0x1000;
    return 0;
}

void audio_stream_stop(unsigned channel) { if (channel < 2) streams[channel].state &= ~0x1000u; }

void audio_stream_rewind(unsigned channel) { if (channel < 2) streams[channel].position = 0; }

void audio_stream_volume(unsigned channel, int left, int right) { spu_core_input_gain(channel, left, right); }

unsigned audio_stream_state(unsigned channel) { return channel < 2 ? streams[channel].state : 0; }

int audio_stream_stereo(unsigned channel) { return channel < 2 && streams[channel].channels == 2; }

unsigned audio_stream_level(unsigned channel) {
    if (channel >= 2 || !(streams[channel].state & 0x1000) || !streams[channel].data || streams[channel].position >= streams[channel].size) return 0;
    size_t available = streams[channel].size - streams[channel].position;
    const unsigned char *sample = streams[channel].data + streams[channel].position;
    return available >= 4 ? u32(sample) : available >= 2 ? u16(sample) : 0;
}

void audio_render(int16_t *output, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        int32_t left = 0, right = 0;
        if (movie_count) {
            spu_core_input(1, movie_pcm[movie_head * 2], movie_pcm[movie_head * 2 + 1]);
            movie_head = (movie_head + 1) % 192000;
            --movie_count;
        }
        midi_tick();
        for (unsigned s = 0; s < 2; ++s) {
            if (streams[s].state == 0x8000 && streams[s].end_frames && !--streams[s].end_frames) streams[s].state = 0;
            if (!(streams[s].state & 0x1000) || !streams[s].data) continue;
            if (streams[s].position >= streams[s].size) {
                streams[s].state = 0x8000;
                streams[s].end_frames = 48000;
                continue;
            }
            const unsigned char *p = streams[s].data + streams[s].position;
            int l = (int16_t)u16(p);
            int r = streams[s].channels == 2 ? (int16_t)u16(p + 2) : l;
            streams[s].position += streams[s].channels * 2;
            spu_core_input(s, l, r);
        }
        spu_mix(&left, &right);
        output[i * 2] = (int16_t)(left < -32768 ? -32768 : left > 32767 ? 32767 : left);
        output[i * 2 + 1] = (int16_t)(right < -32768 ? -32768 : right > 32767 ? 32767 : right);
    }
}

int audio_init(int enabled) {
    audio_reset();
#ifdef PORT_GRAPHICS
    if (enabled) {
        SDL_AudioSpec spec = {.format = SDL_AUDIO_S16, .channels = 2, .freq = 48000};
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "sound: %s\n", SDL_GetError()); return 0; }
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!audio || !SDL_ResumeAudioStreamDevice(audio)) { fprintf(stderr, "sound: %s\n", SDL_GetError()); return 0; }
    }
#else
    (void)enabled;
#endif
    return 1;
}

void audio_frame(void) {
    int16_t samples[960 * 2];
    audio_render(samples, 960);
    if (!dump_checked) {
        dump_checked = 1;
        const char *path = getenv("DC2TK_SOUND_DUMP");
        if (path) dump = fopen(path, "wb");
    }
    if (dump) { fwrite(samples, sizeof(samples), 1, dump); fflush(dump); }
#ifdef PORT_GRAPHICS
    if (audio && SDL_GetAudioStreamQueued(audio) < (int)sizeof(samples) * 5 && !SDL_PutAudioStreamData(audio, samples, sizeof(samples))) fprintf(stderr, "sound: %s\n", SDL_GetError());
#endif
}

void audio_shutdown(void) {
    if (dump) { fclose(dump); dump = NULL; }
#ifdef PORT_GRAPHICS
    SDL_DestroyAudioStream(audio);
    audio = NULL;
#endif
    audio_reset();
}

size_t audio_save(unsigned char *out) {
    size_t at = sizeof(streams);
    if (out) memcpy(out, streams, sizeof(streams));
    for (unsigned i = 0; i < 2; ++i) {
        if (!streams[i].data) continue;
        if (out) memcpy(out + at, streams[i].data, streams[i].size);
        at += streams[i].size;
    }
    return at + midi_save(out ? out + at : NULL);
}

size_t audio_load(const unsigned char *in) {
    for (unsigned i = 0; i < 2; ++i) free(streams[i].data);
    memcpy(streams, in, sizeof(streams));
    size_t at = sizeof(streams);
    for (unsigned i = 0; i < 2; ++i) {
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
