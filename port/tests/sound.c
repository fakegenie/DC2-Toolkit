#include "iop_sound.h"
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

static int rpc(unsigned command, unsigned value) {
    int result = -99;
    assert(port_sound_rpc(0x12345, command, &value, 4, &result, 4) == 1);
    return result;
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
    int result = -99;
    char name[64] = "0010010.wav";
    assert(port_sound_rpc(0x12345, 0x8020, name, 64, &result, 4) == 1 && result == 0);
    assert(rpc(0x80, 0x7fff7fff) == 0);
    assert(rpc(0x50, 0) == 0);
    assert(rpc(0x80b0, 0) == 0x1000);
    assert((unsigned)rpc(0x80e0, 0) == 0xfc1803e8u);
    int16_t output[12];
    port_sound_render(output, 5);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == output[0]);
    assert(output[2] == -1000 && output[3] == -1000);
    assert(output[6] == -2000 && output[7] == -2000);
    assert(output[8] == 0 && output[9] == 0);
    assert(rpc(0x80b0, 0) == 0x8000);
    assert(rpc(0x80b0, 0) == 0x8000);
    int16_t drain[48000 * 2];
    port_sound_render(drain, 47999);
    assert(rpc(0x80b0, 0) == 0x8000);
    port_sound_render(drain, 1);
    assert(rpc(0x80b0, 0) == 0);
    port_sound_render(drain, 960);
    assert(rpc(0x80b0, 0) == 0);
    assert(rpc(0x30, 0) == 0);
    assert(rpc(0x8000, 1) == 0);
    assert(rpc(0x50, 0) == 0);
    port_sound_render(output, 6);
    assert(output[8] >= 990 && output[8] <= 1000 && output[10] == -1000);
    assert(rpc(0x60, 0) == 0);
    assert(rpc(0x80b0, 0) == 0);
    assert(rpc(0x10, 0) == 0);
    assert(rpc(0x50, 0) == -1);
    assert(rpc(0x8a00, 0) == -1);
    wave[22] = 2;
    assert(port_sound_rpc(0x12345, 0x8021, name, 64, &result, 4) == 1 && result == 1);
    assert(rpc(0x81, 0x7fff7fff) == 0);
    assert(rpc(0x51, 0) == 0);
    port_sound_render(output, 2);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == -1000);
    assert(output[2] >= 1990 && output[2] <= 2000 && output[3] == -2000);
    port_sound_reset();
}

static void spu_test(void) {
    unsigned char adpcm[32] = {0};
    memset(adpcm + 2, 0x11, 14);
    adpcm[17] = 1;
    assert(port_spu_transfer(0, adpcm, sizeof(adpcm), 0) == sizeof(adpcm));
    assert(port_spu_transfer(0x1ffff0, adpcm, sizeof(adpcm), 0) == -1);
    port_spu_start(0, 0, 4096, 16384, 8192, 15, 0);
    int32_t left = 0, right = 0;
    for (unsigned i = 0; i < 4; ++i) { left = right = 0; port_spu_sample(&left, &right); }
    assert(left > 0 && left > right);
    for (unsigned i = 0; i < 70; ++i) { left = right = 0; port_spu_sample(&left, &right); }
    assert(!port_spu_active(0));
    adpcm[1] = 7;
    assert(port_spu_transfer(0, adpcm, 16, 0) == 16);
    for (unsigned v = 0; v < 48; ++v) port_spu_start(v, 0, 4096, 16384, 16384, 15, 0);
    for (unsigned i = 0; i < 100; ++i) { left = right = 0; port_spu_sample(&left, &right); }
    for (unsigned v = 0; v < 48; ++v) { assert(port_spu_active(v)); port_spu_stop(v); }
    for (unsigned i = 0; i < 4; ++i) { left = right = 0; port_spu_sample(&left, &right); }
    for (unsigned v = 0; v < 48; ++v) assert(!port_spu_active(v));
    port_spu_start(0, 0, 4096, 16383, 16383, 0x7f0f, 0);
    for (unsigned i = 0; i < 1000; ++i) { left = right = 0; port_spu_sample(&left, &right); assert(!left && !right); }
    assert(port_spu_active(0));
}

