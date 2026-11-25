// Latte shader programs -> GLSL (see latte_shader.h).
//
// A Latte program is a list of control-flow (CF) instructions. Most of them
// point at "clauses": ALU clauses (math, in groups of up to five operations
// that run side by side: units x, y, z, w and t) and TEX clauses (texture
// reads and buffer fetches). EXPORT instructions hand results to the next
// stage (positions and parameters from the vertex shader, colors and depth
// from the pixel shader).
//
// Every register is kept as raw 32-bit integers (ivec4) and converted to
// float where an instruction does float math, which keeps integer data and
// bit tricks exact. Instruction groups read their inputs before any of the
// group's results are written, like the hardware. Branches use the same
// "active mask stack" model the hardware uses for its per-pixel predicates.
//
// Instruction encodings and behaviour follow AMD's R600/R700 ISA documents,
// cross-checked against Cemu's shader decompiler.
#include "latte_shader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- A growing string ----------------------------------------------------------------

typedef struct { char* p; size_t n, cap; } Str;

static void sp(Str* s, const char* fmt, ...) {
    va_list ap;
    for (;;) {
        size_t room = s->cap - s->n;
        va_start(ap, fmt);
        int w = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? room : 0, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if (s->p && (size_t)w < room) { s->n += (size_t)w; return; }
        size_t want = s->cap ? s->cap * 2 : 4096;
        while (want < s->n + (size_t)w + 1) want *= 2;
        char* np = (char*)realloc(s->p, want);
        if (!np) return;
        s->p = np;
        s->cap = want;
    }
}

// --- Decoded instructions --------------------------------------------------------------

enum {
    CF_NOP = 0x00, CF_TEX = 0x01, CF_VTX = 0x02, CF_VTX_TC = 0x03, CF_LOOP_END = 0x05,
    CF_LOOP_START_DX10 = 0x06, CF_LOOP_START_NO_AL = 0x07, CF_LOOP_BREAK = 0x09, CF_JUMP = 0x0A,
    CF_ELSE = 0x0D, CF_POP = 0x0E, CF_CALL = 0x12, CF_CALL_FS = 0x13, CF_RETURN = 0x14,
    CF_EMIT_VERTEX = 0x15, CF_MEM_STREAM0 = 0x20, CF_MEM_STREAM1 = 0x21, CF_MEM_STREAM2 = 0x22,
    CF_MEM_STREAM3 = 0x23, CF_MEM_RING = 0x26,
    CF_EXPORT = 0x27, CF_EXPORT_DONE = 0x28,
    // ALU clauses (flagged so they can't collide with the others)
    CF_ALU = 0x108, CF_ALU_PUSH_BEFORE = 0x109, CF_ALU_POP_AFTER = 0x10A, CF_ALU_POP2_AFTER = 0x10B,
    CF_ALU_BREAK = 0x10E, CF_ALU_ELSE_AFTER = 0x10F,
};

typedef struct { u32 sel, rel, chan, neg, abs; } AluSrc;

typedef struct {
    bool op3, last;
    u32 op, unit;             // unit: 0-3 x/y/z/w, 4 = t
    AluSrc src[3];
    u32 nsrc;
    u32 dst, dst_rel, dst_elem, clamp, write, omod, index_mode;
    u32 update_exec, update_pred;
    u32 lit[4];
    u32 group;
} AluInst;

typedef struct {
    u32 type, addr, count, cond, pop;
    u32 kbank[2], kaddr[2];
    u32 exp_type, exp_base, exp_gpr, exp_burst, exp_sel[4];
    u32 so_size, so_mask;      // MEM_STREAM: array size, component mask
    int depth;
    bool eop;
} CfInst;

#define MAX_CF 1024

typedef struct {
    const LatteShaderKey* key;
    bool vertex;
    const u8* prog;
    u32 size;
    CfInst cf[MAX_CF];
    u32 ncf;
    bool uses_flow;
    int max_depth;
    bool rel_gpr;
    u32 gpr_used[4];          // bitmask of GPRs touched
    Str body;
    LatteShaderOut* out;
    u32 tex_shadow;
    u32 tex_used;
    u32 ub_used;
    u32 cfile_max;
    u16 ub_fixmax[16];        // per uniform block: 1 + the highest fixed index read (see assemble)
    bool uses_ar, uses_cube, uses_grad, uses_kill;
    u32 so_mask;
    bool ps_input_used;
    int indent;
    u32 changed;              // uses something translated differently since: 1 = shader cache 0x53430007
                              // (abs/neg on integer operands), 2 = v0.19 (cube-map array index)
    u32 cube_idx;             // texture units whose cube SET_CUBEMAP_INDEX picks (read as cube-map arrays)
} Ctx;

