#include "audio.h"
#include "spu2.h"
#include "cdvd.h"
#include <assert.h>
#include <string.h>

static unsigned char wave[52];
static unsigned char index_data[64];

static void put32(unsigned char *p, unsigned value) {
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

int port_cdvd_find(const char *path, struct sceCdlFILE *file) {
    memset(file, 0, sizeof(*file));
    if (!strcmp(path, "cdrom0:\\SOUND.HD3;1")) { file->lsn = 1; file->size = sizeof(index_data); return 1; }
    if (!strcmp(path, "cdrom0:\\SOUND.DAT;1")) { file->lsn = 2; file->size = sizeof(wave); return 1; }
    return 0;
}

int port_cdvd_read_file(const struct sceCdlFILE *file, uint64_t offset, void *buffer, size_t size) {
    if (offset > file->size || size > file->size - offset) return 0;
    memcpy(buffer, (file->lsn == 1 ? index_data : wave) + offset, size);
    return 1;
}

static void streams_test(void) {
    memcpy(wave, "RIFF", 4);
    put32(wave + 4, sizeof(wave) - 8);
    memcpy(wave + 8, "WAVEfmt ", 8);
    put32(wave + 16, 16);
    wave[20] = 1;
    wave[22] = 1;
    put32(wave + 24, 48000);
    put32(wave + 28, 96000);
    wave[32] = 2;
    wave[34] = 16;
    memcpy(wave + 36, "data", 4);
    put32(wave + 40, 8);
    wave[44] = 0xe8; wave[45] = 3;
    wave[46] = 0x18; wave[47] = 0xfc;
    wave[48] = 0xd0; wave[49] = 7;
    wave[50] = 0x30; wave[51] = 0xf8;
    put32(index_data, 32);
    put32(index_data + 4, sizeof(wave));
    put32(index_data + 12, 1);
    put32(index_data + 16, 48);
    memcpy(index_data + 32, "0010010.wav", 12);
    assert(audio_stream_open(0, "0010010.wav") == 0);
    audio_stream_volume(0, 0x7fff, 0x7fff);
    assert(audio_stream_play(0) == 0);
    assert(audio_stream_state(0) == 0x1000);
    assert(audio_stream_level(0) == 0xfc1803e8u);
    int16_t output[12];
    audio_render(output, 5);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == output[0]);
    assert(output[2] == -1000 && output[3] == -1000);
    assert(output[6] == -2000 && output[7] == -2000);
    assert(output[8] == 0 && output[9] == 0);
    assert(audio_stream_state(0) == 0x8000);
    assert(audio_stream_state(0) == 0x8000);
    int16_t drain[48000 * 2];
    audio_render(drain, 47999);
    assert(audio_stream_state(0) == 0x8000);
    audio_render(drain, 1);
    assert(audio_stream_state(0) == 0);
    audio_render(drain, 960);
    assert(audio_stream_state(0) == 0);
    audio_stream_rewind(0);
    assert(audio_stream_play(0) == 0);
    audio_render(output, 4);
    assert(output[0] >= 990 && output[0] <= 1000 && output[2] == -1000);
    audio_stream_stop(0);
    assert(audio_stream_state(0) == 0);
    audio_stream_close(0);
    assert(audio_stream_play(0) == -1);
    assert(audio_stream_open(2, "0010010.wav") == -1);
    assert(audio_stream_open_pack(0, "0010010.wav", "pack") == -1);
    wave[22] = 2;
    assert(audio_stream_open(1, "0010010.wav") == 1);
    assert(audio_stream_stereo(1));
    audio_stream_volume(1, 0x7fff, 0x7fff);
    assert(audio_stream_play(1) == 0);
    audio_render(output, 2);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == -1000);
    assert(output[2] >= 1990 && output[2] <= 2000 && output[3] == -2000);
    audio_reset();
}

