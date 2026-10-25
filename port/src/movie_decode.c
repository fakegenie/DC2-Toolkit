#define _POSIX_C_SOURCE 200809L
#include "movie.h"
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(struct mpeg_start_code) == 4, "MPEG start code");
_Static_assert(sizeof(struct mpeg_pack_header) == 14, "MPEG-2 pack header");
_Static_assert(sizeof(struct mpeg_pes_header) == 6, "MPEG PES header");
_Static_assert(sizeof(struct mpeg_pes_extension) == 3, "MPEG PES optional header");
_Static_assert(sizeof(struct mpeg_pts) == 5, "MPEG presentation time stamp");
_Static_assert(sizeof(struct mpeg_pes_packet) == 14, "MPEG PES header with PTS");
_Static_assert(sizeof(struct pss_audio_header) == 4, "PSS audio subheader");
_Static_assert(sizeof(struct pss_sshd) == 40, "PSS SShd header");

static const uint8_t mpeg_start_prefix[3] = {0, 0, 1};

static unsigned mpeg_be16(const uint8_t bytes[2]) { return (unsigned)bytes[0] << 8 | bytes[1]; }

static size_t pss_packet_size(const unsigned char *data, size_t size) {
    const struct mpeg_start_code *start = (const struct mpeg_start_code *)data;
    const struct mpeg_pack_header *pack = (const struct mpeg_pack_header *)data;
    const struct mpeg_pes_header *pes = (const struct mpeg_pes_header *)data;
    if (size < sizeof *start) return 0;
    if (start->stream == MPEG_STREAM_END) return sizeof *start;
    if (start->stream == MPEG_STREAM_PACK) return size < sizeof *pack ? 0 : sizeof *pack + pack->stuffing_length;
    return size < sizeof *pes ? 0 : sizeof *pes + mpeg_be16(pes->length);
}

static int pss_has_header(unsigned stream) {
    return (stream & MPEG_STREAM_AUDIO_MASK) == MPEG_STREAM_AUDIO || (stream & MPEG_STREAM_VIDEO_MASK) == MPEG_STREAM_VIDEO ||
           stream == MPEG_STREAM_PRIVATE_1;
}

static uint64_t mpeg_pts_value(const struct mpeg_pts *pts) {
    return (uint64_t)pts->high << 30 | (uint64_t)pts->middle_high << 22 | (uint64_t)pts->middle_low << 15 | (uint64_t)pts->low_high << 7 | pts->low_low;
}

int port_pss_packet(const unsigned char *data, size_t size, struct port_pss_packet *packet) {
    if (!data || !packet || size < sizeof(struct mpeg_start_code)) return 0;
    const struct mpeg_pes_packet *pes = (const struct mpeg_pes_packet *)data;
    const struct mpeg_pack_header *pack = (const struct mpeg_pack_header *)data;
    if (memcmp(pes->header.start.prefix, mpeg_start_prefix, sizeof mpeg_start_prefix)) return -1;
    memset(packet, 0, sizeof(*packet));
    packet->stream = pes->header.start.stream;
    packet->pts = UINT64_MAX;
    if (packet->stream == MPEG_STREAM_END) return (int)pss_packet_size(data, size);
    if (packet->stream == MPEG_STREAM_PACK) {
        if (size < sizeof *pack) return 0;
        if (pack->version != MPEG_PACK_VERSION_2) return -1;
        size_t count = pss_packet_size(data, size);
        return size < count ? 0 : (int)count;
    }
    if (size < sizeof pes->header) return 0;
    size_t count = pss_packet_size(data, size);
    if (count == sizeof pes->header) return -1;
    if (size < count) return 0;
    if (pss_has_header(packet->stream)) {
        size_t first = offsetof(struct mpeg_pes_packet, pts);
        if (count < first || pes->extension.marker != MPEG_PES_MARKER || first + pes->extension.header_length > count) return -1;
        size_t offset = first + pes->extension.header_length;
        if (pes->extension.pts_dts & MPEG_PES_HAS_PTS) {
            if (pes->extension.header_length < sizeof pes->pts || !pes->pts.marker_high || !pes->pts.marker_middle || !pes->pts.marker_low) return -1;
            packet->pts = mpeg_pts_value(&pes->pts);
        }
        packet->data = data + offset;
        packet->size = count - offset;
    }
    return (int)count;
}

