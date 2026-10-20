#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int port_kernel_run(int (*entry)(void));
void port_kernel_vblank(void);
void port_kernel_host(void);
void port_kernel_yield(void);

#ifdef __cplusplus
}
#endif
