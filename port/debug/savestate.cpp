#include "debug.h"
#include "imgui.h"
#include "mainloop.hpp"
#include "movie.h"
#include "port_debug.h"
#include "port_graphics.h"
#include "port_kernel.h"
#include "port_present.h"
#include "render.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <sys/stat.h>
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

enum { SLOT_LIMIT = 8, THUMB_WIDTH = 160, THUMB_HEIGHT = 120, PAGE = 4096, RUN_LIMIT = 1024, SAVE = 1, LOAD = 2 };
enum { THUMB_BYTES = THUMB_WIDTH * THUMB_HEIGHT * 4, STATE_VERSION = 1 };

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t regions;
    unsigned char uuid[16];
    uint32_t frame;
    int32_t loop;
    int32_t map;
    uint32_t arena_blocks;
    uint64_t field;
    int64_t taken;
    uint64_t arena_used;
    uint64_t arena_peak;
    uint64_t sound;
    uint64_t size;
} Header;

typedef struct {
    uint64_t start;
    uint64_t size;
} Region;

typedef struct {
    uintptr_t start;
    size_t size;
} Run;

typedef struct {
    unsigned char *data;
    size_t size;
    float save_ms;
    float load_ms;
    int loads;
    int texture;
    int shown;
} Slot;

typedef struct {
    int kind;
    int slot;
    int client;
    char path[512];
} Request;

static const char *const restored_objects[] = {
    "port_kernel.o", "port_memory.o", "port_graphics.o", "port_cdvd.o", "port_spu2.o", "port_iop.o",
    "port_devices.o", "port_split_data.o", "port_mg_shadow.o", "port_mg_drawprim.o", "port_frame.o",
    "port_draw.o", "port_mg_visual.o", "port_mg_sprite.o", "port_water.o",
};

static const char *const kept_symbols[][2] = {
    {"port_kernel.o", "_host_context"}, {"port_graphics.o", "_next_field"}, {"port_graphics.o", "_port_field_period"},
    {"port_draw.o", "__ZL5cache"}, {"port_draw.o", "__ZL12frame_target"}, {"port_draw.o", "__ZL12frame_number"},
};

static const char *const loop_names[] = {"menu", "town", "dungeon", "title", "character viewer", "texture viewer",
                                         "map viewer", "sound viewer", "movie viewer", "save conversion"};

static Slot slots[SLOT_LIMIT];
static Request request;
static Run runs[RUN_LIMIT];
static int run_count;
static size_t run_bytes;
static int layout_state;
static char layout_error[256];
static unsigned char build_uuid[16];
static int objects_seen[sizeof restored_objects / sizeof restored_objects[0]];
static int kept_seen[sizeof kept_symbols / sizeof kept_symbols[0]];
static uint32_t thumbnail[THUMB_WIDTH * THUMB_HEIGHT];
static uint64_t host_ns;
static char host_error[256];
static char message[512];
static uint32_t frame_seen = UINT32_MAX;
static int pause_hook;
static char path_text[256] = "slot0.dc2state";

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
    if (strncmp(object, "port_", 5) && strncmp(object, "debug_", 6) && strncmp(object, "imgui_", 6)) return 1;
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
        if (command->cmd == LC_UUID) memcpy(build_uuid, ((const struct uuid_command *)command)->uuid, 16);
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
            if (!strcmp(segment->segname, "__LINKEDIT")) linkedit = (uintptr_t)(segment->vmaddr + slide - segment->fileoff);
            const struct section_64 *section = (const struct section_64 *)(segment + 1);
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
        if (symbols[i].n_type != N_STSYM && symbols[i].n_type != N_GSYM) continue;
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

