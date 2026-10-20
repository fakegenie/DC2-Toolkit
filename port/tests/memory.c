#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "port_window.h"

void *port_new(size_t size);
void *port_new_aligned(size_t size, size_t alignment);
void port_delete(void *pointer);

enum { HEAP = 64 * 1024 * 1024 };

static unsigned int data = 7;
static unsigned int zero;

static int in_ram(void *pointer) {
    return (uintptr_t)pointer >= PORT_WINDOW && (uintptr_t)pointer - PORT_WINDOW < 0x10000000;
}

static void *allocate_thread(void *argument) {
    uintptr_t seed = (uintptr_t)argument;
    for (size_t i = 0; i < 1000; i++) {
        size_t size = 1 + (i * 19 + seed) % 4096;
        unsigned char *block = port_allocate(size, 16);
        memset(block, (int)seed, size);
        assert(block[size - 1] == seed && !((uintptr_t)block & 15));
        port_release(block);
    }
    return NULL;
}

int main(void) {
    *(volatile unsigned int *)((uintptr_t)&data | 0x20000000) = 19;
    assert(data == 19);
    *(volatile unsigned int *)((uintptr_t)&zero | 0x30000000) = 23;
    assert(zero == 23);
    *(volatile int *)(PORT_WINDOW + 0x70000000) = 11;
    assert(*(volatile int *)(PORT_WINDOW + 0x70000000) == 11);
    unsigned char *p = port_allocate(1000, 16);
    assert(in_ram(p));
    memset(p, 71, 1000);
    assert(*(volatile unsigned char *)((uintptr_t)p | 0x20000000) == 71);
    port_release(p);
    port_release(NULL);
    for (size_t alignment = 16; alignment <= 65536; alignment *= 2) {
        void *a = port_allocate(alignment * 2, alignment);
        assert(in_ram(a) && !((uintptr_t)a & (alignment - 1)));
        memset(a, 31, alignment * 2);
        port_release(a);
    }
    unsigned char *empty = port_allocate(0, 16);
    assert(in_ram(empty));
    port_release(empty);
    unsigned char *first = port_new(100);
    unsigned char *second = port_new_aligned(100, 256);
    assert(in_ram(first) && in_ram(second) && !((uintptr_t)second & 255) && first != second);
    port_delete(first);
    port_delete(second);
    void *pieces[64];
    for (int i = 0; i < 64; i++) pieces[i] = port_allocate(1000 + i * 37, 16);
    for (int i = 0; i < 64; i += 2) port_release(pieces[i]);
    for (int i = 1; i < 64; i += 2) port_release(pieces[i]);
    void *large = port_allocate(HEAP - 4096, 16);
    assert(in_ram(large));
    port_release(large);
    pthread_t workers[8];
    for (uintptr_t i = 0; i < 8; i++) assert(!pthread_create(workers + i, NULL, allocate_thread, (void *)(i + 1)));
    for (int i = 0; i < 8; i++) assert(!pthread_join(workers[i], NULL));
    puts("memory checks passed");
}
