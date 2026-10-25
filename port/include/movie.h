#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MPEG_STREAM_END = 0xb9,
    MPEG_STREAM_PACK = 0xba,
    MPEG_STREAM_PRIVATE_1 = 0xbd,
    MPEG_STREAM_AUDIO = 0xc0,
    MPEG_STREAM_AUDIO_MASK = 0xe0,
    MPEG_STREAM_VIDEO = 0xe0,
    MPEG_STREAM_VIDEO_MASK = 0xf0
};
enum { MPEG_PACK_VERSION_2 = 1, MPEG_PES_MARKER = 2, MPEG_PES_HAS_PTS = 2 };
enum { PSS_AUDIO_MARKER = 0xff, PSS_AUDIO_PCM = 0xa0, PSS_SSHD_PCM16 = 1 };

struct mpeg_start_code {
    uint8_t prefix[3];
    uint8_t stream;
};

struct mpeg_pack_header {
    struct mpeg_start_code start;
    uint8_t scr_high : 6;
    uint8_t version : 2;
    uint8_t scr[5];
    uint8_t mux_rate[3];
    uint8_t stuffing_length : 3;
    uint8_t reserved : 5;
};

struct mpeg_pes_header {
    struct mpeg_start_code start;
    uint8_t length[2];
};

struct mpeg_pes_extension {
    uint8_t original : 1;
    uint8_t copyright : 1;
    uint8_t data_alignment : 1;
    uint8_t priority : 1;
    uint8_t scrambling : 2;
    uint8_t marker : 2;
    uint8_t extension : 1;
    uint8_t crc : 1;
    uint8_t copy_info : 1;
    uint8_t rate : 1;
    uint8_t trick_mode : 1;
    uint8_t escr : 1;
    uint8_t pts_dts : 2;
    uint8_t header_length;
};

struct mpeg_pts {
    uint8_t marker_high : 1;
    uint8_t high : 3;
    uint8_t prefix : 4;
    uint8_t middle_high;
    uint8_t marker_middle : 1;
    uint8_t middle_low : 7;
    uint8_t low_high;
    uint8_t marker_low : 1;
    uint8_t low_low : 7;
};

struct mpeg_pes_packet {
    struct mpeg_pes_header header;
    struct mpeg_pes_extension extension;
    struct mpeg_pts pts;
};

struct pss_audio_header {
    uint8_t marker;
    uint8_t type;
    uint8_t reserved[2];
};

struct pss_sshd {
    char tag[4];
    uint32_t size;
    uint32_t type;
    uint32_t rate;
    uint32_t channels;
    uint32_t interleave;
    uint32_t loop_start;
    uint32_t loop_end;
    char body_tag[4];
    uint32_t body_size;
};

struct port_pss_packet {
    unsigned stream;
    const unsigned char *data;
    size_t size;
    uint64_t pts;
};

int port_pss_packet(const unsigned char *data, size_t size, struct port_pss_packet *packet);
int port_movie_open(const char *name, int audio, int loop);
void port_movie_start(void);
void port_movie_step(void);
void port_movie_close(void);
int port_movie_end(void);
int port_movie_started(void);
unsigned port_movie_frames(void);

#ifdef __cplusplus
}
#endif
