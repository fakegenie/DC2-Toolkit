#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t draw_calls;
    uint32_t mesh_calls;
    uint32_t gpu_commands;
    uint32_t render_passes;
    uint32_t pipeline_changes;
    uint32_t vertices;
    uint32_t texture_uploads;
    uint32_t upload_bytes;
    uint32_t downloads;
    uint32_t texture_decodes;
    uint32_t target_creates;
    uint32_t texture_renames;
    uint32_t replays;
    uint32_t mesh_builds;
    uint32_t sorted_draws;
    uint32_t allocations;
    uint32_t frees;
    uint32_t allocation_bytes;
    uint32_t disc_reads;
    uint32_t disc_bytes;
    uint64_t wait_ns;
    uint64_t replay_ns;
    uint64_t gpu_ns;
    uint32_t dropped_fields;
    uint32_t textures_live;
    uint32_t pipelines;
    uint32_t cached_textures;
    uint32_t cached_meshes;
    uint64_t arena_used;
    uint64_t arena_peak;
    uint32_t arena_blocks;
    uint32_t audio_ms;
} PortStats;

extern PortStats port_stats;

#ifdef __cplusplus
}
#endif
