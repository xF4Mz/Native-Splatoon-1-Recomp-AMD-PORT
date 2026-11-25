// The float instructions and paired-single loads/stores, the common cases
// worked out directly (see cpu_fast.c for why that gives DolRecomp's exact
// numbers). One copy, used twice:
//   - cpu_fast.c builds the out-of-line ppc_fadds() etc. from it (what the
//     ordinary game-code files call);
//   - split_inline.h puts it straight into the optimized game-code files
//     (tools/chunk_split.c), so a float instruction there costs a few
//     machine instructions instead of a call.
// Each sfi_*() does the whole instruction: the direct way when it can, else
// DolRecomp's version (ppc_*_exact, in cpu_fast.c).
// (tests/float_fast_test.c compares them with DolRecomp's on random inputs.)
#ifndef SPLAT_FP_FAST_H
#define SPLAT_FP_FAST_H

#include <string.h>
#include <math.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define SPLAT_FI __forceinline
#else
#define SPLAT_FI inline __attribute__((always_inline))
#endif

#define SF_FPSCR_XX 0x02000000u
#define SF_FPSCR_FR 0x00040000u
#define SF_FPSCR_FI 0x00020000u
#define SF_FPSCR_VE 0x00000080u
#define SF_FPSCR_ZE 0x00000010u
#define SF_FPSCR_NI 0x00000004u

extern int splat_exact_fpscr;   // SPLAT_EXACT_FPSCR=1: keep the FPSCR status bits exactly
extern u8* wiiu_fastmem;