static u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void warn(Ctx* c, const char* fmt, ...) {
    c->out->warnings++;
    if (c->out->warnings > 6) return;
    va_list ap;
    va_start(ap, fmt);
    printf("[shader] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void use_gpr(Ctx* c, u32 g) { if (g < 128) c->gpr_used[g >> 5] |= 1u << (g & 31); }

static void line(Ctx* c, const char* fmt, ...) {
    for (int i = 0; i < c->indent; i++) sp(&c->body, "  ");
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sp(&c->body, "%s\n", buf);
}

// --- Control flow parsing -------------------------------------------------------------------

static bool parse_cf(Ctx* c) {
    c->ncf = 0;
    for (u32 i = 0; i * 8 + 8 <= c->size && c->ncf < MAX_CF; i++) {
        u32 w0 = rd32(c->prog + i * 8), w1 = rd32(c->prog + i * 8 + 4);
        CfInst* f = &c->cf[c->ncf];
        memset(f, 0, sizeof(*f));
        u32 inst = (w1 >> 23) & 0x7F;
        if (inst < 0x40) {
            f->type = inst;
            f->eop = (w1 >> 21) & 1;
            f->addr = w0;
            f->count = (((w1 >> 10) & 7) | (((w1 >> 19) & 1) << 3)) + 1;
            f->cond = (w1 >> 8) & 3;
            f->pop = w1 & 7;
            if (inst == CF_EXPORT || inst == CF_EXPORT_DONE) {
                f->exp_base = w0 & 0x1FFF;
                f->exp_type = (w0 >> 13) & 3;
                f->exp_gpr = (w0 >> 15) & 0x7F;
                f->exp_burst = (w1 >> 17) & 0xF;
                for (int k = 0; k < 4; k++) f->exp_sel[k] = (w1 >> (3 * k)) & 7;
            }
            if (inst >= CF_MEM_STREAM0 && inst <= CF_MEM_STREAM3) {
                f->exp_base = w0 & 0x1FFF;
                f->exp_gpr = (w0 >> 15) & 0x7F;
                f->so_size = w1 & 0xFFF;
                f->so_mask = (w1 >> 12) & 0xF;
                f->exp_burst = (w1 >> 17) & 0xF;
            }
            switch (inst) {
            case CF_NOP: case CF_TEX: case CF_LOOP_END: case CF_LOOP_START_DX10: case CF_LOOP_START_NO_AL:
            case CF_LOOP_BREAK: case CF_JUMP: case CF_ELSE: case CF_POP: case CF_CALL_FS: case CF_RETURN:
            case CF_EXPORT: case CF_EXPORT_DONE: case CF_VTX: case CF_VTX_TC:
            case CF_MEM_STREAM0: case CF_MEM_STREAM1: case CF_MEM_STREAM2: case CF_MEM_STREAM3:
                break;
            default:
                warn(c, "control instruction %02X not supported (skipped)", inst);
                break;
            }
            c->ncf++;
            if (f->eop) break;
        } else {
            u32 a = ((w1 >> 26) & 0xF) | 0x100;
            f->type = a;
            f->addr = w0 & 0x3FFFFF;
            f->count = ((w1 >> 18) & 0x7F) + 1;
            f->kbank[0] = (w0 >> 22) & 0xF;
            f->kbank[1] = (w0 >> 26) & 0xF;
            f->kaddr[0] = ((w1 >> 2) & 0xFF) * 16;
            f->kaddr[1] = ((w1 >> 10) & 0xFF) * 16;
            if (a != CF_ALU && a != CF_ALU_PUSH_BEFORE && a != CF_ALU_POP_AFTER && a != CF_ALU_POP2_AFTER &&
                a != CF_ALU_BREAK && a != CF_ALU_ELSE_AFTER) {
                warn(c, "ALU clause type %X not supported", a);
                f->type = CF_NOP;
            }
            c->ncf++;
        }
    }
    // Stack depths, like the hardware's push/pop.
    int d = 0;
    c->max_depth = 0;
    for (u32 i = 0; i < c->ncf; i++) {
        CfInst* f = &c->cf[i];
        switch (f->type) {
        case CF_ALU_PUSH_BEFORE:
            d++;
            if (d > c->max_depth) c->max_depth = d;
            f->depth = d;
            c->uses_flow = true;
            break;
        case CF_ALU_POP_AFTER: f->depth = d; d -= 1; c->uses_flow = true; break;
        case CF_ALU_POP2_AFTER: f->depth = d; d -= 2; c->uses_flow = true; break;
        case CF_POP: f->depth = d; d -= (int)f->pop; c->uses_flow = true; break;
        case CF_ELSE: case CF_ALU_ELSE_AFTER: case CF_LOOP_START_DX10: case CF_LOOP_START_NO_AL:
        case CF_LOOP_END: case CF_LOOP_BREAK: case CF_ALU_BREAK:
            f->depth = d;
            c->uses_flow = true;
            break;
        default: f->depth = d; break;
        }
        if (d < 0) d = 0;
    }
    return true;
}

// --- Operands -----------------------------------------------------------------------------

static const char* comp(u32 i) { static const char* n[] = { "x", "y", "z", "w" }; return n[i & 3]; }

static void reg_name(Ctx* c, u32 g, char* out) {
    use_gpr(c, g);
    if (c->rel_gpr) sprintf(out, "R[%u]", g);
    else sprintf(out, "R%u", g);
}

static const char* ar_name(Ctx* c, u32 mode) {
    c->uses_ar = true;
    switch (mode) {
    case 0: return "AR.x";
    case 1: return "AR.y";
    case 2: return "AR.z";
    case 3: return "AR.w";
    default: return "0";
    }
}

// Raw 32-bit integer expression of an ALU source (before neg/abs).
static void src_raw(Ctx* c, const CfInst* f, const AluInst* in, int i, char* out) {
    const AluSrc* s = &in->src[i];
    u32 sel = s->sel;
    if (sel < 128) {
        if (s->rel) {
            c->rel_gpr = true;
            sprintf(out, "R[clamp(%u + %s, 0, 127)].%s", sel, ar_name(c, in->index_mode), comp(s->chan));
        } else {
            char r[16];
            reg_name(c, sel, r);
            sprintf(out, "%s.%s", r, comp(s->chan));
        }
    } else if (sel < 192) {
        u32 b = sel < 160 ? 0 : 1;
        u32 bank = f->kbank[b];
        u32 idx = (sel - (b ? 160 : 128)) + f->kaddr[b];
        c->ub_used |= 1u << bank;
        if (s->rel) sprintf(out, "ub%u(%u + %s, %u)", bank, idx, ar_name(c, in->index_mode), s->chan);
        else if (new_forms()) {   // (fixed index: by word, see assemble)
            sprintf(out, "ubs%u(%uu)", bank, idx * 4 + s->chan);
            if (idx + 1 > c->ub_fixmax[bank]) c->ub_fixmax[bank] = (u16)(idx + 1 > 65535 ? 65535 : idx + 1);
        }
        else sprintf(out, "ub%u(%u, %u)", bank, idx, s->chan);
    } else if (sel >= 256) {
        u32 idx = sel - 256;
        if (idx + 1 > c->cfile_max) c->cfile_max = idx + 1;
        if (s->rel) {
            c->cfile_max = 256;
            sprintf(out, "cf(%u + %s, %u)", idx, ar_name(c, in->index_mode), s->chan);
        } else if (new_forms()) sprintf(out, "int(d[cf_base + %uu])", idx * 4 + s->chan);   // (all 256 are uploaded: no clamp needed)
        else sprintf(out, "cf(%u, %u)", idx, s->chan);
    } else {
        switch (sel) {
        case 248: strcpy(out, "0"); break;
        case 249: strcpy(out, "1065353216"); break;   // 1.0
        case 250: strcpy(out, "1"); break;
        case 251: strcpy(out, "(-1)"); break;
        case 252: strcpy(out, "1056964608"); break;   // 0.5
        case 253: sprintf(out, "int(%uu)", in->lit[s->chan]); break;
        case 254: sprintf(out, "PV.%s", comp(s->chan)); break;
        case 255: strcpy(out, "PSv"); break;
        default:
            warn(c, "ALU source %u not supported", sel);
            strcpy(out, "0");
            break;
        }
    }
}

static void src_f(Ctx* c, const CfInst* f, const AluInst* in, int i, char* out) {
    char raw[256];
    src_raw(c, f, in, i, raw);
    const AluSrc* s = &in->src[i];
    if (s->abs && s->neg) sprintf(out, "(-abs(intBitsToFloat(%s)))", raw);
    else if (s->abs) sprintf(out, "abs(intBitsToFloat(%s))", raw);
    else if (s->neg) sprintf(out, "(-intBitsToFloat(%s))", raw);
    else sprintf(out, "intBitsToFloat(%s)", raw);
}

// (the source modifiers are float operations on the bits even for integer
// instructions: neg flips the sign bit, abs clears it -- e.g. CNDE_INT
// choosing between x and -x for a texture flip. An integer negate there
// turned -0.5 into 8.0: the blocky particles.)
static void src_i(Ctx* c, const CfInst* f, const AluInst* in, int i, char* out) {
    char raw[256];
    src_raw(c, f, in, i, raw);
    const AluSrc* s = &in->src[i];
    if (s->abs || s->neg) c->changed |= 1;
    if (s->abs && s->neg) sprintf(out, "floatBitsToInt(-abs(intBitsToFloat(%s)))", raw);
    else if (s->abs) sprintf(out, "floatBitsToInt(abs(intBitsToFloat(%s)))", raw);
    else if (s->neg) sprintf(out, "floatBitsToInt(-intBitsToFloat(%s))", raw);
    else strcpy(out, raw);
}

static void src_u(Ctx* c, const CfInst* f, const AluInst* in, int i, char* out) {
    char raw[300];
    src_i(c, f, in, i, raw);
    sprintf(out, "uint(%s)", raw);
}

// --- ALU clauses ---------------------------------------------------------------------------

static bool is_trans_only(u32 op) {
    switch (op) {
    case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69:
    case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79:
        return true;
    default: return false;
    }
}

static u32 parse_alu(Ctx* c, const CfInst* f, AluInst* list, u32 max) {
    u32 n = 0, group = 0, written = 0, group_start = 0;
    u32 lit_mask = 0;
    u32 i = 0;
    while (i < f->count && n < max) {
        u32 off = (f->addr + i) * 8;
        if (off + 8 > c->size) break;
        u32 w0 = rd32(c->prog + off), w1 = rd32(c->prog + off + 4);
        i++;
        AluInst* in = &list[n];
        memset(in, 0, sizeof(*in));
        in->last = (w0 >> 31) & 1;
        in->src[0].sel = w0 & 0x1FF; in->src[0].rel = (w0 >> 9) & 1; in->src[0].chan = (w0 >> 10) & 3; in->src[0].neg = (w0 >> 12) & 1;
        in->src[1].sel = (w0 >> 13) & 0x1FF; in->src[1].rel = (w0 >> 22) & 1; in->src[1].chan = (w0 >> 23) & 3; in->src[1].neg = (w0 >> 25) & 1;
        in->index_mode = (w0 >> 26) & 7;
        in->dst = (w1 >> 21) & 0x7F;
        in->dst_rel = (w1 >> 28) & 1;
        in->dst_elem = (w1 >> 29) & 3;
        in->clamp = (w1 >> 31) & 1;
        in->group = group;
        u32 op3 = (w1 >> 13) & 0x1F;
        if (op3 >= 8) {
            in->op3 = true;
            in->op = op3;
            in->src[2].sel = w1 & 0x1FF; in->src[2].rel = (w1 >> 9) & 1; in->src[2].chan = (w1 >> 10) & 3; in->src[2].neg = (w1 >> 12) & 1;
            in->nsrc = 3;
            in->write = 1;
        } else {
            in->op = (w1 >> 7) & 0x7FF;
            in->src[0].abs = w1 & 1;
            in->src[1].abs = (w1 >> 1) & 1;
            in->update_exec = (w1 >> 2) & 1;
            in->update_pred = (w1 >> 3) & 1;
            in->write = (w1 >> 4) & 1;
            in->omod = (w1 >> 5) & 3;
            in->nsrc = 2;
        }
        for (u32 k = 0; k < in->nsrc; k++)
            if (in->src[k].sel == 253) lit_mask |= 1u << in->src[k].chan;
        u32 unit = in->dst_elem;
        if (!in->op3 && is_trans_only(in->op)) unit = 4;
        if (unit < 4 && (written & (1u << unit))) unit = 4;
        written |= 1u << unit;
        in->unit = unit;
        n++;
        if (in->last) {
            u32 lit[4] = { 0, 0, 0, 0 };
            if (lit_mask) {
                u32 lo = (f->addr + i) * 8;
                if (lo + 8 <= c->size) { lit[0] = rd32(c->prog + lo); lit[1] = rd32(c->prog + lo + 4); }
                if (lit_mask & 0xC) {
                    if (lo + 16 <= c->size) { lit[2] = rd32(c->prog + lo + 8); lit[3] = rd32(c->prog + lo + 12); }
                    i += 2;
                } else {
                    i += 1;
                }
            }
            for (u32 k = group_start; k < n; k++) memcpy(list[k].lit, lit, sizeof(lit));
            lit_mask = 0;
            written = 0;
            group++;
            group_start = n;
        }
    }
    return n;
}

// Is this op's result a float (gets omod/clamp)?
static bool result_is_float(const AluInst* in) {
    if (in->op3) return in->op >= 0x10 && in->op <= 0x14;
    switch (in->op) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69:
    case 0x6A: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        return true;
    default: return false;
    }
}

static const char* tname(u32 unit) { static const char* n[] = { "tx", "ty", "tz", "tw", "tt" }; return n[unit]; }

// Wrap a float expression with the output modifier and clamp, return as int bits.
static void finish_float(const AluInst* in, const char* fexpr, char* out) {
    char tmp[1400];
    const char* m = fexpr;
    if (!in->op3 && in->omod) {
        static const char* mul[] = { "", "2.0", "4.0", "0.5" };
        snprintf(tmp, sizeof(tmp), "((%s) * %s)", fexpr, mul[in->omod]);
        m = tmp;
    }
    if (in->clamp) snprintf(out, 1500, "floatBitsToInt(clamp(%s, 0.0, 1.0))", m);
    else snprintf(out, 1500, "floatBitsToInt(%s)", m);
}

static void emit_pred(Ctx* c, const CfInst* f, const AluInst* in, const char* cond) {
    line(c, "pred = %s;", cond);
    if (f->type == CF_ALU_PUSH_BEFORE) {
        line(c, "am[%d] = pred;", f->depth);
        line(c, "ac[%d] = pred && ac[%d];", f->depth + 1, f->depth);
    } else if (f->type == CF_ALU_BREAK) {
        if (in->update_exec) line(c, "aluBreak = aluBreak || !pred;");
    } else if (in->update_exec) {
        warn(c, "predicate in a plain ALU clause");
    }
}

static int new_forms(void);   // (smaller driver work per shader: see k_helpers)

static void emit_alu_inst(Ctx* c, const CfInst* f, const AluInst* in) {
    char a[400], b[400], d[400], e[1500];
    const char* t = tname(in->unit);
    e[0] = 0;
    if (in->op3) {
        switch (in->op) {
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: {   // MULADD (_M2, _M4, _D2, _IEEE)
            src_f(c, f, in, 0, a); src_f(c, f, in, 1, b); src_f(c, f, in, 2, d);
            char x[1300];
            if (in->op == 0x14) snprintf(x, sizeof(x), "(%s * %s + %s)", a, b, d);
            else snprintf(x, sizeof(x), "(mul_ni(%s, %s) + %s)", a, b, d);
            char y[1400];
            if (in->op == 0x11) snprintf(y, sizeof(y), "(%s * 2.0)", x);
            else if (in->op == 0x12) snprintf(y, sizeof(y), "(%s * 4.0)", x);
            else if (in->op == 0x13) snprintf(y, sizeof(y), "(%s * 0.5)", x);
            else snprintf(y, sizeof(y), "%s", x);
            finish_float(in, y, e);
            break;
        }
        case 0x18: case 0x19: case 0x1A: {   // CNDE, CNDGT, CNDGE
            src_f(c, f, in, 0, a);
            char p[400], q[400];
            src_raw(c, f, in, 1, p); src_raw(c, f, in, 2, q);
            // (neg on the selected values is a float negate)
            char pv[500], qv[500];
            if (in->src[1].neg) snprintf(pv, sizeof(pv), "floatBitsToInt(-intBitsToFloat(%s))", p); else snprintf(pv, sizeof(pv), "%s", p);
            if (in->src[2].neg) snprintf(qv, sizeof(qv), "floatBitsToInt(-intBitsToFloat(%s))", q); else snprintf(qv, sizeof(qv), "%s", q);
            const char* op = in->op == 0x18 ? "==" : in->op == 0x19 ? ">" : ">=";
            if (new_forms()) snprintf(e, sizeof(e), "mix(%s, %s, %s %s 0.0)", qv, pv, a, op);
            else snprintf(e, sizeof(e), "((%s %s 0.0) ? %s : %s)", a, op, pv, qv);
            if (in->clamp) { char z[1500]; snprintf(z, sizeof(z), "floatBitsToInt(clamp(intBitsToFloat(%s), 0.0, 1.0))", e); strcpy(e, z); }
            break;
        }
        case 0x1C: case 0x1D: case 0x1E: {   // CNDE_INT, CNDGT_INT, CNDGE_INT
            src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); src_i(c, f, in, 2, d);
            const char* op = in->op == 0x1C ? "==" : in->op == 0x1D ? ">" : ">=";
            if (new_forms()) snprintf(e, sizeof(e), "mix(%s, %s, %s %s 0)", d, b, a, op);
            else snprintf(e, sizeof(e), "((%s %s 0) ? %s : %s)", a, op, b, d);
            break;
        }
        default:
            warn(c, "ALU op3 %02X not supported", in->op);
            strcpy(e, "0");
            break;
        }
        line(c, "%s = %s;", t, e);
        return;
    }

    u32 op = in->op;
    char fx[1300];
    switch (op) {
    case 0x00: src_f(c, f, in, 0, a); src_f(c, f, in, 1, b); snprintf(fx, sizeof(fx), "(%s + %s)", a, b); finish_float(in, fx, e); break;
    case 0x01: src_f(c, f, in, 0, a); src_f(c, f, in, 1, b); snprintf(fx, sizeof(fx), "mul_ni(%s, %s)", a, b); finish_float(in, fx, e); break;
    case 0x02: src_f(c, f, in, 0, a); src_f(c, f, in, 1, b); snprintf(fx, sizeof(fx), "(%s * %s)", a, b); finish_float(in, fx, e); break;
    case 0x03: case 0x05: src_f(c, f, in, 0, a); src_f(c, f, in, 1, b); snprintf(fx, sizeof(fx), "max(%s, %s)", a, b); finish_float(in, fx, e); break;
    case 0x04: case 0x06: src_f(c, f, in, 0, a); src_f(c, f, in, 1, b); snprintf(fx, sizeof(fx), "min(%s, %s)", a, b); finish_float(in, fx, e); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: {   // SETE/GT/GE/NE -> 1.0 / 0.0
        static const char* ops[] = { "==", ">", ">=", "!=" };
        src_f(c, f, in, 0, a); src_f(c, f, in, 1, b);
        snprintf(fx, sizeof(fx), "((%s %s %s) ? 1.0 : 0.0)", a, ops[op - 8], b);
        finish_float(in, fx, e);
        break;
    }
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: {   // SET*_DX10 -> -1 / 0
        static const char* ops[] = { "==", ">", ">=", "!=" };
        src_f(c, f, in, 0, a); src_f(c, f, in, 1, b);
        snprintf(e, sizeof(e), "((%s %s %s) ? -1 : 0)", a, ops[op - 0x0C], b);
        break;
    }
    case 0x10: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "fract(%s)", a); finish_float(in, fx, e); break;
    case 0x11: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "trunc(%s)", a); finish_float(in, fx, e); break;
    case 0x12: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "ceil(%s)", a); finish_float(in, fx, e); break;
    case 0x13: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "roundEven(%s)", a); finish_float(in, fx, e); break;
    case 0x14: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "floor(%s)", a); finish_float(in, fx, e); break;
    case 0x15: case 0x16: case 0x17: {   // MOVA, MOVA_FLOOR, MOVA_ROUND
        src_f(c, f, in, 0, a);
        line(c, "tf = %s(%s);", op == 0x17 ? "roundEven" : "floor", a);
        line(c, "tf = (tf >= -256.0 && tf <= 255.0) ? tf : -256.0;");
        line(c, "AR.%s = int(tf);", comp(in->dst_elem));
        c->uses_ar = true;
        strcpy(e, "floatBitsToInt(tf)");
        break;
    }
    case 0x18: {   // MOVA_INT
        src_i(c, f, in, 0, a);
        line(c, "ti = %s;", a);
        line(c, "ti = (ti >= -256 && ti <= 255) ? ti : -256;");
        line(c, "AR.%s = ti;", comp(in->dst_elem));
        c->uses_ar = true;
        strcpy(e, "ti");
        break;
    }
    case 0x19: {   // MOV: raw bits unless modifiers say otherwise
        if (in->src[0].abs || in->src[0].neg || in->omod || in->clamp) {
            src_f(c, f, in, 0, a);
            finish_float(in, a, e);
        } else {
            src_raw(c, f, in, 0, e);
        }
        break;
    }
    case 0x1A: return;   // NOP
    case 0x20: case 0x21: case 0x22: case 0x23: {   // PRED_SETE/GT/GE/NE
        static const char* ops[] = { "==", ">", ">=", "!=" };
        src_f(c, f, in, 0, a); src_f(c, f, in, 1, b);
        snprintf(fx, sizeof(fx), "(%s %s %s)", a, ops[op - 0x20], b);
        emit_pred(c, f, in, fx);
        strcpy(e, "(pred ? 1065353216 : 0)");
        break;
    }
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: {   // KILLE/GT/GE/NE
        static const char* ops[] = { "==", ">", ">=", "!=" };
        src_f(c, f, in, 0, a); src_f(c, f, in, 1, b);
        if (!c->vertex) { line(c, "if (%s %s %s) discard;", a, ops[op - 0x2C], b); c->uses_kill = true; }
        strcpy(e, "0");
        break;
    }
    case 0x30: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s & %s)", a, b); break;
    case 0x31: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s | %s)", a, b); break;
    case 0x32: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s ^ %s)", a, b); break;
    case 0x33: src_i(c, f, in, 0, a); snprintf(e, sizeof(e), "(~%s)", a); break;
    case 0x34: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s + %s)", a, b); break;
    case 0x35: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s - %s)", a, b); break;
    case 0x36: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "max(%s, %s)", a, b); break;
    case 0x37: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "min(%s, %s)", a, b); break;
    case 0x38: src_u(c, f, in, 0, a); src_u(c, f, in, 1, b); snprintf(e, sizeof(e), "int(max(%s, %s))", a, b); break;
    case 0x39: src_u(c, f, in, 0, a); src_u(c, f, in, 1, b); snprintf(e, sizeof(e), "int(min(%s, %s))", a, b); break;
    case 0x3A: case 0x3B: case 0x3C: case 0x3D: {
        static const char* ops[] = { "==", ">", ">=", "!=" };
        src_i(c, f, in, 0, a); src_i(c, f, in, 1, b);
        snprintf(e, sizeof(e), "((%s %s %s) ? -1 : 0)", a, ops[op - 0x3A], b);
        break;
    }
    case 0x3E: case 0x3F:
        src_u(c, f, in, 0, a); src_u(c, f, in, 1, b);
        snprintf(e, sizeof(e), "((%s %s %s) ? -1 : 0)", a, op == 0x3E ? ">" : ">=", b);
        break;
    case 0x42: case 0x43: case 0x44: case 0x45: {   // PRED_SET*_INT
        static const char* ops[] = { "==", ">", ">=", "!=" };
        src_i(c, f, in, 0, a); src_i(c, f, in, 1, b);
        snprintf(fx, sizeof(fx), "(%s %s %s)", a, ops[op - 0x42], b);
        emit_pred(c, f, in, fx);
        strcpy(e, "(pred ? 1065353216 : 0)");
        break;
    }
    case 0x46: case 0x47: case 0x48: case 0x49: {   // KILL*_INT
        static const char* ops[] = { "==", ">", ">=", "!=" };
        src_i(c, f, in, 0, a); src_i(c, f, in, 1, b);
        if (!c->vertex) { line(c, "if (%s %s %s) discard;", a, ops[op - 0x46], b); c->uses_kill = true; }
        strcpy(e, "0");
        break;
    }
    case 0x61: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "exp2(%s)", a); finish_float(in, fx, e); break;
    case 0x62: case 0x63:
        src_f(c, f, in, 0, a);
        line(c, "tf = log2(max(%s, 0.0));", a);
        if (op == 0x62) line(c, "if (isinf(tf)) tf = -3.40282347e+38;");
        finish_float(in, "tf", e);
        break;
    case 0x64: case 0x65: case 0x66:   // RECIP_CLAMPED / _FF / _IEEE
        src_f(c, f, in, 0, a);
        line(c, "tf = 1.0 / %s;", a);
        if (op == 0x64) line(c, "if (isinf(tf)) tf = intBitsToFloat((floatBitsToInt(tf) & int(0x80000000u)) | 0x7F7FFFFF);");
        if (op == 0x65) line(c, "if (isinf(tf)) tf = intBitsToFloat(floatBitsToInt(tf) & int(0x80000000u));");
        finish_float(in, "tf", e);
        break;
    case 0x67: case 0x68: case 0x69:   // RECIPSQRT
        src_f(c, f, in, 0, a);
        line(c, "tf = inversesqrt(%s);", a);
        if (op == 0x67) line(c, "if (isinf(tf)) tf = intBitsToFloat((floatBitsToInt(tf) & int(0x80000000u)) | 0x7F7FFFFF);");
        if (op == 0x68) line(c, "if (isinf(tf)) tf = intBitsToFloat(floatBitsToInt(tf) & int(0x80000000u));");
        finish_float(in, "tf", e);
        break;
    case 0x6A: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "sqrt(%s)", a); finish_float(in, fx, e); break;
    case 0x6B: src_f(c, f, in, 0, a); snprintf(e, sizeof(e), "int(%s)", a); break;
    case 0x6C: src_i(c, f, in, 0, a); snprintf(fx, sizeof(fx), "float(%s)", a); finish_float(in, fx, e); break;
    case 0x6D: src_u(c, f, in, 0, a); snprintf(fx, sizeof(fx), "float(%s)", a); finish_float(in, fx, e); break;
    case 0x6E: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "sin((%s) * 6.2831853)", a); finish_float(in, fx, e); break;
    case 0x6F: src_f(c, f, in, 0, a); snprintf(fx, sizeof(fx), "cos((%s) * 6.2831853)", a); finish_float(in, fx, e); break;
    case 0x70: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s >> (%s & 31))", a, b); break;
    case 0x71: src_u(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "int(%s >> uint(%s & 31))", a, b); break;
    case 0x72: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s << (%s & 31))", a, b); break;
    case 0x73: src_i(c, f, in, 0, a); src_i(c, f, in, 1, b); snprintf(e, sizeof(e), "(%s * %s)", a, b); break;
    case 0x75: src_u(c, f, in, 0, a); src_u(c, f, in, 1, b); snprintf(e, sizeof(e), "int(%s * %s)", a, b); break;
    case 0x74:   // MULHI_INT
        src_i(c, f, in, 0, a); src_i(c, f, in, 1, b);
        line(c, "imulExtended(%s, %s, ti, ti2);", a, b);
        strcpy(e, "ti");
        break;
    case 0x76:   // MULHI_UINT
        src_u(c, f, in, 0, a); src_u(c, f, in, 1, b);
        line(c, "umulExtended(%s, %s, tu, tu2);", a, b);
        strcpy(e, "int(tu)");
        break;
    case 0x79: src_f(c, f, in, 0, a); snprintf(e, sizeof(e), "int(uint(max(%s, 0.0)))", a); break;
    default:
        warn(c, "ALU op %02X not supported", op);
        strcpy(e, "0");
        break;
    }
    line(c, "%s = %s;", t, e);
}

