#include "port_fiber.h"
#include "port_kernel.h"
#include "port_mod.h"
#include "port_state.h"
#include "sce/eekernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { THREAD_LIMIT = 32, SEMA_LIMIT = 64, HANDLER_LIMIT = 64, STACK_SIZE = 1024 * 1024 };
enum { UNUSED, DORMANT, READY, RUNNING, WAITING };
enum { NO_THREAD = 0, MAIN_THREAD = 1, FIRST_THREAD = 2 };
enum { NO_SEMAPHORE = 0, FIRST_SEMAPHORE = 1 };
enum { NO_HANDLER = 0, FIRST_HANDLER = 1, HANDLER_APPEND = -1 };
enum { PRIORITY_HIGHEST = 1, PRIORITY_LOWEST = 127 };
enum { INTC_HANDLERS, DMAC_HANDLERS, HANDLER_KINDS };
enum { INTERRUPT_CAUSES = 16, KERNEL_ERROR = -1, STACK_ALIGNMENT = 16 };

static struct {
    PortFiber context;
    void (*entry)(void *);
    void *argument;
    uint64_t order;
    int state;
    int priority;
    int semaphore;
} threads[THREAD_LIMIT];
static unsigned char stacks[THREAD_LIMIT][STACK_SIZE] __attribute__((aligned(STACK_ALIGNMENT)));
static struct { int used; int count; int maximum; } semaphores[SEMA_LIMIT];
static struct { int (*function)(int); int cause; int dma; int next; } handlers[HANDLER_LIMIT];
static int handler_heads[HANDLER_KINDS][INTERRUPT_CAUSES];
static unsigned int intc_enabled;
static unsigned int dmac_enabled;
static uint64_t queue_order;
static int running;
static int interrupt_active;
static PortFiber host_context;
static int (*game_entry)(void);
static int game_result;
static int host_requested;

extern void port_graphics_tick(void);
extern void port_graphics_wait(void);


static void ready(int id) {
    threads[id].state = READY;
    threads[id].order = ++queue_order;
}

static int highest(void) {
    int selected = NO_THREAD;
    for (int i = MAIN_THREAD; i < THREAD_LIMIT; ++i) {
        if (threads[i].state != READY) continue;
        if (!selected || threads[i].priority < threads[selected].priority ||
            (threads[i].priority == threads[selected].priority && threads[i].order < threads[selected].order))
            selected = i;
    }
    return selected;
}

static void schedule(void) {
    int previous = running;
    int next = highest();
    while (!next) {
        port_graphics_tick();
        next = highest();
        if (!next) {
            port_graphics_wait();
            next = highest();
        }
    }
    running = next;
    threads[next].state = RUNNING;
    if (next != previous) port_fiber_switch(&threads[previous].context, &threads[next].context);
}

static void preempt(void) {
    int next = highest();
    if (running && threads[running].state == RUNNING && !interrupt_active && next &&
        threads[next].priority < threads[running].priority) {
        ready(running);
        schedule();
    }
}

void port_kernel_yield(void) {
    if (!running || threads[running].state != RUNNING || interrupt_active) return;
    ready(running);
    schedule();
}

static void thread_entry(void) {
    int id = running;
    threads[id].entry(threads[id].argument);
    threads[id].state = DORMANT;
    schedule();
    abort();
}

int CreateThread(struct ThreadParam *param) {
    if (!param || !param->entry || param->initPriority < PRIORITY_HIGHEST || param->initPriority > PRIORITY_LOWEST) return KERNEL_ERROR;
    for (int i = FIRST_THREAD; i < THREAD_LIMIT; ++i) {
        if (threads[i].state != UNUSED) continue;
        threads[i].entry = param->entry;
        threads[i].priority = param->initPriority;
        threads[i].state = DORMANT;
        return i;
    }
    return KERNEL_ERROR;
}

int StartThread(int id, void *argument) {
    if (id < FIRST_THREAD || id >= THREAD_LIMIT || threads[id].state != DORMANT) return KERNEL_ERROR;
    threads[id].argument = argument;
    port_fiber_make(&threads[id].context, stacks[id], STACK_SIZE, thread_entry);
    ready(id);
    preempt();
    return id;
}

int RotateThreadReadyQueue(int priority) {
    port_graphics_tick();
    if (running && threads[running].priority == priority) {
        ready(running);
        schedule();
    } else {
        int first = NO_THREAD;
        for (int i = MAIN_THREAD; i < THREAD_LIMIT; ++i)
            if (threads[i].state == READY && threads[i].priority == priority &&
                (!first || threads[i].order < threads[first].order)) first = i;
        if (first) ready(first);
    }
    return 0;
}

