#include "audio.h"
#include "spu2.h"
#include "cdvd.h"
#include "sce/libsdr.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

struct test_wave {
    struct riff_header header;
    struct riff_chunk format_chunk;
    struct wav_fmt format;
    struct riff_chunk data_chunk;
    int16_t samples[4];
};
_Static_assert(sizeof(struct test_wave) == 52, "test wave");

struct test_index {
    struct sound_index_entry entries[2];
    char names[2][16];
};
_Static_assert(sizeof(struct test_index) == 64, "test sound index");

enum { INDEX_SECTOR = 1, WAVE_SECTOR = 2 };

static struct test_wave wave;
static struct test_index index_data;

int port_cdvd_find(const char *path, struct sceCdlFILE *file) {
    memset(file, 0, sizeof(*file));
    if (!strcmp(path, "cdrom0:\\SOUND.HD3;1")) { file->lsn = INDEX_SECTOR; file->size = sizeof(index_data); return 1; }
    if (!strcmp(path, "cdrom0:\\SOUND.DAT;1")) { file->lsn = WAVE_SECTOR; file->size = sizeof(wave); return 1; }
    return 0;
}

int port_cdvd_read_file(const struct sceCdlFILE *file, uint64_t offset, void *buffer, size_t size) {
    if (offset > file->size || size > file->size - offset) return 0;
    const unsigned char *source = file->lsn == INDEX_SECTOR ? (const unsigned char *)&index_data : (const unsigned char *)&wave;
    memcpy(buffer, source + offset, size);
    return 1;
}

static void build_wave(unsigned channels) {
    memset(&wave, 0, sizeof wave);
    memcpy(wave.header.chunk.id, "RIFF", sizeof wave.header.chunk.id);
    wave.header.chunk.size = sizeof(wave) - sizeof(struct riff_chunk);
    memcpy(wave.header.form, "WAVE", sizeof wave.header.form);
    memcpy(wave.format_chunk.id, "fmt ", sizeof wave.format_chunk.id);
    wave.format_chunk.size = sizeof wave.format;
    wave.format = (struct wav_fmt){
        .format = WAVE_FORMAT_PCM, .channels = (uint16_t)channels, .sample_rate = AUDIO_RATE,
        .byte_rate = AUDIO_RATE * sizeof(int16_t), .block_align = sizeof(int16_t), .bits_per_sample = 16,
    };
    memcpy(wave.data_chunk.id, "data", sizeof wave.data_chunk.id);
    wave.data_chunk.size = sizeof wave.samples;
    const int16_t samples[] = {1000, -1000, 2000, -2000};
    memcpy(wave.samples, samples, sizeof samples);
}

