#define _POSIX_C_SOURCE 200809L
#include "movie.h"
#include <string.h>

int port_pss_packet(const unsigned char *data, size_t size, struct port_pss_packet *packet) {
    if (!data || !packet || size < 4) return 0;
    if (data[0] || data[1] || data[2] != 1) return -1;
    memset(packet, 0, sizeof(*packet));
    packet->stream = data[3];
    packet->pts = UINT64_MAX;
    if (data[3] == 0xb9) return 4;
    if (data[3] == 0xba) {
        if (size < 14) return 0;
        if ((data[4] & 0xc0) != 0x40) return -1;
        size_t count = 14 + (data[13] & 7);
        return size < count ? 0 : (int)count;
    }
    if (size < 6) return 0;
    size_t count = 6 + ((unsigned)data[4] << 8 | data[5]);
    if (count == 6) return -1;
    if (size < count) return 0;
    if ((data[3] & 0xe0) == 0xc0 || (data[3] & 0xf0) == 0xe0 || data[3] == 0xbd) {
        if (count < 9 || (data[6] & 0xc0) != 0x80 || 9u + data[8] > count) return -1;
        size_t offset = 9 + data[8];
        if (data[7] & 0x80) {
            if (data[8] < 5 || !(data[9] & 1) || !(data[11] & 1) || !(data[13] & 1)) return -1;
            packet->pts = (uint64_t)(data[9] & 14) << 29 | (uint64_t)data[10] << 22 |
                          (uint64_t)(data[11] & 254) << 14 | (uint64_t)data[12] << 7 | data[13] >> 1;
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

static struct {
    struct sceCdlFILE file;
    uint64_t offset;
    AVCodecContext *decoder;
    AVCodecParserContext *parser;
    AVFrame *frame;
    unsigned char input[65542 + AV_INPUT_BUFFER_PADDING_SIZE];
    const unsigned char *video;
    size_t video_size;
    uint64_t video_pts;
    uint32_t *pixels;
    unsigned char audio_header[40];
    unsigned audio_header_size;
    unsigned audio_rate;
    unsigned audio_channels;
    unsigned audio_interleave;
    unsigned audio_size;
    unsigned char audio_block[65536];
    int16_t pcm[32768];
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
    return (double)port_graphics.fields * 0.02;
}

static unsigned movie_le32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static void movie_audio(const unsigned char *data, size_t size) {
    if (!movie.with_audio || size < 4 || data[0] != 0xff || data[1] != 0xa0) return;
    data += 4;
    size -= 4;
    while (size) {
        if (movie.audio_header_size < 40) {
            unsigned count = 40 - movie.audio_header_size;
            if (count > size) count = (unsigned)size;
            memcpy(movie.audio_header + movie.audio_header_size, data, count);
            movie.audio_header_size += count;
            data += count;
            size -= count;
            if (movie.audio_header_size < 40) return;
            const unsigned char *h = movie.audio_header;
            if (memcmp(h, "SShd", 4) || movie_le32(h + 4) != 24 || movie_le32(h + 8) != 1 || memcmp(h + 32, "SSbd", 4)) {
                movie.with_audio = 0;
                fprintf(stderr, "movie: unsupported PCM header\n");
                return;
            }
            movie.audio_rate = movie_le32(h + 12);
            movie.audio_channels = movie_le32(h + 16);
            movie.audio_interleave = movie_le32(h + 20);
            if (movie.audio_rate < 8000 || movie.audio_rate > 192000 || movie.audio_channels < 1 || movie.audio_channels > 2 ||
                movie.audio_interleave < 2 || movie.audio_interleave > 32768 || movie.audio_interleave & 1) {
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
            unsigned frames = movie.audio_interleave / 2;
            for (unsigned i = 0; i < frames; i++) {
                for (unsigned channel = 0; channel < movie.audio_channels; channel++) {
                    const unsigned char *p = movie.audio_block + channel * movie.audio_interleave + i * 2;
                    movie.pcm[i * movie.audio_channels + channel] = (int16_t)((unsigned)p[0] | (unsigned)p[1] << 8);
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
        size_t size = left < 14 ? left : 14;
        if (!port_cdvd_read_file(&movie.file, movie.offset, movie.input, size)) return -1;
        if (size < 4) return -1;
        size_t packet_size;
        if (movie.input[3] == 0xb9) packet_size = 4;
        else if (movie.input[3] == 0xba) {
            if (size < 14) return -1;
            packet_size = 14 + (movie.input[13] & 7);
        } else {
            if (size < 6) return -1;
            packet_size = 6 + ((unsigned)movie.input[4] << 8 | movie.input[5]);
        }
        if (packet_size > left || packet_size > sizeof(movie.input) - AV_INPUT_BUFFER_PADDING_SIZE) return -1;
        if (packet_size > size && !port_cdvd_read_file(&movie.file, movie.offset + size, movie.input + size, packet_size - size)) return -1;
        size = packet_size;
        struct port_pss_packet packet;
        int used = port_pss_packet(movie.input, size, &packet);
        if (used <= 0) return -1;
        movie.offset += (unsigned)used;
        if (packet.stream == 0xb9) return 0;
        if (packet.stream == 0xbd) movie_audio(packet.data, packet.size);
        if (packet.stream == 0xe0 && packet.size) {
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
    if (width < 1 || height < 1 || width > 2048 || height > 2048 || movie.frame->format != AV_PIX_FMT_YUV420P) return -1;
    uint32_t *pixels = realloc(movie.pixels, (size_t)width * height * 4);
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
            pixels[y * width + x] = red | green << 8 | blue << 16 | 0xff000000u;
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