int GetThreadId(void) {
    return running;
}

int ChangeThreadPriority(int id, int priority) {
    if (id < MAIN_THREAD || id >= THREAD_LIMIT || threads[id].state == UNUSED || priority < PRIORITY_HIGHEST || priority > PRIORITY_LOWEST) return KERNEL_ERROR;
    threads[id].priority = priority;
    preempt();
    return 0;
}

int TerminateThread(int id) {
    if (id < FIRST_THREAD || id >= THREAD_LIMIT || id == running || threads[id].state == UNUSED) return KERNEL_ERROR;
    threads[id].state = DORMANT;
    threads[id].semaphore = NO_SEMAPHORE;
    return 0;
}

int DeleteThread(int id) {
    if (id < FIRST_THREAD || id >= THREAD_LIMIT || threads[id].state != DORMANT) return KERNEL_ERROR;
    memset(&threads[id], 0, sizeof threads[id]);
    return 0;
}

int CreateSema(struct SemaParam *param) {
    if (!param || param->maxCount < 1 || param->initCount < 0 || param->initCount > param->maxCount) return KERNEL_ERROR;
    for (int i = FIRST_SEMAPHORE; i < SEMA_LIMIT; ++i) {
        if (semaphores[i].used) continue;
        semaphores[i].used = 1;
        semaphores[i].count = param->initCount;
        semaphores[i].maximum = param->maxCount;
        return i;
    }
    return KERNEL_ERROR;
}

int DeleteSema(int id) {
    if (id < FIRST_SEMAPHORE || id >= SEMA_LIMIT || !semaphores[id].used) return KERNEL_ERROR;
    for (int i = MAIN_THREAD; i < THREAD_LIMIT; ++i)
        if (threads[i].state == WAITING && threads[i].semaphore == id) return KERNEL_ERROR;
    semaphores[id].used = 0;
    return 0;
}

int WaitSema(int id) {
    if (id < FIRST_SEMAPHORE || id >= SEMA_LIMIT || !semaphores[id].used || !running || interrupt_active) return KERNEL_ERROR;
    if (semaphores[id].count) {
        --semaphores[id].count;
        return id;
    }
    threads[running].state = WAITING;
    threads[running].semaphore = id;
    threads[running].order = ++queue_order;
    schedule();
    return id;
}

int SignalSema(int id) {
    if (id < FIRST_SEMAPHORE || id >= SEMA_LIMIT || !semaphores[id].used) return KERNEL_ERROR;
    int waiter = NO_THREAD;
    for (int i = MAIN_THREAD; i < THREAD_LIMIT; ++i)
        if (threads[i].state == WAITING && threads[i].semaphore == id &&
            (!waiter || threads[i].order < threads[waiter].order)) waiter = i;
    if (waiter) {
        threads[waiter].semaphore = NO_SEMAPHORE;
        ready(waiter);
        preempt();
    } else {
        if (semaphores[id].count == semaphores[id].maximum) return KERNEL_ERROR;
        ++semaphores[id].count;
    }
    return id;
}

static int add_handler(int cause, int (*function)(int), int next, int dma) {
    if (cause < 0 || cause >= INTERRUPT_CAUSES || !function) return KERNEL_ERROR;
    if (next > NO_HANDLER && (next >= HANDLER_LIMIT || !handlers[next].function ||
                              handlers[next].cause != cause || handlers[next].dma != dma)) return KERNEL_ERROR;
    if (next < HANDLER_APPEND) return KERNEL_ERROR;
    for (int i = FIRST_HANDLER; i < HANDLER_LIMIT; ++i) {
        if (handlers[i].function) continue;
        handlers[i].function = function;
        handlers[i].cause = cause;
        handlers[i].dma = dma;
        int *link = &handler_heads[dma][cause];
        if (next == HANDLER_APPEND) {
            while (*link) link = &handlers[*link].next;
        } else if (next > NO_HANDLER) {
            link = &handlers[next].next;
        }
        handlers[i].next = *link;
        *link = i;
        return i;
    }
    return KERNEL_ERROR;
}

