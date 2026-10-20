#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __APPLE__
struct mach_header_64;
#ifdef __cplusplus
extern "C" const struct mach_header_64 _mh_execute_header;
#else
extern const struct mach_header_64 _mh_execute_header;
#endif
#define PORT_WINDOW ((uint64_t)(uintptr_t)&_mh_execute_header)
#else
#define PORT_WINDOW UINT64_C(0)
#endif

static inline uint32_t port_address(const void *pointer) {
    return (uint32_t)((uintptr_t)pointer - PORT_WINDOW);
}

#ifdef __cplusplus
extern "C" {
#endif

void *port_allocate(size_t size, size_t alignment);
void port_release(void *pointer);

#ifdef __cplusplus
}
#endif
