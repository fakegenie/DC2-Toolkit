#include "port_state.h"
#include "draw.hpp"
#include "mainloop.hpp"
#include "mglib.hpp"
#include "movie.h"
#include "port_arena.h"
#include "port_graphics.h"
#include "port_kernel.h"
#include "port_present.h"
#include "port_stats.h"
#include "render.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

extern "C" {
#include "audio.h"
extern const char port_build_id[];
}

void GamePadStep(void *arg);

enum { PAGE = 4096, STATE_VERSION = 8, REGION_LIMIT = 2, TARGET_LIMIT = 64 };
static const char STATE_MAGIC[8] = {'D', 'C', '2', 'S', 'T', 'A', 'T', 'E'};
enum { THUMB_BYTES = STATE_THUMB_WIDTH * STATE_THUMB_HEIGHT * 4 };

typedef struct {
    uint64_t start;
    uint64_t size;
} Region;

typedef struct {
    uint64_t start;
    uint64_t size;
    int aligned;
} Target;

typedef struct {
    Target targets[TARGET_LIMIT];
    int target_count;
    std::vector<PortArenaSpan> arena;
    uint64_t arena_low;
    uint64_t arena_size;
    uint64_t slide;
    uint64_t moved;
} Move;

static uint32_t thumbnail[STATE_THUMB_WIDTH * STATE_THUMB_HEIGHT];
static int request_kind;
static PortState *request_state;
static int main_frame_end;
static uint64_t main_frame_stack;
static char problem[1024];
static float host_ms;
static char note[256];

#ifdef __APPLE__
extern char game_start __asm("segment$start$__GAME");
extern char game_end __asm("segment$end$__GAME");

static int game_regions(Region *regions) {
    regions[0] = {(uint64_t)(uintptr_t)&game_start, (uint64_t)(&game_end - &game_start)};
    return 1;
}

static uint64_t image_base(void) {
    return (uint64_t)(uintptr_t)_dyld_get_image_header(0);
}

static int code_or_constant(const struct section_64 *section) {
    return (!strcmp(section->segname, "__TEXT") || !strcmp(section->segname, "__DATA_CONST")) &&
           strcmp(section->sectname, "__cstring");
}

static int image_targets(Target *targets) {
    const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct load_command *command = (const struct load_command *)(header + 1);
    int count = 0;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
        const struct section_64 *sections = (const struct section_64 *)(segment + 1);
        if (command->cmd == LC_SEGMENT_64 && strcmp(segment->segname, "__DATA"))
            for (uint32_t s = 0; s < segment->nsects && count < TARGET_LIMIT; ++s)
                targets[count++] = {sections[s].addr + (uint64_t)slide, sections[s].size, code_or_constant(&sections[s])};
        command = (const struct load_command *)((const char *)command + command->cmdsize);
    }
    return count;
}
#else
extern "C" char __start_game_data[], __stop_game_data[], __start_game_bss[], __stop_game_bss[];

static int game_regions(Region *regions) {
    regions[0] = {(uint64_t)(uintptr_t)__start_game_data, (uint64_t)(__stop_game_data - __start_game_data)};
    regions[1] = {(uint64_t)(uintptr_t)__start_game_bss, (uint64_t)(__stop_game_bss - __start_game_bss)};
    return 2;
}

static uint64_t image_base(void) {
    Dl_info info;
    return dladdr((void *)&image_base, &info) ? (uint64_t)(uintptr_t)info.dli_fbase : 0;
}

typedef struct {
    Target *targets;
    int count;
} Found;

static int holds_image(const struct dl_phdr_info *info) {
    uint64_t self = (uint64_t)(uintptr_t)&image_base;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *segment = &info->dlpi_phdr[i];
        if (segment->p_type == PT_LOAD && self - (info->dlpi_addr + segment->p_vaddr) < segment->p_memsz) return 1;
    }
    return 0;
}

static int segment_targets(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    Found *found = (Found *)data;
    if (!holds_image(info)) return 0;
    for (int i = 0; i < info->dlpi_phnum && found->count < TARGET_LIMIT; ++i) {
        const ElfW(Phdr) *segment = &info->dlpi_phdr[i];
        uint64_t start = info->dlpi_addr + segment->p_vaddr;
        if (segment->p_type == PT_LOAD && !(segment->p_flags & PF_W))
            found->targets[found->count++] = {start, segment->p_memsz, (segment->p_flags & PF_X) != 0};
        if (segment->p_type == PT_GNU_RELRO) found->targets[found->count++] = {start, segment->p_memsz, 1};
    }
    return 1;
}

