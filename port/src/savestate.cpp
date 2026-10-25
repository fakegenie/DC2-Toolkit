#include "port_state.h"
#include "draw.hpp"
#include "mainloop.hpp"
#include "movie.h"
#include "port_graphics.h"
#include "port_kernel.h"
#include "port_mod.h"
#include "port_present.h"
#include "port_arena.h"
#include "port_stats.h"
#include "port_symbols.h"
#include "render.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#endif

extern "C" {
#include "audio.h"
extern long __stack_chk_guard;
}

enum { PAGE = 4096, RUN_LIMIT = 1024, STATE_VERSION = 7 };
static const char STATE_MAGIC[8] = {'D', 'C', '2', 'S', 'T', 'A', 'T', 'E'};
constexpr size_t LAYOUT_TEXT_SIZE = sizeof PortStateHeader::layout * 2 + 1;
enum { THUMB_BYTES = STATE_THUMB_WIDTH * STATE_THUMB_HEIGHT * 4 };

typedef struct {
    uint64_t start;
    uint64_t size;
} Region;

typedef struct {
    uintptr_t start;
    size_t size;
} Run;

typedef struct {
    uint64_t start;
    std::vector<unsigned char> bytes;
} Piece;

typedef struct {
    const PortSymbols *from;
    const PortSymbols *to;
    std::map<std::pair<std::string, std::string>, const PortSymbol *> keys;
    std::unordered_map<std::string, uint64_t> strings;
    uint64_t low;
    uint64_t high;
    uint64_t stack_low;
    uint64_t stack_high;
    int every_return;
    std::unordered_set<uint64_t> returns;
    uint64_t old_slide;
    uint64_t new_slide;
    int same;
    std::vector<PortArenaSpan> arena;
    uint64_t arena_low;
    uint64_t arena_size;
    uint64_t moved;
    uint64_t unresolved;
    std::vector<std::string> changed;
} Move;

