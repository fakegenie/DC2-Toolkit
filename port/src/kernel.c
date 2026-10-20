#include "port_fiber.h"
#include "port_kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { THREAD_LIMIT = 32, SEMA_LIMIT = 64, HANDLER_LIMIT = 64, STACK_SIZE = 1024 * 1024 };
enum { UNUSED, DORMANT, READY, RUNNING, WAITING };

struct ThreadParam {
    int status;
    void (*entry)(void *);
    void *stack;
    int stackSize;
    void *gpReg;
    int initPriority;
    int currentPriority;
    unsigned int attr;
    unsigned int option;
};

struct SemaParam {
    int currentCount;
    int maxCount;
    int initCount;
    int numWaitThreads;
    unsigned int attr;
    unsigned int option;
};

static struct {
    PortFiber context;
    void (*entry)(void *);
    void *argument;
    uint64_t order;
    int state;
    int priority;
    int semaphore;
} threads[THREAD_LIMIT];
static unsigned char stacks[THREAD_LIMIT][STACK_SIZE] __attribute__((aligned(16)));
static struct { int used; int count; int maximum; } semaphores[SEMA_LIMIT];
static struct { int (*function)(int); int cause; int dma; int next; } handlers[HANDLER_LIMIT];
static int handler_heads[2][16];
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

__attribute__((weak)) void debug_host(void) {}

static void ready(int id) {
    threads[id].state = READY;
    threads[id].order = ++queue_order;
}