static int remove_handler(int cause, int id, int dma) {
    if (id < FIRST_HANDLER || id >= HANDLER_LIMIT || !handlers[id].function ||
        handlers[id].cause != cause || handlers[id].dma != dma) return KERNEL_ERROR;
    int *link = &handler_heads[dma][cause];
    while (*link != id) link = &handlers[*link].next;
    *link = handlers[id].next;
    handlers[id].function = NULL;
    return 0;
}

int AddIntcHandler(int cause, int (*function)(int), int next) {
    return add_handler(cause, function, next, INTC_HANDLERS);
}

int AddDmacHandler(int cause, int (*function)(int), int next) {
    return add_handler(cause, function, next, DMAC_HANDLERS);
}

int RemoveIntcHandler(int cause, int id) {
    return remove_handler(cause, id, INTC_HANDLERS);
}

int RemoveDmacHandler(int cause, int id) {
    return remove_handler(cause, id, DMAC_HANDLERS);
}

int EnableIntc(int cause) {
    if (cause < 0 || cause >= INTERRUPT_CAUSES) return KERNEL_ERROR;
    int previous = !!(intc_enabled & (1u << cause));
    intc_enabled |= 1u << cause;
    return previous;
}

int EnableDmac(int cause) {
    if (cause < 0 || cause >= INTERRUPT_CAUSES) return KERNEL_ERROR;
    int previous = !!(dmac_enabled & (1u << cause));
    dmac_enabled |= 1u << cause;
    return previous;
}

int DisableDmac(int cause) {
    if (cause < 0 || cause >= INTERRUPT_CAUSES) return KERNEL_ERROR;
    int previous = !!(dmac_enabled & (1u << cause));
    dmac_enabled &= ~(1u << cause);
    return previous;
}

void port_kernel_vblank(void) {
    ++interrupt_active;
    for (int dma = INTC_HANDLERS; dma < HANDLER_KINDS; ++dma) {
        unsigned int mask = dma == DMAC_HANDLERS ? dmac_enabled : intc_enabled;
        for (int cause = 0; cause < INTERRUPT_CAUSES; ++cause) {
            if (!(mask & (1u << cause)) || (dma == INTC_HANDLERS && cause != INTC_VBLANK_S && cause != INTC_VBLANK_E)) continue;
            for (int i = handler_heads[dma][cause]; i;) {
                int next = handlers[i].next;
                int result = handlers[i].function(cause);
                if (result < 0) break;
                i = next;
            }
        }
    }
    --interrupt_active;
    preempt();
}

int DIntr(void) { return 1; }
int EIntr(void) { return 1; }
void FlushCache(int operation) { (void)operation; }
void Exit__2(int status) { exit(status); }

static void main_entry(void) {
    game_result = game_entry();
    threads[MAIN_THREAD].state = DORMANT;
    running = NO_THREAD;
    port_fiber_switch(&threads[MAIN_THREAD].context, &host_context);
    abort();
}

int port_kernel_run(int (*entry)(void)) {
    game_entry = entry;
    running = MAIN_THREAD;
    threads[MAIN_THREAD].priority = PRIORITY_HIGHEST;
    threads[MAIN_THREAD].state = RUNNING;
    port_fiber_make(&threads[MAIN_THREAD].context, stacks[MAIN_THREAD], STACK_SIZE, main_entry);
    port_fiber_switch(&host_context, &threads[MAIN_THREAD].context);
    while (host_requested) {
        host_requested = 0;
        port_state_host();
        port_mods_host();
        port_fiber_switch(&host_context, &threads[running].context);
    }
    return game_result;
}

size_t port_kernel_returns(uint64_t *slots, size_t limit) {
    size_t count = 0;
    for (int id = 0; id < THREAD_LIMIT; ++id) {
        if (threads[id].state == UNUSED || threads[id].state == DORMANT) continue;
        uintptr_t low = (uintptr_t)stacks[id];
        uintptr_t top = low + STACK_SIZE;
        uintptr_t frame = port_fiber_frame(&threads[id].context);
        if (frame == UINTPTR_MAX) return limit + 1;
        while (frame >= low && frame + 16 <= top) {
            if (count == limit) return limit + 1;
            slots[count++] = frame + 8;
            uintptr_t next = *(const uintptr_t *)frame;
            if (next <= frame) break;
            frame = next;
        }
    }
    return count;
}

void port_kernel_stacks(uint64_t *low, uint64_t *high) {
    *low = (uintptr_t)stacks;
    *high = *low + sizeof stacks;
}

void port_kernel_host(void) {
    host_requested = 1;
    port_fiber_switch(&threads[running].context, &host_context);
}
