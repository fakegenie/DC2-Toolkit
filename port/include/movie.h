#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
