#define _XOPEN_SOURCE 700
#include "port_fiber.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __aarch64__

#ifdef __APPLE__
#define SYMBOL(name) "_" #name
#else
#define SYMBOL(name) #name
#endif

enum { STACK_ALIGNMENT = 16 };

void port_fiber_start(void);

__asm__(".text\n"
        ".p2align 2\n"
        ".globl " SYMBOL(port_fiber_start) "\n"
        SYMBOL(port_fiber_start) ":\n"
        "    blr x19\n"
        "    brk #1\n");

__attribute__((naked)) void port_fiber_switch(PortFiber *from, PortFiber *to) {
    __asm__ volatile("    stp x19, x20, [x0, #%[x19]]\n"
                     "    stp x21, x22, [x0, #%[x21]]\n"
                     "    stp x23, x24, [x0, #%[x23]]\n"
                     "    stp x25, x26, [x0, #%[x25]]\n"
                     "    stp x27, x28, [x0, #%[x27]]\n"
                     "    stp x29, x30, [x0, #%[frame]]\n"
                     "    mov x9, sp\n"
                     "    str x9, [x0, #%[stack]]\n"
                     "    stp d8, d9, [x0, #%[d8]]\n"
                     "    stp d10, d11, [x0, #%[d10]]\n"
                     "    stp d12, d13, [x0, #%[d12]]\n"
                     "    stp d14, d15, [x0, #%[d14]]\n"
                     "    ldp x19, x20, [x1, #%[x19]]\n"
                     "    ldp x21, x22, [x1, #%[x21]]\n"
                     "    ldp x23, x24, [x1, #%[x23]]\n"
                     "    ldp x25, x26, [x1, #%[x25]]\n"
                     "    ldp x27, x28, [x1, #%[x27]]\n"
                     "    ldp x29, x30, [x1, #%[frame]]\n"
                     "    ldr x9, [x1, #%[stack]]\n"
                     "    mov sp, x9\n"
                     "    ldp d8, d9, [x1, #%[d8]]\n"
                     "    ldp d10, d11, [x1, #%[d10]]\n"
                     "    ldp d12, d13, [x1, #%[d12]]\n"
                     "    ldp d14, d15, [x1, #%[d14]]\n"
                     "    ret\n"
                     :
                     : [x19] "i"(offsetof(PortFiber, x19)), [x21] "i"(offsetof(PortFiber, x21)),
                       [x23] "i"(offsetof(PortFiber, x23)), [x25] "i"(offsetof(PortFiber, x25)),
                       [x27] "i"(offsetof(PortFiber, x27)), [frame] "i"(offsetof(PortFiber, frame_pointer)),
                       [stack] "i"(offsetof(PortFiber, stack_pointer)), [d8] "i"(offsetof(PortFiber, d8)),
                       [d10] "i"(offsetof(PortFiber, d10)), [d12] "i"(offsetof(PortFiber, d12)),
                       [d14] "i"(offsetof(PortFiber, d14)));
}

void port_fiber_make(PortFiber *fiber, void *stack, size_t size, void (*entry)(void)) {
    memset(fiber, 0, sizeof *fiber);
    fiber->x19 = (uint64_t)(uintptr_t)entry;
    fiber->link_register = (uint64_t)(uintptr_t)port_fiber_start;
    fiber->stack_pointer = ((uint64_t)(uintptr_t)stack + size) & ~(uint64_t)(STACK_ALIGNMENT - 1);
}

#else

void port_fiber_make(PortFiber *fiber, void *stack, size_t size, void (*entry)(void)) {
    if (getcontext(&fiber->context) < 0) {
        perror("port: fiber");
        exit(1);
    }
    fiber->context.uc_stack.ss_sp = stack;
    fiber->context.uc_stack.ss_size = size;
    fiber->context.uc_link = NULL;
    makecontext(&fiber->context, entry, 0);
}

void port_fiber_switch(PortFiber *from, PortFiber *to) {
    if (swapcontext(&from->context, &to->context) < 0) {
        perror("port: fiber switch");
        exit(1);
    }
}

#endif
