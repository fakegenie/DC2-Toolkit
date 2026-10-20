#define _GNU_SOURCE
#include "iop_sound.h"
#include "spu2.h"
#include "iop_midi.h"
#include "cdvd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef PORT_GRAPHICS
#include <SDL3/SDL.h>
static SDL_AudioStream *audio;
#endif

static struct { void *pointer; size_t size; } allocations[256];
static struct {
    unsigned char *data;
    size_t size, position;
    unsigned channels, mode, state, end_frames;
} streams[2];
static unsigned warnings;
static int16_t movie_pcm[192000 * 2];
static size_t movie_head, movie_count;

static uint32_t u32(const void *address) {
    const unsigned char *p = address;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }

static int unsupported(const char *protocol, unsigned command) {
    if (warnings++ < 24) fprintf(stderr, "sound: unsupported %s command 0x%x\n", protocol, command);
    return -1;
}

uint32_t iop_memory_alloc(size_t size) {
    if (!size || size > 32 * 1024 * 1024) return 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (allocations[i].pointer) continue;
        void *pointer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (pointer == MAP_FAILED) return 0;
        allocations[i].pointer = pointer;
        allocations[i].size = size;
        return (uint32_t)(uintptr_t)pointer;
    }
    return 0;
}

void *iop_memory_resolve(uint32_t address, size_t size) {
    for (unsigned i = 0; i < 256; ++i) {
        uintptr_t start = (uintptr_t)allocations[i].pointer;
        if (!start || address < start) continue;
        size_t offset = address - start;
        if (offset <= allocations[i].size && size <= allocations[i].size - offset) return (unsigned char *)allocations[i].pointer + offset;
    }
    return NULL;
}

size_t iop_memory_size(uint32_t address) {
    for (unsigned i = 0; i < 256; ++i) {
        uintptr_t start = (uintptr_t)allocations[i].pointer;
        if (start && address >= start && address - start < allocations[i].size) return allocations[i].size - (address - start);
    }
    return 0;
}

void iop_memory_free(void *address) {
    for (unsigned i = 0; i < 256; ++i) {
        if (address != allocations[i].pointer) continue;
        if (address) munmap(address, allocations[i].size);
        allocations[i].pointer = NULL;
        allocations[i].size = 0;
        return;
    }
}

void port_sound_movie_clear(void) { movie_head = movie_count = 0; }

size_t port_sound_movie_queued(void) { return movie_count; }

