#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

void mwInit(void) {
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

void *__construct_new_array(void *buffer, void *(*constructor)(void *, int), void *destructor,
                            unsigned int element_size, int count) {
    (void)destructor;
    if (!buffer) return 0;
    uint32_t *header = buffer;
    header[0] = element_size;
    header[1] = (uint32_t)count;
    unsigned char *elements = (unsigned char *)buffer + 16;
    if (constructor) {
        for (uint32_t i = 0; i < (uint32_t)count; ++i) constructor(elements + i * (size_t)element_size, 1);
    }
    return elements;
}
