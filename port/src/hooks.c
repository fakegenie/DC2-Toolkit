#include "port_kernel.h"
#include "port_memory.h"
#include "port_menu.h"
#include "port_stats.h"

PortStats port_stats;

__attribute__((weak)) void port_kernel_yield(void) {}

__attribute__((weak)) void port_debug_replayed(float alpha) { (void)alpha; }

__attribute__((weak)) void port_memory_set(const void *pool, const char *file, int line, int size) {
    (void)pool;
    (void)file;
    (void)line;
    (void)size;
}

__attribute__((weak)) void port_memory_use(const void *pool, int used, int size) {
    (void)pool;
    (void)used;
    (void)size;
}

__attribute__((weak)) int port_menu_open(int open_type, int *param) {
    (void)param;
    return open_type;
}
