#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __APPLE__
#define PORT_WINDOW UINT64_C(0x100000000)
#else
#define PORT_WINDOW UINT64_C(0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *port_allocate(size_t size, size_t alignment);
void port_release(void *pointer);

#ifdef __cplusplus
}
#endif
