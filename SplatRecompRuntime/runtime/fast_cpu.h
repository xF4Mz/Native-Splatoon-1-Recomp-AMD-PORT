// Fast guest memory for the recompiled game code.
//
// DolRecomp's generated code reads and writes guest memory through
// mem_read32() and friends, which live in DolRecomp's cpu.c and look the
// address up through several layers on every access. Here they become
// inline functions that go straight to one big block of PC memory standing
// in for the Wii U's whole 4 GB address space: guest address A lives at
// wiiu_fastmem + A. Only byte order needs fixing (the Wii U is big-endian).
//
// The build points generated.h at this file (DOLRECOMP_CPU_HEADER), so the
// game code picks these up without re-running DolRecomp. When
// wiiu_fastmem is NULL (debug options like --watch that must see every
// write), the original slow path is used instead.
#ifndef FAST_CPU_H
#define FAST_CPU_H

#include "common/types.h"
#include <string.h>

struct CPUState;
extern u8* wiiu_fastmem;   // NULL = use the slow, checked path

// The original DolRecomp functions (wiiu_mem.c forwards to them).
u64 wiiu_slow_read64(struct CPUState* cpu, u32 addr);
u32 wiiu_slow_read32(struct CPUState* cpu, u32 addr);
u16 wiiu_slow_read16(struct CPUState* cpu, u32 addr);
u8  wiiu_slow_read8(struct CPUState* cpu, u32 addr);
void wiiu_slow_write64(struct CPUState* cpu, u32 addr, u64 value);
void wiiu_slow_write32(struct CPUState* cpu, u32 addr, u32 value);
void wiiu_slow_write16(struct CPUState* cpu, u32 addr, u16 value);
void wiiu_slow_write8(struct CPUState* cpu, u32 addr, u8 value);

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#define FAST_BSWAP16(x) _byteswap_ushort(x)
#define FAST_BSWAP32(x) _byteswap_ulong(x)
#define FAST_BSWAP64(x) _byteswap_uint64(x)
#define FAST_LIKELY(x) (x)
#else
#define FAST_BSWAP16(x) __builtin_bswap16(x)
#define FAST_BSWAP32(x) __builtin_bswap32(x)
#define FAST_BSWAP64(x) __builtin_bswap64(x)
#define FAST_LIKELY(x) __builtin_expect(!!(x), 1)
#endif

static inline u64 wiiu_fast_read64(struct CPUState* cpu, u32 addr) {
    if (FAST_LIKELY(wiiu_fastmem)) { u64 v; memcpy(&v, wiiu_fastmem + addr, 8); return FAST_BSWAP64(v); }
    return wiiu_slow_read64(cpu, addr);
}
static inline u32 wiiu_fast_read32(struct CPUState* cpu, u32 addr) {
    if (FAST_LIKELY(wiiu_fastmem)) { u32 v; memcpy(&v, wiiu_fastmem + addr, 4); return FAST_BSWAP32(v); }
    return wiiu_slow_read32(cpu, addr);
}
static inline u16 wiiu_fast_read16(struct CPUState* cpu, u32 addr) {
    if (FAST_LIKELY(wiiu_fastmem)) { u16 v; memcpy(&v, wiiu_fastmem + addr, 2); return FAST_BSWAP16(v); }
    return wiiu_slow_read16(cpu, addr);
}
static inline u8 wiiu_fast_read8(struct CPUState* cpu, u32 addr) {
    if (FAST_LIKELY(wiiu_fastmem)) return wiiu_fastmem[addr];
    return wiiu_slow_read8(cpu, addr);
}
static inline void wiiu_fast_write64(struct CPUState* cpu, u32 addr, u64 value) {
    if (FAST_LIKELY(wiiu_fastmem)) { value = FAST_BSWAP64(value); memcpy(wiiu_fastmem + addr, &value, 8); return; }
    wiiu_slow_write64(cpu, addr, value);
}
static inline void wiiu_fast_write32(struct CPUState* cpu, u32 addr, u32 value) {
    if (FAST_LIKELY(wiiu_fastmem)) { value = FAST_BSWAP32(value); memcpy(wiiu_fastmem + addr, &value, 4); return; }
    wiiu_slow_write32(cpu, addr, value);
}
static inline void wiiu_fast_write16(struct CPUState* cpu, u32 addr, u16 value) {
    if (FAST_LIKELY(wiiu_fastmem)) { value = FAST_BSWAP16(value); memcpy(wiiu_fastmem + addr, &value, 2); return; }
    wiiu_slow_write16(cpu, addr, value);
}
static inline void wiiu_fast_write8(struct CPUState* cpu, u32 addr, u8 value) {
    if (FAST_LIKELY(wiiu_fastmem)) { wiiu_fastmem[addr] = value; return; }
    wiiu_slow_write8(cpu, addr, value);
}

// From here on, every mem_read32(...) etc. (in cpu.h's own inline helpers
// and in the generated code) means the fast version.
#define mem_read64  wiiu_fast_read64
#define mem_read32  wiiu_fast_read32
#define mem_read16  wiiu_fast_read16
#define mem_read8   wiiu_fast_read8
#define mem_write64 wiiu_fast_write64
#define mem_write32 wiiu_fast_write32
#define mem_write16 wiiu_fast_write16
#define mem_write8  wiiu_fast_write8

#include "cpu/cpu.h"

#endif
