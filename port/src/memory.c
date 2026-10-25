#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "port_arena.h"
#include "port_stats.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <TargetConditionals.h>
#endif

enum { ARENA_SIZE = 64 * 1024 * 1024 };

struct Allocation {
    size_t size;
    struct Allocation *next;
    struct Allocation *previous;
    int available;
    int reserved;
};

struct FreeLinks {
    struct Allocation *next;
    struct Allocation *previous;
};

static unsigned char arena[ARENA_SIZE] __attribute__((aligned(64)));
static struct Allocation *allocations;
static struct Allocation *free_head;
static atomic_flag allocation_lock = ATOMIC_FLAG_INIT;

static void lock_allocations(void) {
    while (atomic_flag_test_and_set_explicit(&allocation_lock, memory_order_acquire)) {}
}

static void unlock_allocations(void) {
    atomic_flag_clear_explicit(&allocation_lock, memory_order_release);
}

static struct FreeLinks *free_links(struct Allocation *block) {
    return (struct FreeLinks *)(block + 1);
}

static void push_free(struct Allocation *block) {
    struct FreeLinks *links = free_links(block);
    links->next = free_head;
    links->previous = NULL;
    if (free_head) free_links(free_head)->previous = block;
    free_head = block;
}

static void unlink_free(struct Allocation *block) {
    struct FreeLinks *links = free_links(block);
    if (links->previous) free_links(links->previous)->next = links->next;
    else free_head = links->next;
    if (links->next) free_links(links->next)->previous = links->previous;
}

static void *allocate_low(size_t size, size_t alignment) {
    void *result = NULL;
    lock_allocations();
    if (size > SIZE_MAX - alignment - sizeof(struct Allocation)) goto done;
    if (!allocations) {
        allocations = (struct Allocation *)arena;
        allocations->size = ARENA_SIZE - sizeof *allocations;
        allocations->available = 1;
        push_free(allocations);
    }
    size = (size + 15) & ~(size_t)15;
    if (!size) size = 16;
    for (struct Allocation *block = free_head; block; block = free_links(block)->next) {
        uintptr_t payload = (uintptr_t)(block + 1);
        uintptr_t aligned = (payload + alignment - 1) & ~(alignment - 1);
        size_t prefix = aligned - payload;
        if (prefix && prefix < sizeof *block + 16) {
            aligned += alignment;
            prefix += alignment;
        }
        if (prefix > block->size || size > block->size - prefix) continue;
        if (prefix) {
            struct Allocation *split = (struct Allocation *)(aligned - sizeof *block);
            split->size = block->size - prefix;
            split->next = block->next;
            split->previous = block;
            split->available = 1;
            if (split->next) split->next->previous = split;
            block->size = prefix - sizeof *block;
            block->next = split;
            block = split;
        } else {
            unlink_free(block);
        }
        if (block->size - size >= sizeof *block + 16) {
            struct Allocation *tail = (struct Allocation *)((unsigned char *)(block + 1) + size);
            tail->size = block->size - size - sizeof *block;
            tail->next = block->next;
            tail->previous = block;
            tail->available = 1;
            if (tail->next) tail->next->previous = tail;
            block->next = tail;
            block->size = size;
            push_free(tail);
        }
        block->available = 0;
        result = block + 1;
        port_stats.allocations++;
        port_stats.allocation_bytes += (uint32_t)block->size;
        port_stats.arena_used += block->size + sizeof *block;
        port_stats.arena_blocks++;
        if (port_stats.arena_used > port_stats.arena_peak) port_stats.arena_peak = port_stats.arena_used;
        goto done;
    }
done:
    unlock_allocations();
    return result;
}

void *port_allocate(size_t size, size_t alignment) {
    void *result = allocate_low(size, alignment < 16 ? 16 : alignment);
    if (result) return result;
    fprintf(stderr, "port: the engine's %d MB heap has no room for %zu bytes\n", ARENA_SIZE >> 20, size);
    _exit(1);
}

void port_release(void *pointer) {
    if (!pointer) return;
    lock_allocations();
    struct Allocation *block = (struct Allocation *)pointer - 1;
    block->available = 1;
    port_stats.frees++;
    port_stats.arena_used -= block->size + sizeof *block;
    port_stats.arena_blocks--;
    if (block->next && block->next->available) {
        unlink_free(block->next);
        block->size += sizeof *block + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->previous = block;
    }
    if (block->previous && block->previous->available) {
        struct Allocation *previous = block->previous;
        previous->size += sizeof *block + block->size;
        previous->next = block->next;
        if (previous->next) previous->next->previous = previous;
    } else {
        push_free(block);
    }
    unlock_allocations();
}

size_t port_arena_spans(const unsigned char *saved, size_t size, uint64_t saved_at, PortArenaSpan *spans, size_t limit) {
    size_t count = 0;
    uint64_t at = saved_at;
    if (size > ARENA_SIZE) size = ARENA_SIZE;
    while (at - saved_at + sizeof(struct Allocation) <= size && count < limit) {
        struct Allocation block;
        memcpy(&block, saved + (at - saved_at), sizeof block);
        spans[count].start = at;
        spans[count].live_end = block.available ? at : at + sizeof block + block.size;
        count++;
        if (!block.next) break;
        at = (uint64_t)(uintptr_t)block.next;
    }
    return count;
}

void *port_new(size_t size) {
    return port_allocate(size, 16);
}

void *port_new_aligned(size_t size, size_t alignment) {
    return port_allocate(size, alignment);
}

void port_delete(void *pointer) {
    port_release(pointer);
}

#ifdef __APPLE__
static void disable_address_randomisation(void) {
    if (!TARGET_OS_OSX || !_dyld_get_image_vmaddr_slide(0) || getenv("DC2TK_ASLR")) return;
    char path[4096];
    uint32_t length = sizeof path;
    posix_spawnattr_t attributes;
    short flags = POSIX_SPAWN_SETEXEC | 0x100;
    pid_t pid;
    if (_NSGetExecutablePath(path, &length) || posix_spawnattr_init(&attributes) ||
        posix_spawnattr_setflags(&attributes, flags) ||
        posix_spawn(&pid, path, NULL, &attributes, *_NSGetArgv(), *_NSGetEnviron())) {
        perror("port: restarting without address randomisation");
        _exit(1);
    }
}

__attribute__((constructor(101))) static void port_memory_init(void) {
    disable_address_randomisation();
}
#endif
