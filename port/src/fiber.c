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

void port_fiber_start(void);

__asm__(".text\n"
        ".p2align 2\n"
        ".globl " SYMBOL(port_fiber_switch) "\n"
        SYMBOL(port_fiber_switch) ":\n"
        "    stp x19, x20, [x0, #0]\n"
        "    stp x21, x22, [x0, #16]\n"
        "    stp x23, x24, [x0, #32]\n"
        "    stp x25, x26, [x0, #48]\n"
        "    stp x27, x28, [x0, #64]\n"
        "    stp x29, x30, [x0, #80]\n"
        "    mov x9, sp\n"
        "    str x9, [x0, #96]\n"
        "    stp d8, d9, [x0, #104]\n"
        "    stp d10, d11, [x0, #120]\n"
        "    stp d12, d13, [x0, #136]\n"
        "    stp d14, d15, [x0, #152]\n"
        "    ldp x19, x20, [x1, #0]\n"
        "    ldp x21, x22, [x1, #16]\n"
        "    ldp x23, x24, [x1, #32]\n"
        "    ldp x25, x26, [x1, #48]\n"
        "    ldp x27, x28, [x1, #64]\n"
        "    ldp x29, x30, [x1, #80]\n"
        "    ldr x9, [x1, #96]\n"
        "    mov sp, x9\n"
        "    ldp d8, d9, [x1, #104]\n"
        "    ldp d10, d11, [x1, #120]\n"
        "    ldp d12, d13, [x1, #136]\n"
        "    ldp d14, d15, [x1, #152]\n"
        "    ret\n"
        ".p2align 2\n"
        ".globl " SYMBOL(port_fiber_start) "\n"
        SYMBOL(port_fiber_start) ":\n"
        "    blr x19\n"
        "    brk #1\n");

void port_fiber_make(PortFiber *fiber, void *stack, size_t size, void (*entry)(void)) {
    memset(fiber, 0, sizeof *fiber);
    fiber->registers[0] = (uint64_t)(uintptr_t)entry;
    fiber->registers[11] = (uint64_t)(uintptr_t)port_fiber_start;
    fiber->registers[12] = ((uint64_t)(uintptr_t)stack + size) & ~UINT64_C(15);
}

uintptr_t port_fiber_frame(const PortFiber *fiber) { return (uintptr_t)fiber->registers[10]; }

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

uintptr_t port_fiber_frame(const PortFiber *fiber) {
    (void)fiber;
    return UINTPTR_MAX;
}

void port_fiber_switch(PortFiber *from, PortFiber *to) {
    if (swapcontext(&from->context, &to->context) < 0) {
        perror("port: fiber switch");
        exit(1);
    }
}

#endif