#ifdef PORT_MOVIES
#include "cdvd.h"
#include "port_present.h"
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <stdio.h>
#include <stdlib.h>
#include "port_graphics.h"

#include "audio.h"

enum { MOVIE_RATE_MIN = 8000, MOVIE_RATE_MAX = 192000, MOVIE_CHANNELS = 2, MOVIE_INTERLEAVE_MAX = 32768, MOVIE_SIZE_LIMIT = 2048 };
enum { MOVIE_PCM_LIMIT = MOVIE_INTERLEAVE_MAX / sizeof(int16_t) * MOVIE_CHANNELS };
static const double field_seconds = 1.0 / 50;
static const uint32_t opaque = 0xff000000u;

static struct {
    struct sceCdlFILE file;
    uint64_t offset;
    AVCodecContext *decoder;
    AVCodecParserContext *parser;
    AVFrame *frame;
    unsigned char input[sizeof(struct mpeg_pes_header) + UINT16_MAX + AV_INPUT_BUFFER_PADDING_SIZE];
    const unsigned char *video;
    size_t video_size;
    uint64_t video_pts;
    uint32_t *pixels;
    struct pss_sshd audio_header;
    unsigned audio_header_size;
    unsigned audio_rate;
    unsigned audio_channels;
    unsigned audio_interleave;
    unsigned audio_size;
    unsigned char audio_block[MOVIE_INTERLEAVE_MAX * MOVIE_CHANNELS];
    int16_t pcm[MOVIE_PCM_LIMIT];
    int with_audio;
    int loop;
    int playing;
    int ended;
    int started;
    int parser_flushed;
    int decoder_flushed;
    unsigned frames;
    double start;
    double next;
    double frame_time;
} movie;

static double movie_time(void) {
    return (double)port_fields * field_seconds;
}

static void movie_audio(const unsigned char *data, size_t size) {
    const struct pss_audio_header *subheader = (const struct pss_audio_header *)data;
    if (!movie.with_audio || size < sizeof *subheader || subheader->marker != PSS_AUDIO_MARKER || subheader->type != PSS_AUDIO_PCM) return;
    data += sizeof *subheader;
    size -= sizeof *subheader;
    while (size) {
        if (movie.audio_header_size < sizeof movie.audio_header) {
            unsigned count = (unsigned)sizeof movie.audio_header - movie.audio_header_size;
            if (count > size) count = (unsigned)size;
            memcpy((unsigned char *)&movie.audio_header + movie.audio_header_size, data, count);
            movie.audio_header_size += count;
            data += count;
            size -= count;
            if (movie.audio_header_size < sizeof movie.audio_header) return;
            const struct pss_sshd *h = &movie.audio_header;
            if (memcmp(h->tag, "SShd", sizeof h->tag) || h->size != offsetof(struct pss_sshd, body_tag) - offsetof(struct pss_sshd, type) ||
                h->type != PSS_SSHD_PCM16 || memcmp(h->body_tag, "SSbd", sizeof h->body_tag)) {
                movie.with_audio = 0;
                fprintf(stderr, "movie: unsupported PCM header\n");
                return;
            }
            movie.audio_rate = h->rate;
            movie.audio_channels = h->channels;
            movie.audio_interleave = h->interleave;
            if (movie.audio_rate < MOVIE_RATE_MIN || movie.audio_rate > MOVIE_RATE_MAX || movie.audio_channels < 1 || movie.audio_channels > MOVIE_CHANNELS ||
                movie.audio_interleave < sizeof(int16_t) || movie.audio_interleave > MOVIE_INTERLEAVE_MAX || movie.audio_interleave % sizeof(int16_t)) {
                movie.with_audio = 0;
                return;
            }
        }
        unsigned block = movie.audio_interleave * movie.audio_channels;
        unsigned count = block - movie.audio_size;
        if (count > size) count = (unsigned)size;
        memcpy(movie.audio_block + movie.audio_size, data, count);
        movie.audio_size += count;
        data += count;
        size -= count;
        if (movie.audio_size == block) {
            unsigned frames = movie.audio_interleave / sizeof(int16_t);
            for (unsigned i = 0; i < frames; i++) {
                for (unsigned channel = 0; channel < movie.audio_channels; channel++) {
                    const unsigned char *sample = movie.audio_block + channel * movie.audio_interleave + i * sizeof(int16_t);
                    memcpy(&movie.pcm[i * movie.audio_channels + channel], sample, sizeof(int16_t));
                }
            }
            audio_movie_pcm(movie.pcm, frames, movie.audio_rate, movie.audio_channels);
            movie.audio_size = 0;
        }
    }
}