void port_sound_movie_pcm(const int16_t *samples, size_t frames, unsigned rate, unsigned channels) {
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

void port_sound_reset(void) {
    port_sound_movie_clear();
    for (unsigned i = 0; i < 2; ++i) {
        free(streams[i].data);
        memset(&streams[i], 0, sizeof(streams[i]));
    }
    iop_midi_reset();
    for (unsigned i = 0; i < 256; ++i) iop_memory_free(allocations[i].pointer);
    port_spu_reset();
    warnings = 0;
}

static int stream_open(unsigned channel, const char *name) {
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

static int stream_rpc(unsigned command, const void *send, int send_size) {
    unsigned channel = command & 15;
    unsigned op = command & 0xfff0;
    if (channel >= 2) return -1;
    unsigned value = send && send_size >= 4 ? u32(send) : 0;
    if (op == 0x8020) {
        if (!send || send_size < 1 || !memchr(send, 0, (size_t)send_size)) return -1;
        return stream_open(channel, send);
    }
    if (op == 0x8000) { streams[channel].mode = value; return 0; }
    if (op == 0x80) {
        port_spu_param(0xf80 | channel, value >> 16);
        port_spu_param(0x1080 | channel, value & 0xffff);
        return 0;
    }
    if (op == 0x50) { if (!streams[channel].data) return -1; streams[channel].state = 0x1000; return 0; }
    if (op == 0x60) { streams[channel].state &= ~0x1000u; return 0; }
    if (op == 0x10 || op == 0x70) { free(streams[channel].data); streams[channel].data = NULL; streams[channel].size = 0; streams[channel].state = 0; return 0; }
    if (op == 0x30) { streams[channel].position = 0; return 0; }
    if (op == 0x40 || op == 0x80c0) return streams[channel].data ? 0 : -1;
    if (op == 0x80b0) return (int)streams[channel].state;
    if (op == 0x80d0) return streams[channel].channels == 2;
    if (op == 0x80e0) {
        if (!(streams[channel].state & 0x1000) || !streams[channel].data || streams[channel].position >= streams[channel].size) return 0;
        size_t available = streams[channel].size - streams[channel].position;
        const unsigned char *sample = streams[channel].data + streams[channel].position;
        return available >= 4 ? (int)u32(sample) : available >= 2 ? (int)u16(sample) : 0;
    }
    return unsupported("EZBGM", command);
}

int port_sound_rpc(unsigned server, unsigned command, const void *send, int send_size, void *receive, int receive_size) {
    if (server != 0x12345 && server != 0x12346) return 0;
    unsigned char argument[64] = {0};
    if (send && send_size > 0) memcpy(argument, send, (size_t)send_size < sizeof(argument) ? (size_t)send_size : sizeof(argument));
    if (server == 0x12346 && (command & 0xfff0) == 0x9050 && send && send_size >= 32) {
        unsigned char bank[20];
        memcpy(bank, argument, 4);
        memcpy(bank + 4, argument + 8, 4);
        memcpy(bank + 8, argument + 16, 4);
        memcpy(bank + 12, argument + 24, 8);
        memcpy(argument, bank, sizeof(bank));
    }
    if (receive && receive_size > 0) memset(receive, 0, (size_t)receive_size);
    if (server == 0x12346) iop_midi_rpc(command, argument, send_size, receive, receive_size);
    else {
        int result = stream_rpc(command, argument, send_size < 64 ? send_size : 64);
        if (receive && receive_size >= 4) memcpy(receive, &result, 4);
    }
    return 1;
}

int port_sound_remote(int command, va_list arguments) {
    if (command == 0x8000) { port_spu_reset(); return 0; }
    if (command == 0x80f0) return 1;
    if (command == 0x80d0) {
        (void)va_arg(arguments, int);
        int mode = va_arg(arguments, int);
        void *source = va_arg(arguments, void *);
        unsigned address = va_arg(arguments, unsigned);
        int size = va_arg(arguments, int);
        if (size < 0 || (mode & ~1)) return -1;
        return port_spu_transfer(address, source, (size_t)size, mode & 1);
    }
    if (command != 0x8010 && command != 0x8030 && command != 0x8050 && command != 0x8070 && command != 0x8130) return unsupported("SDRDRV", (unsigned)command);
    unsigned entry = va_arg(arguments, unsigned);
    if (command == 0x8130) {
        const unsigned char *effect = va_arg(arguments, void *);
        if (!effect) return -1;
        unsigned mode = u32(effect + 4);
        int delay = 0, feedback = 0;
        if ((mode & ~0x100u) == 7 || (mode & ~0x100u) == 8) { delay = (int)u32(effect + 12); feedback = (int)u32(effect + 16); }
        return port_spu_effect(entry, mode, (int16_t)u16(effect + 8), (int16_t)u16(effect + 10), delay, feedback);
    }
    unsigned value = (unsigned)va_arg(arguments, int);
    unsigned reg = entry >> 8;
    if (command == 0x8010) {
        if (reg > 16 || (reg > 4 && reg < 8)) return unsupported("SDRDRV parameter", entry);
        if ((reg <= 1 || reg == 9 || reg == 10) && (value & 0x8000)) return unsupported("SDRDRV volume sweep", entry);
        port_spu_param(entry, value);
        return 0;
    }
    if (command == 0x8030) {
        if (reg != 0x15 && reg != 0x16 && (reg < 0x18 || reg > 0x1b)) return unsupported("SDRDRV voice switch", entry);
        port_spu_switch(entry, value);
        return 0;
    }
    if (command == 0x8050) {
        if (reg != 0x1d && reg != 0x20 && reg != 0x21) return unsupported("SDRDRV address", entry);
        port_spu_addr(entry, value);
        return 0;
    }
    if (command == 0x8070) {
        if ((entry & ~1u) == 2) port_spu_effect_enable(entry & 1, (int)value);
        else if ((entry & ~1u) != 0xa) return unsupported("SDRDRV core attribute", entry);
        return 0;
    }
    return unsupported("SDRDRV", command);
}

void port_sound_render(int16_t *output, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        int32_t left = 0, right = 0;
        if (movie_count) {
            port_spu_input(1, movie_pcm[movie_head * 2], movie_pcm[movie_head * 2 + 1]);
            movie_head = (movie_head + 1) % 192000;
            --movie_count;
        }
        iop_midi_tick();
        for (unsigned s = 0; s < 2; ++s) {
            if (streams[s].state == 0x8000 && streams[s].end_frames && !--streams[s].end_frames) streams[s].state = 0;
            if (!(streams[s].state & 0x1000) || !streams[s].data) continue;
            if (streams[s].position >= streams[s].size) {
                if ((streams[s].mode & 0xffff000f) && streams[s].size) streams[s].position = 0;
                else { streams[s].state = 0x8000; streams[s].end_frames = 48000; continue; }
            }
            const unsigned char *p = streams[s].data + streams[s].position;
            int l = (int16_t)u16(p);
            int r = streams[s].channels == 2 ? (int16_t)u16(p + 2) : l;
            streams[s].position += streams[s].channels * 2;
            port_spu_input(s, l, r);
        }
        port_spu_sample(&left, &right);
        output[i * 2] = (int16_t)(left < -32768 ? -32768 : left > 32767 ? 32767 : left);
        output[i * 2 + 1] = (int16_t)(right < -32768 ? -32768 : right > 32767 ? 32767 : right);
    }
}

int port_sound_init(int enabled) {
    port_sound_reset();
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

void port_sound_frame(void) {
    int16_t samples[960 * 2];
    port_sound_render(samples, 960);
#ifdef PORT_GRAPHICS
    if (audio && SDL_GetAudioStreamQueued(audio) < (int)sizeof(samples) * 5 && !SDL_PutAudioStreamData(audio, samples, sizeof(samples))) fprintf(stderr, "sound: %s\n", SDL_GetError());
#endif
}

void port_sound_shutdown(void) {
#ifdef PORT_GRAPHICS
    SDL_DestroyAudioStream(audio);
    audio = NULL;
#endif
    port_sound_reset();
}
