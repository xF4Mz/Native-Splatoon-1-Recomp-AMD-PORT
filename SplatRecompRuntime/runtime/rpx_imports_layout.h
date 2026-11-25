// Shared between the runtime and tools/rpx_patch.c, so both agree on where
// imported data objects live in guest memory.
//
// On a real Wii U, a "data import" (e.g. coreinit's _iob, or a function
// pointer like MEMAllocFromDefaultHeap) lives inside the system library, and
// the loader patches the game's code to point at it. We don't have the
// system libraries, so each data import gets its own 4 KB slot here instead,
// numbered in the order the imports appear in the .rpx symbol table.
#ifndef RPX_IMPORTS_LAYOUT_H
#define RPX_IMPORTS_LAYOUT_H

#define DIMPORT_OBJECT_BASE   0xC1100000u
#define DIMPORT_OBJECT_STRIDE 0x1000u

static inline unsigned dimport_object_address(unsigned index) {
    return DIMPORT_OBJECT_BASE + index * DIMPORT_OBJECT_STRIDE;
}

#endif
