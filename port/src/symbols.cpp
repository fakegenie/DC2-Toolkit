#include "port_symbols.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>

extern "C" {
extern char **port_arguments;
extern const char port_build_id[];
}

enum { TABLE_VERSION = 1, LINE_SIZE = 4096 };

static PortSymbols self;
static int self_state;
static char self_error[700];

static bool earlier(const PortSymbol &a, const PortSymbol &b) { return a.address < b.address; }

// NAME.symbols in the program's folder, NAME being the program's file name without .exe
const char *port_symbols_path(void) {
    static char path[1024];
    if (path[0] || !port_arguments || !port_arguments[0]) return path;
    const char *program = port_arguments[0];
    for (const char *at = program; *at; ++at)
        if (*at == '/' || *at == '\\') program = at + 1;
    size_t length = strlen(program);
    if (length > 4 && !SDL_strcasecmp(program + length - 4, ".exe")) length -= 4;
    const char *base = SDL_GetBasePath();
    snprintf(path, sizeof path, "%s%.*s.symbols", base ? base : "", (int)length, program);
    return path;
}

// The table's addresses are the link's: anchor is where port_build_id was, and the difference to where it is now
// moves them to this launch.
const char *port_symbols_load(const char *path, PortSymbols *symbols) {
    static char error[700];
    symbols->symbols.clear();
    symbols->sections.clear();
    symbols->path = path;
    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(error, sizeof error, "cannot read %s; tools/port.py --debug writes it beside the debug build", path);
        return error;
    }
    char line[LINE_SIZE], name[LINE_SIZE], object[LINE_SIZE], flags[8];
    int version = 0;
    uint64_t anchor = 0, address, size;
    int code;
    if (!fgets(line, sizeof line, file) || sscanf(line, "dc2 symbols %d", &version) != 1 || version != TABLE_VERSION ||
        !fgets(line, sizeof line, file) || sscanf(line, "anchor %" SCNx64, &anchor) != 1) {
        fclose(file);
        snprintf(error, sizeof error, "%s is not a version %d symbol table", path, TABLE_VERSION);
        return error;
    }
    uint64_t slide = (uint64_t)(uintptr_t)port_build_id - anchor;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "section %" SCNx64 " %" SCNx64 " %7s %4095s", &address, &size, flags, name) == 4) {
            symbols->sections.push_back({address + slide, size, strchr(flags, 'x') != NULL, strchr(flags, 'w') != NULL, name});
        } else if (sscanf(line, "symbol %" SCNx64 " %" SCNx64 " %d %4095s %4095s", &address, &size, &code, object, name) == 5) {
            symbols->symbols.push_back({address + slide, size, name, object, code});
        }
    }
    fclose(file);
    std::stable_sort(symbols->symbols.begin(), symbols->symbols.end(), earlier);
    return NULL;
}

const PortSymbols *port_symbols_self(const char **error) {
    if (!self_state) {
        const char *failed = port_symbols_load(port_symbols_path(), &self);
        if (failed) snprintf(self_error, sizeof self_error, "%s", failed);
        self_state = failed ? -1 : 1;
    }
    if (error) *error = self_state > 0 ? NULL : self_error;
    return self_state > 0 ? &self : NULL;
}

const PortSymbol *port_symbol_at(const PortSymbols *symbols, uint64_t address) {
    const std::vector<PortSymbol> &list = symbols->symbols;
    size_t low = 0, high = list.size();
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (list[middle].address <= address) low = middle + 1;
        else high = middle;
    }
    while (low > 0) {
        const PortSymbol *symbol = &list[--low];
        if (address < symbol->address + symbol->size) return symbol;
        if (symbol->size) return NULL;
    }
    return NULL;
}

const PortSection *port_section_at(const PortSymbols *symbols, uint64_t address) {
    for (const PortSection &section : symbols->sections)
        if (address >= section.address && address - section.address < section.size) return &section;
    return NULL;
}

std::string port_symbol_pretty(const std::string &name) {
    int status = 0;
    char *demangled = abi::__cxa_demangle(name.c_str(), NULL, NULL, &status);
    std::string pretty = status == 0 && demangled ? demangled : name;
    free(demangled);
    return pretty;
}

extern "C" int port_symbol_describe(uint64_t address, char *text, size_t size) {
    const PortSymbols *symbols = port_symbols_self(NULL);
    const PortSymbol *symbol = symbols ? port_symbol_at(symbols, address) : NULL;
    if (!symbol) return 0;
    snprintf(text, size, "%s+0x%" PRIx64 " (%s)", port_symbol_pretty(symbol->name).c_str(), address - symbol->address,
             symbol->object.c_str());
    return 1;
}