// DolRecomp's versions (cpu_fast.c)
void ppc_fadds_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsubs_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmuls_exact(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdivs_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fadd_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsub_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmul_exact(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdiv_exact(CPUState* cpu, u8 d, u8 a, u8 b);
bool ppc_fma_exact(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative, f64* output);
void ppc_frsp_exact(CPUState* cpu, u8 d, u8 b);
void ppc_fcmp_exact(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered);
void ppc_ps_add_op_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_sub_op_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_mul_op_exact(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_div_op_exact(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_madd_op_exact(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative);
void ppc_ps_madds0_exact(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_madds1_exact(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum0_exact(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum1_exact(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_muls0_exact(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_muls1_exact(CPUState* cpu, u8 d, u8 a, u8 c);
bool ppc_psq_load_general(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_store_general(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
// the rare cases of the helpers below (cpu_fast.c)
f64 splat_force_25_bit_ool(f64 value);
f64 splat_fma_tie_ool(f64 a, f64 c, f64 addend, f64 result);
f64 splat_extend_f32_ool(u32 bits);
f64 splat_fma_ool(f64 x, f64 y, f64 z);

// fused multiply-add: the processor's instruction when it has one
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
#include <immintrin.h>
extern int splat_has_fma;   // (-1 until splat_fma_ool() first looked)
static SPLAT_FI f64 sf_fma(f64 x, f64 y, f64 z) {
    if (splat_has_fma > 0) return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(x), _mm_set_sd(y), _mm_set_sd(z)));
    return splat_fma_ool(x, y, z);
}
#else
#define sf_fma splat_fma_ool
#endif

static SPLAT_FI u64 sf_bits64(f64 v) { u64 b; memcpy(&b, &v, 8); return b; }
static SPLAT_FI u32 sf_bits32(f32 v) { u32 b; memcpy(&b, &v, 4); return b; }
static SPLAT_FI f64 sf_from64(u64 b) { f64 v; memcpy(&v, &b, 8); return v; }
static SPLAT_FI bool sf_fin(f64 v) { return (sf_bits64(v) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }
// direct results allowed: status bits not wanted, no flush-to-zero, no traps
static SPLAT_FI bool sf_direct(const CPUState* cpu) {
    return !splat_exact_fpscr && !(cpu->fpscr & (SF_FPSCR_NI | SF_FPSCR_VE | SF_FPSCR_ZE));
}

static SPLAT_FI u32 sf_class32(f32 value) {   // = classify_f32
    u32 bits = sf_bits32(value);
    u32 sign = bits >> 31, exponent = bits & 0x7F800000u, fraction = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u) return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0) return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}
static SPLAT_FI u32 sf_class64(f64 value) {   // = classify_f64
    u64 bits = sf_bits64(value);
    u64 sign = bits >> 63, exponent = bits & 0x7FF0000000000000ull, fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0x7FF0000000000000ull) return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0) return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}
static SPLAT_FI u32 sf_fprf(u32 fpscr, u32 cls) { return (fpscr & ~(0x1Fu << 12)) | ((cls & 0x1Fu) << 12); }
static SPLAT_FI u32 sf_summary(u32 fpscr) {   // = ppc_fpscr_updated
    fpscr = (fpscr & ~0x20000000u) | ((fpscr & 0x01F80700u) ? 0x20000000u : 0u);
    fpscr = (fpscr & ~0x40000000u) | ((((fpscr >> 22) & fpscr & 0x000000F8u) != 0) ? 0x40000000u : 0u);
    return fpscr;
}
static SPLAT_FI u32 sf_fi_fr(u32 fpscr, f64 exact, f64 rounded) {   // = set_fi_fr for a non-NaN exact value
    fpscr &= ~(SF_FPSCR_FI | SF_FPSCR_FR);
    if (exact == rounded) return fpscr;
    if ((fpscr & SF_FPSCR_XX) != SF_FPSCR_XX) fpscr |= 0x80000000u;
    fpscr = sf_summary(fpscr | SF_FPSCR_XX);
    fpscr |= SF_FPSCR_FI;
    if (fabs(rounded) > fabs(exact)) fpscr |= SF_FPSCR_FR;
    return fpscr;
}
static SPLAT_FI f64 sf_25bit(f64 value) {   // = force_25_bit
    u64 bits = sf_bits64(value);
    if ((bits & 0x7FF0000000000000ull) == 0 && (bits & 0x000FFFFFFFFFFFFFull) != 0) return splat_force_25_bit_ool(value);
    return sf_from64((bits & 0xFFFFFFFFF8000000ull) + (bits & 0x0000000008000000ull));
}

// fadds / fsubs / fmuls / fdivs. a, b: the operands as used (b already cut
// to 25 bits for fmuls); r: the exact result.
static SPLAT_FI bool sf_single(CPUState* cpu, u8 d, f64 a, f64 b, f64 r) {
    if (sf_direct(cpu)) {
        if (r != r) return false;
        f64 s = (f64)(f32)r;
        cpu->fpr[d] = s;
        cpu->ps1[d] = s;
        return true;
    }
    if ((cpu->fpscr & SF_FPSCR_NI) || !sf_fin(a) || !sf_fin(b) || !sf_fin(r)) return false;
    f32 rounded = (f32)r;
    u32 fp = sf_fi_fr(cpu->fpscr, r, (f64)rounded);
    cpu->fpscr = sf_fprf(fp, sf_class32(rounded));
    cpu->fpr[d] = (f64)rounded;
    cpu->ps1[d] = (f64)rounded;
    return true;
}
static SPLAT_FI void sfi_fadds(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (!sf_single(cpu, d, x, y, x + y)) ppc_fadds_exact(cpu, d, a, b);
}
static SPLAT_FI void sfi_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (!sf_single(cpu, d, x, y, x - y)) ppc_fsubs_exact(cpu, d, a, b);
}
static SPLAT_FI void sfi_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) {
    // (cutting the C operand to 25 bits can turn a NaN into a number: check it first)
    f64 x = cpu->fpr[a], c0 = cpu->fpr[c], y = sf_25bit(c0);
    if (c0 != c0 || !sf_single(cpu, d, x, y, x * y)) ppc_fmuls_exact(cpu, d, a, c);
}
static SPLAT_FI void sfi_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (!sf_single(cpu, d, x, y, x / y)) ppc_fdivs_exact(cpu, d, a, b);
}

// fadd / fsub / fmul / fdiv (double)
static SPLAT_FI bool sf_double(CPUState* cpu, u8 d, f64 a, f64 b, f64 r, bool mul) {
    if (sf_direct(cpu)) {
        if (r != r) return false;
        cpu->fpr[d] = r;
        return true;
    }
    if ((cpu->fpscr & SF_FPSCR_NI) || !sf_fin(a) || !sf_fin(b) || !sf_fin(r)) return false;
    u32 fp = cpu->fpscr;
    if (mul) fp &= ~(SF_FPSCR_FI | SF_FPSCR_FR);
    cpu->fpscr = sf_fprf(fp, sf_class64(r));
    cpu->fpr[d] = r;
    return true;
}
static SPLAT_FI void sfi_fadd(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (!sf_double(cpu, d, x, y, x + y, false)) ppc_fadd_exact(cpu, d, a, b);
}
static SPLAT_FI void sfi_fsub(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (!sf_double(cpu, d, x, y, x - y, false)) ppc_fsub_exact(cpu, d, a, b);
}
static SPLAT_FI void sfi_fmul(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 x = cpu->fpr[a], y = cpu->fpr[c];
    if (!sf_double(cpu, d, x, y, x * y, true)) ppc_fmul_exact(cpu, d, a, c);
}
static SPLAT_FI void sfi_fdiv(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (!sf_double(cpu, d, x, y, x / y, false)) ppc_fdiv_exact(cpu, d, a, b);
}

// = single_fma_value (the C operand cut to 25 bits; a tie in the single
// rounding settled the way the hardware does)
static SPLAT_FI f64 sf_fma_single(CPUState* cpu, f64 a, f64 c, f64 addend) {
    f64 rc = sf_25bit(c);
    f64 result = sf_fma(a, rc, addend);
    if ((cpu->fpscr & 3u) == 0 && (sf_bits64(result) & 0x000000001FFFFFFFull) == 0x0000000010000000ull)
        result = splat_fma_tie_ool(a, rc, addend, result);
    return result;
}

// fmadd(s) / fmsub(s) / fnmadd(s) / fnmsub(s)
static SPLAT_FI bool sfi_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative, f64* output) {
    bool direct = sf_direct(cpu);
    if (!direct && (!sf_fin(a) || !sf_fin(b) || !sf_fin(c))) return ppc_fma_exact(cpu, a, c, b, single, subtract, negative, output);
    f64 addend = subtract ? -b : b;
    f64 result = single ? (f64)(f32)sf_fma_single(cpu, a, c, addend) : sf_fma(a, c, addend);
    if (result != result || c != c) return ppc_fma_exact(cpu, a, c, b, single, subtract, negative, output);
    if (negative) result = -result;
    if (!direct) cpu->fpscr = sf_fprf(cpu->fpscr, single ? sf_class32((f32)result) : sf_class64(result));
    *output = result;
    return true;
}

static SPLAT_FI void sfi_frsp(CPUState* cpu, u8 d, u8 b) {
    f64 value = cpu->fpr[b];
    if (sf_direct(cpu) && value == value) {
        f64 s = (f64)(f32)value;
        cpu->fpr[d] = s;
        cpu->ps1[d] = s;
        return;
    }
    if ((cpu->fpscr & SF_FPSCR_NI) || !sf_fin(value)) { ppc_frsp_exact(cpu, d, b); return; }
    f32 rounded = (f32)value;
    u32 fp = sf_fi_fr(cpu->fpscr, value, (f64)rounded);
    cpu->fpscr = sf_fprf(fp, sf_class32(rounded));
    cpu->fpr[d] = (f64)rounded;
    cpu->ps1[d] = (f64)rounded;
}

static SPLAT_FI void sfi_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) {
    if (a != a || b != b) { ppc_fcmp_exact(cpu, crfd, a, b, ordered); return; }
    u32 compare = a < b ? 8u : a > b ? 4u : 2u;
    if (splat_exact_fpscr) cpu->fpscr = (cpu->fpscr & ~(0xFu << 12)) | (compare << 12);
    u32 shift = 4u * (7u - crfd);
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (compare << shift);
}

// Paired singles: NaN results (and only those) need DolRecomp's NaN rules.
static SPLAT_FI void sf_paired(CPUState* cpu, u8 d, f64 ps0, f64 ps1) {   // = write_paired_result
    f32 s0 = (f32)ps0;
    cpu->fpr[d] = (f64)s0;
    cpu->ps1[d] = (f64)(f32)ps1;
    if (splat_exact_fpscr) cpu->fpscr = sf_fprf(cpu->fpscr, sf_class32(s0));
}
#define SF_NAN2(x, y) ((x) != (x) || (y) != (y))

static SPLAT_FI void sfi_ps_add(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 r0 = cpu->fpr[a] + cpu->fpr[b], r1 = cpu->ps1[a] + cpu->ps1[b];
    if (SF_NAN2(r0, r1)) { ppc_ps_add_op_exact(cpu, d, a, b); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_sub(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 r0 = cpu->fpr[a] - cpu->fpr[b], r1 = cpu->ps1[a] - cpu->ps1[b];
    if (SF_NAN2(r0, r1)) { ppc_ps_sub_op_exact(cpu, d, a, b); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_mul(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c0 = cpu->fpr[c], c1 = cpu->ps1[c];
    f64 r0 = cpu->fpr[a] * sf_25bit(c0), r1 = cpu->ps1[a] * sf_25bit(c1);
    if (SF_NAN2(r0, r1) || SF_NAN2(c0, c1)) { ppc_ps_mul_op_exact(cpu, d, a, c); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_div(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 r0 = cpu->fpr[a] / cpu->fpr[b], r1 = cpu->ps1[a] / cpu->ps1[b];
    if (SF_NAN2(r0, r1)) { ppc_ps_div_op_exact(cpu, d, a, b); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c0 = cpu->fpr[c], k = sf_25bit(c0);
    f64 r0 = cpu->fpr[a] * k, r1 = cpu->ps1[a] * k;
    if (SF_NAN2(r0, r1) || c0 != c0) { ppc_ps_muls0_exact(cpu, d, a, c); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c1 = cpu->ps1[c], k = sf_25bit(c1);
    f64 r0 = cpu->fpr[a] * k, r1 = cpu->ps1[a] * k;
    if (SF_NAN2(r0, r1) || c1 != c1) { ppc_ps_muls1_exact(cpu, d, a, c); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 r0 = cpu->fpr[a] + cpu->ps1[b], r1 = cpu->ps1[c];
    if (r0 != r0) { ppc_ps_sum0_exact(cpu, d, a, c, b); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 r0 = cpu->fpr[c], r1 = cpu->fpr[a] + cpu->ps1[b];
    if (r1 != r1) { ppc_ps_sum1_exact(cpu, d, a, c, b); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_madd(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative) {
    f64 b0 = cpu->fpr[b], b1 = cpu->ps1[b], c0 = cpu->fpr[c], c1 = cpu->ps1[c];
    f64 r0 = sf_fma_single(cpu, cpu->fpr[a], c0, subtract ? -b0 : b0);
    f64 r1 = sf_fma_single(cpu, cpu->ps1[a], c1, subtract ? -b1 : b1);
    if (SF_NAN2(r0, r1) || SF_NAN2(c0, c1)) { ppc_ps_madd_op_exact(cpu, d, a, c, b, subtract, negative); return; }
    if (negative) { r0 = -(f64)(f32)r0; r1 = -(f64)(f32)r1; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 c0 = cpu->fpr[c];
    f64 r0 = sf_fma_single(cpu, cpu->fpr[a], c0, cpu->fpr[b]);
    f64 r1 = sf_fma_single(cpu, cpu->ps1[a], c0, cpu->ps1[b]);
    if (SF_NAN2(r0, r1) || c0 != c0) { ppc_ps_madds0_exact(cpu, d, a, c, b); return; }
    sf_paired(cpu, d, r0, r1);
}
static SPLAT_FI void sfi_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 c1 = cpu->ps1[c];
    f64 r0 = sf_fma_single(cpu, cpu->fpr[a], c1, cpu->fpr[b]);
    f64 r1 = sf_fma_single(cpu, cpu->ps1[a], c1, cpu->ps1[b]);
    if (SF_NAN2(r0, r1) || c1 != c1) { ppc_ps_madds1_exact(cpu, d, a, c, b); return; }
    sf_paired(cpu, d, r0, r1);
}

// --- paired-single loads and stores (psq_l / psq_st) --------------------------
// The common case (plain floats, aligned) straight on guest memory.
static SPLAT_FI u32 sf_ld32(u32 a) {
    u32 v;
    memcpy(&v, wiiu_fastmem + a, 4);
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
static SPLAT_FI void sf_st32(u32 a, u32 v) {
#if defined(_MSC_VER) && !defined(__clang__)
    v = _byteswap_ulong(v);
#else
    v = __builtin_bswap32(v);
#endif
    memcpy(wiiu_fastmem + a, &v, 4);
}
// single bits -> double, as lfs / psq_l do (= extend_f32_bits)
static SPLAT_FI f64 sf_single_from_bits(u32 bits) {
    u32 e = bits & 0x7F800000u;
    if ((e != 0 && e != 0x7F800000u) || (bits & 0x7FFFFFFFu) == 0) {   // an ordinary number or zero: the conversion is exact
        f32 f;
        memcpy(&f, &bits, 4);
        return (f64)f;
    }
    return splat_extend_f32_ool(bits);
}
// double -> single bits, as psq_st does (denormals stored as 0)
static SPLAT_FI u32 sf_psq_store_bits(f64 value) {
    u32 b = sf_bits32((f32)value);
    return ((b & 0x7F800000u) == 0 && (b & 0x007FFFFFu) != 0) ? 0u : b;
}
static SPLAT_FI bool sfi_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    u32 gqr = cpu->gqr[gqr_index & 7u];
    if (wiiu_fastmem && (gqr & 0x00070000u) == 0 && (ea & 3u) == 0 &&
        (cpu->hid2 & PPC_HID2_PSE) && (indexed || (cpu->hid2 & PPC_HID2_LSQE))) {
        cpu->fpr[frD] = sf_single_from_bits(sf_ld32(ea));
        cpu->ps1[frD] = w ? 1.0 : sf_single_from_bits(sf_ld32(ea + 4));
        return true;
    }
    return ppc_psq_load_general(cpu, frD, ea, w, gqr_index, indexed, cia);
}
static SPLAT_FI bool sfi_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    u32 gqr = cpu->gqr[gqr_index & 7u];
    if (wiiu_fastmem && (gqr & 7u) == 0 && (ea & 3u) == 0 &&
        (cpu->hid2 & PPC_HID2_PSE) && (indexed || (cpu->hid2 & PPC_HID2_LSQE))) {
        sf_st32(ea, sf_psq_store_bits(cpu->fpr[frS]));
        if (!w) sf_st32(ea + 4, sf_psq_store_bits(cpu->ps1[frS]));
        return true;
    }
    return ppc_psq_store_general(cpu, frS, ea, w, gqr_index, indexed, cia);
}

#endif