static void spu_test(void) {
    unsigned char adpcm[32] = {0};
    memset(adpcm + 2, 0x11, 14);
    adpcm[17] = 1;
    assert(spu_ram_write(0, adpcm, sizeof(adpcm)));
    assert(!spu_ram_write(0x1ffff0, adpcm, sizeof(adpcm)));
    spu_voice_start(0, 0, 4096, 16384, 8192, 15, 0);
    int32_t left = 0, right = 0;
    for (unsigned i = 0; i < 4; ++i) { left = right = 0; spu_mix(&left, &right); }
    assert(left > 0 && left > right);
    for (unsigned i = 0; i < 70; ++i) { left = right = 0; spu_mix(&left, &right); }
    assert(!spu_voice_active(0));
    adpcm[1] = 7;
    assert(spu_ram_write(0, adpcm, 16));
    for (unsigned v = 0; v < 48; ++v) spu_voice_start(v, 0, 4096, 16384, 16384, 15, 0);
    for (unsigned i = 0; i < 100; ++i) { left = right = 0; spu_mix(&left, &right); }
    for (unsigned v = 0; v < 48; ++v) { assert(spu_voice_active(v)); spu_voice_stop(v); }
    for (unsigned i = 0; i < 4; ++i) { left = right = 0; spu_mix(&left, &right); }
    for (unsigned v = 0; v < 48; ++v) assert(!spu_voice_active(v));
    spu_voice_start(0, 0, 4096, 16383, 16383, 0x7f0f, 0);
    for (unsigned i = 0; i < 1000; ++i) { left = right = 0; spu_mix(&left, &right); assert(!left && !right); }
    assert(spu_voice_active(0));
}

static void reverb_test(void) {
    unsigned char impulse[16] = {0};
    impulse[1] = 1;
    memset(impulse + 2, 0x77, 14);
    for (unsigned mode = 1; mode <= 9; ++mode) {
        spu_reset();
        assert(spu_core_effect(0, mode | 0x100, 32767, 32767, 64, 64) == 0);
        assert(spu_core_effect(2, mode, 0, 0, 0, 0) == -1);
        spu_core_effect_enable(0, 1);
        assert(spu_ram_write(0, impulse, sizeof(impulse)));
        spu_voice_start(0, 0, 4096, 16383, 16383, 15, 0);
        spu_voice_route(0, 12);
        unsigned tail = 0;
        for (unsigned i = 0; i < 48000; ++i) {
            int32_t left = 0, right = 0;
            spu_mix(&left, &right);
            if (i < 10) assert(left == 0 && right == 0);
            if (i > 100 && (left || right)) ++tail;
        }
        assert(tail > 0);
        assert(spu_core_effect(0, mode | 0x100, 32767, 32767, 64, 64) == 0);
        for (unsigned i = 0; i < 100; ++i) {
            int32_t left = 0, right = 0;
            spu_mix(&left, &right);
            assert(left == 0 && right == 0);
        }
    }
    spu_reset();
    spu_ram_write(0, impulse, sizeof(impulse));
    spu_voice_start(0, 0, 4096, 16383, 16383, 15, 0);
    spu_voice_route(0, 1);
    int32_t left = 0, right = 0;
    for (unsigned i = 0; i < 10; ++i) spu_mix(&left, &right);
    assert(left > 0 && right == 0);
    spu_voice_volume(0, -0x2000, 0x2000);
    spu_voice_route(0, 3);
    left = right = 0;
    spu_mix(&left, &right);
    assert(left < 0 && right > 0);
    spu_reset();
    spu_core_input(1, 1000, -1000);
    left = right = 0;
    spu_mix(&left, &right);
    assert(left >= 990 && left <= 1000 && right == -1000);
    spu_core_input_gain(1, 0, 32767);
    spu_core_input(1, 1000, -1000);
    left = right = 0;
    spu_mix(&left, &right);
    assert(!left && right == -1000);
    spu_core_mixer(1, 0);
    spu_core_input(1, 1000, -1000);
    left = right = 0;
    spu_mix(&left, &right);
    assert(!left && !right);
    spu_core_mixer(1, 0xffc);
    assert(spu_core_master_volume(1, 0x8000) == -1);
    assert(spu_core_master_volume(1, 0) == 0);
    spu_core_input(0, 1000, -1000);
    left = right = 0;
    spu_mix(&left, &right);
    assert(!left && !right);
}

int main(void) {
    assert(audio_init(0));
    streams_test();
    spu_test();
    reverb_test();
    audio_reset();
    int16_t movie[4] = {1000, -1000, 2000, -2000};
    audio_movie_pcm(movie, 2, 24000, 2);
    assert(audio_movie_queued() == 4);
    int16_t output[8];
    audio_render(output, 4);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == -1000);
    assert(output[4] >= 1990 && output[4] <= 2000 && output[5] == -2000);
    assert(audio_movie_queued() == 0);
    audio_shutdown();
    return 0;
}
