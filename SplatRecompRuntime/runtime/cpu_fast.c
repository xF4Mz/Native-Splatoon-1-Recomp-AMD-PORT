// DolRecomp's CPU helpers (cpu/cpu.c), built with one change: fused
// multiply-adds use the processor's FMA instruction when it has one.
//
// The game's float math (paired singles: matrices, physics, animation) goes
// through these helpers, and on Windows the C library's fma() works it out
// in software, which made it one of the slowest parts of every frame. The
// instruction gives the exact same, correctly rounded result.
#include <math.h>
#include <stdbool.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define SPLAT_FMA_TARGET
#else
#include <cpuid.h>
#define SPLAT_FMA_TARGET __attribute__((target("fma")))
#endif

int splat_has_fma = -1;   // (fp_fast.h reads it too)

static int splat_detect_fma(void) {
    unsigned a, b, c, d;
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4];
    __cpuid(r, 1);
    c = (unsigned)r[2];
    (void)a; (void)b; (void)d;
#else
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
#endif
    bool fma = (c >> 12) & 1, osxsave = (c >> 27) & 1, avx = (c >> 28) & 1;
    if (!fma || !osxsave || !avx) return 0;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long long xcr0 = _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    unsigned long long xcr0 = ((unsigned long long)hi << 32) | lo;
#endif
    return (xcr0 & 6) == 6;   // the OS saves the wide registers
}

SPLAT_FMA_TARGET static double splat_fma_hw(double x, double y, double z) {
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(x), _mm_set_sd(y), _mm_set_sd(z)));
}

static double splat_fma(double x, double y, double z) {
    if (splat_has_fma < 0) splat_has_fma = splat_detect_fma();
    return splat_has_fma ? splat_fma_hw(x, y, z) : fma(x, y, z);
}
#define fma splat_fma
#endif

// With Microsoft's C library, isnan() / isinf() / isfinite() / fpclassify()
// / signbit() aren't worked out in place: each one is a call into the C
// runtime DLL (_dclass, _fdclass, _dsign...). The float helpers below check
// every result with them, several times per instruction. These do the same
// from the bits, inline.
#if defined(_MSC_VER) && !defined(__clang__)
#include <string.h>
static __forceinline int splat_dclass(double x) {
    unsigned long long b;
    memcpy(&b, &x, 8);
    unsigned long long e = b & 0x7FF0000000000000ull, m = b & 0x000FFFFFFFFFFFFFull;
    if (e == 0x7FF0000000000000ull) return m ? FP_NAN : FP_INFINITE;
    if (e == 0) return m ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}
static __forceinline int splat_fclass(float x) {
    unsigned b;
    memcpy(&b, &x, 4);
    unsigned e = b & 0x7F800000u, m = b & 0x007FFFFFu;
    if (e == 0x7F800000u) return m ? FP_NAN : FP_INFINITE;
    if (e == 0) return m ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}
static __forceinline int splat_dsign(double x) { unsigned long long b; memcpy(&b, &x, 8); return (int)(b >> 63); }
static __forceinline int splat_fsign(float x) { unsigned b; memcpy(&b, &x, 4); return (int)(b >> 31); }
#undef fpclassify
#undef isnan
#undef isinf
#undef isfinite
#undef isnormal
#undef signbit
#define fpclassify(v) (sizeof(v) == sizeof(float) ? splat_fclass((float)(v)) : splat_dclass((double)(v)))
#define isnan(v) (fpclassify(v) == FP_NAN)
#define isinf(v) (fpclassify(v) == FP_INFINITE)
#define isfinite(v) (fpclassify(v) != FP_NAN && fpclassify(v) != FP_INFINITE)
#define isnormal(v) (fpclassify(v) == FP_NORMAL)
#define signbit(v) (sizeof(v) == sizeof(float) ? splat_fsign((float)(v)) : splat_dsign((double)(v)))
#endif

