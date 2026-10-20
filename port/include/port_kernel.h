#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int port_kernel_run(int (*entry)(void));
void port_kernel_vblank(void);

#ifdef __cplusplus
}
#endif
