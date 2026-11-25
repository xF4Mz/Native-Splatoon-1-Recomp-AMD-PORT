// Included by the game-code files tools/chunk_split.c rewrites (the busiest
// ones, built fully optimized), right after generated.h.
//
// There, every float instruction was a call into cpu_fast.c (passing the
// register numbers; the helper loads, works it out, stores), every lfs/stfs
// a call to a float<->bits conversion the compiler didn't inline, and every
// float instruction first checked that the FPU is switched on. Here the
// same work happens in place: the helpers' fast paths (fp_fast.h, the very
// code cpu_fast.c's out-of-line versions run) are inlined, and only the
// unusual cases (NaNs, denormals, special FPSCR modes) still call out.
// Results are bit-for-bit those of the out-of-line helpers
// (scratchpad/difft compares split files with the originals).
#ifndef SPLAT_SPLIT_INLINE_H
#define SPLAT_SPLIT_INLINE_H

#include "fp_fast.h"

// The pieces' CPU state pointer: nothing else points at that memory (game
// memory is elsewhere), so the compiler may keep registers in host
// registers across game-memory reads and writes.
#if defined(_MSC_VER) && !defined(__clang__)
#define SPLIT_RESTRICT __restrict
#else
#define SPLIT_RESTRICT __restrict__
#endif

// (the game came into a split file where its pieces have no label, so the
// file's unsplit code runs instead: counted and noted, see runtime/profiler.c)
void splat_split_fallback(u32 pc);

// A call ("bl", "bctrl"): runs the callee and says whether it came back to
// `ret` with the stack as it was -- then the piece goes on right after the
// call (chunk_split --calls; runtime/main.c).
int splat_call(CPUState* cpu, u32 ret);
// (a call already under way -- the callee's file function was called directly,
// chunk_split --direct -- that didn't come straight back: carry on until it
// returns to `ret` with the stack pointer at `sp`)
int splat_call_sp(CPUState* cpu, u32 ret, u32 sp);
// Each 16 KB game-code file's function (func_X), by (address - 0x02000020) >> 14,
// for calls through a register (chunk_split --direct; filled in runtime/main.c)
extern void (*splat_file_fn[])(CPUState* ctx);
// isync / eieio: only the compiler mustn't move memory accesses across them
#if defined(_MSC_VER) && !defined(__clang__)
#define SPLAT_COMPILER_BARRIER() _ReadWriteBarrier()
#else
#define SPLAT_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

// float instructions
#define ppc_fadds sfi_fadds
#define ppc_fsubs sfi_fsubs
#define ppc_fmuls sfi_fmuls
#define ppc_fdivs sfi_fdivs
#define ppc_fadd sfi_fadd
#define ppc_fsub sfi_fsub
#define ppc_fmul sfi_fmul
#define ppc_fdiv sfi_fdiv
#define ppc_fma sfi_fma
#define ppc_frsp sfi_frsp
#define ppc_fcmp sfi_fcmp
#define ppc_ps_add_op sfi_ps_add
#define ppc_ps_sub_op sfi_ps_sub
#define ppc_ps_mul_op sfi_ps_mul
#define ppc_ps_div_op sfi_ps_div
#define ppc_ps_muls0 sfi_ps_muls0
#define ppc_ps_muls1 sfi_ps_muls1
#define ppc_ps_sum0 sfi_ps_sum0
#define ppc_ps_sum1 sfi_ps_sum1
#define ppc_ps_madd_op sfi_ps_madd
#define ppc_ps_madds0 sfi_ps_madds0
#define ppc_ps_madds1 sfi_ps_madds1
// paired-single loads / stores
#define ppc_psq_load_inline sfi_psq_load
#define ppc_psq_store_inline sfi_psq_store

// "Is the FPU switched on?" before every float instruction: it always is for
// the game (Cafe OS runs games with MSR[FP] set; the runtime sets it for
// every game thread and the game code never changes the MSR).
#undef ppc_fp_available_inline
#define ppc_fp_available_inline(cpu, cia) 1