static int layout_ready(void) {
    if (!layout_state) layout_state = read_layout() ? 1 : -1;
    return layout_state > 0;
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

static int allowed_region(uint64_t start, uint64_t size) {
    for (int i = 0; i < run_count; ++i)
        if (start >= runs[i].start && size <= runs[i].size && start - runs[i].start <= runs[i].size - size) return 1;
    uint64_t low = PORT_WINDOW + 0x10000000, high = PORT_WINDOW + 0x20000000;
    if (start >= low && size <= high - low && start - low <= high - low - size) return 1;
    return start == PORT_WINDOW + 0x70000000 && size == 16384;
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

static const char *loop_name(int loop) {
    return loop >= 0 && loop < (int)(sizeof loop_names / sizeof loop_names[0]) ? loop_names[loop] : "unknown";
}

static const Header *slot_header(const Slot *slot) {
    return slot->data ? (const Header *)slot->data : NULL;
}

static void save_state(Slot *slot) {
    std::vector<Run> regions = memory_regions();
    std::vector<unsigned char> maps;
    size_t sound = audio_save(NULL);
    size_t size = sizeof(Header) + THUMB_BYTES + sound;
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
        snprintf(host_error, sizeof host_error, "no memory for a %zu byte savestate", size);
        return;
    }
    Header header;
    memset(&header, 0, sizeof header);
    memcpy(header.magic, "DC2STATE", 8);
    header.version = STATE_VERSION;
    header.regions = (uint32_t)regions.size();
    memcpy(header.uuid, build_uuid, 16);
    header.frame = debug_frame_total;
    header.loop = GetNowLoopNo();
    header.map = GetNowInitArg() ? GetNowInitArg()->map_no : -1;
    header.field = port_graphics.fields;
    header.taken = (int64_t)time(NULL);
    header.arena_used = port_stats.arena_used;
    header.arena_peak = port_stats.arena_peak;
    header.arena_blocks = port_stats.arena_blocks;
    header.sound = sound;
    header.size = size;
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
    audio_save(at);
    free(slot->data);
    slot->data = data;
    slot->size = size;
    slot->loads = 0;
    slot->load_ms = 0;
    slot->shown = 0;
}

static const char *check_state(const unsigned char *data, size_t size) {
    if (size < sizeof(Header) + THUMB_BYTES) return "the savestate is truncated";
    const Header *header = (const Header *)data;
    if (memcmp(header->magic, "DC2STATE", 8) || header->version != STATE_VERSION) return "not a savestate of this version";
    if (memcmp(header->uuid, build_uuid, 16)) return "the savestate is from a different build";
    if (header->size != size) return "the savestate is truncated";
    const unsigned char *at = data + sizeof(Header) + THUMB_BYTES;
    const unsigned char *end = data + size;
    for (uint32_t i = 0; i < header->regions; ++i) {
        Region record;
        if ((size_t)(end - at) < sizeof record) return "the savestate is truncated";
        memcpy(&record, at, sizeof record);
        at += sizeof record;
        if (!allowed_region(record.start, record.size)) return "the savestate writes outside the game's memory";
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

static void load_state(const Slot *slot) {
    const char *problem = check_state(slot->data, slot->size);
    if (problem) {
        snprintf(host_error, sizeof host_error, "%s", problem);
        return;
    }
    const Header *header = (const Header *)slot->data;
    if (!port_movie_end()) port_movie_close();
    for (const Run &region : window_runs()) memset((void *)region.start, 0, region.size);
    const unsigned char *at = slot->data + sizeof(Header) + THUMB_BYTES;
    for (uint32_t i = 0; i < header->regions; ++i) {
        Region record;
        memcpy(&record, at, sizeof record);
        at += sizeof record;
        size_t pages = (record.size + PAGE - 1) / PAGE;
        const unsigned char *map = at;
        at += pages;
        for (size_t page = 0; page < pages; ++page) {
            unsigned char *target = (unsigned char *)(uintptr_t)record.start + page * PAGE;
            size_t length = page_length(record.size, page);
            if (map[page]) {
                memcpy(target, at, length);
                at += length;
            } else {
                memset(target, 0, length);
            }
        }
    }
    audio_load(at);
    port_stats.arena_used = header->arena_used;
    port_stats.arena_peak = header->arena_peak;
    port_stats.arena_blocks = header->arena_blocks;
}

extern "C" void debug_host(void) {
    uint64_t start = now_ns();
    host_error[0] = 0;
    if (request.kind == SAVE) save_state(&slots[request.slot]);
    if (request.kind == LOAD) load_state(&slots[request.slot]);
    host_ns = now_ns() - start;
}

static void capture_thumbnail(void) {
    int width = 0, height = 0;
    int target = port_display_target(&width, &height);
    memset(thumbnail, 0, sizeof thumbnail);
    if (!target || width <= 0 || height <= 0) return;
    std::vector<uint32_t> pixels((size_t)width * (size_t)height);
    render_download(target, 0, 0, width, height, pixels.data());
    for (int y = 0; y < THUMB_HEIGHT; ++y)
        for (int x = 0; x < THUMB_WIDTH; ++x)
            thumbnail[y * THUMB_WIDTH + x] = pixels[(size_t)(y * height / THUMB_HEIGHT) * (size_t)width +
                                                    (size_t)(x * width / THUMB_WIDTH)] | 0xff000000u;
}

static void show_thumbnail(Slot *slot) {
    if (!slot->data || slot->shown) return;
    if (!slot->texture) slot->texture = render_texture(THUMB_WIDTH, THUMB_HEIGHT);
    render_upload(slot->texture, 0, 0, THUMB_WIDTH, THUMB_HEIGHT, (const uint32_t *)(slot->data + sizeof(Header)));
    slot->shown = 1;
}

static void state_path(const char *name, char *path, size_t size) {
    if (strchr(name, '/')) snprintf(path, size, "%s", name);
    else snprintf(path, size, "build/port/savestates/%s", name);
}

static const char *write_state(const Slot *slot, const char *name) {
    static char problem[600];
    char path[512];
    state_path(name, path, sizeof path);
    mkdir("build/port/savestates", 0755);
    FILE *file = fopen(path, "wb");
    if (!file) {
        snprintf(problem, sizeof problem, "cannot write %s", path);
        return problem;
    }
    size_t written = fwrite(slot->data, 1, slot->size, file);
    if (fclose(file) || written != slot->size) {
        snprintf(problem, sizeof problem, "cannot write %s", path);
        return problem;
    }
    return NULL;
}

static const char *read_state(Slot *slot, const char *name) {
    static char problem[600];
    char path[512];
    state_path(name, path, sizeof path);
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(problem, sizeof problem, "cannot read %s", path);
        return problem;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char *data = size > 0 ? (unsigned char *)malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(data);
        snprintf(problem, sizeof problem, "cannot read %s", path);
        return problem;
    }
    fclose(file);
    const char *invalid = check_state(data, (size_t)size);
    if (invalid) {
        free(data);
        snprintf(problem, sizeof problem, "%s: %s", path, invalid);
        return problem;
    }
    free(slot->data);
    slot->data = data;
    slot->size = (size_t)size;
    slot->save_ms = 0;
    slot->load_ms = 0;
    slot->loads = 0;
    slot->shown = 0;
    return NULL;
}

static void print_slot(DebugReply *reply, int index) {
    const Slot *slot = &slots[index];
    const Header *header = slot_header(slot);
    debug_print(reply, "{\"slot\": %d, \"used\": %d", index, header != NULL);
    if (header)
        debug_print(reply, ", \"frame\": %u, \"field\": %llu, \"loop\": %d, \"scene\": \"%s\", \"map\": %d, \"taken\": %lld, "
                           "\"bytes\": %zu, \"save_ms\": %.2f, \"load_ms\": %.2f, \"loads\": %d",
                    header->frame, (unsigned long long)header->field, header->loop, loop_name(header->loop), header->map,
                    (long long)header->taken, slot->size, slot->save_ms, slot->load_ms, slot->loads);
    debug_print(reply, "}");
}

static void finish_request(void) {
    Slot *slot = &slots[request.slot];
    float ms = (float)host_ns / 1e6f;
    DebugReply reply;
    memset(&reply, 0, sizeof reply);
    reply.client = request.client;
    reply.status = 200;
    const char *problem = host_error[0] ? host_error : NULL;
    if (!problem && request.kind == SAVE) {
        slot->save_ms = ms;
        show_thumbnail(slot);
        if (request.path[0]) problem = write_state(slot, request.path);
    }
    if (!problem && request.kind == LOAD) {
        slot->load_ms = ms;
        slot->loads++;
        show_thumbnail(slot);
    }
    if (problem) {
        debug_fail(&reply, "%s", problem);
        snprintf(message, sizeof message, "%s slot %d failed: %s", request.kind == SAVE ? "save" : "load", request.slot,
                 problem);
    } else {
        debug_print(&reply, "{\"%s\": ", request.kind == SAVE ? "saved" : "loaded");
        print_slot(&reply, request.slot);
        debug_print(&reply, ", \"ms\": %.2f, \"at_frame\": %u}", ms, debug_frame_total);
        snprintf(message, sizeof message, "%s slot %d in %.1f ms (%.1f MB)", request.kind == SAVE ? "saved" : "loaded",
                 request.slot, ms, (double)slot->size / 1048576.0);
    }
    request.kind = 0;
    debug_finish(&reply);
}

void debug_savestate_frame(void) {
    if (debug_frame_total == frame_seen) pause_hook = 1;
    frame_seen = debug_frame_total;
    if (!request.kind) return;
    host_error[0] = 0;
    host_ns = 0;
    if (request.kind == SAVE) {
        if (!port_movie_end() && port_movie_started()) {
            snprintf(host_error, sizeof host_error, "a movie is playing; save after it");
            finish_request();
            return;
        }
        capture_thumbnail();
    }
    port_kernel_host();
    finish_request();
}

static int request_state(const DebugArgs *args, DebugReply *reply, int kind) {
    long slot = debug_arg_int(args, "slot", 0);
    const char *path = debug_arg(args, "path", "");
    if (slot < 0 || slot >= SLOT_LIMIT) return debug_fail(reply, "slot must be 0 to %d", SLOT_LIMIT - 1);
    if (request.kind) return debug_fail(reply, "a savestate is already waiting for the end of the frame");
    if (!layout_ready()) return debug_fail(reply, "%s", layout_error);
    if (strlen(path) >= sizeof request.path) return debug_fail(reply, "path is too long");
    if (kind == LOAD && *path) {
        const char *problem = read_state(&slots[slot], path);
        if (problem) return debug_fail(reply, "%s", problem);
    }
    if (kind == LOAD && !slots[slot].data) return debug_fail(reply, "slot %ld is empty", slot);
    request.kind = kind;
    request.slot = (int)slot;
    request.client = reply->client;
    snprintf(request.path, sizeof request.path, "%s", kind == SAVE ? path : "");
    if (debug_paused && debug_steps == 0 && !pause_hook) debug_steps = 1;
    reply->deferred = 1;
    return 0;
}

static int command_save(const DebugArgs *args, DebugReply *reply) {
    return request_state(args, reply, SAVE);
}

static int command_load(const DebugArgs *args, DebugReply *reply) {
    return request_state(args, reply, LOAD);
}

static int command_savestates(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_print(reply, "{\"pending\": %d, \"slots\": [", request.kind);
    for (int i = 0; i < SLOT_LIMIT; ++i) {
        if (i) debug_print(reply, ", ");
        print_slot(reply, i);
    }
    debug_print(reply, "], \"message\": ");
    debug_print_string(reply, message);
    debug_print(reply, "}");
    return 0;
}

extern const DebugCommand debug_savestate_commands[] = {
    {"save", command_save, 1, "save the whole game into slot=N (0-7) at the end of the frame; path=name also writes "
                              "build/port/savestates/name"},
    {"load", command_load, 1, "load slot=N at the end of the frame; path=name first reads build/port/savestates/name"},
    {"savestates", command_savestates, 1, "list the savestate slots"},
    {NULL, NULL, 0, NULL},
};

static void describe_age(char *text, size_t size, int64_t taken) {
    long seconds = (long)(time(NULL) - taken);
    if (seconds < 60) snprintf(text, size, "%lds ago", seconds);
    else if (seconds < 3600) snprintf(text, size, "%ldm ago", seconds / 60);
    else snprintf(text, size, "%ldh ago", seconds / 3600);
}

void debug_savestate_panel(void) {
    if (!layout_ready()) {
        ImGui::TextWrapped("savestates are unavailable: %s", layout_error);
        return;
    }
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("file", path_text, sizeof path_text);
    ImGui::SameLine();
    ImGui::TextDisabled("under build/port/savestates");
    if (request.kind) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s slot %d at the end of the frame",
                                         request.kind == SAVE ? "saving" : "loading", request.slot);
    else ImGui::TextUnformatted(message[0] ? message : "no savestate taken yet");
    if (!ImGui::BeginTable("savestates", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) return;
    ImGui::TableSetupColumn("frame", ImGuiTableColumnFlags_WidthFixed, (float)THUMB_WIDTH + 8);
    ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableHeadersRow();
    for (int i = 0; i < SLOT_LIMIT; ++i) {
        Slot *slot = &slots[i];
        const Header *header = slot_header(slot);
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        SDL_GPUTexture *texture = slot->shown ? render_gpu_texture(slot->texture) : NULL;
        if (texture) ImGui::Image((ImTextureID)(uintptr_t)texture, ImVec2(THUMB_WIDTH, THUMB_HEIGHT));
        else ImGui::Dummy(ImVec2(THUMB_WIDTH, header ? THUMB_HEIGHT : 24));
        ImGui::TableNextColumn();
        ImGui::Text("slot %d", i);
        if (header) {
            char age[32];
            describe_age(age, sizeof age, header->taken);
            ImGui::Text("frame %u, field %llu", header->frame, (unsigned long long)header->field);
            ImGui::Text("%s, map %d", loop_name(header->loop), header->map);
            ImGui::Text("%s, %.1f MB", age, (double)slot->size / 1048576.0);
            ImGui::Text("save %.1f ms, load %.1f ms, %d loads", slot->save_ms, slot->load_ms, slot->loads);
        } else {
            ImGui::TextDisabled("empty");
        }
        ImGui::TableNextColumn();
        char line[320];
        if (ImGui::Button("Save", ImVec2(70, 0))) {
            snprintf(line, sizeof line, "save?slot=%d", i);
            debug_run(line);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!header);
        if (ImGui::Button("Load", ImVec2(70, 0))) {
            snprintf(line, sizeof line, "load?slot=%d", i);
            debug_run(line);
        }
        if (ImGui::Button("Write", ImVec2(70, 0))) {
            const char *problem = write_state(slot, path_text);
            snprintf(message, sizeof message, "%s", problem ? problem : "written");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Read", ImVec2(70, 0))) {
            snprintf(line, sizeof line, "load?slot=%d&path=%s", i, path_text);
            debug_run(line);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}
