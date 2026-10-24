#include "port_kernel.h"
#include "port_memory.h"
#include "port_stats.h"

PortStats port_stats;

__attribute__((weak)) void port_kernel_yield(void) {}

__attribute__((weak)) void port_debug_replayed(float alpha) { (void)alpha; }

__attribute__((weak)) void port_memory_set(const void *pool, const void *site, int size) {
    (void)pool;
    (void)site;
    (void)size;
}

__attribute__((weak)) void port_memory_use(const void *pool, int used, int size) {
    (void)pool;
    (void)used;
    (void)size;
}
