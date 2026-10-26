#include "debug.h"
#include "imgui.h"
#include "port_arena.h"
#include "port_symbols.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

enum { WATCH_LIMIT = 32, MATCH_LIMIT = 40, VALUE_LIMIT = 64 };

typedef struct {
    const char *name;
    int size;
} Type;

typedef struct {
    char label[160];
    uint64_t address;
    int type;
    int count;
} Watch;

typedef struct {
    const PortSymbol *symbol;
    uint64_t address;
    uint64_t room;
    char label[160];
} Target;

enum {
    TYPE_I8, TYPE_U8, TYPE_I16, TYPE_U16, TYPE_I32, TYPE_U32, TYPE_I64, TYPE_U64, TYPE_F32, TYPE_F64, TYPE_PTR,
    TYPE_STR, TYPE_HEX
};

static const Type types[] = {{"i8", 1},  {"u8", 1},  {"i16", 2}, {"u16", 2}, {"i32", 4}, {"u32", 4}, {"i64", 8},
                             {"u64", 8}, {"f32", 4}, {"f64", 8}, {"ptr", 8}, {"str", 1}, {"hex", 1}};
static_assert(sizeof types / sizeof types[0] == TYPE_HEX + 1);

static Watch watches[WATCH_LIMIT];
static int watch_count;
static std::vector<std::string> pretty_names;
static char add_text[160];
static char add_type[8] = "i32";

static int type_index(const char *name) {
    for (int i = 0; i < (int)(sizeof types / sizeof types[0]); ++i)
        if (!strcmp(types[i].name, name)) return i;
    return -1;
}

static const PortSymbols *symbols(void) { return port_symbols_self(NULL); }

static void name_symbols(const PortSymbols *found) {
    if (pretty_names.size() == found->symbols.size()) return;
    pretty_names.clear();
    for (const PortSymbol &symbol : found->symbols) pretty_names.push_back(port_symbol_pretty(symbol.name));
}

static int named(size_t index, const PortSymbol &symbol, const char *name) {
    const std::string &pretty = pretty_names[index];
    size_t length = strlen(name);
    if (pretty == name || symbol.name == name) return 1;
    return pretty.size() > length + 2 && !pretty.compare(pretty.size() - length, length, name) &&
           !pretty.compare(pretty.size() - length - 2, 2, "::");
}

// Memory the port knows is there: the program's sections, from the symbol table, and the engine's arena. Writes go only
// to sections that can be written and to the arena.
static int within(uint64_t address, size_t size, int writing) {
    uint64_t start, length;
    port_arena_bounds(&start, &length);
    if (address >= start && address - start <= length && size <= length - (address - start)) return 1;
    const PortSymbols *found = symbols();
    const PortSection *section = found ? port_section_at(found, address) : NULL;
    return section && (section->writable || !writing) && size <= section->size - (address - section->address);
}

static int readable(uint64_t address, void *out, size_t size) {
    if (!within(address, size, 0)) return 0;
    memcpy(out, (const void *)(uintptr_t)address, size);
    return 1;
}

static int missing_target(const DebugArgs *args, DebugReply *reply, Target *target) {
    const char *error = NULL;
    const PortSymbols *found = port_symbols_self(&error);
    const char *address = debug_arg(args, "address", NULL);
    long offset = debug_arg_int(args, "offset", 0);
    memset(target, 0, sizeof *target);
    if (address) {
        target->address = strtoull(address, NULL, 0) + (uint64_t)offset;
        target->room = UINT64_MAX;
        target->symbol = found ? port_symbol_at(found, target->address) : NULL;
        snprintf(target->label, sizeof target->label, "0x%llx", (unsigned long long)target->address);
        return 0;
    }
    const char *name = debug_arg(args, "name", NULL);
    if (!name) return debug_fail(reply, "name=SYMBOL or address=0x... [offset=N]");
    if (!found) return debug_fail(reply, "%s", error);
    name_symbols(found);
    const char *in = debug_arg(args, "in", "");
    std::vector<size_t> hits;
    for (size_t i = 0; i < found->symbols.size(); ++i) {
        const PortSymbol &symbol = found->symbols[i];
        if (in[0] && symbol.object.find(in) == std::string::npos) continue;
        if (named(i, symbol, name)) hits.push_back(i);
    }
    if (hits.size() != 1) {
        debug_print(reply, "{\"error\": \"%s\", \"matches\": [", hits.empty() ? "no symbol has that name; try symbols match=..." : "several symbols have that name; add in=OBJECT");
        for (size_t i = 0; i < hits.size() && i < MATCH_LIMIT; ++i) {
            const PortSymbol &symbol = found->symbols[hits[i]];
            debug_print(reply, "%s{\"name\": ", i ? ", " : "");
            debug_print_string(reply, pretty_names[hits[i]].c_str());
            debug_print(reply, ", \"object\": \"%s\", \"address\": \"0x%llx\"}", symbol.object.c_str(), (unsigned long long)symbol.address);
        }
        debug_print(reply, "]}");
        reply->status = 400;
        return 1;
    }
    const PortSymbol *symbol = &found->symbols[hits[0]];
    target->symbol = symbol;
    target->address = symbol->address + (uint64_t)offset;
    target->room = offset >= 0 && (uint64_t)offset < symbol->size ? symbol->size - (uint64_t)offset : 0;
    snprintf(target->label, sizeof target->label, "%s%s%ld", pretty_names[hits[0]].c_str(), offset ? "+" : "", offset);
    if (!offset) snprintf(target->label, sizeof target->label, "%s", pretty_names[hits[0]].c_str());
    return 0;
}

