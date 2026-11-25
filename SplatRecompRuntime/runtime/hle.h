// HLE ("high-level emulation") of the Wii U's system libraries.
//
// When recompiled code calls into coreinit, the console's OS, it jumps to
// an import address. We catch that jump and run a PC version of the
// function instead. Each one is a small C function below; adding support
// for more of the OS means adding more of these.
#ifndef HLE_H
#define HLE_H

#include "cpu/cpu.h"
#include "rpx_loader.h"
#include <stdio.h>

typedef void (*HleFunction)(CPUState* cpu);

typedef struct {
    const char* name;
    HleFunction fn;
} HleEntry;

// Set up thunks for every import in the image.
void hle_init(CPUState* cpu, RpxImage* image);

// DolRecomp's host_call hook: returns true if `address` was a system call
// we handled (and moved pc back to the caller).
bool hle_host_call(CPUState* cpu, u32 address);

// Call guest code from the host (used to run __preinit_user, etc.).
// Returns false if execution got stuck.
bool hle_call_guest(CPUState* cpu, u32 address);

// Debugging: print the first n system calls (--trace n).
void hle_set_trace(int n);

// Print the last few system calls (for crash reports).
void hle_print_trail(FILE* out);

// Print where the game is (call chain, threads) when it stops itself (main.c).
void runtime_report_halt(CPUState* cpu);
void runtime_print_chain(CPUState* cpu, FILE* out, int max_depth, bool with_code);
// Stop at the game's first failed self-check (assert) instead of skipping it.
void hle_set_strict_asserts(bool on);
// Name of the system function at an import/thunk address (NULL if none).
const char* hle_name_at(u32 address);
// Print the system functions called most since the last call to this.
void hle_report_calls(FILE* out, double seconds);
// Log the main thread's system calls per frame and the time spent in them (over `frames` frames).
void hle_report_main_calls(u32 frames);
// A snapshot of every system function's call count, and the full list of what was called since it (the freeze watchdog).
void hle_calls_snapshot(void);
void hle_report_calls_since_snapshot(FILE* out, const char* title);
bool hle_strict(void);

// End the program, as when the game calls exit() (defined in main.c).
void runtime_exit(int code);

// Argument helpers (PowerPC calling convention: r3, r4, ... then stack).
static inline u32 arg(CPUState* c, int i) { return c->gpr[3 + i]; }
static inline void ret(CPUState* c, u32 v) { c->gpr[3] = v; }

// Guest heap (simple allocator inside guest memory).
u32 hle_heap_alloc(u32 size, u32 align);
void hle_heap_free(u32 addr);
u32 hle_heap_free_bytes(void);

// Formats a guest printf-style call (fmt in r[fmt_reg], varargs after).
int hle_format(CPUState* c, int fmt_reg, char* out, size_t out_size);

#endif