// DOT4 / DOT4_IEEE / MAX4 / CUBE use all four slots x..w together.
static bool is_reduction(const AluInst* in) {
    return !in->op3 && (in->op == 0x50 || in->op == 0x51 || in->op == 0x52 || in->op == 0x53);
}

static void emit_reduction(Ctx* c, const CfInst* f, const AluInst* g[4]) {
    char a[4][400], b[4][400];
    for (int k = 0; k < 4; k++) {
        src_f(c, f, g[k], 0, a[k]);
        src_f(c, f, g[k], 1, b[k]);
    }
    u32 op = g[0]->op;
    if (op == 0x50 || op == 0x51) {
        if (op == 0x50)
            line(c, "tf = mul_ni(%s, %s) + mul_ni(%s, %s) + mul_ni(%s, %s) + mul_ni(%s, %s);",
                 a[0], b[0], a[1], b[1], a[2], b[2], a[3], b[3]);
        else
            line(c, "tf = dot(vec4(%s, %s, %s, %s), vec4(%s, %s, %s, %s));",
                 a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        for (int k = 0; k < 4; k++) {
            char e[1500];
            finish_float(g[k], "tf", e);
            line(c, "%s = %s;", tname(g[k]->unit), e);
        }
    } else if (op == 0x53) {
        line(c, "tf = max(max(%s, %s), max(%s, %s));", a[0], a[1], a[2], a[3]);
        for (int k = 0; k < 4; k++) {
            char e[1500];
            finish_float(g[k], "tf", e);
            line(c, "%s = %s;", tname(g[k]->unit), e);
        }
    } else {   // CUBE
        c->uses_cube = true;
        line(c, "redcCUBE(vec4(%s, %s, %s, %s), vec4(%s, %s, %s, %s), cubeSTM, cubeFace);",
             a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        static const char* part[] = { "cubeSTM.x", "cubeSTM.y", "cubeSTM.z", "float(cubeFace)" };
        for (int k = 0; k < 4; k++) {
            char e[1500];
            finish_float(g[k], part[k], e);
            line(c, "%s = %s;", tname(g[k]->unit), e);
        }
    }
}

static void emit_alu_clause(Ctx* c, const CfInst* f) {
    static AluInst list[1024];
    u32 n = parse_alu(c, f, list, 1024);
    u32 i = 0;
    while (i < n) {
        u32 start = i;
        while (i < n && !list[i].last) i++;
        u32 end = i < n ? i + 1 : n;
        i = end;
        // compute
        u32 units = 0;
        const AluInst* red[4] = { NULL, NULL, NULL, NULL };
        int nred = 0;
        for (u32 k = start; k < end; k++) {
            if (is_reduction(&list[k]) && list[k].unit < 4) {
                if (nred < 4) red[nred++] = &list[k];
                continue;
            }
            emit_alu_inst(c, f, &list[k]);
            if (list[k].op3 || list[k].op != 0x1A) units |= 1u << list[k].unit;
        }
        if (nred) {
            if (nred == 4) {
                // order by unit x, y, z, w
                const AluInst* g[4] = { NULL, NULL, NULL, NULL };
                for (int k = 0; k < 4; k++) g[red[k]->unit & 3] = red[k];
                bool ok = g[0] && g[1] && g[2] && g[3];
                if (ok) emit_reduction(c, f, g);
                else emit_reduction(c, f, red);   // (unusual layout)
            } else {
                warn(c, "reduction with %d slots", nred);
                const AluInst* g[4] = { red[0], red[nred > 1 ? 1 : 0], red[nred > 2 ? 2 : 0], red[nred > 3 ? 3 : 0] };
                emit_reduction(c, f, g);
            }
            for (int k = 0; k < nred; k++) units |= 1u << red[k]->unit;
        }
        // write registers
        for (u32 k = start; k < end; k++) {
            const AluInst* in = &list[k];
            if (!in->write) continue;
            if (!in->op3 && (in->op == 0x1A)) continue;
            if (in->dst_rel) {
                c->rel_gpr = true;
                use_gpr(c, in->dst);
                line(c, "R[clamp(%u + %s, 0, 127)].%s = %s;", in->dst, ar_name(c, in->index_mode), comp(in->dst_elem), tname(in->unit));
            } else {
                char r[16];
                reg_name(c, in->dst, r);
                line(c, "%s.%s = %s;", r, comp(in->dst_elem), tname(in->unit));
            }
        }
        // previous-result registers for the next group
        for (u32 u = 0; u < 4; u++)
            if (units & (1u << u)) line(c, "PV.%s = %s;", comp(u), tname(u));
        if (units & 16) line(c, "PSv = tt;");
    }
}

// --- TEX clauses ---------------------------------------------------------------------------

static const LatteTexKey* tex_key(Ctx* c, u32 unit) {
    static const LatteTexKey none = { 0, 0 };
    if (unit >= LS_TEX_UNITS) return &none;
    return c->vertex ? &c->key->vs_tex[unit] : &c->key->ps_tex[unit];
}

static void sel_expr(Ctx* c, u32 gpr, u32 sel, char* out) {
    char r[16];
    reg_name(c, gpr, r);
    if (sel < 4) sprintf(out, "intBitsToFloat(%s.%s)", r, comp(sel));
    else if (sel == 4) strcpy(out, "0.0");
    else if (sel == 5) strcpy(out, "1.0");
    else strcpy(out, "0.0");
}

static void emit_tex(Ctx* c, u32 w0, u32 w1, u32 w2) {
    u32 op = w0 & 0x1F;
    u32 unit = (w0 >> 8) & 0xFF;
    u32 src = (w0 >> 16) & 0x7F;
    u32 dst = w1 & 0x7F;
    u32 dsel[4] = { (w1 >> 9) & 7, (w1 >> 12) & 7, (w1 >> 15) & 7, (w1 >> 18) & 7 };
    u32 ssel[4] = { (w2 >> 20) & 7, (w2 >> 23) & 7, (w2 >> 26) & 7, (w2 >> 29) & 7 };
    bool unnorm_x = ((w1 >> 28) & 1) == 0, unnorm_y = ((w1 >> 29) & 1) == 0;
    int offx = (int)((w2 >> 0) & 0x1F), offy = (int)((w2 >> 5) & 0x1F), offz = (int)((w2 >> 10) & 0x1F);
    if (offx & 0x10) offx -= 32;
    if (offy & 0x10) offy -= 32;
    if (offz & 0x10) offz -= 32;
    char r[16], dr[16];
    reg_name(c, src, r);
    reg_name(c, dst, dr);
    char s[4][160];
    for (int k = 0; k < 4; k++) sel_expr(c, src, ssel[k], s[k]);

    if (op == 0x00) {   // VFETCH: uniform block read by index
        if (unit >= 128 && unit < 144) {
            u32 bank = unit - 128;
            c->ub_used |= 1u << bank;
            u32 sx = (w0 >> 24) & 3;
            u32 offset = (w2 & 0xFFFF) / 16;   // byte offset -> vec4s
            // read the index first: the destination may be the same register
            line(c, "ti = %s.%s + %u;", r, comp(sx), offset);
            line(c, "tiv = ivec4(ub%u(ti, 0), ub%u(ti, 1), ub%u(ti, 2), ub%u(ti, 3));", bank, bank, bank, bank);
            for (int k = 0; k < 4; k++)
                if (dsel[k] < 4) line(c, "%s.%s = tiv.%s;", dr, comp(k), comp(dsel[k]));
                else if (dsel[k] == 4) line(c, "%s.%s = 0;", dr, comp(k));
                else if (dsel[k] == 5) line(c, "%s.%s = 1065353216;", dr, comp(k));
        } else {
            warn(c, "buffer fetch from resource %u not supported", unit);
        }
        return;
    }
    if (op == 0x0B || op == 0x0C) {   // SET_GRADIENTS_H / V
        c->uses_grad = true;
        line(c, "%s = vec4(%s, %s, %s, %s);", op == 0x0B ? "gradH" : "gradV", s[0], s[1], s[2], s[3]);
        return;
    }
    if (op == 0x07 || op == 0x08) {   // GET_GRADIENTS_H / V
        if (c->vertex) return;
        const char* fn = op == 0x07 ? "dFdx" : "dFdy";
        line(c, "tv = %s(vec4(%s, %s, %s, %s));", fn, s[0], s[1], s[2], s[3]);
        for (int k = 0; k < 4; k++)
            if (dsel[k] < 4) line(c, "%s.%s = floatBitsToInt(tv.%s);", dr, comp(k), comp(dsel[k]));
        return;
    }
    if (op == 0x0E) {   // SET_CUBEMAP_INDEX: which cube of a cube-map array the unit's next reads use
        // (Splatoon's characters pick one of two environment maps per pixel this
        // way; always taking the first gave the purple sheen under the chin.)
        if (c->vertex || unit >= LS_TEX_UNITS) return;
        c->cube_idx |= 1u << unit;
        c->changed |= 2;
        line(c, "cubeIdx%u = %s;", unit, s[0]);
        return;
    }
    if (op == 0x02) { warn(c, "memory read in shader not supported"); return; }

    const LatteTexKey* tk = tex_key(c, unit);
    if (unit >= LS_TEX_UNITS || (c->vertex && unit >= LS_VS_TEX_UNITS)) {
        warn(c, "texture unit %u not supported", unit);
        return;
    }
    c->tex_used |= 1u << unit;
    u32 dim = tk->dim ? tk->dim - 1 : 1;
    bool shadow = op == 0x18 || op == 0x19 || op == 0x1B;
    if (shadow) c->tex_shadow |= 1u << unit;
    char tex[16];
    sprintf(tex, "tex%u", unit);
    bool is_int = tk->kind != 0;

    if (op == 0x04) {   // GET_TEXTURE_RESINFO
        line(c, "ti = textureQueryLevels(%s);", tex);
        line(c, "ti2 = clamp(%s.%s, 0, ti - 1);", r, comp(ssel[3] & 3));
        switch (dim) {
        case 0: line(c, "tiv = ivec4(textureSize(%s, ti2), 0, 0, ti);", tex); break;
        case 2: line(c, "tiv = ivec4(textureSize(%s, ti2), ti);", tex); break;
        case 3: line(c, "tiv = ivec4(textureSize(%s, ti2).xy, 0, ti);", tex); break;
        case 5: case 7: line(c, "tiv = ivec4(textureSize(%s, ti2), ti);", tex); break;
        default: line(c, "tiv = ivec4(textureSize(%s, ti2), 0, ti);", tex); break;
        }
        for (int k = 0; k < 4; k++)
            if (dsel[k] < 4) line(c, "%s.%s = tiv.%s;", dr, comp(k), comp(dsel[k]));
            else if (dsel[k] == 4 || dsel[k] == 5) line(c, "%s.%s = %d;", dr, comp(k), dsel[k] == 5 ? 1 : 0);
        return;
    }

    // Coordinates.
    char coord[700];
    int ncoord;
    switch (dim) {
    case 0: ncoord = 1; break;
    case 2: ncoord = 3; break;
    case 3: ncoord = 3; break;
    case 4: ncoord = 2; break;
    case 5: case 7: ncoord = 3; break;
    default: ncoord = 2; break;
    }
    char cx[200], cy[200];
    if (unnorm_x && op != 0x03 && dim != 3) sprintf(cx, "(%s / float(textureSize(%s, 0).x))", s[0], tex); else strcpy(cx, s[0]);
    if (unnorm_y && op != 0x03 && dim != 3 && ncoord >= 2 && dim != 4) sprintf(cy, "(%s / float(textureSize(%s, 0).y))", s[1], tex); else strcpy(cy, s[1]);
    if (dim == 3) {
        c->uses_cube = true;
        sprintf(coord, "CUBEC%u(cubeDir(vec2(%s, %s), int(%s)))", unit, s[0], s[1], s[2]);
    } else if (ncoord == 1) {
        sprintf(coord, "%s", cx);
    } else if (ncoord == 2) {
        sprintf(coord, "vec2(%s, %s)", cx, cy);
    } else if (dim == 5 || dim == 7) {
        sprintf(coord, "vec3(%s, %s, roundEven(%s))", cx, cy, s[2]);
    } else {
        sprintf(coord, "vec3(%s, %s, %s)", cx, cy, s[2]);
    }
    char offs[80] = "";
    bool has_off = (offx || offy || offz) && dim != 3;
    if (has_off) {
        if (ncoord == 1 || dim == 4) sprintf(offs, ", %d", offx / 2);
        else if (dim == 2) sprintf(offs, ", ivec3(%d, %d, %d)", offx / 2, offy / 2, offz / 2);
        else sprintf(offs, ", ivec2(%d, %d)", offx / 2, offy / 2);
    }

    char call[1600];
    switch (op) {
    case 0x03: {   // LD: integer texel coordinates
        char ic[300];
        if (ncoord == 1) sprintf(ic, "%s.%s", r, comp(ssel[0] & 3));
        else if (ncoord == 2 || dim == 3) sprintf(ic, "ivec2(%s.%s, %s.%s)", r, comp(ssel[0] & 3), r, comp(ssel[1] & 3));
        else sprintf(ic, "ivec3(%s.%s, %s.%s, %s.%s)", r, comp(ssel[0] & 3), r, comp(ssel[1] & 3), r, comp(ssel[2] & 3));
        sprintf(call, "texelFetch(%s, %s, %s.%s)", tex, ic, r, comp(ssel[3] & 3));
        break;
    }
    case 0x0F:   // FETCH4 (gather)
        if (ncoord >= 2 && dim != 2) sprintf(call, "textureGather(%s, %s)", tex, coord);
        else sprintf(call, "texture(%s, %s)", tex, coord);
        break;
    case 0x10:
        if (((w1 >> 28) & 0xF) == 0 && (dim == 1 || dim == 0)) {   // all coordinates in texels: a plain read
            if (dim == 0) sprintf(call, "texelFetch(%s, int(%s), 0)", tex, s[0]);
            else sprintf(call, "texelFetch(%s, ivec2(%s, %s), 0)", tex, s[0], s[1]);
            has_off = false;
        } else {
            sprintf(call, c->vertex ? "textureLod(%s, %s, 0.0)" : "texture(%s, %s)", tex, coord);
        }
        break;
    case 0x11: sprintf(call, "textureLod(%s, %s, %s)", tex, coord, s[3]); break;
    case 0x12: sprintf(call, c->vertex ? "textureLod(%s, %s, %s)" : "texture(%s, %s, %s)", tex, coord, s[3]); break;
    case 0x13: sprintf(call, "textureLod(%s, %s, 0.0)", tex, coord); break;
    case 0x14: {
        c->uses_grad = true;
        const char* sw = ncoord == 1 ? "x" : (ncoord == 2 || dim == 5 || dim == 7) ? "xy" : "xyz";
        sprintf(call, "textureGrad(%s, %s, gradH.%s, gradV.%s)", tex, coord, sw, sw);
        break;
    }
    case 0x18: case 0x19: case 0x1B: {   // SAMPLE_C (+L / LZ)
        const char* cmp = op == 0x19 ? s[2] : s[3];
        char cc[900];
        if (dim == 3) sprintf(cc, "vec4(%s, %s)", coord, cmp);
        else if (ncoord == 1) sprintf(cc, "vec3(%s, 0.0, %s)", coord, cmp);
        else if (ncoord == 2) sprintf(cc, "vec3(%s, %s)", coord, cmp);
        else sprintf(cc, "vec4(%s, %s)", coord, cmp);
        bool lod_ok = dim == 0 || dim == 1;   // textureLod exists for 1D/2D shadow samplers
        if (op == 0x19 && lod_ok) sprintf(call, "vec4(textureLod(%s, %s, %s))", tex, cc, s[3]);
        else if (op == 0x1B && lod_ok) sprintf(call, "vec4(textureLod(%s, %s, 0.0))", tex, cc);
        else if (c->vertex && lod_ok) sprintf(call, "vec4(textureLod(%s, %s, 0.0))", tex, cc);
        else sprintf(call, "vec4(texture(%s, %s))", tex, cc);
        has_off = false;
        break;
    }
    case 0x06:   // GET_COMP_TEX_LOD
        if (c->vertex || dim == 3) sprintf(call, "vec4(0.0)");
        else sprintf(call, "vec4(textureQueryLod(%s, %s).y)", tex, coord);
        has_off = false;
        break;
    default:
        warn(c, "texture instruction %02X not supported", op);
        sprintf(call, "vec4(0.0)");
        has_off = false;
        break;
    }
    if (has_off && op != 0x03 && op != 0x0F) {
        // re-issue with offsets: texture -> textureOffset etc.
        char c2[1700];
        if (strncmp(call, "textureLod(", 11) == 0) {
            size_t len = strlen(call);
            call[len - 1] = 0;
            sprintf(c2, "textureLodOffset(%s%s)", call + 11, offs);
        } else if (strncmp(call, "texture(", 8) == 0) {
            size_t len = strlen(call);
            call[len - 1] = 0;
            sprintf(c2, "textureOffset(%s%s)", call + 8, offs);
        } else if (strncmp(call, "textureGrad(", 12) == 0) {
            size_t len = strlen(call);
            call[len - 1] = 0;
            sprintf(c2, "textureGradOffset(%s%s)", call + 12, offs);
        } else {
            strcpy(c2, call);
        }
        strcpy(call, c2);
    }
    if (is_int && !shadow && op != 0x06) line(c, "tiv = ivec4(%s);", call);
    else line(c, "tv = %s;", call);
    for (int k = 0; k < 4; k++) {
        if (dsel[k] == 7) continue;
        if (dsel[k] < 4) {
            if (is_int && !shadow && op != 0x06) line(c, "%s.%s = tiv.%s;", dr, comp(k), comp(dsel[k]));
            else line(c, "%s.%s = floatBitsToInt(tv.%s);", dr, comp(k), comp(dsel[k]));
        } else if (dsel[k] == 4) line(c, "%s.%s = 0;", dr, comp(k));
        else if (dsel[k] == 5) line(c, "%s.%s = %s;", dr, comp(k), is_int ? "1" : "1065353216");
    }
}

static void emit_tex_clause(Ctx* c, const CfInst* f) {
    for (u32 i = 0; i < f->count; i++) {
        u32 off = f->addr * 8 + i * 16;
        if (off + 16 > c->size) break;
        emit_tex(c, rd32(c->prog + off), rd32(c->prog + off + 4), rd32(c->prog + off + 8));
    }
}

// --- Vertex fetch (the fetch shader) --------------------------------------------------------

typedef struct { u8 comps; u8 bits[4]; u8 bytes; u8 fl; } VtxFmt;   // fl: 1 = float components, 2 = packed small floats

static bool vtx_format(u32 f, VtxFmt* o) {
    memset(o, 0, sizeof(*o));
    switch (f) {
    case 0x01: *o = (VtxFmt){ 1, { 8 }, 1, 0 }; return true;
    case 0x02: *o = (VtxFmt){ 2, { 4, 4 }, 1, 0 }; return true;
    case 0x05: *o = (VtxFmt){ 1, { 16 }, 2, 0 }; return true;
    case 0x06: *o = (VtxFmt){ 1, { 16 }, 2, 1 }; return true;
    case 0x07: *o = (VtxFmt){ 2, { 8, 8 }, 2, 0 }; return true;
    case 0x0D: *o = (VtxFmt){ 1, { 32 }, 4, 0 }; return true;
    case 0x0E: *o = (VtxFmt){ 1, { 32 }, 4, 1 }; return true;
    case 0x0F: *o = (VtxFmt){ 2, { 16, 16 }, 4, 0 }; return true;
    case 0x10: *o = (VtxFmt){ 2, { 16, 16 }, 4, 1 }; return true;
    case 0x15: *o = (VtxFmt){ 3, { 11, 11, 10 }, 4, 0 }; return true;
    case 0x16: *o = (VtxFmt){ 3, { 11, 11, 10 }, 4, 2 }; return true;
    case 0x19: *o = (VtxFmt){ 4, { 10, 10, 10, 2 }, 4, 0 }; return true;
    case 0x1A: *o = (VtxFmt){ 4, { 8, 8, 8, 8 }, 4, 0 }; return true;
    case 0x1B: *o = (VtxFmt){ 4, { 2, 10, 10, 10 }, 4, 0 }; return true;
    case 0x1D: *o = (VtxFmt){ 2, { 32, 32 }, 8, 0 }; return true;
    case 0x1E: *o = (VtxFmt){ 2, { 32, 32 }, 8, 1 }; return true;
    case 0x1F: *o = (VtxFmt){ 4, { 16, 16, 16, 16 }, 8, 0 }; return true;
    case 0x20: *o = (VtxFmt){ 4, { 16, 16, 16, 16 }, 8, 1 }; return true;
    case 0x22: *o = (VtxFmt){ 4, { 32, 32, 32, 32 }, 16, 0 }; return true;
    case 0x23: *o = (VtxFmt){ 4, { 32, 32, 32, 32 }, 16, 1 }; return true;
    case 0x2F: *o = (VtxFmt){ 3, { 32, 32, 32 }, 12, 0 }; return true;
    case 0x30: *o = (VtxFmt){ 3, { 32, 32, 32 }, 12, 1 }; return true;
    default: return false;
    }
}

static void emit_attrib(Ctx* c, u32 w0, u32 w1, u32 w2, u32 gpr) {
    u32 buf = ((w0 >> 8) & 0xFF) - 0xA0;
    u32 fetch_type = (w0 >> 5) & 3;
    u32 src_sel = (w0 >> 24) & 3;
    u32 dsel[4] = { (w1 >> 9) & 7, (w1 >> 12) & 7, (w1 >> 15) & 7, (w1 >> 18) & 7 };
    u32 fmt = (w1 >> 22) & 0x3F, nfa = (w1 >> 28) & 3, sgn = (w1 >> 30) & 1;
    u32 offset = w2 & 0xFFFF, swap = (w2 >> 16) & 3;
    if (buf >= 16) { warn(c, "fetch from buffer %u not supported", buf + 0xA0); return; }
    VtxFmt vf;
    if (!vtx_format(fmt, &vf)) { warn(c, "vertex format %02X not supported", fmt); return; }
    if (fetch_type == 1) c->out->attrib_instanced |= 1u << buf;
    else c->out->attrib_used |= 1u << buf;
    if (offset + vf.bytes > c->out->attrib_extent[buf]) c->out->attrib_extent[buf] = offset + vf.bytes;
    char r[16];
    reg_name(c, gpr, r);
    line(c, "{ // attribute buffer %u, format %02X, offset %u", buf, fmt, offset);
    c->indent++;
    if (fetch_type == 1) {
        if (src_sel == 3 || src_sel == 0) line(c, "int idx = gl_InstanceIndex;");
        else line(c, "int idx = gl_InstanceIndex / max(int(HDR(%d)), 1);", HDR_STEP_RATE + (int)src_sel - 1);
    } else {
        line(c, "int idx = gl_VertexIndex;");
    }
    line(c, "int a = int(HDR(%d)) + idx * int(HDR(%d)) + %u;", HDR_ATTR_BASE + (int)buf, HDR_ATTR_STRIDE + (int)buf, offset);
    u32 words = (vf.bytes + 3) / 4;
    for (u32 k = 0; k < words; k++) {
        const char* sw = swap == 1 ? "sw16" : swap == 2 ? "sw32" : "";
        if (vf.bytes == 1) sw = "";                      // single bytes: nothing to swap
        else if (vf.bytes == 2 && swap == 2) sw = "sw16";   // a 16-bit value in a 32-bit swap
        line(c, "uint w%u = %s(rd(a + %u));", k, sw, k * 4);
    }
    // components
    u32 bit = 0;
    char cv[4][200];
    for (u32 k = 0; k < 4; k++) {
        if (k >= vf.comps) {
            strcpy(cv[k], k == 3 ? (nfa == 1 ? "1" : "1065353216") : "0");
            continue;
        }
        u32 bits = vf.bits[k];
        u32 word = bit / 32, sh = bit % 32;
        char raw[80];
        if (bits == 32) sprintf(raw, "w%u", word);
        else sprintf(raw, "bitfieldExtract(w%u, %u, %u)", word, sh, bits);
        bit += bits;
        if (vf.fl == 1) {
            if (bits == 32) sprintf(cv[k], "int(%s)", raw);
            else sprintf(cv[k], "floatBitsToInt(unpackHalf2x16(%s).x)", raw);
        } else if (vf.fl == 2) {
            sprintf(cv[k], "floatBitsToInt(%s(%s))", bits == 10 ? "uf10" : "uf11", raw);
        } else {
            char sv[160];
            if (sgn && bits < 32) sprintf(sv, "bitfieldExtract(int(%s), 0, %u)", raw, bits);
            else if (sgn) sprintf(sv, "int(%s)", raw);
            else sprintf(sv, "%s", raw);
            double maxv = sgn ? (double)((1ull << (bits - 1)) - 1) : (double)((1ull << bits) - 1);
            if (nfa == 0) {
                if (sgn) sprintf(cv[k], "floatBitsToInt(max(float(%s) / %.1f, -1.0))", sv, maxv);
                else sprintf(cv[k], "floatBitsToInt(float(%s) / %.1f)", sv, maxv);
            } else if (nfa == 1) {
                sprintf(cv[k], "int(%s)", sv);
            } else {
                sprintf(cv[k], "floatBitsToInt(float(%s))", sv);
            }
        }
    }
    for (u32 k = 0; k < 4; k++) {
        if (dsel[k] == 7) continue;
        if (dsel[k] < 4) line(c, "%s.%s = %s;", r, comp(k), cv[dsel[k]]);
        else if (dsel[k] == 4) line(c, "%s.%s = 0;", r, comp(k));
        else if (dsel[k] == 5) line(c, "%s.%s = %s;", r, comp(k), nfa == 1 ? "1" : "1065353216");
    }
    c->indent--;
    line(c, "}");
}

static void emit_fetch(Ctx* c) {
    const LatteShaderKey* k = c->key;
    if (!k->fs || k->fs_size < 8) return;
    for (u32 i = 0; i * 8 + 8 <= k->fs_size; i++) {
        u32 w0 = rd32(k->fs + i * 8), w1 = rd32(k->fs + i * 8 + 4);
        u32 inst = (w1 >> 23) & 0x7F;
        if (inst == CF_VTX || inst == CF_VTX_TC) {
            u32 count = (((w1 >> 10) & 7) | (((w1 >> 19) & 1) << 3)) + 1;
            for (u32 n = 0; n < count; n++) {
                u32 off = w0 * 8 + n * 16;
                if (off + 16 > k->fs_size) break;
                u32 v0 = rd32(k->fs + off), v1 = rd32(k->fs + off + 4), v2 = rd32(k->fs + off + 8);
                u32 vinst = v0 & 0x1F;
                u32 gpr;
                if (vinst == 1) {   // semantic fetch: goes to the register the vertex shader asks for
                    u32 sem = v1 & 0xFF;
                    gpr = 0xFFFFFFFF;
                    for (u32 s = 0; s < 32; s++)
                        if (k->vtx_semantic[s] == sem) { gpr = s + 1; break; }
                    if (gpr == 0xFFFFFFFF) continue;
                } else if (vinst == 0) {
                    gpr = v1 & 0x7F;
                } else continue;
                emit_attrib(c, v0, v1, v2, gpr);
            }
        }
        if (inst == CF_RETURN || ((w1 >> 21) & 1)) break;
    }
}

// --- Exports ---------------------------------------------------------------------------------

static void export_vec(Ctx* c, u32 gpr, const u32 sel[4], char* out) {
    char r[16];
    reg_name(c, gpr, r);
    char p[4][48];
    for (int k = 0; k < 4; k++) {
        if (sel[k] < 4) sprintf(p[k], "%s.%s", r, comp(sel[k]));
        else if (sel[k] == 5) strcpy(p[k], "1065353216");
        else strcpy(p[k], "0");
    }
    sprintf(out, "ivec4(%s, %s, %s, %s)", p[0], p[1], p[2], p[3]);
}

static int color_index(u32 mask, u32 export_index) {
    int counter = 0;
    for (int m = 0; m < 8; m++) {
        if (((mask >> (m * 4)) & 0xF) == 0) continue;
        if ((u32)counter == export_index) return m;
        counter++;
    }
    return -1;
}

static void emit_export(Ctx* c, const CfInst* f) {
    const LatteShaderKey* k = c->key;
    for (u32 b = 0; b <= f->exp_burst; b++) {
        u32 base = f->exp_base + b, gpr = f->exp_gpr + b;
        char v[400];
        export_vec(c, gpr, f->exp_sel, v);
        if (c->vertex) {
            if (f->exp_type == 1 && base == 60) {
                line(c, "pos = intBitsToFloat(%s);", v);
            } else if (f->exp_type == 1 && base == 61) {
                if (k->points) line(c, "gl_PointSize = intBitsToFloat(%s).x;", v);
            } else if (f->exp_type == 2 && base < 40) {
                u32 sem = k->vs_out_semantic[base];
                u32 n = k->ps_in_control0 & 0x3F;
                u32 pos_addr = (k->ps_in_control0 >> 8) & 1 ? (k->ps_in_control0 >> 10) & 0x1F : 0xFFFF;
                for (u32 i = 0; i < n && i < 32; i++) {
                    if (i == pos_addr) continue;
                    if ((k->ps_input_cntl[i] & 0xFF) != sem) continue;
                    char mask[5] = "";
                    int m = 0;
                    for (int q = 0; q < 4; q++) if (f->exp_sel[q] != 7) mask[m++] = "xyzw"[q];
                    mask[m] = 0;
                    if (m == 0) continue;
                    if (m == 4) line(c, "o%u = intBitsToFloat(%s);", i, v);
                    else line(c, "o%u.%s = intBitsToFloat(%s).%s;", i, mask, v, mask);
                }
            }
        } else {
            if (f->exp_type == 0 && base < 8) {
                int m = color_index(k->cb_shader_mask, base);
                if (m < 0) continue;
                c->out->color_outputs |= 1u << m;
                u32 at = k->alpha_test;
                if (m == 0 && (at & 8)) {
                    u32 fn = at & 7;
                    static const char* ops[] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
                    if (fn == 0) line(c, "discard;");
                    else if (fn != 7)
                        line(c, "if (!(intBitsToFloat(%s).w %s uintBitsToFloat(HDR(%d)))) discard;", v, ops[fn], HDR_ALPHA_REF);
                }
                if (k->cb_kind[m] == 1) line(c, "c%d = %s;", m, v);
                else if (k->cb_kind[m] == 2) line(c, "c%d = uvec4(%s);", m, v);
                else line(c, "c%d = intBitsToFloat(%s);", m, v);
            } else if (f->exp_type == 0 && base == 61) {
                c->out->writes_depth = true;
                line(c, "gl_FragDepth = intBitsToFloat(%s).x;", v);
            }
        }
    }
}

// --- Whole program -------------------------------------------------------------------------

static void emit_body(Ctx* c) {
    for (u32 i = 0; i < c->ncf; i++) {
        const CfInst* f = &c->cf[i];
        int D = f->depth;
        bool flow = c->uses_flow;
        switch (f->type) {
        case CF_ALU:
            if (flow) { line(c, "if (ac[%d]) {", D + 1); c->indent++; }
            emit_alu_clause(c, f);
            if (flow) { c->indent--; line(c, "}"); }
            break;
        case CF_ALU_PUSH_BEFORE:
            line(c, "if (ac[%d]) {", D);
            c->indent++;
            line(c, "am[%d] = am[%d];", D, D - 1);
            line(c, "ac[%d] = ac[%d];", D + 1, D);
            emit_alu_clause(c, f);
            c->indent--;
            line(c, "} else { am[%d] = false; ac[%d] = false; }", D, D + 1);
            break;
        case CF_ALU_POP_AFTER:
        case CF_ALU_POP2_AFTER: {
            line(c, "if (ac[%d]) {", D + 1);
            c->indent++;
            emit_alu_clause(c, f);
            c->indent--;
            line(c, "}");
            int p = f->type == CF_ALU_POP_AFTER ? 1 : 2;
            int t = D - p;
            if (t < 0) t = 0;
            line(c, "ac[%d] = am[%d] && ac[%d];", t + 1, t, t);
            break;
        }
        case CF_ALU_ELSE_AFTER:
            line(c, "if (ac[%d]) {", D + 1);
            c->indent++;
            emit_alu_clause(c, f);
            c->indent--;
            line(c, "}");
            line(c, "am[%d] = !am[%d];", D, D);
            line(c, "ac[%d] = am[%d] && ac[%d];", D + 1, D, D);
            break;
        case CF_ALU_BREAK:
            line(c, "if (ac[%d]) {", D + 1);
            c->indent++;
            line(c, "bool aluBreak = false;");
            emit_alu_clause(c, f);
            line(c, "if (aluBreak) break;");
            c->indent--;
            line(c, "}");
            break;
        case CF_TEX:
            if (flow) { line(c, "if (ac[%d]) {", D + 1); c->indent++; }
            emit_tex_clause(c, f);
            if (flow) { c->indent--; line(c, "}"); }
            break;
        case CF_EXPORT:
        case CF_EXPORT_DONE:
            emit_export(c, f);
            break;
        case CF_MEM_STREAM0: case CF_MEM_STREAM1: case CF_MEM_STREAM2: case CF_MEM_STREAM3:
            if (c->vertex) {   // stream out: write the register's components to buffer n
                u32 n = f->type - CF_MEM_STREAM0;
                c->so_mask |= 1u << n;
                char r[16];
                reg_name(c, f->exp_gpr, r);
                u32 count = f->so_size + 1;
                if (count > 4) count = 4;
                line(c, "if (HDR(%d) != 0u) {", HDR_SO_ON);
                c->indent++;
                line(c, "uint sob = HDR(%d) + uint(gl_VertexIndex - int(HDR(%d)) + int(HDR(%d)) * gl_InstanceIndex) * HDR(%d);",
                     HDR_SO_BASE + n, HDR_SO_FIRST, HDR_SO_VPI, HDR_SO_STRIDE + n);
                for (u32 q = 0; q < count; q++)
                    if (f->so_mask & (1u << q)) line(c, "so[sob + %uu] = uint(%s.%s);", f->exp_base + q, r, comp(q));
                c->indent--;
                line(c, "}");
            }
            break;
        case CF_ELSE:
            line(c, "am[%d] = !am[%d];", D, D);
            line(c, "ac[%d] = am[%d] && ac[%d];", D + 1, D, D);
            break;
        case CF_POP: {
            int t = D - (int)f->pop;
            if (t < 0) t = 0;
            line(c, "ac[%d] = am[%d] && ac[%d];", t + 1, t, t);
            break;
        }
        case CF_LOOP_START_DX10:
        case CF_LOOP_START_NO_AL:
            line(c, "{");
            c->indent++;
            line(c, "bool loopActive%u = ac[%d];", i, D + 1);
            line(c, "int loopGuard%u = 0;", i);
            line(c, "while (ac[%d] && loopGuard%u < 4096) {", D + 1, i);
            c->indent++;
            line(c, "loopGuard%u++;", i);
            break;
        case CF_LOOP_END: {
            // find the matching start to name its variable
            int nest = 0;
            u32 start = 0;
            for (int j = (int)i - 1; j >= 0; j--) {
                if (c->cf[j].type == CF_LOOP_END) nest++;
                else if (c->cf[j].type == CF_LOOP_START_DX10 || c->cf[j].type == CF_LOOP_START_NO_AL) {
                    if (nest == 0) { start = (u32)j; break; }
                    nest--;
                }
            }
            c->indent--;
            line(c, "}");
            line(c, "ac[%d] = loopActive%u;", D + 1, start);
            c->indent--;
            line(c, "}");
            break;
        }
        case CF_LOOP_BREAK:
            line(c, "if (ac[%d]) break;", D + 1);
            break;
        case CF_CALL_FS:
            if (c->vertex) emit_fetch(c);
            break;
        default:
            break;
        }
    }
}

static const char* sampler_type(const LatteTexKey* t, bool shadow, bool cube_array) {
    u32 dim = t->dim ? t->dim - 1u : 1u;
    const char* prefix = t->kind == 1 ? "i" : t->kind == 2 ? "u" : "";
    if (shadow) prefix = "";
    static char buf[32];
    const char* base;
    switch (dim) {
    case 0: base = "sampler1D"; break;
    case 2: base = "sampler3D"; break;
    case 3: base = cube_array ? "samplerCubeArray" : "samplerCube"; break;
    case 4: base = "sampler1DArray"; break;
    case 5: case 7: base = "sampler2DArray"; break;
    default: base = "sampler2D"; break;
    }
    snprintf(buf, sizeof(buf), "%s%s%s", prefix, base, shadow && dim != 2 ? "Shadow" : "");
    return buf;
}

// Less work for the graphics driver's first-time compile (cold plaza, Oct 19, the same ~700
// pipelines: 23% less driver time in all; the multiply below is about half of it):
//  - selects, not branches: glslang turns "c ? x : y" into an if/else unless both x and y are plain
//    variables or constants, and every multiply and conditional move was one. So the multiply is
//    computed into a variable first, and the conditional moves use mix(f, t, cond) (one select).
//    Same values, but without the branches the driver arranges other float math differently:
//    frame replays showed a few pixels per frame (at most 19 of 1280x720) off by 1-2 of 255
//    (rounding; marking the product "precise" doesn't bring the old values back). Everything
//    else below gave bit-identical replays.
//  - uniform blocks: their place and size are read once at the start of main, and reads at a fixed
//    index skip the clamp, so the driver can merge them into wide loads. Exact because the upload
//    copies a block's last vec4 up to the highest fixed index the shader reads (ShaderEnt.ub_fixed,
//    from the "splat.ub" note in the SPIR-V); reads through the address register still clamp.
//  - uniform registers (all 256 always uploaded): fixed-index reads skip the clamp.
// (SPLAT_SHADER_OLD_FORMS=1: the old forms, for comparing pictures. The disk cache keeps working
// either way: its shaders read the same values.)
static const char* k_helpers =
    "float mul_ni(float a, float b) { float p = a * b; return (a == 0.0 || b == 0.0) ? 0.0 : p; }\n"
    "uint rd(int a) { int w = a >> 2; int s = (a & 3) << 3; uint lo = d[w]; if (s == 0) return lo; return (lo >> uint(s)) | (d[w + 1] << uint(32 - s)); }\n"
    "uint sw16(uint v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }\n"
    "uint sw32(uint v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }\n"
    "uint sw(uint v) { return v; }\n"
    "float uf11(uint v) { uint e = (v >> 6) & 31u, m = v & 63u; if (e == 0u) return float(m) * exp2(-20.0); if (e == 31u) return uintBitsToFloat(0x7F800000u | (m << 17)); return uintBitsToFloat(((e + 112u) << 23) | (m << 17)); }\n"
    "float uf10(uint v) { uint e = (v >> 5) & 31u, m = v & 31u; if (e == 0u) return float(m) * exp2(-19.0); if (e == 31u) return uintBitsToFloat(0x7F800000u | (m << 18)); return uintBitsToFloat(((e + 112u) << 23) | (m << 18)); }\n";

static const char* k_mul_branchy = "float mul_ni(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n";
static int new_forms(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPLAT_SHADER_OLD_FORMS"); on = !(e && e[0] == '1'); }
    return on;
}

static const char* k_cube =
    "void redcCUBE(vec4 src0, vec4 src1, out vec3 stm, out int faceId) {\n"
    "  float x = src1.y, y = src1.x, z = src0.x;\n"
    "  float ax = abs(x), ay = abs(y), az = abs(z);\n"
    "  if (az >= ax && az >= ay) { stm = vec3(-y, z < 0.0 ? -x : x, z * 2.0); faceId = z < 0.0 ? 5 : 4; }\n"
    "  else if (ay >= ax) { stm = vec3(y < 0.0 ? -z : z, x, y * 2.0); faceId = y < 0.0 ? 3 : 2; }\n"
    "  else { stm = vec3(-y, x < 0.0 ? z : -z, x * 2.0); faceId = x < 0.0 ? 1 : 0; }\n"
    "}\n"
    "vec3 cubeDir(vec2 st, int faceId) {\n"
    "  vec2 tc = (st - vec2(1.5)) * 2.0;\n"
    "  if (faceId == 0) return vec3(1.0, -tc.y, -tc.x);\n"
    "  if (faceId == 1) return vec3(-1.0, -tc.y, tc.x);\n"
    "  if (faceId == 2) return vec3(tc.x, 1.0, tc.y);\n"
    "  if (faceId == 3) return vec3(tc.x, -1.0, -tc.y);\n"
    "  if (faceId == 4) return vec3(tc.x, -tc.y, 1.0);\n"
    "  return vec3(-tc.x, -tc.y, -1.0);\n"
    "}\n";

static const char* interp_qual(u32 cntl) {
    if (cntl & (1u << 10)) return "flat ";
    if (cntl & (1u << 12)) return "noperspective ";
    if (cntl & (1u << 11)) return "centroid ";
    return "";
}

// Units read as cube-map arrays: cube textures whose cube SET_CUBEMAP_INDEX
// picks (pixel shaders; not with depth compare).
static u32 cube_array_units(const Ctx* c) {
    if (c->vertex) return 0;
    u32 m = 0;
    for (u32 u = 0; u < LS_TEX_UNITS; u++)
        if ((c->cube_idx >> u & 1) && (c->tex_used >> u & 1) && !(c->tex_shadow >> u & 1) && c->key->ps_tex[u].dim == 4) m |= 1u << u;
    return m;
}

static char* assemble(Ctx* c) {
    Str s = { 0 };
    const LatteShaderKey* k = c->key;
    u32 cube_arr = cube_array_units(c);
    sp(&s, "#version 450\n");
    // (the highest fixed index read per uniform block, for the upload: glsl_compile.c copies this
    // line into the SPIR-V as an OpSourceExtension, which the driver ignores)
    if (new_forms()) {
        bool any = false;
        for (u32 b = 0; b < 16; b++) {
            if (!(c->ub_used & (1u << b)) || !c->ub_fixmax[b] || c->ub_fixmax[b] > 255) continue;
            sp(&s, any ? ",%u:%u" : "// spv-source-extension: splat.ub=%u:%u", b, c->ub_fixmax[b]);
            any = true;
        }
        if (any) sp(&s, "\n");
    }
    sp(&s, "layout(std430, set = 0, binding = 0) readonly buffer DrawData { uint d[]; };\n");
    sp(&s, "layout(push_constant) uniform PC { uint hdr; } pc;\n");
    sp(&s, "#define HDR(i) d[pc.hdr + uint(i)]\n");
    // A depth pre-pass and the later EQUAL colour pass run different shaders
    // that compute the same position; no fused multiply-adds, so both get the
    // exact same depth like on the console.
    if (c->vertex) sp(&s, "precise gl_Position;\ninvariant gl_Position;\n");
    // textures
    for (u32 u = 0; u < LS_TEX_UNITS; u++) {
        if (!(c->tex_used & (1u << u))) continue;
        const LatteTexKey* t = c->vertex ? &k->vs_tex[u] : &k->ps_tex[u];
        u32 binding = c->vertex ? LS_BINDING_VS_TEX + u : LS_BINDING_PS_TEX + u;
        sp(&s, "layout(set = 0, binding = %u) uniform %s tex%u;\n", binding, sampler_type(t, (c->tex_shadow >> u) & 1, (cube_arr >> u) & 1), u);
        // cube-map coordinates: + the cube's index for cube-map arrays
        if ((t->dim ? t->dim - 1u : 1u) == 3) {
            if ((cube_arr >> u) & 1) sp(&s, "float cubeIdx%u = 0.0;\n#define CUBEC%u(d) vec4(d, cubeIdx%u)\n", u, u, u);
            else sp(&s, "#define CUBEC%u(d) (d)\n", u);
        }
    }
    // stage interface
    u32 n_in = k->ps_in_control0 & 0x3F;
    if (n_in > 32) n_in = 32;
    u32 pos_addr = (k->ps_in_control0 >> 8) & 1 ? (k->ps_in_control0 >> 10) & 0x1F : 0xFFFF;
    for (u32 i = 0; i < n_in; i++) {
        if (i == pos_addr) continue;
        sp(&s, "layout(location = %u) %s%s vec4 %s%u;\n", i, interp_qual(k->ps_input_cntl[i]), c->vertex ? "out" : "in",
           c->vertex ? "o" : "i", i);
    }
    if (!c->vertex) {
        for (int m = 0; m < 8; m++) {
            if (!(c->out->color_outputs & (1u << m))) continue;
            const char* ty = k->cb_kind[m] == 1 ? "ivec4" : k->cb_kind[m] == 2 ? "uvec4" : "vec4";
            sp(&s, "layout(location = %d) out %s c%d;\n", m, ty, m);
        }
    }
    if (c->vertex && c->so_mask)
        sp(&s, "layout(std430, set = 0, binding = %d) buffer StreamOut { uint so[]; };\n", LS_BINDING_STREAMOUT);
    if (!new_forms()) { sp(&s, "%s", k_mul_branchy); sp(&s, "%s", strchr(k_helpers, '\n') + 1); }
    else sp(&s, "%s", k_helpers);
    if (c->uses_cube) sp(&s, "%s", k_cube);
    // uniform access
    const char* ubhdr = c->vertex ? "34" : "50";
    const char* ubsize = c->vertex ? "72" : "88";
    for (u32 b = 0; b < 16; b++) {
        if (!(c->ub_used & (1u << b))) continue;
        // (the block's place and last index are read once, at the start of main: below)
        if (new_forms()) {
            sp(&s, "#define ub%u(i, ch) int(d[ub%u_base + uint(clamp((i), 0, ub%u_last)) * 4u + uint(ch)])\n", b, b, b);
            if (c->ub_fixmax[b] > 255) sp(&s, "#define ubs%u(o) ub%u(int((o) >> 2u), int((o) & 3u))\n", b, b);   // (no padding that far)
            else sp(&s, "#define ubs%u(o) int(d[ub%u_base + (o)])\n", b, b);
        }
        else
        sp(&s, "int ub%u(int i, int ch) { uint sz = max(HDR(%s + %u), 1u); return int(d[HDR(%s + %u) + uint(clamp(i, 0, int(sz) - 1)) * 4u + uint(ch)]); }\n",
           b, ubsize, b, ubhdr, b);
    }
    if (c->cfile_max && new_forms())
        sp(&s, "#define cf(i, ch) int(d[cf_base + uint(clamp((i), 0, 255)) * 4u + uint(ch)])\n");
    else if (c->cfile_max)
        sp(&s, "int cf(int i, int ch) { return int(d[HDR(%d) + uint(clamp(i, 0, 255)) * 4u + uint(ch)]); }\n",
           c->vertex ? HDR_VS_CFILE : HDR_PS_CFILE);
    sp(&s, "void main() {\n");
    if (new_forms()) {
        for (u32 b = 0; b < 16; b++)
            if (c->ub_used & (1u << b))
                sp(&s, "  uint ub%u_base = HDR(%s + %u); int ub%u_last = int(max(HDR(%s + %u), 1u)) - 1;\n", b, ubhdr, b, b, ubsize, b);
        if (c->cfile_max) sp(&s, "  uint cf_base = HDR(%d);\n", c->vertex ? HDR_VS_CFILE : HDR_PS_CFILE);
    }
    // registers
    if (c->rel_gpr) {
        sp(&s, "  ivec4 R[128];\n  for (int i = 0; i < 128; i++) R[i] = ivec4(0);\n");
    } else {
        for (u32 g = 0; g < 128; g++)
            if (c->gpr_used[g >> 5] & (1u << (g & 31))) sp(&s, "  ivec4 R%u = ivec4(0);\n", g);
    }
    sp(&s, "  ivec4 PV = ivec4(0); int PSv = 0; ivec4 AR = ivec4(0);\n");
    sp(&s, "  int tx = 0, ty = 0, tz = 0, tw = 0, tt = 0; float tf = 0.0; int ti = 0, ti2 = 0; uint tu = 0u, tu2 = 0u;\n");
    sp(&s, "  vec4 tv = vec4(0.0); ivec4 tiv = ivec4(0); bool pred = false; vec4 gradH = vec4(0.0), gradV = vec4(0.0);\n");
    if (c->uses_cube) sp(&s, "  vec3 cubeSTM; int cubeFace;\n");
    if (c->uses_flow) {
        int nm = c->max_depth + 2;
        sp(&s, "  bool am[%d]; bool ac[%d];\n", nm, nm + 1);
        sp(&s, "  for (int i = 0; i < %d; i++) am[i] = false;\n  for (int i = 0; i < %d; i++) ac[i] = false;\n", nm, nm + 1);
        sp(&s, "  am[0] = true; ac[0] = true; ac[1] = true;\n");
    }
    char r0[16];
    if (c->vertex) {
        sp(&s, "  vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);\n");
        for (u32 i = 0; i < n_in; i++) {
            if (i == pos_addr) continue;
            u32 dv = (k->ps_input_cntl[i] >> 8) & 3;
            static const char* defs[] = { "vec4(0.0)", "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(1.0, 1.0, 1.0, 0.0)", "vec4(1.0)" };
            sp(&s, "  o%u = %s;\n", i, defs[dv]);
        }
        if (c->rel_gpr) strcpy(r0, "R[0]"); else strcpy(r0, "R0");
        if (c->rel_gpr || (c->gpr_used[0] & 1)) sp(&s, "  %s = ivec4(gl_VertexIndex, 0, 0, gl_InstanceIndex);\n", r0);
    } else {
        for (u32 i = 0; i < n_in; i++) {
            if (!c->rel_gpr && !(c->gpr_used[i >> 5] & (1u << (i & 31)))) continue;
            char ri[16];
            if (c->rel_gpr) sprintf(ri, "R[%u]", i); else sprintf(ri, "R%u", i);
            if (i == pos_addr)
                sp(&s, "  %s = floatBitsToInt(vec4(gl_FragCoord.xy, gl_FragCoord.z, 1.0 / gl_FragCoord.w));\n", ri);
            else
                sp(&s, "  %s = floatBitsToInt(i%u);\n", ri, i);
        }
        u32 ctl1 = k->ps_in_control1;
        if ((ctl1 >> 8) & 1) {
            u32 addr = (ctl1 >> 12) & 0x1F, chan = (ctl1 >> 9) & 3;
            if (c->rel_gpr || (c->gpr_used[addr >> 5] & (1u << (addr & 31)))) {
                char ri[16];
                if (c->rel_gpr) sprintf(ri, "R[%u]", addr); else sprintf(ri, "R%u", addr);
                const char* ff = getenv("SPLAT_FACE_FLIP") ? "!gl_FrontFacing" : "gl_FrontFacing";
                if ((ctl1 >> 11) & 1) sp(&s, "  %s.%s = %s ? -1 : 0;\n", ri, comp(chan), ff);
                // (otherwise the sign tells: +1.0 front, -1.0 back)
                else sp(&s, "  %s.%s = %s ? 1065353216 : -1082130432;\n", ri, comp(chan), ff);
            }
        }
        u32 gen = (k->ps_in_control0 >> 15) & 0xF;
        if (gen) {
            u32 addr = (k->ps_in_control0 >> 19) & 0x7F;
            if (addr < 128 && (c->rel_gpr || (c->gpr_used[addr >> 5] & (1u << (addr & 31))))) {
                char ri[16];
                if (c->rel_gpr) sprintf(ri, "R[%u]", addr); else sprintf(ri, "R%u", addr);
                sp(&s, "  %s = floatBitsToInt(gl_PointCoord.xyxy);\n", ri);
            }
        }
    }
    if (c->body.p) sp(&s, "%s", c->body.p);
    if (c->vertex) {
        // special state: positions already in window coordinates
        bool window_space = (k->vte_cntl & 0x3F) != 0x3F && (k->vte_cntl & 1) == 0;
        if (window_space)
            sp(&s, "  pos.xy = pos.xy * vec2(uintBitsToFloat(HDR(%d)), uintBitsToFloat(HDR(%d))) - vec2(1.0);\n",
               HDR_WIN_SCALE, HDR_WIN_SCALE + 1);
        sp(&s, "  gl_Position = pos;\n");
        if (!((k->clip_cntl >> 19) & 1)) sp(&s, "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n");
        if (k->points) {
            bool writes = strstr(c->body.p ? c->body.p : "", "gl_PointSize") != NULL;
            if (!writes) sp(&s, "  gl_PointSize = uintBitsToFloat(HDR(%d));\n", HDR_POINT_SIZE);
        }
    }
    sp(&s, "}\n");
    return s.p;
}

static u32 g_changed;

static bool decompile(const LatteShaderKey* key, bool vertex, LatteShaderOut* out) {
    memset(out, 0, sizeof(*out));
    Ctx* c = (Ctx*)calloc(1, sizeof(Ctx));
    if (!c) return false;
    c->key = key;
    c->vertex = vertex;
    c->prog = vertex ? key->vs : key->ps;
    c->size = vertex ? key->vs_size : key->ps_size;
    c->out = out;
    c->indent = 1;
    if (!c->prog || c->size < 8) { free(c); return false; }
    parse_cf(c);
    // first pass: find out whether relative register access is used
    emit_body(c);
    if (c->rel_gpr) {   // regenerate with the register array
        free(c->body.p);
        memset(&c->body, 0, sizeof(c->body));
        LatteShaderOut keep = *out;
        memset(out, 0, sizeof(*out));
        out->warnings = keep.warnings;
        c->tex_used = c->tex_shadow = c->ub_used = c->so_mask = c->cube_idx = 0;
        c->cfile_max = 0;
        memset(c->ub_fixmax, 0, sizeof(c->ub_fixmax));
        emit_body(c);
    }
    out->tex_used = c->tex_used;
    out->tex_shadow = c->tex_shadow;
    out->so_mask = c->so_mask;
    out->ub_used = c->ub_used;
    out->cfile_count = c->cfile_max;
    if (!vertex) LS_PS_CUBE_ARRAYS(out) = cube_array_units(c);
    out->glsl = assemble(c);
    g_changed = c->changed;
    free(c->body.p);
    free(c);
    return out->glsl != NULL;
}

// Did the last shader translated use something the translator now handles
// differently than when the disk cache was written? (Such shaders are
// translated again instead of loaded from the cache.)
// (bits: see Ctx.changed)
u32 latte_shader_changed_since_cache(void) { return g_changed; }

bool latte_decompile_vs(const LatteShaderKey* key, LatteShaderOut* out) { return decompile(key, true, out); }
bool latte_decompile_ps(const LatteShaderKey* key, LatteShaderOut* out) { return decompile(key, false, out); }

// Texture units a program samples (a quick walk over its TEX clauses). Used
// to leave units the program doesn't read out of the shader key: games leave
// old textures bound, and those must not make new shader variants.
u32 latte_shader_tex_mask(const u8* prog, u32 size) {
    u32 mask = 0;
    if (!prog) return 0;
    for (u32 i = 0; i * 8 + 8 <= size && i < 4096; i++) {
        u32 w0 = rd32(prog + i * 8), w1 = rd32(prog + i * 8 + 4);
        u32 inst = (w1 >> 23) & 0x7F;
        if (inst >= 0x40) continue;   // ALU clause
        if (inst == CF_TEX) {
            u32 count = (((w1 >> 10) & 7) | (((w1 >> 19) & 1) << 3)) + 1;
            for (u32 k = 0; k < count; k++) {
                u32 off = w0 * 8 + k * 16;
                if (off + 16 > size) break;
                u32 t0 = rd32(prog + off);
                u32 op = t0 & 0x1F, unit = (t0 >> 8) & 0xFF;
                if (op == 0x00 || op == 0x02) continue;   // uniform / memory reads
                if (unit < 32) mask |= 1u << unit;
            }
        }
        if ((w1 >> 21) & 1) break;   // end of program
    }
    return mask;
}

// Parameter exports (to the pixel shader) a vertex program writes: bit n = export base n (0..39), bursts included --
// the same walk as parse_cf, so exactly the bases emit_export reads the semantic table for. The renderer leaves the
// table's other entries (left over from earlier programs) out of the vertex shader's class.
u64 latte_shader_param_exports(const u8* prog, u32 size) {
    u64 mask = 0;
    if (!prog) return ~0ull;
    for (u32 i = 0; i * 8 + 8 <= size && i < MAX_CF; i++) {
        u32 w0 = rd32(prog + i * 8), w1 = rd32(prog + i * 8 + 4);
        u32 inst = (w1 >> 23) & 0x7F;
        if (inst >= 0x40) continue;   // ALU clause
        if (inst == CF_EXPORT || inst == CF_EXPORT_DONE) {
            u32 base = w0 & 0x1FFF, type = (w0 >> 13) & 3, burst = (w1 >> 17) & 0xF;
            if (type == 2)
                for (u32 b = 0; b <= burst; b++)
                    if (base + b < 40) mask |= 1ull << (base + b);
        }
        if ((w1 >> 21) & 1) break;   // end of program
    }
    return mask;
}
