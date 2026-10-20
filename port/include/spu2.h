#pragma once
#include <stddef.h>
#include <stdint.h>
void port_spu_reset(void);
int port_spu_transfer(unsigned address, void *data, size_t size, int read);
void port_spu_param(unsigned entry, unsigned value);
void port_spu_addr(unsigned entry, unsigned value);
void port_spu_switch(unsigned entry, unsigned value);
void port_spu_sample(int32_t *left, int32_t *right);
int port_spu_active(unsigned voice);
void port_spu_start(unsigned voice, unsigned address, unsigned pitch, int left, int right, unsigned adsr1, unsigned adsr2);
void port_spu_stop(unsigned voice);
void port_spu_route(unsigned voice, unsigned route);
int port_spu_effect(unsigned core, unsigned mode, int left, int right, int delay, int feedback);
void port_spu_effect_enable(unsigned core, int enabled);
void port_spu_input(unsigned core, int left, int right);
int port_spu_level(unsigned voice);
