#include "port_state.h"
#include "draw.hpp"
#include "mainloop.hpp"
#include "movie.h"
#include "port_graphics.h"
#include "port_kernel.h"
#include "port_mod.h"
#include "port_present.h"
#include "port_stats.h"
#include "render.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
typedef char ResidentPage;
#else
typedef unsigned char ResidentPage;
#endif

extern "C" {
#include "audio.h"
}

enum { PAGE = 4096, RUN_LIMIT = 1024, STATE_VERSION = 4 };
enum { THUMB_BYTES = STATE_THUMB_WIDTH * STATE_THUMB_HEIGHT * 4 };

typedef struct {
    uint64_t start;
    uint64_t size;
} Region;

typedef struct {
    uintptr_t start;
    size_t size;
} Run;

static const char *const restored_objects[] = {
    "port_kernel.o", "port_memory.o", "port_graphics.o", "port_cdvd.o", "port_spu2.o", "port_iop.o",
    "port_devices.o", "port_split_data.o", "port_mg_shadow.o", "port_mg_drawprim.o", "port_frame.o",
    "port_draw.o", "port_mg_visual.o", "port_mg_sprite.o", "port_water.o",
};

static const char *const kept_symbols[][2] = {
    {"port_kernel.o", "_host_context"}, {"port_graphics.o", "_next_field"}, {"port_graphics.o", "_port_field_period"},
    {"port_draw.o", "__ZL5cache"}, {"port_draw.o", "__ZL12frame_target"}, {"port_draw.o", "__ZL12frame_number"},
    {"port_draw.o", "__ZL8palettes"}, {"port_draw.o", "__ZL6copies"}, {"port_mg_visual.o", "__ZL11face_meshes"},
    {"port_mg_visual.o", "__ZL12sorted_draws"}, {"port_mg_shadow.o", "__ZL12shadow_faces"},
    {"port_mg_sprite.o", "__ZL14sprite_packets"}, {"port_mg_sprite.o", "__ZL16previous_packets"},
    {"port_water.o", "__ZL5grids"},
};

static Run runs[RUN_LIMIT];
static int run_count;
static size_t run_bytes;
static int layout_state;
static char layout_error[256];
static uint64_t layout_id[2];
static int objects_seen[sizeof restored_objects / sizeof restored_objects[0]];
static int kept_seen[sizeof kept_symbols / sizeof kept_symbols[0]];
static uint32_t thumbnail[STATE_THUMB_WIDTH * STATE_THUMB_HEIGHT];
static int request_kind;
static PortState *request_state;
static char problem[256];
static float host_ms;


static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void add_run(uintptr_t start, uintptr_t end) {
    if (end <= start) return;
    run_bytes += end - start;
    if (run_count && runs[run_count - 1].start + runs[run_count - 1].size == start) {
        runs[run_count - 1].size += end - start;
        return;
    }
    if (run_count == RUN_LIMIT) {
        snprintf(layout_error, sizeof layout_error, "more than %d memory runs", RUN_LIMIT);
        return;
    }
    runs[run_count].start = start;
    runs[run_count].size = end - start;
    run_count++;
}

static int restored_object(const char *object) {
    if (strncmp(object, "port_", 5) && strncmp(object, "mod_", 4) && strncmp(object, "imgui_", 6)) return 1;
    for (size_t i = 0; i < sizeof restored_objects / sizeof restored_objects[0]; ++i)
        if (!strcmp(object, restored_objects[i])) {
            objects_seen[i] = 1;
            return 1;
        }
    return 0;
}

static int kept_symbol(const char *object, const char *symbol) {
    for (size_t i = 0; i < sizeof kept_symbols / sizeof kept_symbols[0]; ++i)
        if (!strcmp(object, kept_symbols[i][0]) && !strcmp(symbol, kept_symbols[i][1])) {
            kept_seen[i] = 1;
            return 1;
        }
    return 0;
}

#ifdef __APPLE__
static int game_object(const char *object) {
    return strncmp(object, "mod_", 4) && strncmp(object, "imgui_", 6);
}

