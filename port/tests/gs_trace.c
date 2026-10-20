#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "gs.h"
#include "gs_memory.h"
#include "gs_raster.h"
#include "port_graphics.h"

#define TRACE_MAXIMUM UINT64_C(8589934592)
#define PAYLOAD_LIMIT (16u * 1024u * 1024u)

enum { TRACE_RESET = 1, TRACE_FEED, TRACE_SUBMIT, TRACE_REGISTER, TRACE_LOAD, TRACE_STORE, TRACE_RGBA, TRACE_END };

struct trace_record {
    uint32_t operation;
    uint32_t size;
    uint64_t field;
    int32_t result;
    uint32_t reserved;
};

static const unsigned char trace_magic[16] = {'D','C','2','G','S','T','R',0,2,0,0,0,0,0,0,0};

static void trace_fail(const char *message) {
    fprintf(stderr, "GS trace: %s\n", message);
    exit(1);
}

static uint64_t trace_limit(void) {
    const char *setting = getenv("DC2TK_GS_TRACE_MAX_BYTES");
    if (!setting) return UINT64_C(1073741824);
    char *end;
    errno = 0;
    unsigned long long value = strtoull(setting, &end, 10);
    if (errno || end == setting || *end || value < 16 || value > TRACE_MAXIMUM)
        trace_fail("capture limit must be 16 through 8589934592 bytes");
    return value;
}

static uint64_t output_hash(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t store_size(const uint64_t *packet, size_t bytes) {
    unsigned psm = (unsigned)(packet[4] >> 24) & 63;
    unsigned bits = 0;
    switch (psm) {
        case 0: case 48: bits = 32; break;
        case 1: case 49: bits = 24; break;
        case 2: case 10: case 50: case 58: bits = 16; break;
        case 19: case 27: bits = 8; break;
        case 20: case 36: case 44: bits = 4; break;
    }
    uint64_t pixels = (packet[8] & 4095) * ((packet[8] >> 32) & 4095);
    uint64_t size = (pixels * bits + 7) / 8;
    if (!bits || size > bytes) trace_fail("invalid successful image store size");
    return (size_t)size;
}

static uint64_t rgba_hash(const uint32_t *pixels, unsigned width, unsigned height, size_t stride) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned row = 0; row < height; ++row)
        hash = output_hash(hash, pixels + row * stride, (size_t)width * sizeof(*pixels));
    return hash;
}

