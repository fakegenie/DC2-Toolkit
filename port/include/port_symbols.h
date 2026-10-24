#pragma once

#include <stdint.h>
#include <string>
#include <vector>

typedef struct {
    uint64_t address;
    uint64_t size;
    std::string name;
    std::string object;
    int section;
    int code;
} PortSymbol;

typedef struct {
    char segment[17];
    char name[17];
    uint64_t address;
    uint64_t size;
    uint64_t offset;
} PortSection;

typedef struct {
    std::vector<PortSymbol> symbols;
    std::vector<PortSection> sections;
    std::vector<unsigned char> file;
    std::string path;
} PortSymbols;

const char *port_symbols_load(const char *path, PortSymbols *symbols);
const PortSymbols *port_symbols_self(const char **error);
const char *port_symbols_executable(void);
const PortSymbol *port_symbol_at(const PortSymbols *symbols, uint64_t address);
const PortSection *port_section_at(const PortSymbols *symbols, uint64_t address);
std::string port_symbol_pretty(const std::string &name);