static void fingerprint(const char *name, uint64_t address) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const char *letter = name; *letter; ++letter) hash = (hash ^ (unsigned char)*letter) * UINT64_C(1099511628211);
    layout_id[0] += hash ^ address;
    layout_id[1] ^= (hash + address) * UINT64_C(0x9E3779B97F4A7C15);
}

static int read_layout(void) {
    const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct load_command *command = (const struct load_command *)(header + 1);
    const struct symtab_command *symtab = NULL;
    uintptr_t linkedit = 0;
    uintptr_t lows[8], highs[8];
    int sections = 0;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SYMTAB) symtab = (const struct symtab_command *)command;
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;

            if (!strcmp(segment->segname, "__LINKEDIT")) linkedit = (uintptr_t)(segment->vmaddr + slide - segment->fileoff);
            const struct section_64 *section = (const struct section_64 *)(segment + 1);
            for (uint32_t s = 0; s < segment->nsects; ++s)
                if (strcmp(section[s].sectname, "__text")) fingerprint(section[s].sectname, section[s].addr);
            for (uint32_t s = 0; s < segment->nsects && sections < 8; ++s) {
                if (strcmp(section[s].segname, "__DATA")) continue;
                if (strcmp(section[s].sectname, "__data") && strcmp(section[s].sectname, "__bss") &&
                    strcmp(section[s].sectname, "__common"))
                    continue;
                lows[sections] = (uintptr_t)(section[s].addr + slide);
                highs[sections] = lows[sections] + (uintptr_t)section[s].size;
                sections++;
            }
        }
        command = (const struct load_command *)((const char *)command + command->cmdsize);
    }
    if (!symtab || !linkedit || !sections) {
        snprintf(layout_error, sizeof layout_error, "the executable has no symbol table or data sections");
        return 0;
    }
    const struct nlist_64 *symbols = (const struct nlist_64 *)(linkedit + symtab->symoff);
    const char *strings = (const char *)(linkedit + symtab->stroff);
    std::vector<uint64_t> marks;
    const char *object = "";
    int objects = 0, unresolved = 0;
    for (uint32_t i = 0; i < symtab->nsyms; ++i) {
        const char *name = strings + symbols[i].n_un.n_strx;
        if (symbols[i].n_type == N_OSO) {
            const char *slash = strrchr(name, '/');
            object = slash ? slash + 1 : name;
            objects++;
            continue;
        }
        if (symbols[i].n_type == N_FUN && name[0] && game_object(object)) fingerprint(name, symbols[i].n_value);
        if (symbols[i].n_type != N_STSYM && symbols[i].n_type != N_GSYM) continue;
        if (game_object(object)) fingerprint(name, symbols[i].n_value);
        uintptr_t address = (uintptr_t)(symbols[i].n_value + slide);
        if (symbols[i].n_type == N_GSYM) {
            void *found = name[0] == '_' ? dlsym(RTLD_MAIN_ONLY, name + 1) : NULL;
            if (!found) {
                unresolved++;
                continue;
            }
            address = (uintptr_t)found;
        }
        int restore = restored_object(object) && !kept_symbol(object, name);
        marks.push_back((uint64_t)address << 1 | (uint64_t)restore);
    }
    if (!objects) {
        snprintf(layout_error, sizeof layout_error, "the executable has no debug map; build with -g and do not strip it");
        return 0;
    }
    std::sort(marks.begin(), marks.end());
    for (int s = 0; s < sections; ++s) {
        size_t first = 0;
        while (first < marks.size() && (marks[first] >> 1) < lows[s]) first++;
        if (first == marks.size() || (marks[first] >> 1) >= highs[s]) continue;
        uintptr_t start = lows[s];
        int restore = (int)(marks[first] & 1);
        for (size_t i = first; i < marks.size() && (marks[i] >> 1) < highs[s]; ++i) {
            uintptr_t address = (uintptr_t)(marks[i] >> 1);
            if (restore) add_run(start, address);
            start = address;
            restore = (int)(marks[i] & 1);
        }
        if (restore) add_run(start, highs[s]);
    }
    for (size_t i = 0; i < sizeof restored_objects / sizeof restored_objects[0]; ++i)
        if (!objects_seen[i]) {
            snprintf(layout_error, sizeof layout_error, "%s is not linked; update the savestate tables", restored_objects[i]);
            return 0;
        }
    for (size_t i = 0; i < sizeof kept_symbols / sizeof kept_symbols[0]; ++i)
        if (!kept_seen[i]) {
            snprintf(layout_error, sizeof layout_error, "%s has no %s; update the savestate tables", kept_symbols[i][0],
                     kept_symbols[i][1]);
            return 0;
        }
    if (layout_error[0]) return 0;
    fprintf(stderr, "savestate: %d memory runs, %.1f MB, %d of %zu data symbols unresolved\n", run_count,
            (double)run_bytes / 1048576.0, unresolved, marks.size() + (size_t)unresolved);
    return 1;
}
#else
static int read_layout(void) {
    snprintf(layout_error, sizeof layout_error, "savestates read the macOS debug map and need the macOS build");
    return 0;
}
#endif

