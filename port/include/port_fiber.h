#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __aarch64__
typedef struct {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t frame_pointer;
    uint64_t link_register;
    uint64_t stack_pointer;
    double d8, d9, d10, d11, d12, d13, d14, d15;
} PortFiber;
#else
#include <ucontext.h>
typedef struct {
    ucontext_t context;
} PortFiber;
#endif

void port_fiber_make(PortFiber *fiber, void *stack, size_t size, void (*entry)(void));
void port_fiber_switch(PortFiber *from, PortFiber *to);