static int movie_packet(void) {
    while (movie.offset < movie.file.size) {
        size_t left = movie.file.size - movie.offset;
        size_t size = left < sizeof(struct mpeg_pack_header) ? left : sizeof(struct mpeg_pack_header);
        if (!port_cdvd_read_file(&movie.file, movie.offset, movie.input, size)) return -1;
        size_t packet_size = pss_packet_size(movie.input, size);
        if (!packet_size) return -1;
        if (packet_size > left || packet_size > sizeof(movie.input) - AV_INPUT_BUFFER_PADDING_SIZE) return -1;
        if (packet_size > size && !port_cdvd_read_file(&movie.file, movie.offset + size, movie.input + size, packet_size - size)) return -1;
        size = packet_size;
        struct port_pss_packet packet;
        int used = port_pss_packet(movie.input, size, &packet);
        if (used <= 0) return -1;
        movie.offset += (unsigned)used;
        if (packet.stream == MPEG_STREAM_END) return 0;
        if (packet.stream == MPEG_STREAM_PRIVATE_1) movie_audio(packet.data, packet.size);
        if (packet.stream == MPEG_STREAM_VIDEO && packet.size) {
            movie.video = packet.data;
            movie.video_size = packet.size;
            movie.video_pts = packet.pts;
            memset(movie.input + used, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            return 1;
        }
    }
    return 0;
}

static unsigned movie_byte(int value) {
    if (value < 0) return 0;
    return value > 255 ? 255 : (unsigned)value;
}

static int movie_picture(void) {
    int width = movie.frame->width;
    int height = movie.frame->height;
    if (width < 1 || height < 1 || width > MOVIE_SIZE_LIMIT || height > MOVIE_SIZE_LIMIT || movie.frame->format != AV_PIX_FMT_YUV420P) return -1;
    uint32_t *pixels = realloc(movie.pixels, (size_t)width * height * sizeof *pixels);
    if (!pixels) return -1;
    movie.pixels = pixels;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int luma = movie.frame->data[0][y * movie.frame->linesize[0] + x] - 16;
            int cb = movie.frame->data[1][(y / 2) * movie.frame->linesize[1] + x / 2] - 128;
            int cr = movie.frame->data[2][(y / 2) * movie.frame->linesize[2] + x / 2] - 128;
            unsigned red = movie_byte((298 * luma + 409 * cr + 128) >> 8);
            unsigned green = movie_byte((298 * luma - 100 * cb - 208 * cr + 128) >> 8);
            unsigned blue = movie_byte((298 * luma + 516 * cb + 128) >> 8);
            pixels[y * width + x] = red | green << 8 | blue << 16 | opaque;
        }
    }
    if (movie.decoder->framerate.num > 0 && movie.decoder->framerate.den > 0)
        movie.frame_time = (double)movie.decoder->framerate.den / movie.decoder->framerate.num;
    if (!movie.started) {
        movie.start = movie_time();
        fprintf(stderr, "movie: playing %dx%d %.3f fps\n", width, height, 1.0 / movie.frame_time);
    }
    movie.started = 1;
    movie.frames++;
    movie.next = movie.start + movie.frames * movie.frame_time;
    port_present_movie(pixels, (unsigned)width, (unsigned)height);
    return 1;
}

