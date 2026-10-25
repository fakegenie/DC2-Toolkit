#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int port_kernel_run(int (*entry)(void));
void port_kernel_vblank(void);
void port_kernel_host(void);
size_t port_kernel_returns(uint64_t *slots, size_t limit);
void port_kernel_stacks(uint64_t *low, uint64_t *high);
void port_kernel_yield(void);

#ifdef __cplusplus
}
#endif