static int highest(void) {
    int selected = 0;
    for (int i = 1; i < THREAD_LIMIT; ++i) {
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
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
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
    if (!param || !param->entry || param->initPriority < 1 || param->initPriority > 127) return -1;
    for (int i = 2; i < THREAD_LIMIT; ++i) {
        if (threads[i].state != UNUSED) continue;
        threads[i].entry = param->entry;
        threads[i].priority = param->initPriority;
        threads[i].state = DORMANT;
        return i;
    }
    return -1;
}

int StartThread(int id, void *argument) {
    if (id < 2 || id >= THREAD_LIMIT || threads[id].state != DORMANT) return -1;
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
        int first = 0;
        for (int i = 1; i < THREAD_LIMIT; ++i)
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
    if (id < 1 || id >= THREAD_LIMIT || threads[id].state == UNUSED || priority < 1 || priority > 127) return -1;
    threads[id].priority = priority;
    preempt();
    return 0;
}

int TerminateThread(int id) {
    if (id < 2 || id >= THREAD_LIMIT || id == running || threads[id].state == UNUSED) return -1;
    threads[id].state = DORMANT;
    threads[id].semaphore = 0;
    return 0;
}

int DeleteThread(int id) {
    if (id < 2 || id >= THREAD_LIMIT || threads[id].state != DORMANT) return -1;
    memset(&threads[id], 0, sizeof threads[id]);
    return 0;
}

int CreateSema(struct SemaParam *param) {
    if (!param || param->maxCount < 1 || param->initCount < 0 || param->initCount > param->maxCount) return -1;
    for (int i = 1; i < SEMA_LIMIT; ++i) {
        if (semaphores[i].used) continue;
        semaphores[i].used = 1;
        semaphores[i].count = param->initCount;
        semaphores[i].maximum = param->maxCount;
        return i;
    }
    return -1;
}

int DeleteSema(int id) {
    if (id < 1 || id >= SEMA_LIMIT || !semaphores[id].used) return -1;
    for (int i = 1; i < THREAD_LIMIT; ++i)
        if (threads[i].state == WAITING && threads[i].semaphore == id) return -1;
    semaphores[id].used = 0;
    return 0;
}

int WaitSema(int id) {
    if (id < 1 || id >= SEMA_LIMIT || !semaphores[id].used || !running || interrupt_active) return -1;
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
    if (id < 1 || id >= SEMA_LIMIT || !semaphores[id].used) return -1;
    int waiter = 0;
    for (int i = 1; i < THREAD_LIMIT; ++i)
        if (threads[i].state == WAITING && threads[i].semaphore == id &&
            (!waiter || threads[i].order < threads[waiter].order)) waiter = i;
    if (waiter) {
        threads[waiter].semaphore = 0;
        ready(waiter);
        preempt();
    } else {
        if (semaphores[id].count == semaphores[id].maximum) return -1;
        ++semaphores[id].count;
    }
    return id;
}

static int add_handler(int cause, int (*function)(int), int next, int dma) {
    if (cause < 0 || cause > 15 || !function) return -1;
    if (next > 0 && (next >= HANDLER_LIMIT || !handlers[next].function ||
                     handlers[next].cause != cause || handlers[next].dma != dma)) return -1;
    if (next < -1) return -1;
    for (int i = 1; i < HANDLER_LIMIT; ++i) {
        if (handlers[i].function) continue;
        handlers[i].function = function;
        handlers[i].cause = cause;
        handlers[i].dma = dma;
        int *link = &handler_heads[dma][cause];
        if (next == -1) {
            while (*link) link = &handlers[*link].next;
        } else if (next > 0) {
            link = &handlers[next].next;
        }
        handlers[i].next = *link;
        *link = i;
        return i;
    }
    return -1;
}

static int remove_handler(int cause, int id, int dma) {
    if (id < 1 || id >= HANDLER_LIMIT || !handlers[id].function ||
        handlers[id].cause != cause || handlers[id].dma != dma) return -1;
    int *link = &handler_heads[dma][cause];
    while (*link != id) link = &handlers[*link].next;
    *link = handlers[id].next;
    handlers[id].function = NULL;
    return 0;
}

int AddIntcHandler(int cause, int (*function)(int), int next) {
    return add_handler(cause, function, next, 0);
}

int AddDmacHandler(int cause, int (*function)(int), int next) {
    return add_handler(cause, function, next, 1);
}

int RemoveIntcHandler(int cause, int id) {
    return remove_handler(cause, id, 0);
}

int RemoveDmacHandler(int cause, int id) {
    return remove_handler(cause, id, 1);
}

int EnableIntc(int cause) {
    if (cause < 0 || cause > 15) return -1;
    int previous = !!(intc_enabled & (1u << cause));
    intc_enabled |= 1u << cause;
    return previous;
}

int EnableDmac(int cause) {
    if (cause < 0 || cause > 15) return -1;
    int previous = !!(dmac_enabled & (1u << cause));
    dmac_enabled |= 1u << cause;
    return previous;
}

int DisableDmac(int cause) {
    if (cause < 0 || cause > 15) return -1;
    int previous = !!(dmac_enabled & (1u << cause));
    dmac_enabled &= ~(1u << cause);
    return previous;
}

void port_kernel_vblank(void) {
    ++interrupt_active;
    for (int dma = 0; dma < 2; ++dma) {
        unsigned int mask = dma ? dmac_enabled : intc_enabled;
        for (int cause = 0; cause < 16; ++cause) {
            if (!(mask & (1u << cause)) || (!dma && cause != 2 && cause != 3)) continue;
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
    threads[1].state = DORMANT;
    running = 0;
    port_fiber_switch(&threads[1].context, &host_context);
    abort();
}

int port_kernel_run(int (*entry)(void)) {
    game_entry = entry;
    running = 1;
    threads[1].priority = 1;
    threads[1].state = RUNNING;
    port_fiber_make(&threads[1].context, stacks[1], STACK_SIZE, main_entry);
    port_fiber_switch(&host_context, &threads[1].context);
    while (host_requested) {
        host_requested = 0;
        debug_host();
        port_fiber_switch(&host_context, &threads[running].context);
    }
    return game_result;
}

void port_kernel_host(void) {
    host_requested = 1;
    port_fiber_switch(&threads[running].context, &host_context);
}
