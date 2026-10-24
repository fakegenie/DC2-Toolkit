#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATE_RESUMABLE __attribute__((optnone, noinline))

enum { STATE_SAVE = 1, STATE_LOAD = 2 };
enum { STATE_THUMB_WIDTH = 160, STATE_THUMB_HEIGHT = 120 };

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t regions;
    unsigned char layout[16];
    uint32_t frame;
    int32_t loop;
    int32_t map;
    uint32_t arena_blocks;
    uint64_t field;
    int64_t taken;
    uint64_t arena_used;
    uint64_t arena_peak;
    uint64_t sound;
    uint64_t size;
    uint64_t base;
    uint64_t guard;
} PortStateHeader;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t room;
} PortState;

const char *port_state_ready(void);
int port_state_request(int kind, PortState *state);
int port_state_pending(void);
const char *port_state_problem(void);
const char *port_state_note(void);
void port_state_force(int force);
float port_state_ms(void);
const char *port_state_keep(void);
STATE_RESUMABLE void port_state_frame(void);
void port_state_host(void);
const PortStateHeader *port_state_header(const PortState *state);
const uint32_t *port_state_thumbnail(const PortState *state);
const char *port_state_write(const PortState *state, const char *path);
const char *port_state_read(PortState *state, const char *path);

#ifdef __cplusplus
}
#endif
