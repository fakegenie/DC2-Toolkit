#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *port_allocate(size_t size, size_t alignment);
void port_release(void *pointer);
typedef struct {
    uint64_t start;
    uint64_t live_end;
} PortArenaSpan;

size_t port_arena_spans(const unsigned char *saved, size_t size, uint64_t saved_at, PortArenaSpan *spans, size_t limit);

#ifdef __cplusplus
}
#endif
