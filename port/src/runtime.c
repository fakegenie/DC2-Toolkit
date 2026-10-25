#include "port_game.h"
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

PORT_GAME_BEGIN
static uint32_t rand_next = 1;
static int floor_seed_forced = -1;
static int floor_seed_last = -1;
PORT_GAME_END

void mwInit(void) {
}

void srand(unsigned int seed) {
    rand_next = seed;
}

unsigned int port_rand_state(void) {
    return rand_next;
}

int port_floor_seed(int drawn) {
    floor_seed_last = floor_seed_forced >= 0 ? floor_seed_forced : drawn;
    floor_seed_forced = -1;
    return floor_seed_last;
}

void port_floor_seed_force(int seed) {
    floor_seed_forced = seed;
}

int port_floor_seed_last(void) {
    return floor_seed_last;
}

int rand(void) {
    rand_next = rand_next * 1103515245u + 12345u;
    return (int)(rand_next & INT_MAX);
}

int fptosi(float value) {
    if (isnan(value)) return 0;
    if (value >= 2147483648.0f) return INT_MAX;
    if (value <= -2147483648.0f) return INT_MIN;
    return (int)value;
}

unsigned int fptoui(float value) {
    if (!(value > 0.0f)) return 0;
    if (value >= 4294967296.0f) return UINT_MAX;
    return (unsigned int)value;
}

typedef struct {
    uint32_t element_size;
    uint32_t count;
    uint32_t padding[2];
} ArrayCookie;

void *__construct_new_array(void *buffer, void *(*constructor)(void *, int), void *destructor,
                            unsigned int element_size, int count) {
    (void)destructor;
    if (!buffer) return 0;
    ArrayCookie *cookie = buffer;
    cookie->element_size = element_size;
    cookie->count = (uint32_t)count;
    unsigned char *elements = (unsigned char *)(cookie + 1);
    if (constructor) {
        for (uint32_t i = 0; i < (uint32_t)count; ++i) constructor(elements + i * (size_t)element_size, 1);
    }
    return elements;
}