static const char *const kept_symbols[][2] = {
    {"port_kernel.o", "_host_context"}, {"port_cdvd.o", "_disc"}, {"port_cdvd.o", "_disc_size"},
    {"port_graphics.o", "_next_field"}, {"port_graphics.o", "_port_field_period"},
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
static int kept_seen[sizeof kept_symbols / sizeof kept_symbols[0]];
static uint32_t thumbnail[STATE_THUMB_WIDTH * STATE_THUMB_HEIGHT];
static int request_kind;
static PortState *request_state;
static char problem[1024];
static float host_ms;
static int force_load;
static char note[256];
static PortSymbols old_build;
static char old_layout[LAYOUT_TEXT_SIZE];
static char builds_folder[1024];


#ifdef __APPLE__
static uint64_t image_base(void) {
    return (uint64_t)(uintptr_t)_dyld_get_image_header(0);
}
#else
static uint64_t image_base(void) {
    return 0;
}
#endif

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void layout_text(const unsigned char *layout, char *text) {
    for (size_t i = 0; i < sizeof PortStateHeader::layout; ++i) snprintf(text + i * 2, 3, "%02x", layout[i]);
}

static void build_path(const unsigned char *layout, char *path, size_t size) {
    char text[LAYOUT_TEXT_SIZE];
    layout_text(layout, text);
    const char *executable = port_symbols_executable();
    const char *slash = strrchr(executable, '/');
    int folder = slash ? (int)(slash - executable) : 0;
    const char *name = slash ? slash + 1 : executable;
    if (builds_folder[0]) snprintf(path, size, "%s/%s/%s", builds_folder, text, name);
    else snprintf(path, size, "%.*s/builds/%s/%s", folder, executable, text, name);
}

static void make_folders(char *path) {
    for (char *at = path + 1; *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        mkdir(path, 0755);
        *at = '/';
    }
}

static const char *keep_build(void) {
    static char failure[1200];
    char path[1024];
    build_path((const unsigned char *)layout_id, path, sizeof path);
    struct stat existing;
    if (!stat(path, &existing)) return NULL;
    make_folders(path);
    FILE *in = fopen(port_symbols_executable(), "rb");
    char temporary[1100];
    snprintf(temporary, sizeof temporary, "%s.part", path);
    FILE *out = fopen(temporary, "wb");
    char buffer[1 << 16];
    size_t length = 0;
    int ok = in && out;
    while (ok && (length = fread(buffer, 1, sizeof buffer, in)) > 0) ok = fwrite(buffer, 1, length, out) == length;
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    if (!ok || rename(temporary, path)) {
        remove(temporary);
        snprintf(failure, sizeof failure, "the state was written but %s could not be kept", path);
        return failure;
    }
    chmod(path, 0755);
    return NULL;
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
                if (strcmp(section[s].segname, "__PS2RAM") && strcmp(section[s].segname, "__PS2PORT")) continue;
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
        int restore = !kept_symbol(object, name);
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

static void restore_bytes(uint64_t start, const unsigned char *data, size_t size) {
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
    std::vector<Run> regions(runs, runs + run_count);
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
    unsigned char *data = state->room >= size ? state->data : (unsigned char *)malloc(size);
    if (!data) {
        snprintf(problem, sizeof problem, "no memory for a %zu byte savestate", size);
        return;
    }
    PortStateHeader header;
    memset(&header, 0, sizeof header);
    memcpy(header.magic, STATE_MAGIC, sizeof header.magic);
    header.version = STATE_VERSION;
    header.regions = (uint32_t)regions.size();
    memcpy(header.layout, layout_id, sizeof header.layout);
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
    header.guard = (uint64_t)__stack_chk_guard;
    port_kernel_stacks(&header.stack_low, &header.stack_high);
    header.return_count = port_kernel_returns(header.return_slots, STATE_RETURN_LIMIT);
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
    if (data != state->data) {
        free(state->data);
        state->room = size;
    }
    state->data = data;
    state->size = size;
}

static int kept(const unsigned char *layout) {
    char path[1024];
    struct stat existing;
    build_path(layout, path, sizeof path);
    return !stat(path, &existing);
}

static int image_segment(const PortSection &section) {
    return strcmp(section.segment, "__PAGEZERO") && strcmp(section.segment, "__LINKEDIT");
}

static uint64_t text_base(const PortSymbols *symbols) {
    for (const PortSection &section : symbols->sections)
        if (!strcmp(section.segment, "__TEXT")) return section.address - section.offset;
    return 0;
}

static int string_section(const PortSection *section) {
    return section && (!strcmp(section->name, "__cstring") || !strcmp(section->name, "__oslogstring"));
}

static const char *open_old_build(const unsigned char *layout) {
    char text[LAYOUT_TEXT_SIZE];
    layout_text(layout, text);
    if (!strcmp(text, old_layout)) return NULL;
    old_layout[0] = 0;
    char path[1024];
    build_path(layout, path, sizeof path);
    const char *failed = port_symbols_load(path, &old_build);
    if (failed) return failed;
    snprintf(old_layout, sizeof old_layout, "%s", text);
    return NULL;
}

static void prepare_move(Move *move, const PortStateHeader *header, const PortSymbols *from, const PortSymbols *to) {
    move->old_slide = header->base - text_base(from);
    move->new_slide = image_base() - text_base(to);
    move->same = from == to;
    move->stack_low = header->stack_low;
    move->stack_high = header->stack_high;
    move->every_return = header->return_count > STATE_RETURN_LIMIT;
    for (uint64_t i = 0; i < header->return_count && i < STATE_RETURN_LIMIT; ++i) move->returns.insert(header->return_slots[i]);
    move->from = from;
    move->to = to;
    move->moved = move->unresolved = 0;
    for (const PortSymbol &symbol : to->symbols) move->keys.emplace(std::make_pair(symbol.name, symbol.object), &symbol);
    for (const PortSection &section : to->sections) {
        if (!string_section(&section)) continue;
        const char *text = (const char *)to->file.data() + section.offset;
        for (uint64_t at = 0; at < section.size;) {
            size_t length = strnlen(text + at, (size_t)(section.size - at));
            move->strings.emplace(std::string(text + at, length), section.address + at);
            at += length + 1;
        }
    }
    move->low = UINT64_MAX;
    move->high = 0;
    for (const PortSection &section : from->sections) {
        if (!image_segment(section) || !section.size) continue;
        if (section.address < move->low) move->low = section.address;
        if (section.address + section.size > move->high) move->high = section.address + section.size;
    }
}

static void note_name(std::vector<std::string> *names, const std::string &name) {
    for (const std::string &seen : *names)
        if (seen == name) return;
    names->push_back(name);
}

static int live_return(const Move *move, uint64_t where) {
    if (where < move->stack_low || where >= move->stack_high || move->every_return) return 1;
    return move->returns.count(where) != 0;
}

static int in_arena_block(const Move *move, uint64_t value) {
    for (const PortArenaSpan &span : move->arena)
        if (value >= span.start && value <= span.live_end) return 1;
    return 0;
}

static int pointer_like(const Move *move, uint64_t where, uint64_t value, int engine) {
    const PortSection *section = port_section_at(move->from, value - move->old_slide);
    if (!section) return 0;
    int stack = where >= move->stack_low && where < move->stack_high;
    if (engine && !stack && !strcmp(section->segment, "__DATA")) return 0;
    if (move->arena.empty() || value - move->arena_low >= move->arena_size) return 1;
    return in_arena_block(move, value);
}

static int move_address(Move *move, uint64_t where, uint64_t value, uint64_t *moved) {
    const PortSection *section = port_section_at(move->from, value);
    if (!section) return 0;
    if (move->same) {
        *moved = value;
        return 1;
    }
    if (string_section(section)) {
        const char *base = (const char *)move->from->file.data() + section->offset;
        uint64_t at = value - section->address;
        uint64_t start = at;
        while (start > 0 && base[start - 1]) start--;
        size_t length = strnlen(base + start, (size_t)(section->size - start));
        auto found = move->strings.find(std::string(base + start, length));
        if (found == move->strings.end()) return 0;
        *moved = found->second + (at - start);
        return 1;
    }
    const PortSymbol *symbol = port_symbol_at(move->from, value);
    if (!symbol) return 0;
    auto found = move->keys.find(std::make_pair(symbol->name, symbol->object));
    if (found == move->keys.end()) return 0;
    uint64_t offset = value - symbol->address;
    if (symbol->code && offset && found->second->size != symbol->size && live_return(move, where))
        note_name(&move->changed, symbol->name);
    *moved = found->second->address + offset;
    return 1;
}

static void move_words(Move *move, unsigned char *bytes, uint64_t start, size_t size, int engine) {
    for (size_t at = (size_t)((4 - start % 4) % 4); at + 8 <= size; at += 4) {
        uint64_t value;
        memcpy(&value, bytes + at, 8);
        uint64_t image = value - move->old_slide;
        if (image < move->low || image >= move->high || !pointer_like(move, start + at, value, engine)) continue;
        uint64_t moved;
        if (!move_address(move, start + at, image, &moved)) {
            move->unresolved++;
            continue;
        }
        moved += move->new_slide;
        if (moved != value) memcpy(bytes + at, &moved, 8);
        move->moved++;
        at += 4;
    }
}

static void decode_regions(const unsigned char *data, std::vector<Piece> *pieces, const unsigned char **tail) {
    const PortStateHeader *header = (const PortStateHeader *)data;
    const unsigned char *at = data + sizeof(PortStateHeader) + THUMB_BYTES;
    for (uint32_t i = 0; i < header->regions; ++i) {
        Region record;
        memcpy(&record, at, sizeof record);
        at += sizeof record;
        size_t pages = (record.size + PAGE - 1) / PAGE;
        const unsigned char *map = at;
        at += pages;
        Piece piece;
        piece.start = record.start;
        piece.bytes.assign((size_t)record.size, 0);
        for (size_t page = 0; page < pages; ++page) {
            size_t length = page_length(record.size, page);
            if (!map[page]) continue;
            memcpy(piece.bytes.data() + page * PAGE, at, length);
            at += length;
        }
        pieces->push_back(std::move(piece));
    }
    *tail = at;
}

static void place_piece(Move *move, const Piece &piece, int write) {
    uint64_t start = piece.start - move->old_slide;
    uint64_t end = start + piece.bytes.size();
    const std::vector<PortSymbol> &list = move->from->symbols;
    size_t low = 0, high = list.size();
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (list[middle].address + list[middle].size <= start) low = middle + 1;
        else high = middle;
    }
    for (size_t i = low; i < list.size() && list[i].address < end; ++i) {
        const PortSymbol &symbol = list[i];
        uint64_t first = symbol.address > start ? symbol.address : start;
        uint64_t last = symbol.address + symbol.size < end ? symbol.address + symbol.size : end;
        if (symbol.code || first >= last) continue;
        auto found = move->keys.find(std::make_pair(symbol.name, symbol.object));
        if (found == move->keys.end()) continue;
        uint64_t room = found->second->size < symbol.size ? found->second->size : symbol.size;
        if (found->second->size != symbol.size) note_name(&move->changed, symbol.name);
        if (last > symbol.address + room) last = symbol.address + room;
        if (!write || first >= last) continue;
        restore_bytes(found->second->address + move->new_slide + (first - symbol.address),
                      piece.bytes.data() + (first - start), (size_t)(last - first));
    }
}

static void swap_guard(std::vector<Piece> *pieces, uint64_t old_guard) {
    uint64_t guard = (uint64_t)__stack_chk_guard;
    if (old_guard == guard) return;
    for (Piece &piece : *pieces)
        for (size_t at = (size_t)((8 - piece.start % 8) % 8); at + 8 <= piece.bytes.size(); at += 8) {
            uint64_t value;
            memcpy(&value, piece.bytes.data() + at, 8);
            if (value == old_guard) memcpy(piece.bytes.data() + at, &guard, 8);
        }
}

static size_t add_names(size_t length, const char *intro, const std::vector<std::string> &names, size_t shown) {
    if (length < sizeof problem) length += (size_t)snprintf(problem + length, sizeof problem - length, "%s", intro);
    for (size_t i = 0; i < names.size() && i < shown && length < sizeof problem; ++i)
        length += (size_t)snprintf(problem + length, sizeof problem - length, " %s", port_symbol_pretty(names[i]).c_str());
    if (names.size() > shown && length < sizeof problem)
        length += (size_t)snprintf(problem + length, sizeof problem - length, " and %zu more", names.size() - shown);
    if (length < sizeof problem) length += (size_t)snprintf(problem + length, sizeof problem - length, "; ");
    return length;
}

static void find_arena(Move *move, const std::vector<Piece> &pieces, uint64_t blocks) {
    move->arena.clear();
    for (const PortSymbol &symbol : move->from->symbols) {
        if (symbol.name != "_arena" || symbol.object != "port_memory.o") continue;
        uint64_t at = symbol.address + move->old_slide;
        for (const Piece &piece : pieces) {
            if (at < piece.start || at + symbol.size > piece.start + piece.bytes.size()) continue;
            move->arena.resize((size_t)(blocks * 2 + 2));
            move->arena.resize(port_arena_spans(piece.bytes.data() + (at - piece.start), symbol.size, at,
                                                move->arena.data(), move->arena.size()));
            move->arena_low = at;
            move->arena_size = symbol.size;
        }
    }
}

static int move_pieces(const PortStateHeader *header, std::vector<Piece> *pieces, Move *move) {
    int same = !memcmp(header->layout, layout_id, sizeof header->layout);
    const char *failed = same ? NULL : open_old_build(header->layout);
    const char *error = NULL;
    const PortSymbols *current = port_symbols_self(&error);
    if (failed || !current) {
        snprintf(problem, sizeof problem, "%s", failed ? failed : error);
        return 0;
    }
    prepare_move(move, header, same ? current : &old_build, current);
    find_arena(move, *pieces, header->arena_blocks);
    for (Piece &piece : *pieces) move_words(move, piece.bytes.data(), piece.start, piece.bytes.size(), 1);
    for (const Piece &piece : *pieces) place_piece(move, piece, 0);
    if (move->changed.empty() || force_load) return 1;
    size_t length = add_names(0, "this build sizes these differently:", move->changed, 6);
    if (length < sizeof problem) snprintf(problem + length, sizeof problem - length, "force=1 loads anyway");
    return 0;
}

static const char *check_state(const unsigned char *data, size_t size) {
    if (size < sizeof(PortStateHeader) + THUMB_BYTES) return "the savestate is truncated";
    const PortStateHeader *header = (const PortStateHeader *)data;
    if (memcmp(header->magic, STATE_MAGIC, sizeof header->magic) || header->version != STATE_VERSION) return "not a savestate of this version";
    if (memcmp(header->layout, layout_id, sizeof header->layout) && !kept(header->layout))
        return "the savestate is from another build, and that build was not kept beside this one";
    if (header->size != size) return "the savestate is truncated";
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
    std::vector<Piece> pieces;
    const unsigned char *tail = NULL;
    decode_regions(state->data, &pieces, &tail);
    std::vector<unsigned char> sound(tail, (const unsigned char *)state->data + state->size);
    swap_guard(&pieces, header->guard);
    int rebuilt = memcmp(header->layout, layout_id, sizeof header->layout) != 0;
    int moved = rebuilt || header->base != image_base();
    Move move;
    if (moved && !move_pieces(header, &pieces, &move)) return;
    if (!port_movie_end()) port_movie_close();
    for (const Piece &piece : pieces) {
        if (moved) place_piece(&move, piece, 1);
        else restore_bytes(piece.start, piece.bytes.data(), piece.bytes.size());
    }
    unsigned char *visual = sound.data() + audio_load(sound.data());
    std::vector<size_t> keys;
    if (moved) visual_keys(visual, &keys);
    for (size_t key : keys) move_words(&move, visual + key, 0, 8, 0);
    visual_load(visual);
    port_stats.arena_used = header->arena_used;
    port_stats.arena_peak = header->arena_peak;
    port_stats.arena_blocks = header->arena_blocks;
    if (moved)
        snprintf(note, sizeof note, "from %s%s: %llu pointers moved, %llu left as they were%s",
                 rebuilt ? "build " : "a launch at another address", rebuilt ? old_layout : "",
                 (unsigned long long)move.moved, (unsigned long long)move.unresolved,
                 move.changed.empty() ? "" : ", forced");
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

const char *port_state_note(void) { return note[0] ? note : NULL; }

void port_state_force(int force) { force_load = force; }

float port_state_ms(void) { return host_ms; }

void port_state_builds(const char *folder) { snprintf(builds_folder, sizeof builds_folder, "%s", folder); }

void port_state_drop_builds(void) {
    if (!builds_folder[0]) return;
    char current[LAYOUT_TEXT_SIZE];
    layout_text((const unsigned char *)layout_id, current);
    const char *executable = port_symbols_executable();
    const char *slash = strrchr(executable, '/');
    const char *name = slash ? slash + 1 : executable;
    DIR *folder = opendir(builds_folder);
    if (!folder) return;
    for (struct dirent *entry = readdir(folder); entry; entry = readdir(folder)) {
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, current)) continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s/%s", builds_folder, entry->d_name, name);
        remove(path);
        snprintf(path, sizeof path, "%s/%s", builds_folder, entry->d_name);
        rmdir(path);
    }
    closedir(folder);
}

const char *port_state_keep(void) { return port_state_ready() ? port_state_ready() : keep_build(); }

STATE_RESUMABLE void port_state_frame(void) {
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
    const PortStateHeader *header = port_state_header(state);
    return header && !memcmp(header->layout, layout_id, sizeof header->layout) ? keep_build() : NULL;
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
    state->room = (size_t)size;
    return NULL;
}