// lfs and friends: single bits -> double (an ordinary number or zero: the
// plain conversion, which is exact; the rest as DolRecomp does it)
static SPLAT_FI f64 splat_f32_from_bits_fi(u32 bits) {
    u32 e = bits & 0x7F800000u;
    if ((e != 0 && e != 0x7F800000u) || (bits & 0x7FFFFFFFu) == 0) {
        f32 f;
        memcpy(&f, &bits, 4);
        return (f64)f;
    }
    return dolrecomp_f32_from_bits(bits);
}
// stfs and friends: DolRecomp's double -> single bits, inlined
static SPLAT_FI u32 splat_f32_to_bits_fi(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    u32 exp = (u32)((bits >> 52) & 0x7FFu);
    if (exp > 896 || (bits & 0x7FFFFFFFFFFFFFFFull) == 0)
        return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
    if (exp >= 874) {
        u32 result = (u32)(0x80000000u | ((bits & 0x000FFFFFFFFFFFFFull) >> 21));
        result >>= 905 - exp;
        result |= (u32)((bits >> 32) & 0x80000000u);
        return result;
    }
    return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
}
static SPLAT_FI u32 splat_rotl32_fi(u32 value, u32 sh) {
    sh &= 31u;
    return sh ? ((value << sh) | (value >> (32u - sh))) : value;
}
// Game memory: as fast_cpu.h, except that the slow path (only taken with
// debugging options, see wiiu_mem.c) finds the CPU state itself instead of
// being handed the pieces' one -- handing it over made the compiler write
// every register back to the CPU state before each memory access.
u64 wiiu_slow_read64_cur(u32 addr);
u32 wiiu_slow_read32_cur(u32 addr);
u16 wiiu_slow_read16_cur(u32 addr);
u8 wiiu_slow_read8_cur(u32 addr);
void wiiu_slow_write64_cur(u32 addr, u64 value);
void wiiu_slow_write32_cur(u32 addr, u32 value);
void wiiu_slow_write16_cur(u32 addr, u16 value);
void wiiu_slow_write8_cur(u32 addr, u8 value);
static SPLAT_FI u64 sm_read64(u32 a) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) { u64 v; memcpy(&v, m + a, 8); return FAST_BSWAP64(v); }
    return wiiu_slow_read64_cur(a);
}
static SPLAT_FI u32 sm_read32(u32 a) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) { u32 v; memcpy(&v, m + a, 4); return FAST_BSWAP32(v); }
    return wiiu_slow_read32_cur(a);
}
static SPLAT_FI u16 sm_read16(u32 a) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) { u16 v; memcpy(&v, m + a, 2); return FAST_BSWAP16(v); }
    return wiiu_slow_read16_cur(a);
}
static SPLAT_FI u8 sm_read8(u32 a) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) return m[a];
    return wiiu_slow_read8_cur(a);
}
static SPLAT_FI void sm_write64(u32 a, u64 v) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) { v = FAST_BSWAP64(v); memcpy(m + a, &v, 8); return; }
    wiiu_slow_write64_cur(a, v);
}
static SPLAT_FI void sm_write32(u32 a, u32 v) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) { v = FAST_BSWAP32(v); memcpy(m + a, &v, 4); return; }
    wiiu_slow_write32_cur(a, v);
}
static SPLAT_FI void sm_write16(u32 a, u16 v) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) { v = FAST_BSWAP16(v); memcpy(m + a, &v, 2); return; }
    wiiu_slow_write16_cur(a, v);
}
static SPLAT_FI void sm_write8(u32 a, u8 v) {
    u8* m = wiiu_fastmem;
    if (FAST_LIKELY(m)) { m[a] = v; return; }
    wiiu_slow_write8_cur(a, v);
}
// ... and in the pieces (chunk_split --membase), through a base pointer each
// piece reads once (and returns to the unsplit code when it's NULL): read
// from the global, the compiler loaded it again after every store (a store
// might have changed it, for all it knew) and tested it every time.
static SPLAT_FI u64 sm_read64_b(u8* m, u32 a) { u64 v; memcpy(&v, m + a, 8); return FAST_BSWAP64(v); }
static SPLAT_FI u32 sm_read32_b(u8* m, u32 a) { u32 v; memcpy(&v, m + a, 4); return FAST_BSWAP32(v); }
static SPLAT_FI u16 sm_read16_b(u8* m, u32 a) { u16 v; memcpy(&v, m + a, 2); return FAST_BSWAP16(v); }
static SPLAT_FI u8 sm_read8_b(u8* m, u32 a) { return m[a]; }
static SPLAT_FI void sm_write64_b(u8* m, u32 a, u64 v) { v = FAST_BSWAP64(v); memcpy(m + a, &v, 8); }
static SPLAT_FI void sm_write32_b(u8* m, u32 a, u32 v) { v = FAST_BSWAP32(v); memcpy(m + a, &v, 4); }
static SPLAT_FI void sm_write16_b(u8* m, u32 a, u16 v) { v = FAST_BSWAP16(v); memcpy(m + a, &v, 2); }
static SPLAT_FI void sm_write8_b(u8* m, u32 a, u8 v) { m[a] = v; }
#undef mem_read64
#undef mem_read32
#undef mem_read16
#undef mem_read8
#undef mem_write64
#undef mem_write32
#undef mem_write16
#undef mem_write8
#define mem_read64(cpu, a) sm_read64(a)
#define mem_read32(cpu, a) sm_read32(a)
#define mem_read16(cpu, a) sm_read16(a)
#define mem_read8(cpu, a) sm_read8(a)
#define mem_write64(cpu, a, v) sm_write64(a, v)
#define mem_write32(cpu, a, v) sm_write32(a, v)
#define mem_write16(cpu, a, v) sm_write16(a, v)
#define mem_write8(cpu, a, v) sm_write8(a, v)

#define dolrecomp_f32_from_bits splat_f32_from_bits_fi
#define dolrecomp_f32_to_bits splat_f32_to_bits_fi
#define dolrecomp_rotl32 splat_rotl32_fi

#endif
