#include "port_kernel.h"
#include "port_stats.h"

PortStats port_stats;

__attribute__((weak)) void port_kernel_yield(void) {}