const char *port_state_ready(void) {
    if (!layout_state) layout_state = read_layout() ? 1 : -1;
    return layout_state > 0 ? NULL : layout_error;
}

static std::vector<Run> window_runs(void) {
    std::vector<Run> found;
    uintptr_t low = PORT_WINDOW + 0x10000000, high = PORT_WINDOW + 0x20000000;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    std::vector<ResidentPage> resident((high - low) / page);
    if (mincore((void *)low, high - low, resident.data())) return found;
    for (size_t i = 0; i < resident.size(); ++i) {
        if (!(resident[i] & 1)) continue;
        uintptr_t start = low + i * page;
        if (!found.empty() && found.back().start + found.back().size == start) found.back().size += page;
        else found.push_back({start, page});
    }
    return found;
}

static std::vector<Run> memory_regions(void) {
    std::vector<Run> regions(runs, runs + run_count);
    std::vector<Run> window = window_runs();
    regions.insert(regions.end(), window.begin(), window.end());
    regions.push_back({(uintptr_t)(PORT_WINDOW + 0x70000000), 16384});
    return regions;
}

static int inside_window(uint64_t start, uint64_t size) {
    uint64_t low = PORT_WINDOW + 0x10000000, high = PORT_WINDOW + 0x20000000;
    if (start >= low && size <= high - low && start - low <= high - low - size) return 1;
    return start == PORT_WINDOW + 0x70000000 && size == 16384;
}

static void restore_bytes(uint64_t start, const unsigned char *data, size_t size) {
    if (inside_window(start, size)) {
        if (data) memcpy((void *)(uintptr_t)start, data, size);
        else memset((void *)(uintptr_t)start, 0, size);
        return;
    }
    for (int i = 0; i < run_count; ++i) {
        uint64_t low = start > runs[i].start ? start : runs[i].start;
        uint64_t high = start + size < runs[i].start + runs[i].size ? start + size : runs[i].start + runs[i].size;
        if (low >= high) continue;
        if (data) memcpy((void *)(uintptr_t)low, data + (low - start), (size_t)(high - low));
        else memset((void *)(uintptr_t)low, 0, (size_t)(high - low));
    }
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
    std::vector<Run> regions = memory_regions();
    std::vector<unsigned char> maps;
    size_t sound = audio_save(NULL) + visual_save(NULL);
    size_t size = sizeof(PortStateHeader) + THUMB_BYTES + sound;
    for (const Run &region : regions) {
        size_t pages = (region.size + PAGE - 1) / PAGE;
        size += sizeof(Region) + pages;
        for (size_t page = 0; page < pages; ++page) {
            size_t length = page_length(region.size, page);
            unsigned char stored = !zero_page((const unsigned char *)region.start + page * PAGE, length);
            maps.push_back(stored);
            if (stored) size += length;
        }
    }
    unsigned char *data = (unsigned char *)malloc(size);
    if (!data) {
        snprintf(problem, sizeof problem, "no memory for a %zu byte savestate", size);
        return;
    }
    PortStateHeader header;
    memset(&header, 0, sizeof header);
    memcpy(header.magic, "DC2STATE", 8);
    header.version = STATE_VERSION;
    header.regions = (uint32_t)regions.size();
    memcpy(header.layout, layout_id, 16);
    header.loop = GetNowLoopNo();
    header.map = GetNowInitArg() ? GetNowInitArg()->map_no : -1;
    header.field = port_graphics.fields;
    header.taken = (int64_t)time(NULL);
    header.arena_used = port_stats.arena_used;
    header.arena_peak = port_stats.arena_peak;
    header.arena_blocks = port_stats.arena_blocks;
    header.sound = sound;
    header.size = size;
    header.base = PORT_WINDOW;
    unsigned char *at = data;
    memcpy(at, &header, sizeof header);
    at += sizeof header;
    memcpy(at, thumbnail, THUMB_BYTES);
    at += THUMB_BYTES;
    size_t map = 0;
    for (const Run &region : regions) {
        Region record = {region.start, region.size};
        size_t pages = (region.size + PAGE - 1) / PAGE;
        memcpy(at, &record, sizeof record);
        at += sizeof record;
        memcpy(at, maps.data() + map, pages);
        at += pages;
        for (size_t page = 0; page < pages; ++page, ++map) {
            if (!maps[map]) continue;
            size_t length = page_length(region.size, page);
            memcpy(at, (const unsigned char *)region.start + page * PAGE, length);
            at += length;
        }
    }
    visual_save(at + audio_save(at));
    free(state->data);
    state->data = data;
    state->size = size;
}

