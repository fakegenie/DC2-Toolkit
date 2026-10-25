#include "movie.h"
#include "cdvd.h"
#include "port_graphics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t picture_hash;
static unsigned pictures;
static unsigned changed;
static size_t audio_frames;
static uint64_t audio_hash = 1469598103934665603ull;
static unsigned audible;
uint64_t port_fields;

static void step(void) {
    port_fields += 2;
    port_movie_step();
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
void audio_movie_clear(void) {}
void audio_movie_pcm(const int16_t *samples, size_t frames, unsigned rate, unsigned channels) {
    assert(samples && frames && rate == 48000 && channels == 2);
    audio_frames += frames;
    for (size_t i = 0; i < frames * channels; i++) {
        audible += samples[i] != 0;
        audio_hash = (audio_hash ^ (uint16_t)samples[i]) * 1099511628211ull;
    }
}

static void parser(void) {
    struct port_pss_packet packet;
    struct {
        struct mpeg_pes_packet header;
        unsigned char payload[3];
    } pes = {
        .header = {
            .header = {.start = {.prefix = {0, 0, 1}, .stream = MPEG_STREAM_VIDEO}, .length = {0, 11}},
            .extension = {.marker = MPEG_PES_MARKER, .pts_dts = MPEG_PES_HAS_PTS, .header_length = sizeof(struct mpeg_pts)},
            .pts = {.marker_high = 1, .prefix = 2, .marker_middle = 1, .marker_low = 1, .low_low = 1},
        },
        .payload = {1, 2, 3},
    };
    unsigned char *pes_bytes = (unsigned char *)&pes;
    assert(port_pss_packet(NULL, 0, &packet) == 0);
    for (size_t i = 0; i < sizeof(pes); i++) assert(port_pss_packet(pes_bytes, i, &packet) == 0);
    assert(port_pss_packet(pes_bytes, sizeof(pes), &packet) == sizeof(pes));
    assert(packet.stream == MPEG_STREAM_VIDEO && packet.pts == 1 && packet.size == sizeof pes.payload && !memcmp(packet.data, pes.payload, sizeof pes.payload));
    pes.header.extension.header_length = 20;
    assert(port_pss_packet(pes_bytes, sizeof(pes), &packet) == -1);
    pes.header.extension.header_length = sizeof(struct mpeg_pts);
    pes.header.pts.marker_middle = 0;
    assert(port_pss_packet(pes_bytes, sizeof(pes), &packet) == -1);
    struct {
        struct mpeg_pack_header header;
        unsigned char stuffing[7];
    } pack = {.header = {.start = {.prefix = {0, 0, 1}, .stream = MPEG_STREAM_PACK}, .scr_high = 4, .version = MPEG_PACK_VERSION_2, .stuffing_length = 7}};
    assert(port_pss_packet((unsigned char *)&pack, sizeof pack - 1, &packet) == 0);
    assert(port_pss_packet((unsigned char *)&pack, sizeof pack, &packet) == sizeof pack);
    struct mpeg_start_code end = {.prefix = {0, 0, 1}, .stream = MPEG_STREAM_END};
    assert(port_pss_packet((unsigned char *)&end, sizeof(end), &packet) == sizeof end);
    end.prefix[0] = 1;
    assert(port_pss_packet((unsigned char *)&end, sizeof(end), &packet) == -1);
}

int main(int argc, char **argv) {
    parser();
    if (argc == 1) return 0;
    assert(argc == 3);
    assert(port_cdvd_init(argv[1]));
    assert(port_movie_open(argv[2], 1, 0));
    assert(!port_movie_end() && !port_movie_started());
    port_movie_start();
    for (unsigned i = 0; !port_movie_end() && i < 20000; i++) step();
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
        step();
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
