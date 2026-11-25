// Loads a Wii U .rpx into guest memory the way the console's loader does:
// unpacks each section to its address and records which addresses are
// imports (calls into the Wii U's system libraries).
#ifndef RPX_LOADER_H
#define RPX_LOADER_H

#include "common/types.h"
#include <stdbool.h>
#include <stdio.h>

#define RPX_MAX_IMPORTS 4096

typedef struct {
    u32 address;        // where the game jumps (function) or reads (data)
    bool is_data;       // data import (e.g. a function-pointer variable)
    u32 data_object;    // data imports: where the object lives (see rpx_imports_layout.h)
    char library[32];   // e.g. "coreinit"
    char name[96];      // e.g. "OSReport"
} RpxImport;

typedef struct {
    char name[40];
    u32 type, flags, address, size;
} RpxSection;

typedef struct {
    u32 address;
    char name[96];
} RpxSymbol;

#define RPX_MAX_SECTIONS 128

typedef struct {
    u32 entry_point;
    u32 sda_base;       // r13
    u32 sda2_base;      // r2
    u32 preinit_user;   // exported __preinit_user, 0 if absent
    u32 data_end;       // first free address after .data/.bss
    u32 import_min, import_max;   // address range covered by imports
    u32 import_count;
    RpxImport imports[RPX_MAX_IMPORTS];
    u32 section_count;
    RpxSection sections[RPX_MAX_SECTIONS];
    u32 symbol_count;
    RpxSymbol* symbols;   // every named symbol, sorted by address
} RpxImage;

bool rpx_load_into_memory(const char* path, RpxImage* image);
const RpxImport* rpx_find_import(const RpxImage* image, u32 address);

// Print the sections and symbols around an address (for debugging).
void rpx_describe_address(const RpxImage* image, u32 address, FILE* out);
void rpx_print_sections(const RpxImage* image, FILE* out);

// Debugging: report relocations that touch this address while loading.
void rpx_debug_address(u32 addr);

#endif