static uint64_t memory_hash(void) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < 4u * 1024u * 1024u; ++i) {
        hash ^= port_gs_memory_read_byte(i);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

#ifndef GS_TRACE_REPLAY
static FILE *trace_file;
static uint64_t trace_bytes;
static int trace_finished;
static uint64_t capture_limit;

static void trace_write(const void *data, size_t size) {
    if (size > capture_limit - trace_bytes) trace_fail("capture exceeds configured byte limit");
    if (fwrite(data, 1, size, trace_file) != size) trace_fail("cannot write capture");
    trace_bytes += size;
}

static void record(unsigned operation, int result, const void *prefix, size_t prefix_size,
                   const void *data, size_t size) {
    if (!trace_file || trace_finished) return;
    if (size > PAYLOAD_LIMIT || prefix_size > PAYLOAD_LIMIT - size) trace_fail("operation exceeds payload bound");
    struct trace_record header = {operation, (uint32_t)(prefix_size + size), port_graphics.fields, result, 0};
    trace_write(&header, sizeof(header));
    if (prefix_size) trace_write(prefix, prefix_size);
    if (size) trace_write(data, size);
}

static void trace_finish(void) {
    if (!trace_file || trace_finished) return;
    if (port_gs_raster_flush()) trace_fail("final raster flush failed");
    uint64_t hash = memory_hash();
    record(TRACE_END, 0, &hash, sizeof(hash), NULL, 0);
    trace_finished = 1;
    if (fclose(trace_file)) trace_fail("cannot close capture");
    trace_file = NULL;
    fprintf(stderr, "GS trace: wrote %" PRIu64 " bytes, field=%" PRIu64 " VRAM=%016" PRIx64 "\n",
            trace_bytes, port_graphics.fields, hash);
}

void __real_port_gs_reset(void);
int __real_port_gif_feed(const void *data, size_t bytes, unsigned path);
int __real_port_gif_submit(const void *data, size_t bytes);
int __real_port_gs_write_register(unsigned address, uint64_t value);
int __real_port_gs_image_load(const uint64_t *packet, const void *source, size_t bytes);
int __real_port_gs_image_store(const uint64_t *packet, void *destination, size_t bytes);
int __real_port_gs_read_rgba(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y,
                            unsigned width, unsigned height, uint32_t *rgba, size_t stride_pixels);

void __wrap_port_gs_reset(void) {
    if (!trace_file && !trace_finished) {
        const char *path = getenv("DC2TK_GS_TRACE");
        if (path && *path) {
            capture_limit = trace_limit();
            trace_file = fopen(path, "wb");
            if (!trace_file) trace_fail("cannot open capture");
            trace_write(trace_magic, sizeof(trace_magic));
            if (atexit(trace_finish)) trace_fail("cannot register capture completion");
        }
    }
    __real_port_gs_reset();
    record(TRACE_RESET, 0, NULL, 0, NULL, 0);
}

int __wrap_port_gif_feed(const void *data, size_t bytes, unsigned path) {
    int result = __real_port_gif_feed(data, bytes, path);
    uint64_t argument = path;
    record(TRACE_FEED, result, &argument, sizeof(argument), data, bytes);
    return result;
}

int __wrap_port_gif_submit(const void *data, size_t bytes) {
    int result = __real_port_gif_submit(data, bytes);
    record(TRACE_SUBMIT, result, NULL, 0, data, bytes);
    return result;
}

int __wrap_port_gs_write_register(unsigned address, uint64_t value) {
    int result = __real_port_gs_write_register(address, value);
    uint64_t arguments[2] = {address, value};
    record(TRACE_REGISTER, result, arguments, sizeof(arguments), NULL, 0);
    return result;
}

int __wrap_port_gs_image_load(const uint64_t *packet, const void *source, size_t bytes) {
    int result = __real_port_gs_image_load(packet, source, bytes);
    record(TRACE_LOAD, result, packet, 9 * sizeof(uint64_t), source, bytes);
    return result;
}

int __wrap_port_gs_image_store(const uint64_t *packet, void *destination, size_t bytes) {
    int result = __real_port_gs_image_store(packet, destination, bytes);
    uint64_t arguments[15];
    memcpy(arguments, packet, 13 * sizeof(uint64_t));
    arguments[13] = bytes;
    arguments[14] = result ? 0 : output_hash(UINT64_C(14695981039346656037), destination, store_size(packet, bytes));
    record(TRACE_STORE, result, arguments, sizeof(arguments), NULL, 0);
    return result;
}

int __wrap_port_gs_read_rgba(unsigned base256, unsigned width64, unsigned psm, unsigned x, unsigned y,
                            unsigned width, unsigned height, uint32_t *rgba, size_t stride_pixels) {
    int result = __real_port_gs_read_rgba(base256, width64, psm, x, y, width, height, rgba, stride_pixels);
    uint64_t arguments[9] = {base256, width64, psm, x, y, width, height, stride_pixels, 0};
    if (!result) arguments[8] = rgba_hash(rgba, width, height, stride_pixels);
    record(TRACE_RGBA, result, arguments, sizeof(arguments), NULL, 0);
    return result;
}
#else
static double seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) trace_fail("cannot read clock");
    return now.tv_sec + now.tv_nsec * 1e-9;
}

static uint64_t counter_delta(uint64_t value, uint64_t previous) {
    return value >= previous ? value - previous : value;
}