static int image_targets(Target *targets) {
    Found found = {targets, 0};
    dl_iterate_phdr(segment_targets, &found);
    Region regions[REGION_LIMIT];
    int region_count = game_regions(regions);
    for (int i = 0; i < region_count && found.count < TARGET_LIMIT; ++i)
        targets[found.count++] = {regions[i].start, regions[i].size, 0};
    return found.count;
}
#endif

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static size_t page_length(size_t size, size_t page) {
    return size - page * PAGE < PAGE ? size - page * PAGE : PAGE;
}

static int zero_page(const unsigned char *data, size_t size) {
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        memcpy(&word, data + i, 8);
        if (word) return 0;
    }
    for (; i < size; ++i)
        if (data[i]) return 0;
    return 1;
}

static void save_state(PortState *state) {
    Region regions[REGION_LIMIT];
    int region_count = game_regions(regions);
    std::vector<unsigned char> maps;
    size_t sound = audio_save(NULL) + visual_save(NULL);
    size_t size = sizeof(PortStateHeader) + THUMB_BYTES + sound;
    for (int i = 0; i < region_count; ++i) {
        size_t pages = (regions[i].size + PAGE - 1) / PAGE;
        size += sizeof(Region) + pages;
        for (size_t page = 0; page < pages; ++page) {
            size_t length = page_length(regions[i].size, page);
            unsigned char stored = !zero_page((const unsigned char *)(uintptr_t)regions[i].start + page * PAGE, length);
            maps.push_back(stored);
            if (stored) size += length;
        }
    }
    unsigned char *data = state->room >= size ? state->data : (unsigned char *)malloc(size);
    if (!data) {
        snprintf(problem, sizeof problem, "no memory for a %zu byte savestate", size);
        return;
    }
    PortStateHeader header;
    memset(&header, 0, sizeof header);
    memcpy(header.magic, STATE_MAGIC, sizeof header.magic);
    header.version = STATE_VERSION;
    header.regions = (uint32_t)region_count;
    memcpy(header.build, port_build_id, sizeof header.build);
    header.loop = GetNowLoopNo();
    header.map = GetNowInitArg() ? GetNowInitArg()->map_no : -1;
    header.field = port_fields;
    header.taken = (int64_t)time(NULL);
    header.arena_used = port_stats.arena_used;
    header.arena_peak = port_stats.arena_peak;
    header.arena_blocks = port_stats.arena_blocks;
    header.sound = sound;
    header.size = size;
    header.base = image_base();
    unsigned char *at = data;
    memcpy(at, &header, sizeof header);
    at += sizeof header;
    memcpy(at, thumbnail, THUMB_BYTES);
    at += THUMB_BYTES;
    size_t map = 0;
    for (int i = 0; i < region_count; ++i) {
        size_t pages = (regions[i].size + PAGE - 1) / PAGE;
        memcpy(at, &regions[i], sizeof regions[i]);
        at += sizeof regions[i];
        memcpy(at, maps.data() + map, pages);
        at += pages;
        for (size_t page = 0; page < pages; ++page, ++map) {
            if (!maps[map]) continue;
            size_t length = page_length(regions[i].size, page);
            memcpy(at, (const unsigned char *)(uintptr_t)regions[i].start + page * PAGE, length);
            at += length;
        }
    }
    visual_save(at + audio_save(at));
    if (data != state->data) {
        free(state->data);
        state->room = size;
    }
    state->data = data;
    state->size = size;
}

static int in_arena_block(const Move *move, uint64_t value) {
    for (const PortArenaSpan &span : move->arena)
        if (value >= span.start && value <= span.live_end) return 1;
    return 0;
}

static int old_image_pointer(const Move *move, uint64_t value) {
    for (int i = 0; i < move->target_count; ++i) {
        const Target *target = &move->targets[i];
        if (value - (target->start - move->slide) >= target->size) continue;
        if (target->aligned && value % 4) return 0;
        return value - move->arena_low >= move->arena_size || in_arena_block(move, value);
    }
    return 0;
}

static void move_words(Move *move, unsigned char *bytes, uint64_t start, size_t size) {
    for (size_t at = (size_t)((4 - start % 4) % 4); at + 8 <= size; at += 4) {
        uint64_t value;
        memcpy(&value, bytes + at, 8);
        if (!old_image_pointer(move, value)) continue;
        value += move->slide;
        memcpy(bytes + at, &value, 8);
        move->moved++;
        at += 4;
    }
}