static const char *check_state(const unsigned char *data, size_t size) {
    if (size < sizeof(PortStateHeader) + THUMB_BYTES) return "the savestate is truncated";
    const PortStateHeader *header = (const PortStateHeader *)data;
    if (memcmp(header->magic, "DC2STATE", 8) || header->version != STATE_VERSION) return "not a savestate of this version";
    if (memcmp(header->layout, layout_id, 16)) return "the savestate is from a build with the game's code or data in other places";
    if (header->size != size) return "the savestate is truncated";
    if (header->base != PORT_WINDOW) return "the savestate is from an earlier launch, and the game now sits at another address";
    const unsigned char *at = data + sizeof(PortStateHeader) + THUMB_BYTES;
    const unsigned char *end = data + size;
    for (uint32_t i = 0; i < header->regions; ++i) {
        Region record;
        if ((size_t)(end - at) < sizeof record) return "the savestate is truncated";
        memcpy(&record, at, sizeof record);
        at += sizeof record;
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

static void load_state(const PortState *state) {
    const char *invalid = check_state(state->data, state->size);
    if (invalid) {
        snprintf(problem, sizeof problem, "%s", invalid);
        return;
    }
    const PortStateHeader *header = (const PortStateHeader *)state->data;
    if (!port_movie_end()) port_movie_close();
    for (const Run &region : window_runs()) memset((void *)region.start, 0, region.size);
    const unsigned char *at = state->data + sizeof(PortStateHeader) + THUMB_BYTES;
    for (uint32_t i = 0; i < header->regions; ++i) {
        Region record;
        memcpy(&record, at, sizeof record);
        at += sizeof record;
        size_t pages = (record.size + PAGE - 1) / PAGE;
        const unsigned char *map = at;
        at += pages;
        for (size_t page = 0; page < pages; ++page) {
            size_t length = page_length(record.size, page);
            restore_bytes(record.start + page * PAGE, map[page] ? at : NULL, length);
            if (map[page]) at += length;
        }
    }
    visual_load(at + audio_load(at));
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
    const char *unready = port_state_ready();
    if (unready) {
        snprintf(problem, sizeof problem, "%s", unready);
        return 0;
    }
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

const char *port_state_problem(void) { return problem[0] ? problem : NULL; }

float port_state_ms(void) { return host_ms; }

void port_state_frame(void) {
    if (!request_kind) return;
    if (request_kind == STATE_SAVE && !port_movie_end() && port_movie_started()) {
        snprintf(problem, sizeof problem, "a movie is playing; save after it");
        request_kind = 0;
        return;
    }
    if (request_kind == STATE_SAVE) capture_thumbnail();
    port_kernel_host();
    request_kind = 0;
}

void port_state_host(void) {
    if (!request_kind) return;
    uint64_t start = now_ns();
    if (request_kind == STATE_SAVE) save_state(request_state);
    if (request_kind == STATE_LOAD) load_state(request_state);
    host_ms = (float)(now_ns() - start) / 1e6f;
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
    const char *unready = port_state_ready();
    if (unready) return unready;
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
    return NULL;
}