static void describe_pointer(char *text, size_t size, uint64_t value) {
    const PortSymbols *found = symbols();
    const PortSymbol *symbol = found && value ? port_symbol_at(found, value) : NULL;
    if (symbol)
        snprintf(text, size, "0x%llx %s+0x%llx", (unsigned long long)value, port_symbol_pretty(symbol->name).c_str(),
                 (unsigned long long)(value - symbol->address));
    else snprintf(text, size, "0x%llx", (unsigned long long)value);
}

static void format_value(char *text, size_t size, int type, const unsigned char *bytes) {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    switch (type) {
    case TYPE_I8: memcpy(&i8, bytes, 1); snprintf(text, size, "%d", i8); break;
    case TYPE_U8: snprintf(text, size, "%u", bytes[0]); break;
    case TYPE_I16: memcpy(&i16, bytes, 2); snprintf(text, size, "%d", i16); break;
    case TYPE_U16: memcpy(&i16, bytes, 2); snprintf(text, size, "%u", (uint16_t)i16); break;
    case TYPE_I32: memcpy(&i32, bytes, 4); snprintf(text, size, "%d", i32); break;
    case TYPE_U32: memcpy(&i32, bytes, 4); snprintf(text, size, "%u", (uint32_t)i32); break;
    case TYPE_I64: memcpy(&i64, bytes, 8); snprintf(text, size, "%lld", (long long)i64); break;
    case TYPE_U64: memcpy(&i64, bytes, 8); snprintf(text, size, "%llu", (unsigned long long)i64); break;
    case TYPE_F32: memcpy(&f32, bytes, 4); snprintf(text, size, std::isfinite(f32) ? "%.6g" : "\"%f\"", f32); break;
    case TYPE_F64: memcpy(&f64, bytes, 8); snprintf(text, size, std::isfinite(f64) ? "%.10g" : "\"%f\"", f64); break;
    case TYPE_PTR: memcpy(&i64, bytes, 8); describe_pointer(text, size, (uint64_t)i64); break;
    default: snprintf(text, size, "%02x", bytes[0]); break;
    }
}

static int read_values(uint64_t address, int type, int count, std::string *out) {
    int size = types[type].size;
    std::vector<unsigned char> bytes((size_t)size * (size_t)count + 1, 0);
    if (!readable(address, bytes.data(), (size_t)size * (size_t)count)) return 0;
    if (type == TYPE_STR) {
        *out = std::string((const char *)bytes.data(), strnlen((const char *)bytes.data(), (size_t)count));
        return 1;
    }
    char text[200];
    out->clear();
    for (int i = 0; i < count; ++i) {
        format_value(text, sizeof text, type, bytes.data() + i * size);
        if (i && type != TYPE_HEX) *out += ", ";
        *out += text;
    }
    return 1;
}

static void print_value(DebugReply *reply, int type, const std::string &text) {
    if (type == TYPE_STR || type == TYPE_HEX || type == TYPE_PTR) {
        debug_print_string(reply, text.c_str());
        return;
    }
    debug_print(reply, "[%s]", text.c_str());
}