static void prepare_move(Move *move, const PortStateHeader *header) {
    uint64_t arena_start;
    move->slide = image_base() - header->base;
    move->target_count = image_targets(move->targets);
    if (move->target_count < TARGET_LIMIT)
        move->targets[move->target_count++] = {main_frame_stack, port_kernel_main_stack_top() - main_frame_stack, 0};
    move->moved = 0;
    port_arena_bounds(&arena_start, &move->arena_size);
    move->arena_low = arena_start - move->slide;
    move->arena.resize((size_t)(header->arena_blocks * 2 + 2));
    move->arena.resize(port_arena_spans((const unsigned char *)(uintptr_t)arena_start, (size_t)move->arena_size,
                                        move->arena_low, move->arena.data(), move->arena.size()));
}

static const char *check_state(const unsigned char *data, size_t size) {
    if (size < sizeof(PortStateHeader) + THUMB_BYTES) return "the savestate is truncated";
    const PortStateHeader *header = (const PortStateHeader *)data;
    if (memcmp(header->magic, STATE_MAGIC, sizeof header->magic) || header->version != STATE_VERSION) return "not a savestate of this version";
    if (memcmp(header->build, port_build_id, sizeof header->build)) return "the savestate is from another build";
    if (header->size != size) return "the savestate is truncated";
    Region regions[REGION_LIMIT];
    if (header->regions != (uint32_t)game_regions(regions)) return "the savestate does not fit this build";
    const unsigned char *at = data + sizeof(PortStateHeader) + THUMB_BYTES;
    const unsigned char *end = data + size;
    for (uint32_t i = 0; i < header->regions; ++i) {
        Region record;
        if ((size_t)(end - at) < sizeof record) return "the savestate is truncated";
        memcpy(&record, at, sizeof record);
        at += sizeof record;
        if (record.start - header->base != regions[i].start - image_base() || record.size != regions[i].size)
            return "the savestate does not fit this build";
        size_t pages = (record.size + PAGE - 1) / PAGE;
        if ((size_t)(end - at) < pages) return "the savestate is truncated";
        const unsigned char *map = at;
        at += pages;
        for (size_t page = 0; page < pages; ++page) {
            if (!map[page]) continue;
            size_t length = page_length(record.size, page);
            if ((size_t)(end - at) < length) return "the savestate is truncated";
            at += length;
        }
    }
    if ((size_t)(end - at) != header->sound) return "the savestate's sound does not fit";
    return NULL;
}

static void restore_regions(const unsigned char *data) {
    const PortStateHeader *header = (const PortStateHeader *)data;
    Region regions[REGION_LIMIT];
    game_regions(regions);
    const unsigned char *at = data + sizeof(PortStateHeader) + THUMB_BYTES;
    for (uint32_t i = 0; i < header->regions; ++i) {
        at += sizeof(Region);
        size_t pages = (regions[i].size + PAGE - 1) / PAGE;
        const unsigned char *map = at;
        at += pages;
        unsigned char *live = (unsigned char *)(uintptr_t)regions[i].start;
        for (size_t page = 0; page < pages; ++page) {
            size_t length = page_length(regions[i].size, page);
            if (!map[page]) {
                memset(live + page * PAGE, 0, length);
                continue;
            }
            memcpy(live + page * PAGE, at, length);
            at += length;
        }
    }
}

static void move_state(const PortStateHeader *header, unsigned char *visual) {
    Move move;
    Region regions[REGION_LIMIT];
    std::vector<size_t> keys;
    prepare_move(&move, header);
    int region_count = game_regions(regions);
    for (int i = 0; i < region_count; ++i)
        move_words(&move, (unsigned char *)(uintptr_t)regions[i].start, regions[i].start, (size_t)regions[i].size);
    visual_keys(visual, &keys);
    for (size_t key : keys) move_words(&move, visual + key, 0, 8);
    snprintf(note, sizeof note, "from a launch at another address: %llu pointers moved", (unsigned long long)move.moved);
}

