#define _POSIX_C_SOURCE 200809L
#include "movie.h"
#include "cdvd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t picture_hash;
static unsigned pictures;
static unsigned changed;
static size_t audio_frames;
static uint64_t audio_hash = 1469598103934665603ull;
static unsigned audible;
static double elapsed;

int __wrap_clock_gettime(clockid_t clock, struct timespec *time) {
    (void)clock;
    elapsed += 0.041;
    time->tv_sec = (time_t)elapsed;
    time->tv_nsec = (long)((elapsed - time->tv_sec) * 1000000000);
    return 0;
}

void port_present_movie(const uint32_t *rgba, unsigned width, unsigned height) {
    assert(rgba && width && height && width <= 2048 && height <= 2048);
    uint64_t hash = 1469598103934665603ull;
    for (unsigned i = 0; i < width * height; i++) {
        assert((rgba[i] >> 24) == 255);
        hash = (hash ^ rgba[i]) * 1099511628211ull;
    }
    changed += pictures && picture_hash != hash;
    picture_hash = hash;
    pictures++;
}

void port_present_movie_clear(void) {}
void port_sound_movie_clear(void) {}
void port_sound_movie_pcm(const int16_t *samples, size_t frames, unsigned rate, unsigned channels) {
    assert(samples && frames && rate == 48000 && channels == 2);
    audio_frames += frames;
    for (size_t i = 0; i < frames * channels; i++) {
        audible += samples[i] != 0;
        audio_hash = (audio_hash ^ (uint16_t)samples[i]) * 1099511628211ull;
    }
}

static void parser(void) {
    struct port_pss_packet packet;
    unsigned char pes[] = {0,0,1,0xe0,0,11,0x80,0x80,5,0x21,0,1,0,3,1,2,3};
    assert(port_pss_packet(NULL, 0, &packet) == 0);
    for (size_t i = 0; i < sizeof(pes); i++) assert(port_pss_packet(pes, i, &packet) == 0);
    assert(port_pss_packet(pes, sizeof(pes), &packet) == sizeof(pes));
    assert(packet.stream == 0xe0 && packet.pts == 1 && packet.size == 3 && !memcmp(packet.data, "\1\2\3", 3));
    pes[8] = 20;
    assert(port_pss_packet(pes, sizeof(pes), &packet) == -1);
    pes[8] = 5;
    pes[11] = 0;
    assert(port_pss_packet(pes, sizeof(pes), &packet) == -1);
    unsigned char pack[21] = {0,0,1,0xba,0x44};
    pack[13] = 7;
    assert(port_pss_packet(pack, 20, &packet) == 0);
    assert(port_pss_packet(pack, 21, &packet) == 21);
    unsigned char end[] = {0,0,1,0xb9};
    assert(port_pss_packet(end, sizeof(end), &packet) == 4);
    end[0] = 1;
    assert(port_pss_packet(end, sizeof(end), &packet) == -1);
}

int main(int argc, char **argv) {
    parser();
    if (argc == 1) return 0;
    assert(argc == 3);
    assert(port_cdvd_init(argv[1]));
    assert(port_movie_open(argv[2], 1, 0));
    assert(!port_movie_end() && !port_movie_started());
    port_movie_start();
    for (unsigned i = 0; !port_movie_end() && i < 20000; i++) port_movie_step();
    assert(port_movie_end() && port_movie_started());
    assert(pictures == port_movie_frames() && pictures > 100 && changed > 20);
    if (!strcmp(argv[2], "TUTO/VTUTO6.PSS")) assert(pictures == 1275 && audio_frames == 2448128 && audible == 2328886 && audio_hash == 0xccb04ff0603af46dull);
    if (!strcmp(argv[2], "L5LOGO.PSS")) assert(pictures == 150 && audio_frames == 288512 && audible == 567916);
    printf("movie: %u frames, %u changes, %zu PCM frames, %u nonzero samples, last RGBA hash %016llx\n",
           pictures, changed, audio_frames, audible, (unsigned long long)picture_hash);
    port_movie_close();
    assert(port_movie_end());
    assert(port_movie_open(argv[2], 0, 1));
    port_movie_start();
    unsigned previous = 0;
    unsigned loops = 0;
    for (unsigned i = 0; !loops && i < 20000; i++) {
        port_movie_step();
        unsigned count = port_movie_frames();
        loops += count < previous;
        previous = count;
    }
    assert(loops && !port_movie_end());
    port_movie_close();
    assert(!port_movie_open("missing.pss", 0, 0));
    assert(port_movie_end() && port_movie_started());
    port_cdvd_close();
    return 0;
}
