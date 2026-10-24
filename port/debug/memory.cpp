#include "debug.h"
#include "port_memory.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>

enum { SITE_LIMIT = 1024, POOL_LIMIT = 16384 };

typedef struct {
    const void *site;
    int size;
    int peak;
    int peak_size;
    int overflows;
    int worst;
    int worst_size;
} Site;

typedef struct {
    const void *pool;
    int site;
} Pool;

static Site sites[SITE_LIMIT];
static int site_count = 1;
static Pool pools[POOL_LIMIT];

static Pool *pool_of(const void *pool) {
    uintptr_t hash = ((uintptr_t)pool >> 4) * UINT64_C(0x9E3779B97F4A7C15);
    for (int probe = 0; probe < POOL_LIMIT; ++probe) {
        Pool *slot = &pools[(hash + probe) % POOL_LIMIT];
        if (slot->pool == pool || !slot->pool) return slot;
    }
    return NULL;
}

static int site_index(const void *site) {
    for (int i = 1; i < site_count; ++i)
        if (sites[i].site == site) return i;
    if (site_count == SITE_LIMIT) return 0;
    sites[site_count].site = site;
    return site_count++;
}

static void site_name(const void *site, char *text, size_t size) {
    Dl_info info;
    if (!site || !dladdr(site, &info) || !info.dli_sname) {
        snprintf(text, size, "%p", site);
        return;
    }
    int status = 0;
    char *readable = abi::__cxa_demangle(info.dli_sname, NULL, NULL, &status);
    snprintf(text, size, "%s+0x%lx", status == 0 ? readable : info.dli_sname,
             (unsigned long)((const char *)site - (const char *)info.dli_saddr));
    free(readable);
}

extern "C" void port_memory_set(const void *pool, const void *site, int size) {
    Pool *slot = pool_of(pool);
    if (!slot) return;
    slot->pool = pool;
    slot->site = site_index(site);
    if (size > sites[slot->site].size) sites[slot->site].size = size;
}

extern "C" void port_memory_use(const void *pool, int used, int size) {
    if (size <= 0) return;
    Pool *slot = pool_of(pool);
    Site *site = &sites[slot && slot->pool ? slot->site : 0];
    if (!site->peak_size || (int64_t)used * site->peak_size > (int64_t)site->peak * size) {
        site->peak = used;
        site->peak_size = size;
    }
    if (used < size) return;
    if (!site->overflows++) {
        char name[256];
        site_name(site->site, name, sizeof name);
        fprintf(stderr, "memory over %d/%d in the pool set up at %s\n", used, size, name);
    }
    if (!site->worst_size || (int64_t)used * site->worst_size > (int64_t)site->worst * size) {
        site->worst = used;
        site->worst_size = size;
    }
}

static int fuller(const void *a, const void *b) {
    const Site *left = &sites[*(const int *)a];
    const Site *right = &sites[*(const int *)b];
    double left_full = left->peak_size ? (double)left->peak / left->peak_size : 0.0;
    double right_full = right->peak_size ? (double)right->peak / right->peak_size : 0.0;
    return (right_full > left_full) - (right_full < left_full);
}

static int command_memory(const DebugArgs *args, DebugReply *reply) {
    double least = debug_arg_number(args, "full", 0.75);
    int order[SITE_LIMIT];
    for (int i = 0; i < site_count; ++i) order[i] = i;
    qsort(order, (size_t)site_count, sizeof order[0], fuller);
    debug_print(reply, "{\"sites\": %d, \"pools\": [", site_count - 1);
    int shown = 0;
    for (int i = 0; i < site_count; ++i) {
        Site *site = &sites[order[i]];
        if (!site->peak_size || ((double)site->peak / site->peak_size < least && !site->overflows)) continue;
        Dl_info info;
        char name[256];
        site_name(site->site, name, sizeof name);
        long offset = site->site && dladdr(site->site, &info) ? (long)((const char *)site->site - (const char *)info.dli_fbase) : 0;
        debug_print(reply, "%s{\"site\": ", shown++ ? ", " : "");
        debug_print_string(reply, order[i] ? name : "unknown");
        debug_print(reply, ", \"offset\": \"0x%lx\", \"size\": %d, \"peak\": %d, \"peak_size\": %d, \"full\": %.3f, "
                           "\"overflows\": %d, \"worst\": %d, \"worst_size\": %d}",
                    offset, site->size, site->peak, site->peak_size, (double)site->peak / site->peak_size, site->overflows,
                    site->worst, site->worst_size);
    }
    debug_print(reply, "]}");
    if (debug_arg_int(args, "reset", 0))
        for (int i = 0; i < site_count; ++i) sites[i] = (Site){sites[i].site, sites[i].size, 0, 0, 0, 0, 0};
    return 0;
}

extern const DebugCommand debug_memory_commands[] = {
    {"memory", command_memory, 1, "engine memory pools by how full they got: full=0.75 hides emptier ones, reset=1 "
                                  "clears the counts after replying"},
    {NULL, NULL, 0, NULL},
};