static void reverb_test(void) {
    unsigned char impulse[16] = {0};
    impulse[1] = 1;
    memset(impulse + 2, 0x77, 14);
    for (unsigned mode = 1; mode <= 9; ++mode) {
        port_spu_reset();
        assert(port_spu_effect(0, mode | 0x100, 32767, 32767, 64, 64) == 0);
        assert(port_spu_effect(2, mode, 0, 0, 0, 0) == -1);
        port_spu_effect_enable(0, 1);
        assert(port_spu_transfer(0, impulse, sizeof(impulse), 0) == 16);
        port_spu_start(0, 0, 4096, 16383, 16383, 15, 0);
        port_spu_route(0, 12);
        unsigned tail = 0;
        for (unsigned i = 0; i < 48000; ++i) {
            int32_t left = 0, right = 0;
            port_spu_sample(&left, &right);
            if (i < 10) assert(left == 0 && right == 0);
            if (i > 100 && (left || right)) ++tail;
        }
        assert(tail > 0);
        assert(port_spu_effect(0, mode | 0x100, 32767, 32767, 64, 64) == 0);
        for (unsigned i = 0; i < 100; ++i) {
            int32_t left = 0, right = 0;
            port_spu_sample(&left, &right);
            assert(left == 0 && right == 0);
        }
    }
    port_spu_reset();
    port_spu_transfer(0, impulse, sizeof(impulse), 0);
    port_spu_start(0, 0, 4096, 16383, 16383, 15, 0);
    port_spu_route(0, 1);
    int32_t left = 0, right = 0;
    for (unsigned i = 0; i < 10; ++i) port_spu_sample(&left, &right);
    assert(left > 0 && right == 0);
    port_spu_param(0, 0x6000);
    port_spu_param(0x100, 0x2000);
    port_spu_route(0, 3);
    left = right = 0;
    port_spu_sample(&left, &right);
    assert(left < 0 && right > 0);
    port_spu_reset();
    port_spu_input(1, 1000, -1000);
    left = right = 0;
    port_spu_sample(&left, &right);
    assert(left >= 990 && left <= 1000 && right == -1000);
    port_spu_param(0xf81, 0);
    port_spu_input(1, 1000, -1000);
    left = right = 0;
    port_spu_sample(&left, &right);
    assert(!left && right == -1000);
    port_spu_param(0x801, 0);
    port_spu_input(1, 1000, -1000);
    left = right = 0;
    port_spu_sample(&left, &right);
    assert(!left && !right);
    port_spu_param(0x801, 0xffc);
    port_spu_param(0x981, 0);
    port_spu_input(0, 1000, -1000);
    left = right = 0;
    port_spu_sample(&left, &right);
    assert(!left && right == -1000);
}

int main(void) {
    assert(port_sound_init(0));
    uint32_t address = iop_memory_alloc(64);
    assert(address && iop_memory_size(address + 3) == 61);
    assert(iop_memory_resolve(address, 64));
    assert(!iop_memory_resolve(address + 63, 2));
    iop_memory_free((void *)(uintptr_t)address);
    assert(!iop_memory_resolve(address, 1));
    streams_test();
    spu_test();
    reverb_test();
    port_sound_reset();
    int16_t movie[4] = {1000, -1000, 2000, -2000};
    port_sound_movie_pcm(movie, 2, 24000, 2);
    assert(port_sound_movie_queued() == 4);
    int16_t output[8];
    port_sound_render(output, 4);
    assert(output[0] >= 990 && output[0] <= 1000 && output[1] == -1000);
    assert(output[4] >= 1990 && output[4] <= 2000 && output[5] == -2000);
    assert(port_sound_movie_queued() == 0);
    port_sound_shutdown();
    return 0;
}