static int command_peek(const DebugArgs *args, DebugReply *reply) {
    Target target;
    if (missing_target(args, reply, &target)) return 0;
    const char *type_name = debug_arg(args, "type", NULL);
    int type = type_name ? type_index(type_name) : -1;
    if (type_name && type < 0) return debug_fail(reply, "type=i8|u8|i16|u16|i32|u32|i64|u64|f32|f64|ptr|str|hex");
    int count = (int)debug_arg_int(args, "count", type == TYPE_STR ? 128 : type == TYPE_HEX ? 64 : 1);
    if (count < 1 || count > 4096) return debug_fail(reply, "count=1..4096");
    uint64_t wanted = type < 0 ? 4 : (uint64_t)types[type].size * (uint64_t)count;
    if (wanted > target.room && !debug_arg_int(args, "force", 0) && type != TYPE_STR)
        return debug_fail(reply, "%s has %llu bytes from there; force=1 reads past it", target.label, (unsigned long long)target.room);
    debug_print(reply, "{\"name\": ");
    debug_print_string(reply, target.label);
    debug_print(reply, ", \"address\": \"0x%llx\"", (unsigned long long)target.address);
    if (target.symbol) debug_print(reply, ", \"object\": \"%s\", \"size\": %llu", target.symbol->object.c_str(), (unsigned long long)target.symbol->size);
    std::string text;
    if (type >= 0) {
        if (!read_values(target.address, type, count, &text)) return debug_fail(reply, "0x%llx cannot be read", (unsigned long long)target.address);
        debug_print(reply, ", \"type\": \"%s\", \"value\": ", types[type].name);
        print_value(reply, type, text);
        debug_print(reply, "}");
        return 0;
    }
    const char *shown[] = {"i32", "u32", "f32", "ptr"};
    for (const char *name : shown) {
        int index = type_index(name);
        if ((uint64_t)types[index].size > target.room) continue;
        if (!read_values(target.address, index, 1, &text)) return debug_fail(reply, "0x%llx cannot be read", (unsigned long long)target.address);
        debug_print(reply, ", \"%s\": ", name);
        print_value(reply, index, text);
    }
    read_values(target.address, TYPE_HEX, (int)(target.room < 32 ? target.room : 32), &text);
    debug_print(reply, ", \"hex\": ");
    debug_print_string(reply, text.c_str());
    debug_print(reply, "}");
    return 0;
}

static int command_poke(const DebugArgs *args, DebugReply *reply) {
    Target target;
    if (missing_target(args, reply, &target)) return 0;
    int type = type_index(debug_arg(args, "type", ""));
    const char *value = debug_arg(args, "value", NULL);
    if (type < 0 || type >= TYPE_STR || !value) return debug_fail(reply, "type=i8|u8|i16|u16|i32|u32|i64|u64|f32|f64|ptr value=N");
    int size = types[type].size;
    if ((uint64_t)size > target.room) return debug_fail(reply, "%s has %llu bytes from there", target.label, (unsigned long long)target.room);
    unsigned char before[8];
    if (!within(target.address, (size_t)size, 1)) return debug_fail(reply, "0x%llx cannot be written", (unsigned long long)target.address);
    if (!readable(target.address, before, (size_t)size)) return debug_fail(reply, "0x%llx cannot be read", (unsigned long long)target.address);
    unsigned char bytes[8];
    float f32 = strtof(value, NULL);
    double f64 = strtod(value, NULL);
    long long whole = strtoll(value, NULL, 0);
    if (type == TYPE_F32) memcpy(bytes, &f32, sizeof f32);
    else if (type == TYPE_F64) memcpy(bytes, &f64, sizeof f64);
    else memcpy(bytes, &whole, 8);
    memcpy((void *)(uintptr_t)target.address, bytes, (size_t)size);
    std::string now;
    read_values(target.address, type, 1, &now);
    char was[200];
    format_value(was, sizeof was, type, before);
    debug_print(reply, "{\"name\": ");
    debug_print_string(reply, target.label);
    debug_print(reply, ", \"type\": \"%s\", \"was\": ", types[type].name);
    debug_print_string(reply, was);
    debug_print(reply, ", \"value\": ");
    debug_print_string(reply, now.c_str());
    debug_print(reply, "}");
    return 0;
}

static int command_symbols(const DebugArgs *args, DebugReply *reply) {
    const char *error = NULL;
    const PortSymbols *found = port_symbols_self(&error);
    if (!found) return debug_fail(reply, "%s", error);
    name_symbols(found);
    const char *match = debug_arg(args, "match", "");
    const char *in = debug_arg(args, "in", "");
    int code = (int)debug_arg_int(args, "code", 0);
    int limit = (int)debug_arg_int(args, "limit", 50);
    if (!match[0] && !in[0]) return debug_fail(reply, "match=TEXT [in=OBJECT] [code=1] [limit=50]");
    debug_print(reply, "{\"symbols\": [");
    int listed = 0, total = 0;
    for (size_t i = 0; i < found->symbols.size(); ++i) {
        const PortSymbol &symbol = found->symbols[i];
        if (symbol.code != code) continue;
        if (in[0] && symbol.object.find(in) == std::string::npos) continue;
        if (match[0] && pretty_names[i].find(match) == std::string::npos) continue;
        total++;
        if (listed >= limit) continue;
        debug_print(reply, "%s{\"name\": ", listed++ ? ", " : "");
        debug_print_string(reply, pretty_names[i].c_str());
        debug_print(reply, ", \"object\": \"%s\", \"address\": \"0x%llx\", \"size\": %llu}", symbol.object.c_str(),
                    (unsigned long long)symbol.address, (unsigned long long)symbol.size);
    }
    debug_print(reply, "], \"total\": %d}", total);
    return 0;
}