static void load_state(const PortState *state) {
    const char *invalid = check_state(state->data, state->size);
    if (invalid) {
        snprintf(problem, sizeof problem, "%s", invalid);
        return;
    }
    const PortStateHeader *header = (const PortStateHeader *)state->data;
    std::vector<unsigned char> sound(state->data + state->size - header->sound, state->data + state->size);
    if (!port_movie_end()) port_movie_close();
    restore_regions(state->data);
    unsigned char *visual = sound.data() + audio_load(sound.data());
    if (header->base != image_base()) move_state(header, visual);
    visual_load(visual);
    port_stats.arena_used = header->arena_used;
    port_stats.arena_peak = header->arena_peak;
    port_stats.arena_blocks = header->arena_blocks;
}

static void capture_thumbnail(void) {
    int width = 0, height = 0;
    int target = port_display_target(&width, &height);
    memset(thumbnail, 0, sizeof thumbnail);
    if (!target || width <= 0 || height <= 0) return;
    std::vector<uint32_t> pixels((size_t)width * (size_t)height);
    render_download(target, 0, 0, width, height, pixels.data());
    for (int y = 0; y < STATE_THUMB_HEIGHT; ++y)
        for (int x = 0; x < STATE_THUMB_WIDTH; ++x)
            thumbnail[y * STATE_THUMB_WIDTH + x] =
                pixels[(size_t)(y * height / STATE_THUMB_HEIGHT) * (size_t)width + (size_t)(x * width / STATE_THUMB_WIDTH)] |
                0xff000000u;
}

int port_state_request(int kind, PortState *state) {
    problem[0] = 0;
    note[0] = 0;
    if (request_kind) {
        snprintf(problem, sizeof problem, "a savestate is already waiting for the end of the frame");
        return 0;
    }
    if (kind == STATE_LOAD && !state->data) {
        snprintf(problem, sizeof problem, "there is no savestate to load");
        return 0;
    }
    request_kind = kind;
    request_state = state;
    return 1;
}

int port_state_pending(void) { return request_kind; }

void port_state_cancel(const char *reason) {
    request_kind = 0;
    snprintf(problem, sizeof problem, "%s", reason);
}

const char *port_state_problem(void) { return problem[0] ? problem : NULL; }

const char *port_state_note(void) { return note[0] ? note : NULL; }

float port_state_ms(void) { return host_ms; }

int port_state_possible(void) { return main_frame_end && port_kernel_quiet(GamePadStep); }

void port_state_frame(void) {
    if (!request_kind || !port_state_possible()) return;
    if (request_kind == STATE_SAVE && !port_movie_end() && port_movie_started()) {
        snprintf(problem, sizeof problem, "a movie is playing; save after it");
        request_kind = 0;
        return;
    }
    uint64_t start = now_ns();
    if (request_kind == STATE_SAVE) {
        capture_thumbnail();
        save_state(request_state);
    }
    if (request_kind == STATE_LOAD) load_state(request_state);
    host_ms = (float)(now_ns() - start) / 1e6f;
    request_kind = 0;
}

void port_end_main_frame(void) {
    main_frame_stack = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    main_frame_end = 1;
    mgEndFrame(NULL);
    main_frame_end = 0;
}

const PortStateHeader *port_state_header(const PortState *state) {
    return state->data ? (const PortStateHeader *)state->data : NULL;
}

const uint32_t *port_state_thumbnail(const PortState *state) {
    return state->data ? (const uint32_t *)(state->data + sizeof(PortStateHeader)) : NULL;
}

const char *port_state_write(const PortState *state, const char *path) {
    static char failure[600];
    FILE *file = fopen(path, "wb");
    if (!file) {
        snprintf(failure, sizeof failure, "cannot write %s", path);
        return failure;
    }
    size_t written = fwrite(state->data, 1, state->size, file);
    if (fclose(file) || written != state->size) {
        snprintf(failure, sizeof failure, "cannot write %s", path);
        return failure;
    }
    return NULL;
}

const char *port_state_read(PortState *state, const char *path) {
    static char failure[600];
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(failure, sizeof failure, "cannot read %s", path);
        return failure;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char *data = size > 0 ? (unsigned char *)malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(data);
        snprintf(failure, sizeof failure, "cannot read %s", path);
        return failure;
    }
    fclose(file);
    const char *invalid = check_state(data, (size_t)size);
    if (invalid) {
        free(data);
        snprintf(failure, sizeof failure, "%s: %s", path, invalid);
        return failure;
    }
    free(state->data);
    state->data = data;
    state->size = (size_t)size;
    state->room = (size_t)size;
    return NULL;
}
