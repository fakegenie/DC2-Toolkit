#pragma once

#include <stdint.h>

void port_gs_clut_reset(void);
uint64_t port_gs_clut_generation(void);
int port_gs_clut_load(const uint64_t regs[128], unsigned context);
uint32_t port_gs_clut_read(const uint64_t regs[128], unsigned context, unsigned index);