static void print_watches(DebugReply *reply) {
    debug_print(reply, "{\"watches\": [");
    for (int i = 0; i < watch_count; ++i) {
        std::string text;
        const Watch *watch = &watches[i];
        int ok = read_values(watch->address, watch->type, watch->count, &text);
        debug_print(reply, "%s{\"index\": %d, \"name\": ", i ? ", " : "", i);
        debug_print_string(reply, watch->label);
        debug_print(reply, ", \"type\": \"%s\", \"value\": ", types[watch->type].name);
        if (ok) print_value(reply, watch->type, text);
        else debug_print(reply, "null");
        debug_print(reply, "}");
    }
    debug_print(reply, "]}");
}

static int command_watch(const DebugArgs *args, DebugReply *reply) {
    if (debug_arg_int(args, "clear", 0)) watch_count = 0;
    long removed = debug_arg_int(args, "remove", -1);
    if (removed >= 0 && removed < watch_count) {
        memmove(&watches[removed], &watches[removed + 1], sizeof watches[0] * (size_t)(watch_count - removed - 1));
        watch_count--;
    }
    if (debug_arg(args, "name", NULL) || debug_arg(args, "address", NULL)) {
        if (watch_count == WATCH_LIMIT) return debug_fail(reply, "%d watches already; remove=N first", WATCH_LIMIT);
        Target target;
        if (missing_target(args, reply, &target)) return 0;
        int type = type_index(debug_arg(args, "type", "i32"));
        if (type < 0) return debug_fail(reply, "type=i8|u8|i16|u16|i32|u32|i64|u64|f32|f64|ptr|str|hex");
        Watch *watch = &watches[watch_count++];
        snprintf(watch->label, sizeof watch->label, "%s", target.label);
        watch->address = target.address;
        watch->type = type;
        watch->count = (int)debug_arg_int(args, "count", type == TYPE_STR ? 64 : 1);
        if (watch->count < 1 || watch->count > VALUE_LIMIT) watch->count = 1;
    }
    print_watches(reply);
    return 0;
}

void debug_watch_panel(void) {
    ImGui::SetNextItemWidth(320);
    ImGui::InputText("name", add_text, sizeof add_text);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputText("type", add_type, sizeof add_type);
    ImGui::SameLine();
    if (ImGui::Button("Add") && add_text[0]) {
        char line[400];
        snprintf(line, sizeof line, "watch name=%s type=%s", add_text, add_type);
        debug_run(line);
    }
    ImGui::TextDisabled("names as in the source (CastDist, UkiWaitLoop::boze_cnt); in=object picks among several");
    if (!ImGui::BeginTable("watches", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("name");
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("value");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    int removed = -1;
    for (int i = 0; i < watch_count; ++i) {
        std::string text;
        if (!read_values(watches[i].address, watches[i].type, watches[i].count, &text)) text = "(unreadable)";
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(watches[i].label);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(types[watches[i].type].name);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(text.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(i);
        if (ImGui::SmallButton("x")) removed = i;
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (removed >= 0) {
        memmove(&watches[removed], &watches[removed + 1], sizeof watches[0] * (size_t)(watch_count - removed - 1));
        watch_count--;
    }
}

extern const DebugCommand debug_watch_commands[] = {
    {"peek", command_peek, 0, "name=SYMBOL [in=OBJECT] or address=0x... [offset=N] [type=i8..u64|f32|f64|ptr|str|hex] [count=N] reads any global or static; no type shows i32, u32, f32, ptr and hex"},
    {"poke", command_poke, 0, "name=SYMBOL or address=0x... [offset=N] type=i32|f32|... value=N writes it"},
    {"symbols", command_symbols, 0, "match=TEXT [in=OBJECT] [code=1] [limit=50] lists globals and statics (functions with code=1) by source name"},
    {"watch", command_watch, 0, "name=SYMBOL or address=0x... [offset=N] [type=i32] [count=N] adds a watch; remove=N, clear=1; no arguments lists them with their values"},
    {NULL, NULL, 0, NULL},
};
