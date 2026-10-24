#include "port_symbols.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <map>
#include <utility>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#endif

static PortSymbols self;
static int self_state;
static char self_error[600];

static int read_file(const char *path, std::vector<unsigned char> *bytes) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes->resize(size > 0 ? (size_t)size : 0);
    size_t read = size > 0 ? fread(bytes->data(), 1, (size_t)size, file) : 0;
    fclose(file);
    return size > 0 && read == (size_t)size;
}

const char *port_symbols_executable(void) {
    static char path[1024];
#ifdef __APPLE__
    uint32_t length = sizeof path;
    if (!path[0] && _NSGetExecutablePath(path, &length)) path[0] = 0;
#endif
    return path;
}

#ifdef __APPLE__
static bool earlier(const PortSymbol &a, const PortSymbol &b) { return a.address < b.address; }

static int section_end(const PortSymbols *symbols, int section, uint64_t *end) {
    if (section < 1 || section > (int)symbols->sections.size()) return 0;
    const PortSection *found = &symbols->sections[(size_t)section - 1];
    *end = found->address + found->size;
    return 1;
}

static void read_sections(const unsigned char *file, PortSymbols *symbols, const symtab_command **symtab) {
    const mach_header_64 *header = (const mach_header_64 *)file;
    const load_command *command = (const load_command *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SYMTAB) *symtab = (const symtab_command *)command;
        if (command->cmd == LC_SEGMENT_64) {
            const segment_command_64 *segment = (const segment_command_64 *)command;
            const section_64 *section = (const section_64 *)(segment + 1);
            for (uint32_t s = 0; s < segment->nsects; ++s) {
                PortSection entry;
                memset(&entry, 0, sizeof entry);
                memcpy(entry.segment, section[s].segname, 16);
                memcpy(entry.name, section[s].sectname, 16);
                entry.address = section[s].addr;
                entry.size = section[s].size;
                entry.offset = section[s].offset;
                symbols->sections.push_back(entry);
            }
        }
        command = (const load_command *)((const unsigned char *)command + command->cmdsize);
    }
}

static void read_names(const unsigned char *file, const symtab_command *symtab, PortSymbols *symbols) {
    const nlist_64 *entries = (const nlist_64 *)(file + symtab->symoff);
    const char *strings = (const char *)(file + symtab->stroff);
    std::map<std::pair<uint64_t, std::string>, std::string> objects;
    std::map<std::string, std::string> global_objects;
    std::map<uint64_t, uint64_t> function_sizes;
    std::string object;
    uint64_t function = 0;
    for (uint32_t i = 0; i < symtab->nsyms; ++i) {
        const char *name = strings + entries[i].n_un.n_strx;
        uint8_t type = entries[i].n_type;
        if (type == N_OSO) {
            const char *slash = strrchr(name, '/');
            object = slash ? slash + 1 : name;
        } else if (type == N_FUN && name[0]) {
            function = entries[i].n_value;
            objects[{function, name}] = object;
        } else if (type == N_FUN) {
            function_sizes[function] = entries[i].n_value;
        } else if (type == N_STSYM) {
            objects[{entries[i].n_value, name}] = object;
        } else if (type == N_GSYM) {
            global_objects[name] = object;
        }
    }
    for (uint32_t i = 0; i < symtab->nsyms; ++i) {
        const nlist_64 *entry = &entries[i];
        if ((entry->n_type & N_STAB) || (entry->n_type & N_TYPE) != N_SECT) continue;
        PortSymbol symbol;
        symbol.address = entry->n_value;
        symbol.size = 0;
        symbol.name = strings + entry->n_un.n_strx;
        symbol.section = entry->n_sect;
        const PortSection *section = entry->n_sect >= 1 && entry->n_sect <= symbols->sections.size()
                                         ? &symbols->sections[entry->n_sect - 1u]
                                         : NULL;
        symbol.code = section && !strcmp(section->name, "__text");
        auto local = objects.find({symbol.address, symbol.name});
        auto global = global_objects.find(symbol.name);
        symbol.object = local != objects.end() ? local->second : global != global_objects.end() ? global->second : "";
        auto size = function_sizes.find(symbol.address);
        if (symbol.code && size != function_sizes.end()) symbol.size = size->second;
        symbols->symbols.push_back(symbol);
    }
    std::stable_sort(symbols->symbols.begin(), symbols->symbols.end(), earlier);
    std::vector<PortSymbol> &list = symbols->symbols;
    size_t next = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (next <= i) next = i + 1;
        while (next < list.size() && list[next].address == list[i].address) next++;
        uint64_t end = 0;
        int bounded = section_end(symbols, list[i].section, &end);
        uint64_t limit = next < list.size() && (!bounded || list[next].address < end) ? list[next].address : end;
        if (!list[i].size && limit > list[i].address) list[i].size = limit - list[i].address;
    }
}
#endif

const char *port_symbols_load(const char *path, PortSymbols *symbols) {
    static char error[700];
    symbols->symbols.clear();
    symbols->sections.clear();
    symbols->path = path;
    if (!read_file(path, &symbols->file)) {
        snprintf(error, sizeof error, "cannot read %s", path);
        return error;
    }
#ifdef __APPLE__
    const unsigned char *file = symbols->file.data();
    if (symbols->file.size() < sizeof(mach_header_64) || ((const mach_header_64 *)file)->magic != MH_MAGIC_64) {
        snprintf(error, sizeof error, "%s is not a 64-bit Mach-O executable", path);
        return error;
    }
    const symtab_command *symtab = NULL;
    read_sections(file, symbols, &symtab);
    if (!symtab) {
        snprintf(error, sizeof error, "%s has no symbol table", path);
        return error;
    }
    read_names(file, symtab, symbols);
    return NULL;
#else
    snprintf(error, sizeof error, "symbols are read from macOS executables only");
    return error;
#endif
}

const PortSymbols *port_symbols_self(const char **error) {
    if (!self_state) {
        const char *failed = port_symbols_load(port_symbols_executable(), &self);
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
    const char *mangled = name.c_str();
    if (mangled[0] == '_') mangled++;
    int status = 0;
    char *demangled = abi::__cxa_demangle(mangled, NULL, NULL, &status);
    std::string pretty = status == 0 && demangled ? demangled : mangled;
    free(demangled);
    return pretty;
}
