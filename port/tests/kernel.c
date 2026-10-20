#define _XOPEN_SOURCE 700
#include <assert.h>
#include <stdint.h>
#include "../src/kernel.c"
static int trace[32];
static int position;
static int semaphore;
static int worker_id;
static int signal_from_tick;
void port_graphics_tick(void) {
    if (signal_from_tick) {
        signal_from_tick = 0;
        port_kernel_vblank();
    }
}
static void worker(void *argument) {
    assert((uintptr_t)&argument < 0x10000000);
    assert(argument == &trace);
    trace[position++] = 1;
    assert(WaitSema(semaphore) == semaphore);
    trace[position++] = 3;
}
static void second(void *argument) {
    (void)argument;
    trace[position++] = 5;
    RotateThreadReadyQueue(10);
    trace[position++] = 7;
}
static int interrupt_first(int cause) { assert(cause == 2); trace[position++] = 8; return 0; }
static int interrupt_last(int cause) { assert(cause == 2); trace[position++] = 9; return 0; }
static int interrupt_signal(int cause) { assert(cause == 2); return SignalSema(semaphore); }
static int game(void) {
    assert(GetThreadId() == 1);
    assert(ChangeThreadPriority(1, 10) == 0);
    struct SemaParam sema = {.maxCount = 1, .initCount = 0};
    semaphore = CreateSema(&sema);
    assert(semaphore > 0);
    struct ThreadParam param = {.entry = worker, .initPriority = 5};
    worker_id = CreateThread(&param);
    assert(StartThread(worker_id, &trace) == worker_id);
    trace[position++] = 2;
    assert(DeleteSema(semaphore) == -1);
    assert(SignalSema(semaphore) == semaphore);
    trace[position++] = 4;
    assert(DeleteThread(worker_id) == 0);
    assert(DeleteSema(semaphore) == 0);
    param.entry = second;
    param.initPriority = 10;
    int other = CreateThread(&param);
    assert(StartThread(other, NULL) == other);
    assert(position == 4);
    RotateThreadReadyQueue(10);
    trace[position++] = 6;
    RotateThreadReadyQueue(10);
    assert(DeleteThread(other) == 0);
    int last = AddIntcHandler(2, interrupt_last, -1);
    int first = AddIntcHandler(2, interrupt_first, 0);
    assert(EnableIntc(2) == 0);
    port_kernel_vblank();
    assert(RemoveIntcHandler(2, first) == 0);
    assert(RemoveIntcHandler(2, last) == 0);
    assert(position == 9);
    for (int i = 0; i < 9; ++i) assert(trace[i] == i + 1);
    semaphore = CreateSema(&sema);
    int signal_handler = AddIntcHandler(2, interrupt_signal, 0);
    signal_from_tick = 1;
    assert(WaitSema(semaphore) == semaphore);
    assert(!signal_from_tick && GetThreadId() == 1);
    assert(RemoveIntcHandler(2, signal_handler) == 0);
    assert(DeleteSema(semaphore) == 0);
    return 43;
}
int main(void) { assert(port_kernel_run(game) == 43); puts("kernel checks passed"); }