static void streams_test(void) {
    build_wave(1);
    index_data.entries[0] = (struct sound_index_entry){
        .name_offset = offsetof(struct test_index, names[0]), .byte_size = sizeof(wave), .start_sector = 0, .sector_count = 1,
    };
    index_data.entries[1] = (struct sound_index_entry){.name_offset = offsetof(struct test_index, names[1])};
    strcpy(index_data.names[0], "0010010.wav");
    assert(audio_stream_open(0, "0010010.wav") == 0);
    audio_stream_volume(0, INT16_MAX, INT16_MAX);
    assert(audio_stream_play(0) == 0);
    assert(audio_stream_state(0) == AUDIO_STREAM_PLAYING);
    assert(audio_stream_level(0) == ((uint32_t)(uint16_t)wave.samples[1] << 16 | (uint16_t)wave.samples[0]));
    int16_t output[12];
    audio_render(output, 5);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == output[0]);
    assert(output[2] == -1000 && output[3] == -1000);
    assert(output[6] == -2000 && output[7] == -2000);
    assert(output[8] == 0 && output[9] == 0);
    assert(audio_stream_state(0) == AUDIO_STREAM_ENDED);
    assert(audio_stream_state(0) == AUDIO_STREAM_ENDED);
    int16_t drain[AUDIO_RATE * 2];
    audio_render(drain, AUDIO_RATE - 1);
    assert(audio_stream_state(0) == AUDIO_STREAM_ENDED);
    audio_render(drain, 1);
    assert(audio_stream_state(0) == AUDIO_STREAM_STOPPED);
    audio_render(drain, AUDIO_FRAMES_PER_FIELD);
    assert(audio_stream_state(0) == AUDIO_STREAM_STOPPED);
    audio_stream_rewind(0);
    assert(audio_stream_play(0) == 0);
    audio_render(output, 4);
    assert(output[0] >= 990 && output[0] <= 1000 && output[2] == -1000);
    audio_stream_stop(0);
    assert(audio_stream_state(0) == AUDIO_STREAM_STOPPED);
    audio_stream_close(0);
    assert(audio_stream_play(0) == -1);
    assert(audio_stream_open(AUDIO_STREAMS, "0010010.wav") == -1);
    assert(audio_stream_open_pack(0, "0010010.wav", "pack") == -1);
    wave.format.channels = 2;
    assert(audio_stream_open(1, "0010010.wav") == 1);
    assert(audio_stream_stereo(1));
    audio_stream_volume(1, INT16_MAX, INT16_MAX);
    assert(audio_stream_play(1) == 0);
    audio_render(output, 2);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == -1000);
    assert(output[2] >= 1990 && output[2] <= 2000 && output[3] == -2000);
    audio_reset();
}

static const struct spu_adsr1 sustain_full = {.sustain_level = SPU_SUSTAIN_LEVEL_MAX};
static const struct spu_adsr2 no_adsr2 = {0};

static void spu_test(void) {
    struct spu_adpcm_block adpcm[2];
    memset(adpcm, 0, sizeof adpcm);
    memset(adpcm[0].data, 0x11, sizeof adpcm[0].data);
    adpcm[1].flags = SPU_ADPCM_LOOP_END;
    assert(spu_ram_write(0, adpcm, sizeof(adpcm)));
    assert(!spu_ram_write(SPU_RAM_SIZE - sizeof adpcm[0], adpcm, sizeof(adpcm)));
    spu_voice_start(0, 0, SPU_PITCH_UNITY, 16384, 8192, sustain_full, no_adsr2);
    int32_t left = 0, right = 0;
    for (unsigned i = 0; i < 4; ++i) { left = right = 0; spu_mix(&left, &right); }
    assert(left > 0 && left > right);
    for (unsigned i = 0; i < 70; ++i) { left = right = 0; spu_mix(&left, &right); }
    assert(!spu_voice_active(0));
    adpcm[0].flags = SPU_ADPCM_LOOP_END | SPU_ADPCM_LOOP_REPEAT | SPU_ADPCM_LOOP_START;
    assert(spu_ram_write(0, adpcm, sizeof adpcm[0]));
    for (unsigned v = 0; v < SPU_VOICES; ++v) spu_voice_start(v, 0, SPU_PITCH_UNITY, 16384, 16384, sustain_full, no_adsr2);
    for (unsigned i = 0; i < 100; ++i) { left = right = 0; spu_mix(&left, &right); }
    for (unsigned v = 0; v < SPU_VOICES; ++v) { assert(spu_voice_active(v)); spu_voice_stop(v); }
    for (unsigned i = 0; i < 4; ++i) { left = right = 0; spu_mix(&left, &right); }
    for (unsigned v = 0; v < SPU_VOICES; ++v) assert(!spu_voice_active(v));
    struct spu_adsr1 no_attack = {.sustain_level = SPU_SUSTAIN_LEVEL_MAX, .attack_rate = SPU_ATTACK_RATE_MAX};
    spu_voice_start(0, 0, SPU_PITCH_UNITY, SPU_VOLUME_MAX, SPU_VOLUME_MAX, no_attack, no_adsr2);
    for (unsigned i = 0; i < 1000; ++i) { left = right = 0; spu_mix(&left, &right); assert(!left && !right); }
    assert(spu_voice_active(0));
}

