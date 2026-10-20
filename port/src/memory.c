#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "port_stats.h"
#include "port_window.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <spawn.h>
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

void *port_new(size_t size) {
    return port_allocate(size, 16);
}

void *port_new_aligned(size_t size, size_t alignment) {
    return port_allocate(size, alignment);
}

void port_delete(void *pointer) {
    port_release(pointer);
}

static void memory_error(const char *operation) {
    perror(operation);
    _exit(1);
}

#ifdef __APPLE__
__attribute__((used, section("__PS2,__window,zerofill"))) static unsigned char window[0x30000000];
__attribute__((used, section("__PS2SPR,__scratchpad,zerofill"))) static unsigned char scratchpad[16384];

static void disable_address_randomisation(void) {
    if (!_dyld_get_image_vmaddr_slide(0)) return;
    char path[4096];
    uint32_t length = sizeof path;
    posix_spawnattr_t attributes;
    short flags = POSIX_SPAWN_SETEXEC | 0x100;
    pid_t pid;
    if (_NSGetExecutablePath(path, &length) || posix_spawnattr_init(&attributes) ||
        posix_spawnattr_setflags(&attributes, flags) ||
        posix_spawn(&pid, path, NULL, &attributes, *_NSGetArgv(), *_NSGetEnviron()))
        memory_error("port: restarting without address randomisation");
}

static void mirror(uintptr_t start, size_t size, uintptr_t offset, const char *operation) {
    mach_vm_address_t target = start + offset;
    vm_prot_t current, maximum;
    if (mach_vm_remap(mach_task_self(), &target, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), start, FALSE,
                      &current, &maximum, VM_INHERIT_NONE) != KERN_SUCCESS || target != start + offset) {
        fprintf(stderr, "%s failed\n", operation);
        _exit(1);
    }
}

__attribute__((constructor(101))) static void port_memory_init(void) {
    disable_address_randomisation();
    unsigned long size;
    uintptr_t start = (uintptr_t)getsegmentdata(&_mh_execute_header, "__DATA", &size);
    if (!start || start < PORT_WINDOW || start + size - PORT_WINDOW >= 0x10000000 ||
        (uintptr_t)window != PORT_WINDOW + 0x10000000 || (uintptr_t)scratchpad != PORT_WINDOW + 0x70000000) {
        fputs("port: linked data lies outside the PS2 address range\n", stderr);
        _exit(1);
    }
    mirror(start, size, 0x20000000, "port: uncached RAM mapping");
    mirror(start, size, 0x30000000, "port: DMA RAM mapping");
}
#else
extern char __data_start[];
extern char _end[];

__attribute__((constructor(101))) static void port_memory_init(void) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) memory_error("port: page size");
    uintptr_t start = (uintptr_t)__data_start & ~((uintptr_t)page - 1);
    uintptr_t end = ((uintptr_t)_end + page - 1) & ~((uintptr_t)page - 1);
    size_t size = end - start;
    if (end >= 0x10000000) {
        fputs("port: linked data lies outside the PS2 address range\n", stderr);
        _exit(1);
    }
    int fd = memfd_create("darkchronicle-ram", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) memory_error("port: RAM backing");
    void *copy = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (copy == MAP_FAILED) memory_error("port: RAM copy");
    memcpy(copy, (void *)start, size);
    if (mmap((void *)start, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
        memory_error("port: RAM mapping");
    if (mmap((void *)(start + 0x20000000), size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) == MAP_FAILED)
        memory_error("port: uncached RAM mapping");
    if (mmap((void *)(start + 0x30000000), size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) == MAP_FAILED)
        memory_error("port: DMA RAM mapping");
    munmap(copy, size);
    close(fd);
    if (mmap((void *)0x70000000, 16384, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
        memory_error("port: scratchpad mapping");
    if (mmap((void *)0x10000000, 65536, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
        memory_error("port: EE register mapping");
    if (mmap((void *)0x12000000, 8192, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
        memory_error("port: GS register mapping");
}
#endif
