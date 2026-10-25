#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __aarch64__
typedef struct {
    uint64_t registers[21];
} PortFiber;
#else
#include <ucontext.h>
typedef struct {
    ucontext_t context;
} PortFiber;
#endif

void port_fiber_make(PortFiber *fiber, void *stack, size_t size, void (*entry)(void));
void port_fiber_switch(PortFiber *from, PortFiber *to);
uintptr_t port_fiber_frame(const PortFiber *fiber);
