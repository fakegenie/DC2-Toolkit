#pragma once
#include <stdint.h>

typedef struct {
    float x, y, s, t, q;
    double z;
    uint32_t rgba;
    unsigned u, v, fog;
} PortGsVertex;

typedef struct {
    uint64_t regs[128];
    PortGsVertex vertices[3];
    unsigned prim;
    unsigned count;
} PortGsDraw;

typedef struct {
    uint64_t draws;
    uint64_t queued;
    uint64_t serial_aliases;
    uint64_t flushes;
    uint64_t largest_batch;
    uint64_t parallel_batches;
} PortGsRasterStats;

int port_gs_raster(uint64_t *regs, unsigned prim, const PortGsVertex *v, unsigned count);
void port_gs_raster_threads(unsigned count);
uint64_t port_gs_raster_parallel_calls(void);
int port_gs_raster_enqueue(const uint64_t *regs, unsigned prim, const PortGsVertex *vertices, unsigned count);
int port_gs_raster_flush(void);
void port_gs_raster_reset(void);
int port_gs_raster_batching(unsigned enabled);
void port_gs_raster_stats(PortGsRasterStats *stats);
