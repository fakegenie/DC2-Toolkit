#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <malloc.h>
static unsigned int data = 7;
static unsigned int zero;
static void *allocate_thread(void *argument) {
    uintptr_t seed = (uintptr_t)argument;
    for (size_t i = 0; i < 1000; i++) {
        size_t size = 1 + (i * 19 + seed) % 4096;
        unsigned char *data = malloc(size);
        assert(data);
        memset(data, (int)seed, size);
        unsigned char *grown = realloc(data, size + 1024);
        assert(grown && grown[size - 1] == seed);
        free(grown);
    }
    return NULL;
}

int main(void) {
    *(volatile unsigned int *)((uintptr_t)&data | 0x20000000) = 19;
    assert(data == 19);
    *(volatile unsigned int *)((uintptr_t)&zero | 0x30000000) = 23;
    assert(zero == 23);
    *(volatile int *)0x70000000 = 11;
    assert(*(volatile int *)0x70000000 == 11);
    unsigned char *p = malloc(1000);
    assert(p && (uintptr_t)p < 0x10000000);
    assert(malloc_usable_size(p) >= 1000);
    assert(malloc_usable_size(NULL) == 0);
    memset(p, 71, 1000);
    assert(*(volatile unsigned char *)((uintptr_t)p | 0x20000000) == 71);
    unsigned char *q = realloc(p, 2000);
    assert(q && q[999] == 71);
    free(q);
    for (size_t alignment = 16; alignment <= 65536; alignment *= 2) {
        void *a = aligned_alloc(alignment, alignment * 2);
        assert(a && !((uintptr_t)a & (alignment - 1)));
        memset(a, 31, alignment * 2);
        free(a);
    }
    unsigned char *c = calloc(100, 100);
    assert(c && c[9999] == 0);
    free(c);
    assert(!calloc((size_t)-1, 2));
    pthread_t workers[8];
    for (uintptr_t i = 0; i < 8; i++) assert(!pthread_create(workers + i, NULL, allocate_thread, (void *)(i + 1)));
    for (int i = 0; i < 8; i++) assert(!pthread_join(workers[i], NULL));
    puts("memory checks passed");
}