static void reverb_test(void) {
    struct spu_adpcm_block impulse;
    memset(&impulse, 0, sizeof impulse);
    impulse.flags = SPU_ADPCM_LOOP_END;
    memset(impulse.data, 0x77, sizeof impulse.data);
    for (unsigned mode = SD_REV_MODE_ROOM; mode <= SD_REV_MODE_PIPE; ++mode) {
        spu_reset();
        assert(spu_core_effect(0, mode | SD_REV_MODE_CLEAR_WA, INT16_MAX, INT16_MAX, 64, 64) == 0);
        assert(spu_core_effect(SPU_CORES, mode, 0, 0, 0, 0) == -1);
        spu_core_effect_enable(0, 1);
        assert(spu_ram_write(0, &impulse, sizeof(impulse)));
        spu_voice_start(0, 0, SPU_PITCH_UNITY, SPU_VOLUME_MAX, SPU_VOLUME_MAX, sustain_full, no_adsr2);
        spu_voice_route(0, SPU_ROUTE_WET_LEFT | SPU_ROUTE_WET_RIGHT);
        unsigned tail = 0;
        for (unsigned i = 0; i < SPU_SAMPLE_RATE; ++i) {
            int32_t left = 0, right = 0;
            spu_mix(&left, &right);
            if (i < 10) assert(left == 0 && right == 0);
            if (i > 100 && (left || right)) ++tail;
        }
        assert(tail > 0);
        assert(spu_core_effect(0, mode | SD_REV_MODE_CLEAR_WA, INT16_MAX, INT16_MAX, 64, 64) == 0);
        for (unsigned i = 0; i < 100; ++i) {
            int32_t left = 0, right = 0;
            spu_mix(&left, &right);
            assert(left == 0 && right == 0);
        }
    }
    spu_reset();
    spu_ram_write(0, &impulse, sizeof(impulse));
    spu_voice_start(0, 0, SPU_PITCH_UNITY, SPU_VOLUME_MAX, SPU_VOLUME_MAX, sustain_full, no_adsr2);
    spu_voice_route(0, SPU_ROUTE_DRY_LEFT);
    int32_t left = 0, right = 0;
    for (unsigned i = 0; i < 10; ++i) spu_mix(&left, &right);
    assert(left > 0 && right == 0);
    spu_voice_volume(0, -0x2000, 0x2000);
    spu_voice_route(0, SPU_ROUTE_DRY_LEFT | SPU_ROUTE_DRY_RIGHT);
    left = right = 0;
    spu_mix(&left, &right);
    assert(left < 0 && right > 0);
    spu_reset();
    spu_core_input(SPU_CORE_MOVIE, 1000, -1000);
    left = right = 0;
    spu_mix(&left, &right);
    assert(left >= 990 && left <= 1000 && right == -1000);
    spu_core_input_gain(SPU_CORE_MOVIE, 0, INT16_MAX);
    spu_core_input(SPU_CORE_MOVIE, 1000, -1000);
    left = right = 0;
    spu_mix(&left, &right);
    assert(!left && right == -1000);
    spu_core_mixer(SPU_CORE_MOVIE, 0);
    spu_core_input(SPU_CORE_MOVIE, 1000, -1000);
    left = right = 0;
    spu_mix(&left, &right);
    assert(!left && !right);
    spu_core_mixer(SPU_CORE_MOVIE, SPU_MIX_ALL & ~SPU_MIX_EXTERNAL_WET);
    assert(spu_core_master_volume(SPU_CORE_MOVIE, SPU_VOLUME_SWEEP) == -1);
    assert(spu_core_master_volume(SPU_CORE_MOVIE, 0) == 0);
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
    audio_movie_pcm(movie, 2, AUDIO_RATE / 2, 2);
    assert(audio_movie_queued() == 4);
    int16_t output[8];
    audio_render(output, 4);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == -1000);
    assert(output[4] >= 1990 && output[4] <= 2000 && output[5] == -2000);
    assert(audio_movie_queued() == 0);
    audio_shutdown();
    return 0;
}
