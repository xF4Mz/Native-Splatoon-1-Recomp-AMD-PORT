// Wii U guest memory for recompiled code.
//
// DolRecomp's CPU core only knows GameCube/Wii addresses, so we switch its
// built-in RAM off and route every load/store through these functions,
// which map the Wii U's address layout onto host memory.
#ifndef WIIU_MEM_H
#define WIIU_MEM_H

#include "cpu/cpu.h"

// Wii U address regions (the same layout the console's loader uses for
// an .rpx built by the Wii U toolchain).
#define WIIU_CODE_BASE   0x02000000u
#define WIIU_CODE_SIZE   0x0E000000u   // 224 MB window, allocated on demand
#define WIIU_DATA_BASE   0x10000000u
#define WIIU_DATA_SIZE   0x40000000u   // 1 GB window: data, bss, heap, stacks
#define WIIU_LOAD_BASE   0xC0000000u
#define WIIU_LOAD_SIZE   0x02000000u   // import tables, symbols, HLE thunks

// The console's special memory areas, handed out through MEMGetBaseHeapHandle:
// MEM1 (32 MB of fast memory) and the foreground bucket (40 MB the running
// game gets for itself).
#define WIIU_MEM1_BASE   0xF4000000u
#define WIIU_MEM1_SIZE   0x02000000u
#define WIIU_FG_BASE     0xE0000000u
#define WIIU_FG_SIZE     0x02800000u
// The "overlay arena": extra memory a game can switch on (OSEnableOverlayArena).
#define WIIU_OVERLAY_BASE 0xA0000000u
#define WIIU_OVERLAY_SIZE 0x1C000000u

// Inside the data window: where our heap and the main stack live. A real
// Wii U gives a game about 1 GB of main memory (MEM2) and games plan their
// memory around that, so the heap runs from just after the program's data
// (see hle_init) up to below the main stack. WIIU_HEAP_BASE is only the
// upper bound for where program data may sit.
#define WIIU_HEAP_BASE   0x30000000u
#define WIIU_HEAP_END    0x4F000000u
#define WIIU_STACK_TOP   0x4FFF0000u
#define WIIU_STACK_SIZE  0x00100000u

bool wiiu_mem_init(void);
void wiiu_mem_shutdown(void);
unsigned long long wiiu_mem_resident_bytes(void);   // the game memory really in RAM (slow-ish)

// Host pointer for a guest address range, or NULL if unmapped.
u8* wiiu_mem_ptr(u32 addr, u32 size);

// Plug the memory map into a DolRecomp CPUState.
void wiiu_mem_attach(CPUState* cpu);

// Big-endian helpers for runtime code.
u32  wiiu_read32(u32 addr);
void wiiu_write32(u32 addr, u32 value);
u16  wiiu_read16(u32 addr);
void wiiu_write16(u32 addr, u16 value);
u8   wiiu_read8(u32 addr);
void wiiu_write8(u32 addr, u8 value);
// Little-endian 32-bit access (GPU programs are stored little-endian).
u32  wiiu_read32_le(u32 addr);
void wiiu_write32_le(u32 addr, u32 value);

// Copy a NUL-terminated guest string into a host buffer.
void wiiu_read_string(u32 addr, char* out, size_t out_size);

// Turn off fast memory (every access goes through the checked path, which
// reports accesses to unmapped addresses).
void wiiu_mem_use_slow_path(void);

// Change tracking for the renderer's copies of game memory (textures):
// cache flushes, copies and file reads mark 4 KB pages as changed.
void wiiu_mem_mark(u32 addr, u32 size);
u32  wiiu_mem_generation(void);                          // current change counter
bool wiiu_mem_changed_since(u32 addr, u32 size, u32 gen); // any page marked after `gen`?
u32  wiiu_mem_region_generation(u32 region);             // last change counter in that 1 MB region (addr >> 20)

// Debugging: log every game write to [addr, end) (end 0 = just addr).
void wiiu_mem_watch(u32 addr, u32 end);

// Atomic compare-and-swap of a guest word (big-endian values).
bool wiiu_cas32(u32 addr, u32 expect, u32 value);

#endif