static void report_stats(uint64_t field, PortGsRasterStats *previous, PortGsRasterStats *total) {
    PortGsRasterStats current;
    port_gs_raster_stats(&current);
    PortGsRasterStats delta = {
        counter_delta(current.draws, previous->draws),
        counter_delta(current.queued, previous->queued),
        counter_delta(current.serial_aliases, previous->serial_aliases),
        counter_delta(current.flushes, previous->flushes),
        current.largest_batch,
        counter_delta(current.parallel_batches, previous->parallel_batches)
    };
    total->draws += delta.draws;
    total->queued += delta.queued;
    total->serial_aliases += delta.serial_aliases;
    total->flushes += delta.flushes;
    if (delta.largest_batch > total->largest_batch) total->largest_batch = delta.largest_batch;
    total->parallel_batches += delta.parallel_batches;
    printf("stats field=%" PRIu64 " draws=%" PRIu64 " queued=%" PRIu64 " serial_aliases=%" PRIu64
           " flushes=%" PRIu64 " largest_batch=%" PRIu64 " parallel_batches=%" PRIu64 "\n",
           field, delta.draws, delta.queued, delta.serial_aliases, delta.flushes, delta.largest_batch, delta.parallel_batches);
    *previous = current;
}

static int replay(const struct trace_record *header, unsigned char *data, unsigned version) {
    uint64_t arguments[15] = {0};
    size_t size = header->size;
    size_t count = size < sizeof(arguments) ? size : sizeof(arguments);
    memcpy(arguments, data, count);
    switch (header->operation) {
        case TRACE_RESET:
            if (size) trace_fail("invalid reset payload");
            port_gs_reset();
            return 0;
        case TRACE_FEED:
            if (size < 8 || arguments[0] < 1 || arguments[0] > 3) trace_fail("invalid GIF feed payload");
            return port_gif_feed(data + 8, size - 8, (unsigned)arguments[0]);
        case TRACE_SUBMIT:
            return port_gif_submit(data, size);
        case TRACE_REGISTER:
            if (size != 16 || arguments[0] > 127) trace_fail("invalid register payload");
            return port_gs_write_register((unsigned)arguments[0], arguments[1]);
        case TRACE_LOAD:
            if (size < 72) trace_fail("invalid image load payload");
            return port_gs_image_load(arguments, data + 72, size - 72);
        case TRACE_STORE: {
            if (size != (version == 1 ? 112u : 120u) || arguments[13] > PAYLOAD_LIMIT) trace_fail("invalid image store payload");
            void *output = malloc(arguments[13] ? (size_t)arguments[13] : 1);
            if (!output) trace_fail("cannot allocate image store");
            int result = port_gs_image_store(arguments, output, (size_t)arguments[13]);
            if (!result && version == 2 && output_hash(UINT64_C(14695981039346656037), output,
                                                      store_size(arguments, (size_t)arguments[13])) != arguments[14])
                trace_fail("image store output hash differs from capture");
            free(output);
            return result;
        }
        case TRACE_RGBA: {
            if (size != (version == 1 ? 64u : 72u)) trace_fail("invalid display payload");
            for (int i = 0; i < 7; ++i) if (arguments[i] > UINT32_MAX) trace_fail("display argument exceeds bounds");
            if (arguments[5] > 2048 || arguments[6] > 2048 || arguments[7] > 4096 || arguments[7] < arguments[5])
                trace_fail("display dimensions exceed bounds");
            size_t pixels = (size_t)arguments[7] * (size_t)arguments[6];
            if (pixels > PAYLOAD_LIMIT / sizeof(uint32_t)) trace_fail("display buffer exceeds bounds");
            uint32_t *output = malloc(pixels ? pixels * sizeof(uint32_t) : 1);
            if (!output) trace_fail("cannot allocate display output");
            int result = port_gs_read_rgba((unsigned)arguments[0], (unsigned)arguments[1], (unsigned)arguments[2],
                                         (unsigned)arguments[3], (unsigned)arguments[4], (unsigned)arguments[5],
                                         (unsigned)arguments[6], output, (size_t)arguments[7]);
            if (!result && version == 2 && rgba_hash(output, (unsigned)arguments[5], (unsigned)arguments[6],
                                                    (size_t)arguments[7]) != arguments[8])
                trace_fail("display output hash differs from capture");
            free(output);
            return result;
        }
        case TRACE_END:
            if (size != 8 || port_gs_raster_flush()) trace_fail("invalid final payload or raster flush failure");
            if (memory_hash() != arguments[0]) trace_fail("VRAM hash differs from capture");
            printf("VRAM=%016" PRIx64 "\n", arguments[0]);
            return 0;
        default:
            trace_fail("unknown operation");
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 3) trace_fail("usage: gs-replay TRACE THREADS");
    char *end;
    errno = 0;
    unsigned long threads = strtoul(argv[2], &end, 10);
    if (errno || *end || threads < 1 || threads > 12) trace_fail("threads must be 1 through 12");
    port_gs_raster_threads((unsigned)threads);
    struct stat status;
    if (stat(argv[1], &status) || status.st_size < 16 || (uint64_t)status.st_size > trace_limit())
        trace_fail("capture size exceeds bounds or cannot be read");
    FILE *file = fopen(argv[1], "rb");
    if (!file) trace_fail("cannot open capture");
    unsigned char magic[16];
    if (fread(magic, 1, 16, file) != 16 || memcmp(magic, trace_magic, 8) ||
        (magic[8] != 1 && magic[8] != 2) || memcmp(magic + 9, trace_magic + 9, 7)) trace_fail("invalid capture header");
    unsigned version = magic[8];
    if (version == 1) fprintf(stderr, "GS trace: version 1 has no intermediate output hashes\n");
    unsigned char *payload = malloc(PAYLOAD_LIMIT);
    if (!payload) trace_fail("cannot allocate replay payload");
    uint64_t consumed = 16, field = 0, operations = 0;
    uint64_t output_checks = 0;
    double field_time = 0, total_time = 0;
    int finished = 0;
    PortGsRasterStats previous_stats = {0}, total_stats = {0};
    while (consumed < (uint64_t)status.st_size) {
        struct trace_record header;
        if (fread(&header, 1, sizeof(header), file) != sizeof(header)) trace_fail("truncated operation header");
        consumed += sizeof(header);
        if (header.reserved || header.size > PAYLOAD_LIMIT || header.size > (uint64_t)status.st_size - consumed)
            trace_fail("operation payload exceeds bounds");
        if (header.field < field) trace_fail("field counter moved backwards");
        if (operations && header.field != field) {
            printf("field=%" PRIu64 " seconds=%.6f cumulative=%.6f\n", field, field_time, total_time);
            report_stats(field, &previous_stats, &total_stats);
            field_time = 0;
        }
        field = header.field;
        if (!operations && header.operation != TRACE_RESET) trace_fail("capture must start with reset");
        if (fread(payload, 1, header.size, file) != header.size) trace_fail("truncated payload");
        consumed += header.size;
        double start = seconds();
        int result = replay(&header, payload, version);
        double duration = seconds() - start;
        field_time += duration;
        total_time += duration;
        if (result != header.result) trace_fail("operation result differs from capture");
        if (version == 2 && !result && (header.operation == TRACE_STORE || header.operation == TRACE_RGBA)) ++output_checks;
        ++operations;
        if (header.operation == TRACE_END) {
            if (consumed != (uint64_t)status.st_size) trace_fail("data follows final record");
            finished = 1;
        }
    }
    if (!finished) trace_fail("capture has no final VRAM hash");
    printf("checks version=%u output_hashes=%" PRIu64 "\n", version, output_checks);
    printf("field=%" PRIu64 " seconds=%.6f cumulative=%.6f operations=%" PRIu64 " threads=%lu\n",
           field, field_time, total_time, operations, threads);
    report_stats(field, &previous_stats, &total_stats);
    printf("stats total draws=%" PRIu64 " queued=%" PRIu64 " serial_aliases=%" PRIu64 " flushes=%" PRIu64
           " largest_batch=%" PRIu64 " parallel_batches=%" PRIu64 "\n", total_stats.draws, total_stats.queued,
           total_stats.serial_aliases, total_stats.flushes, total_stats.largest_batch, total_stats.parallel_batches);
    free(payload);
    fclose(file);
    return 0;
}
#endif