// The paired-single loads and stores (psq_l / psq_st) are all over the
// game's math. DolRecomp's versions reach memory through its general
// address lookup and a callback, twice per instruction; the common case
// (plain floats, aligned) is done here straight on guest memory instead.
#define ppc_psq_load ppc_psq_load_general
#define ppc_psq_store ppc_psq_store_general
// The float instructions too (below): DolRecomp's versions are kept, under
// these names, for the unusual cases.
#define ppc_fadds ppc_fadds_exact
#define ppc_fsubs ppc_fsubs_exact
#define ppc_fmuls ppc_fmuls_exact
#define ppc_fdivs ppc_fdivs_exact
#define ppc_fadd ppc_fadd_exact
#define ppc_fsub ppc_fsub_exact
#define ppc_fmul ppc_fmul_exact
#define ppc_fdiv ppc_fdiv_exact
#define ppc_fma ppc_fma_exact
#define ppc_frsp ppc_frsp_exact
#define ppc_fcmp ppc_fcmp_exact
#define ppc_ps_add_op ppc_ps_add_op_exact
#define ppc_ps_sub_op ppc_ps_sub_op_exact
#define ppc_ps_mul_op ppc_ps_mul_op_exact
#define ppc_ps_div_op ppc_ps_div_op_exact
#define ppc_ps_madd_op ppc_ps_madd_op_exact
#define ppc_ps_madds0 ppc_ps_madds0_exact
#define ppc_ps_madds1 ppc_ps_madds1_exact
#define ppc_ps_sum0 ppc_ps_sum0_exact
#define ppc_ps_sum1 ppc_ps_sum1_exact
#define ppc_ps_muls0 ppc_ps_muls0_exact
#define ppc_ps_muls1 ppc_ps_muls1_exact
#include "cpu/cpu.c"
#undef ppc_psq_load
#undef ppc_psq_store
#undef ppc_fadds
#undef ppc_fsubs
#undef ppc_fmuls
#undef ppc_fdivs
#undef ppc_fadd
#undef ppc_fsub
#undef ppc_fmul
#undef ppc_fdiv
#undef ppc_fma
#undef ppc_frsp
#undef ppc_fcmp
#undef ppc_ps_add_op
#undef ppc_ps_sub_op
#undef ppc_ps_mul_op
#undef ppc_ps_div_op
#undef ppc_ps_madd_op
#undef ppc_ps_madds0
#undef ppc_ps_madds1
#undef ppc_ps_sum0
#undef ppc_ps_sum1
#undef ppc_ps_muls0
#undef ppc_ps_muls1

// --- Float instructions -----------------------------------------------------
// The game's float math (fadds, fmuls, fmadds, the paired-single ps_*
// family...) goes through these on every instruction. DolRecomp's versions
// work everything out through several layers of helper calls, and every
// instruction also updates the FPSCR status bits (inexact, fraction rounded,
// result class), which chains each float instruction to the one before.
//
// Splatoon never looks at those bits: its code has no mffs / mcrfs /
// mtfsf / mtfsb / "record" float instructions at all (checked over its whole
// program). So by default the results are worked out directly and the
// status bits are left alone (SPLAT_EXACT_FPSCR=1 keeps them exactly, at
// DolRecomp's cost). The numbers themselves are always exactly DolRecomp's:
// anything giving a NaN (whose sign and payload follow PowerPC rules, not
// the PC's), or flush-to-zero / trapping modes, goes to DolRecomp's version.
// The fast paths are in fp_fast.h (also built straight into the optimized
// game-code files); tests/float_fast_test.c compares them on millions of
// random inputs.
int splat_exact_fpscr = 0;

// (the rare cases fp_fast.h hands back)
f64 splat_force_25_bit_ool(f64 value) { return force_25_bit(value); }
f64 splat_fma_tie_ool(f64 a, f64 c, f64 addend, f64 result) { return correct_single_fma_tie(a, c, addend, result); }
f64 splat_extend_f32_ool(u32 bits) { return extend_f32_bits(bits); }
f64 splat_fma_ool(f64 x, f64 y, f64 z) { return fma(x, y, z); }

#include "fp_fast.h"

bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) { return sfi_psq_load(cpu, frD, ea, w, gqr, indexed, cia); }
bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) { return sfi_psq_store(cpu, frS, ea, w, gqr, indexed, cia); }
void ppc_fadds(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_fadds(cpu, d, a, b); }
void ppc_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_fsubs(cpu, d, a, b); }
void ppc_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) { sfi_fmuls(cpu, d, a, c); }
void ppc_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_fdivs(cpu, d, a, b); }
void ppc_fadd(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_fadd(cpu, d, a, b); }
void ppc_fsub(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_fsub(cpu, d, a, b); }
void ppc_fmul(CPUState* cpu, u8 d, u8 a, u8 c) { sfi_fmul(cpu, d, a, c); }
void ppc_fdiv(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_fdiv(cpu, d, a, b); }
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative, f64* output) {
    return sfi_fma(cpu, a, c, b, single, subtract, negative, output);
}
void ppc_frsp(CPUState* cpu, u8 d, u8 b) { sfi_frsp(cpu, d, b); }
void ppc_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) { sfi_fcmp(cpu, crfd, a, b, ordered); }
void ppc_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_ps_add(cpu, d, a, b); }
void ppc_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_ps_sub(cpu, d, a, b); }
void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c) { sfi_ps_mul(cpu, d, a, c); }
void ppc_ps_div_op(CPUState* cpu, u8 d, u8 a, u8 b) { sfi_ps_div(cpu, d, a, b); }
void ppc_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) { sfi_ps_muls0(cpu, d, a, c); }
void ppc_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) { sfi_ps_muls1(cpu, d, a, c); }
void ppc_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) { sfi_ps_sum0(cpu, d, a, c, b); }
void ppc_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) { sfi_ps_sum1(cpu, d, a, c, b); }
void ppc_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative) { sfi_ps_madd(cpu, d, a, c, b, subtract, negative); }
void ppc_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) { sfi_ps_madds0(cpu, d, a, c, b); }
void ppc_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) { sfi_ps_madds1(cpu, d, a, c, b); }
