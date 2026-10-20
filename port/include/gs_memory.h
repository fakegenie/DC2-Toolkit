#pragma once

#include <stdint.h>

void port_gs_memory_reset(void);
uint8_t port_gs_memory_read_byte(unsigned address);
void port_gs_memory_write_byte(unsigned address, uint8_t value);
uint32_t port_gs_memory_read(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y);
void port_gs_memory_write(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y, uint32_t value);
