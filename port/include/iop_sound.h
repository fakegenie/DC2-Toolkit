#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
void *iop_memory_resolve(uint32_t address, size_t size);
size_t iop_memory_size(uint32_t address);
uint32_t iop_memory_alloc(size_t size);
void iop_memory_free(void *address);
int port_sound_init(int enabled);
void port_sound_shutdown(void);
void port_sound_frame(void);
void port_sound_render(int16_t *output, size_t frames);
void port_sound_reset(void);
int port_sound_rpc(unsigned server, unsigned command, const void *send, int send_size, void *receive, int receive_size);
int port_sound_remote(int command, va_list arguments);
void port_sound_movie_pcm(const int16_t *samples, size_t frames, unsigned rate, unsigned channels);
void port_sound_movie_clear(void);
size_t port_sound_movie_queued(void);
