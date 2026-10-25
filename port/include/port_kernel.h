#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int port_kernel_run(int (*entry)(void));
void port_kernel_vblank(void);
int port_kernel_quiet(void (*parked)(void *));
uint64_t port_kernel_main_stack_top(void);
void port_kernel_yield(void);

#ifdef __cplusplus
}
#endif
