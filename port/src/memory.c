#define _GNU_SOURCE
#include <malloc.h>
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern char __data_start[];
extern char _end[];

struct Allocation {
    size_t size;
    struct Allocation *next;
    int available;
    int reserved[3];
};

static struct Allocation *allocations;
static atomic_flag allocation_lock = ATOMIC_FLAG_INIT;

static void lock_allocations(void) {
    while (atomic_flag_test_and_set_explicit(&allocation_lock, memory_order_acquire)) {}
}

static void unlock_allocations(void) {
    atomic_flag_clear_explicit(&allocation_lock, memory_order_release);
}

static void *allocate_low(size_t size, size_t alignment) {
    void *result = NULL;
    lock_allocations();
    if (size > SIZE_MAX - alignment - sizeof(struct Allocation)) {
        errno = ENOMEM;
        goto done;
    }
    if (!allocations) {
        size_t arena_size = 64 * 1024 * 1024;
        int fd = memfd_create("darkchronicle-heap", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, arena_size) < 0) {
            if (fd >= 0) close(fd);
            goto done;
        }
        void *arena = mmap((void *)0x08000000, arena_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (arena == MAP_FAILED) {
            close(fd);
            goto done;
        }
        void *uncached = mmap((void *)0x28000000, arena_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        void *dma = mmap((void *)0x38000000, arena_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        close(fd);
        if (uncached == MAP_FAILED || dma == MAP_FAILED) {
            munmap(arena, arena_size);
            if (uncached != MAP_FAILED) munmap(uncached, arena_size);
            if (dma != MAP_FAILED) munmap(dma, arena_size);
            errno = ENOMEM;
            goto done;
        }
        allocations = arena;
        allocations->size = arena_size - sizeof *allocations;
        allocations->available = 1;
    }
    size = (size + 15) & ~(size_t)15;
    if (!size) size = 16;
    for (struct Allocation *block = allocations; block; block = block->next) {
        if (!block->available) continue;
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
            split->available = 1;
            block->size = prefix - sizeof *block;
            block->next = split;
            block = split;
        }
        if (block->size - size >= sizeof *block + 16) {
            struct Allocation *tail = (struct Allocation *)((unsigned char *)(block + 1) + size);
            tail->size = block->size - size - sizeof *block;
            tail->next = block->next;
            tail->available = 1;
            block->next = tail;
            block->size = size;
        }
        block->available = 0;
        result = block + 1;
        goto done;
    }
    errno = ENOMEM;
done:
    unlock_allocations();
    return result;
}

void *malloc(size_t size) {
    return allocate_low(size, 16);
}

void free(void *pointer) {
    if (!pointer) return;
    lock_allocations();
    struct Allocation *block = (struct Allocation *)pointer - 1;
    block->available = 1;
    for (block = allocations; block && block->next;) {
        if (block->available && block->next->available) {
            block->size += sizeof *block + block->next->size;
            block->next = block->next->next;
        } else {
            block = block->next;
        }
    }
    unlock_allocations();
}

size_t malloc_usable_size(void *pointer) {
    if (!pointer) return 0;
    lock_allocations();
    size_t size = ((struct Allocation *)pointer - 1)->size;
    unlock_allocations();
    return size;
}

void *calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    void *pointer = allocate_low(count * size, 16);
    if (pointer) memset(pointer, 0, count * size);
    return pointer;
}

void *realloc(void *pointer, size_t size) {
    if (!pointer) return allocate_low(size, 16);
    if (!size) {
        free(pointer);
        return NULL;
    }
    struct Allocation *block = (struct Allocation *)pointer - 1;
    lock_allocations();
    size_t previous_size = block->size;
    unlock_allocations();
    if (size <= previous_size) return pointer;
    void *replacement = allocate_low(size, 16);
    if (replacement) {
        memcpy(replacement, pointer, previous_size);
        free(pointer);
    }
    return replacement;
}

int posix_memalign(void **pointer, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
    void *result = allocate_low(size, alignment < 16 ? 16 : alignment);
    if (!result) return ENOMEM;
    *pointer = result;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size) {
    if (!alignment || (alignment & (alignment - 1)) || size % alignment) {
        errno = EINVAL;
        return NULL;
    }
    return allocate_low(size, alignment < 16 ? 16 : alignment);
}

static void memory_error(const char *operation) {
    perror(operation);
    _exit(1);
}

__attribute__((constructor(101))) static void port_memory_init(void) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) memory_error("port: page size");
    uintptr_t start = (uintptr_t)__data_start & ~((uintptr_t)page - 1);
    uintptr_t end = ((uintptr_t)_end + page - 1) & ~((uintptr_t)page - 1);
    size_t size = end - start;
    if (end >= 0x10000000) {
        fputs("port: linked data exceeds the PS2 address range\n", stderr);
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
    mallopt(M_MMAP_MAX, 0);
}
