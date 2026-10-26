#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "name+0xoffset (object)" for an address in the program, from the debug build's symbol table; 0 when it has none
int port_symbol_describe(uint64_t address, char *text, size_t size);

#ifdef __cplusplus
}

#include <string>
#include <vector>

typedef struct {
    uint64_t address;
    uint64_t size;
    std::string name;
    std::string object;
    int code;
} PortSymbol;

typedef struct {
    uint64_t address;
    uint64_t size;
    int code;
    int writable;
    std::string name;
} PortSection;

// The symbol table tools/port.py writes beside the debug binary as NAME.symbols, at the addresses of this launch
typedef struct {
    std::vector<PortSymbol> symbols;
    std::vector<PortSection> sections;
    std::string path;
} PortSymbols;

const char *port_symbols_load(const char *path, PortSymbols *symbols);
const PortSymbols *port_symbols_self(const char **error);
const char *port_symbols_path(void);
const PortSymbol *port_symbol_at(const PortSymbols *symbols, uint64_t address);
const PortSection *port_section_at(const PortSymbols *symbols, uint64_t address);
std::string port_symbol_pretty(const std::string &name);
#endif