void port_movie_close(void) {
    port_present_movie_clear();
    audio_movie_clear();
    avcodec_free_context(&movie.decoder);
    if (movie.parser) av_parser_close(movie.parser);
    av_frame_free(&movie.frame);
    free(movie.pixels);
    memset(&movie, 0, sizeof(movie));
    movie.ended = 1;
}

int port_movie_open(const char *name, int audio, int loop) {
    port_movie_close();
    if (!name) return 0;
    char path[1024];
    if (!port_cdvd_find(name, &movie.file)) {
        snprintf(path, sizeof(path), "MOVIE/%s", name);
        if (!port_cdvd_find(path, &movie.file)) {
            fprintf(stderr, "movie: cannot find %s on disc\n", name);
            return 0;
        }
    }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    movie.decoder = avcodec_alloc_context3(codec);
    movie.parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    movie.frame = av_frame_alloc();
    if (!codec || !movie.decoder || !movie.parser || !movie.frame || avcodec_open2(movie.decoder, codec, NULL) < 0) {
        port_movie_close();
        return 0;
    }
    movie.with_audio = audio;
    movie.loop = loop;
    movie.ended = 0;
    movie.frame_time = 1.0 / 25;
    fprintf(stderr, "movie: opened %s (%u bytes)\n", name, movie.file.size);
    return 1;
}

void port_movie_start(void) { movie.playing = !movie.ended; }
int port_movie_end(void) { return movie.ended; }
int port_movie_started(void) { return movie.started || movie.ended; }
unsigned port_movie_frames(void) { return movie.frames; }

void port_movie_step(void) {
    if (!movie.playing || movie.ended || (movie.started && movie_time() < movie.next)) return;
    for (;;) {
        int status = avcodec_receive_frame(movie.decoder, movie.frame);
        if (status == 0) {
            if (movie_picture() > 0) return;
            break;
        }
        if (status == AVERROR_EOF) {
            if (movie.loop) {
                avcodec_flush_buffers(movie.decoder);
                av_parser_close(movie.parser);
                movie.parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
                if (!movie.parser) break;
                movie.offset = 0;
                movie.video_size = 0;
                movie.parser_flushed = 0;
                movie.decoder_flushed = 0;
                movie.audio_header_size = 0;
                movie.audio_size = 0;
                movie.started = 0;
                movie.frames = 0;
                audio_movie_clear();
                continue;
            }
            movie.ended = 1;
            movie.playing = 0;
            fprintf(stderr, "movie: ended after %u frames\n", movie.frames);
            return;
        }
        if (status != AVERROR(EAGAIN)) break;
        unsigned char *output = NULL;
        int output_size = 0;
        if (!movie.video_size && !movie.parser_flushed) {
            int read = movie_packet();
            if (read < 0) break;
            if (!read) movie.parser_flushed = 1;
        }
        if (!movie.decoder_flushed) {
            int used = av_parser_parse2(movie.parser, movie.decoder, &output, &output_size,
                                        movie.video_size ? movie.video : NULL, (int)movie.video_size,
                                        movie.video_pts == UINT64_MAX ? AV_NOPTS_VALUE : (int64_t)movie.video_pts,
                                        AV_NOPTS_VALUE, -1);
            if (used < 0) break;
            if (used) movie.video += used;
            movie.video_size -= (unsigned)used;
            movie.video_pts = UINT64_MAX;
            if (output_size) {
                AVPacket packet = {0};
                packet.data = output;
                packet.size = output_size;
                packet.pts = movie.parser->pts;
                if (avcodec_send_packet(movie.decoder, &packet) < 0) break;
            } else if (movie.parser_flushed) {
                if (avcodec_send_packet(movie.decoder, NULL) < 0) break;
                movie.decoder_flushed = 1;
            }
        } else break;
    }
    movie.ended = 1;
    movie.playing = 0;
    fprintf(stderr, "movie: decode failed at byte %llu\n", (unsigned long long)movie.offset);
}

#else
int port_movie_open(const char *name, int audio, int loop) { return 0; }
void port_movie_start(void) {}
void port_movie_step(void) {}
void port_movie_close(void) {}
int port_movie_end(void) { return 1; }
int port_movie_started(void) { return 1; }
unsigned port_movie_frames(void) { return 0; }
#endif
