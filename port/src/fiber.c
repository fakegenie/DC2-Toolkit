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

#elif defined(_WIN64)

enum { STACK_ALIGNMENT = 16 };

void port_fiber_start(void);

// The new fiber's stack is 16-byte aligned here, so after 32 bytes of shadow space the call leaves the entry function
// with the alignment the Win64 calling convention gives it.
__asm__(".text\n"
        ".p2align 4\n"
        ".globl port_fiber_start\n"
        "port_fiber_start:\n"
        "    subq $32, %rsp\n"
        "    callq *%rbx\n"
        "    ud2\n");

// The Win64 callee-saved registers, rsp and the resume address, and the thread information block's stack bounds
// (gs:0x08 and gs:0x10), which Windows checks when it unwinds or grows a stack.
__attribute__((naked)) void port_fiber_switch(PortFiber *from, PortFiber *to) {
    __asm__ volatile("    movq %%rbx, %c[rbx](%%rcx)\n"
                     "    movq %%rbp, %c[rbp](%%rcx)\n"
                     "    movq %%rdi, %c[rdi](%%rcx)\n"
                     "    movq %%rsi, %c[rsi](%%rcx)\n"
                     "    movq %%r12, %c[r12](%%rcx)\n"
                     "    movq %%r13, %c[r13](%%rcx)\n"
                     "    movq %%r14, %c[r14](%%rcx)\n"
                     "    movq %%r15, %c[r15](%%rcx)\n"
                     "    movq (%%rsp), %%rax\n"
                     "    movq %%rax, %c[resume](%%rcx)\n"
                     "    leaq 8(%%rsp), %%rax\n"
                     "    movq %%rax, %c[stack](%%rcx)\n"
                     "    movq %%gs:0x08, %%rax\n"
                     "    movq %%rax, %c[base](%%rcx)\n"
                     "    movq %%gs:0x10, %%rax\n"
                     "    movq %%rax, %c[limit](%%rcx)\n"
                     "    movdqu %%xmm6, %c[xmm]+0(%%rcx)\n"
                     "    movdqu %%xmm7, %c[xmm]+16(%%rcx)\n"
                     "    movdqu %%xmm8, %c[xmm]+32(%%rcx)\n"
                     "    movdqu %%xmm9, %c[xmm]+48(%%rcx)\n"
                     "    movdqu %%xmm10, %c[xmm]+64(%%rcx)\n"
                     "    movdqu %%xmm11, %c[xmm]+80(%%rcx)\n"
                     "    movdqu %%xmm12, %c[xmm]+96(%%rcx)\n"
                     "    movdqu %%xmm13, %c[xmm]+112(%%rcx)\n"
                     "    movdqu %%xmm14, %c[xmm]+128(%%rcx)\n"
                     "    movdqu %%xmm15, %c[xmm]+144(%%rcx)\n"
                     "    movq %c[rbx](%%rdx), %%rbx\n"
                     "    movq %c[rbp](%%rdx), %%rbp\n"
                     "    movq %c[rdi](%%rdx), %%rdi\n"
                     "    movq %c[rsi](%%rdx), %%rsi\n"
                     "    movq %c[r12](%%rdx), %%r12\n"
                     "    movq %c[r13](%%rdx), %%r13\n"
                     "    movq %c[r14](%%rdx), %%r14\n"
                     "    movq %c[r15](%%rdx), %%r15\n"
                     "    movq %c[base](%%rdx), %%rax\n"
                     "    movq %%rax, %%gs:0x08\n"
                     "    movq %c[limit](%%rdx), %%rax\n"
                     "    movq %%rax, %%gs:0x10\n"
                     "    movdqu %c[xmm]+0(%%rdx), %%xmm6\n"
                     "    movdqu %c[xmm]+16(%%rdx), %%xmm7\n"
                     "    movdqu %c[xmm]+32(%%rdx), %%xmm8\n"
                     "    movdqu %c[xmm]+48(%%rdx), %%xmm9\n"
                     "    movdqu %c[xmm]+64(%%rdx), %%xmm10\n"
                     "    movdqu %c[xmm]+80(%%rdx), %%xmm11\n"
                     "    movdqu %c[xmm]+96(%%rdx), %%xmm12\n"
                     "    movdqu %c[xmm]+112(%%rdx), %%xmm13\n"
                     "    movdqu %c[xmm]+128(%%rdx), %%xmm14\n"
                     "    movdqu %c[xmm]+144(%%rdx), %%xmm15\n"
                     "    movq %c[stack](%%rdx), %%rsp\n"
                     "    jmpq *%c[resume](%%rdx)\n"
                     :
                     : [rbx] "i"(offsetof(PortFiber, rbx)), [rbp] "i"(offsetof(PortFiber, rbp)),
                       [rdi] "i"(offsetof(PortFiber, rdi)), [rsi] "i"(offsetof(PortFiber, rsi)),
                       [r12] "i"(offsetof(PortFiber, r12)), [r13] "i"(offsetof(PortFiber, r13)),
                       [r14] "i"(offsetof(PortFiber, r14)), [r15] "i"(offsetof(PortFiber, r15)),
                       [stack] "i"(offsetof(PortFiber, stack_pointer)), [resume] "i"(offsetof(PortFiber, resume)),
                       [base] "i"(offsetof(PortFiber, stack_base)), [limit] "i"(offsetof(PortFiber, stack_limit)),
                       [xmm] "i"(offsetof(PortFiber, xmm)));
}

void port_fiber_make(PortFiber *fiber, void *stack, size_t size, void (*entry)(void)) {
    memset(fiber, 0, sizeof *fiber);
    fiber->rbx = (uint64_t)(uintptr_t)entry;
    fiber->resume = (uint64_t)(uintptr_t)port_fiber_start;
    fiber->stack_pointer = ((uint64_t)(uintptr_t)stack + size) & ~(uint64_t)(STACK_ALIGNMENT - 1);
    fiber->stack_base = fiber->stack_pointer;
    fiber->stack_limit = (uint64_t)(uintptr_t)stack;
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
