// lwarx / stwcx. (the PowerPC's "load and reserve" / "store if still
// reserved", which the game builds its atomic counters and lock-free queues
// from) for game threads running on several PC cores at once.
//
// The build rewrites the few game-code files that use them (see
// CMakeLists.txt) to call these instead of DolRecomp's single-thread
// version: stwcx. now only stores if the word still holds what lwarx. read,
// checked and stored in one atomic step, so two cores can't both "win".
#ifndef SPLAT_ATOMIC_H
#define SPLAT_ATOMIC_H

#include "host_thread.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

extern HOST_THREAD_LOCAL u32 splat_reserve_value;   // what this thread's last lwarx. read (host byte order)
bool splat_cas32_slow(u32 addr, u32 expect, u32 value);   // (when fast memory is off)

static inline u32 splat_lwarx(CPUState* ctx, u32 ea) {
    u32 v = mem_read32(ctx, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_valid = true;
    splat_reserve_value = v;
    return v;
}

static inline bool splat_stwcx(CPUState* ctx, u32 ea, u32 v) {
    bool held = ctx->reserve_valid && ea == ctx->reserve_addr;
    ctx->reserve_valid = false;
    if (!held) return false;
    if (!wiiu_fastmem) return splat_cas32_slow(ea, splat_reserve_value, v);
    u32 expect = FAST_BSWAP32(splat_reserve_value), want = FAST_BSWAP32(v);
#if defined(_MSC_VER)
    return (u32)_InterlockedCompareExchange((volatile long*)(wiiu_fastmem + ea), (long)want, (long)expect) == expect;
#else
    return __atomic_compare_exchange_n((u32*)(wiiu_fastmem + ea), &expect, want, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}

#endif
