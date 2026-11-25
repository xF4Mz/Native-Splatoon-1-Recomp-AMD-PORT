// chunk_split: rewrites a DolRecomp game-code file so a C compiler can
// optimize it.
//
// DolRecomp puts 16 KB of game code (4096 instructions) into one C function
// with a label per instruction. Compilers give up optimizing a function that
// big (Visual Studio: warning C4883), so the game code ran unoptimized:
// every value went through the stack and every memory access was a call.
// This splits that function into small "piece" functions of a few hundred
// instructions each, cut where a game function ends when possible. A branch
// to an instruction in another piece sets ctx->pc and returns 1; the
// file's original function (same name, same entry) loops, calling the piece
// that holds ctx->pc, until a piece returns 0 (leave this file, as before).
//
//   chunk_split [--size N] in.c out.c [in2.c out2.c ...]
//   chunk_split [--size N] --list file      (lines of "in.c|out.c")
//
// An output file is only written when its text changes (so the build only
// recompiles what changed). Files that don't look like DolRecomp output are
// copied unchanged.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

// (memmem isn't in every C library)
static const char* find_mem(const char* hay, size_t n, const char* needle, size_t k) {
    if (k == 0 || n < k) return NULL;
    for (size_t i = 0; i + k <= n; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, k) == 0) return hay + i;
    return NULL;
}

typedef struct { char* p; size_t n, cap; } Buf;
static void put(Buf* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = (char*)realloc(b->p, b->cap);
        if (!b->p) { fprintf(stderr, "chunk_split: out of memory\n"); exit(1); }
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(Buf* b, const char* s) { put(b, s, strlen(s)); }
static void printf_(Buf* b, const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) put(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static char* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* p = (char*)malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(p); return NULL; }
    fclose(f);
    p[n] = 0;
    *len = (size_t)n;
    return p;
}
// The split file lives in another folder than the original: its
// #include "../generated.h" becomes #include "generated.h" (found through
// the include path), followed by runtime/split_inline.h (the float helpers
// done in place). The small loop functions DolRecomp puts at the top get
// the same unaliased CPU state pointer as the pieces (SPLIT_RESTRICT).
static void put_line_fixed(Buf* b, const char* s, size_t n) {
    static const char inc[] = "#include \"../generated.h\"";
    static const char sig[] = "(CPUState* ctx) {";
    size_t m = n;
    while (m && (s[m - 1] == '\r' || s[m - 1] == ' ')) m--;
    if (n >= sizeof(inc) - 1 && memcmp(s, inc, sizeof(inc) - 1) == 0) {
        puts_(b, "#include \"generated.h\"\n#include \"split_inline.h\"");
        put(b, s + sizeof(inc) - 1, n - (sizeof(inc) - 1));
    } else if (n > 17 && memcmp(s, "static void loop_", 17) == 0 && m >= sizeof(sig) - 1 &&
               memcmp(s + m - (sizeof(sig) - 1), sig, sizeof(sig) - 1) == 0) {
        put(b, s, m - (sizeof(sig) - 1));
        puts_(b, "(CPUState* SPLIT_RESTRICT ctx) {");
        put(b, s + m, n - m);
    } else put(b, s, n);
}
static int write_if_changed(const char* path, const char* data, size_t len) {
    size_t old_len = 0;
    char* old = read_file(path, &old_len);
    if (old && old_len == len && memcmp(old, data, len) == 0) { free(old); return 0; }
    free(old);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "chunk_split: can't write %s\n", path); return -1; }
    fwrite(data, 1, len, f);
    fclose(f);
    return 1;
}

// lines of the file
typedef struct { const char* s; size_t n; } Line;
static Line* split_lines(char* text, size_t len, size_t* count) {
    size_t cap = 1024, n = 0;
    Line* L = (Line*)malloc(cap * sizeof(Line));
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') j++;
        if (n == cap) { cap *= 2; L = (Line*)realloc(L, cap * sizeof(Line)); }
        L[n].s = text + i;
        L[n].n = j - i;   // (without the newline; a trailing \r is kept)
        n++;
        i = j + 1;
    }
    *count = n;
    return L;
}
static int starts(const Line* l, const char* pre) {
    size_t k = strlen(pre);
    return l->n >= k && memcmp(l->s, pre, k) == 0;
}
static int hex8(const char* s, unsigned* out) {
    unsigned v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return 0;
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 1;
}
// "label_XXXXXXXX:" at the start of a line
static int label_line(const Line* l, unsigned* addr) {
    return l->n >= 15 && memcmp(l->s, "label_", 6) == 0 && l->s[14] == ':' && hex8(l->s + 6, addr);
}

typedef struct { unsigned addr; size_t line; int piece; int keep; int func; } Lab;
static int g_all_labels = 0;   // --all-labels: every instruction stays an entry point (no fallback copy)
// --funcs: a piece per game function (cut at every function start: the
// targets of calls in the file, and stack-frame prologues right after a
// return), and a call to a function in the same file calls its piece
// directly -- the compiler can then inline small functions into their
// callers (and drop the register traffic around those calls). Very long
// functions are still cut every 4x --size instructions.
static int g_funcs = 0;
static size_t g_total_labels, g_total_kept;
// --entries file: more entry points, one hex address per line (where the
// game was seen coming in off the rules below: runtime/profiler.c writes
// debug_output/split_entries.txt)
static unsigned* g_extra;
static size_t g_nextra, g_extra_cap;
static void read_entries(const char* path) {
    size_t len = 0;
    char* t = read_file(path, &len);
    if (!t) return;
    for (char* line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ') line++;
        if (line[0] == '0' && (line[1] == 'x' || line[1] == 'X')) line += 2;
        unsigned a;
        if (strlen(line) < 8 || !hex8(line, &a)) continue;
        if (g_nextra == g_extra_cap) {
            g_extra_cap = g_extra_cap ? g_extra_cap * 2 : 256;
            g_extra = (unsigned*)realloc(g_extra, g_extra_cap * sizeof(unsigned));
        }
        g_extra[g_nextra++] = a;
    }
    free(t);
}
static Lab* g_labs;
static size_t g_nlabs;
static int find_lab(unsigned addr) {   // (labels are in address order)
    size_t lo = 0, hi = g_nlabs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_labs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_nlabs && g_labs[lo].addr == addr ? (int)lo : -1;
}

// "    // XXXXXXXX: mnemonic operands" (DolRecomp's comment before each
// instruction's code): its address, and where the mnemonic starts
static int insn_line(const Line* l, unsigned* addr, const char** mn, size_t* mn_len) {
    if (l->n < 18 || memcmp(l->s, "    // ", 7) != 0 || l->s[15] != ':' || !hex8(l->s + 7, addr)) return 0;
    const char* p = l->s + 16;
    const char* e = l->s + l->n;
    while (p < e && *p == ' ') p++;
    const char* q = p;
    while (q < e && *q != ' ' && *q != '\r') q++;
    *mn = p;
    *mn_len = (size_t)(q - p);
    return 1;
}
static int mn_is(const char* mn, size_t n, const char* w) { return n == strlen(w) && memcmp(mn, w, n) == 0; }
static int line_is(const Line* l, const char* text) {   // (ignoring indentation and a trailing \r)
    const char* p = l->s;
    const char* e = l->s + l->n;
    while (p < e && *p == ' ') p++;
    while (e > p && (e[-1] == '\r' || e[-1] == ' ')) e--;
    return (size_t)(e - p) == strlen(text) && memcmp(p, text, (size_t)(e - p)) == 0;
}
static void keep_addr(unsigned a) { int k = find_lab(a); if (k >= 0) g_labs[k].keep = 1; }

// --calls: "ctx->lr = 0xR;" / "ctx->pc = ...;" / "return;" (DolRecomp's
// bl / bctrl / bcl / blrl) become a call that comes straight back.
static int g_calls = 0;
// --direct: a call to a fixed address in the game code runs that address's
// file function right away (func_X(ctx)) instead of going through
// splat_call's dispatch loop; when the callee comes back to the return
// address with the stack as it was (the usual case) the caller carries on,
// anything else is handed to splat_call_sp (the dispatch loop, as before).
// --abi: after such a call only the registers a called function may change
// (r0, r3-r12, by the PowerPC calling rules) are read back from ctx; the
// rest (r1, r2, r13-r31) keep their values in the caller's locals. Calls
// into --abi-except ranges (the compiler's register save/restore helpers,
// which do change r14-r31) and calls that took the slow way read back all.
static int g_direct = 0, g_abi = 0;
static size_t g_direct_calls = 0, g_local_calls = 0;   // (for the summary)
static unsigned g_keep_call[8], g_nkeep_call;           // --keep-call addr: always through splat_call (runtime hooks)
static unsigned g_abi_lo[4], g_abi_hi[4], g_nabi_ex;    // --abi-except lo-hi
#define DIRECT_LO 0x02000020u   // the game code with a file function per 16 KB
#define DIRECT_HI 0x02DD0020u   // (the last file and the import trampolines after it: through splat_call)
static int direct_target(unsigned t) {
    if (!g_direct || t < DIRECT_LO || t >= DIRECT_HI || (t & 3)) return 0;
    for (unsigned k = 0; k < g_nkeep_call; k++) if (g_keep_call[k] == t) return 0;
    return 1;
}
static int abi_target(int known, unsigned t) {
    if (!g_abi) return 0;
    if (!known) return 1;   // (through a pointer: never one of the helpers)
    for (unsigned k = 0; k < g_nabi_ex; k++) if (t >= g_abi_lo[k] && t < g_abi_hi[k]) return 0;
    return 1;
}
// A call inside the file as DolRecomp writes it: "ctx->lr = 0xRAu;" then
// "goto label_T;" (the callee's blr comes back through the return dispatch)
// (or, for a call backwards, with the loop-budget check in between:
//   if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) { ctx->pc = T; return; }
// ) -- returns how many lines the call takes (2 or 6), 0 if it isn't one
static int call_target(const Line* l, unsigned* t);
static int goto_line(const Line* l, unsigned* tgt) {
    const char* q = l->s;
    const char* qe = l->s + l->n;
    while (q < qe && *q == ' ') q++;
    return qe - q >= 20 && memcmp(q, "goto label_", 11) == 0 && hex8(q + 11, tgt) && q[19] == ';';
}
static int local_call_at(const Line* L, size_t i, size_t to, unsigned* ret, unsigned* tgt) {
    if (i + 1 >= to) return 0;
    const char* p = L[i].s;
    const char* e = L[i].s + L[i].n;
    while (p < e && *p == ' ') p++;
    if (e - p < 22 || memcmp(p, "ctx->lr = 0x", 12) != 0 || !hex8(p + 12, ret) || memcmp(p + 20, "u;", 2) != 0) return 0;
    if (goto_line(&L[i + 1], tgt)) return 2;
    unsigned t2;
    if (i + 5 < to && find_mem(L[i + 1].s, L[i + 1].n, "DOLRECOMP_C_LOOP_CYCLE_BUDGET", 29) && call_target(&L[i + 2], &t2) &&
        line_is(&L[i + 3], "return;") && line_is(&L[i + 4], "}") && goto_line(&L[i + 5], tgt) && *tgt == t2)
        return 6;
    return 0;
}

// The call's fixed target ("ctx->pc = 0xXXXXXXXXu;"), or 0 (through a register)
static int call_target(const Line* l, unsigned* t) {
    const char* q = l->s;
    const char* qe = l->s + l->n;
    while (q < qe && *q == ' ') q++;
    return qe - q >= 22 && memcmp(q, "ctx->pc = 0x", 12) == 0 && hex8(q + 12, t) && memcmp(q + 20, "u;", 2) == 0;
}
static int call_at(const Line* L, size_t i, size_t to, unsigned* ret) {
    if (i + 2 >= to) return 0;
    const char* p = L[i].s;
    const char* e = L[i].s + L[i].n;
    while (p < e && *p == ' ') p++;
    if (e - p < 22 || memcmp(p, "ctx->lr = 0x", 12) != 0 || !hex8(p + 12, ret) || memcmp(p + 20, "u;", 2) != 0) return 0;
    const char* q = L[i + 1].s;
    const char* qe = L[i + 1].s + L[i + 1].n;
    while (q < qe && *q == ' ') q++;
    if (qe - q < 11 || memcmp(q, "ctx->pc = ", 10) != 0) return 0;
    return line_is(&L[i + 2], "return;");
}

// Which instructions can the game come into this file at? Only those keep
// their label (and a case in their piece's switch); between them the code
// runs straight through, which the compiler can optimize far better (with a
// label at every instruction, every value had to be in the CPU state at
// every instruction). The entry points: the file's first instruction;
// every branch target inside the file; return addresses (after bl / bctrl
// / blrl: the "ctx->lr = X" values); every address the code leaves the
// file with ("ctx->pc = X; return"); the instruction after anything that
// leaves unconditionally (blr, b, bctr, rfi, sc, traps, the instructions
// done by DolRecomp's fallback) -- where a function after it starts, or the
// game comes back to; and function prologues (stwu r1,-N(r1) / mflr r0),
// for functions reached through pointers. Coming in anywhere else (a
// jump table or function pointer the rules missed) runs the file's
// original, unsplit code instead (func_X_full), which has every label.
static void find_entry_points(const Line* L, size_t from, size_t to) {
    if (g_all_labels) { for (size_t k = 0; k < g_nlabs; k++) g_labs[k].keep = 1; return; }
    g_labs[0].keep = 1;
    for (size_t i = from; i < to; i++) {
        const Line* l = &L[i];
        const char* s = l->s;
        size_t n = l->n;
        // branch targets, return addresses, exits
        for (size_t j = 0; j + 19 <= n; j++) {
            unsigned a;
            if (s[j] == 'g' && j + 19 <= n && memcmp(s + j, "goto label_", 11) == 0 && hex8(s + j + 11, &a)) keep_addr(a);
            else if (s[j] == 'c' && j + 22 <= n && memcmp(s + j, "ctx->lr = 0x", 12) == 0 && hex8(s + j + 12, &a)) keep_addr(a);
            else if (s[j] == 'c' && j + 22 <= n && memcmp(s + j, "ctx->pc = 0x", 12) == 0 && hex8(s + j + 12, &a)) {
                // (followed by leaving: return / the end of a loop function)
                size_t k = i + 1;
                while (k < to && L[k].n == 0) k++;
                if (k < to && (line_is(&L[k], "return;") || line_is(&L[k], "return 0;") || line_is(&L[k], "return 1;") || line_is(&L[k], "}")))
                    keep_addr(a);
            } else if (s[j] == 'c' && j + 19 <= n && memcmp(s + j, "case 0x", 7) == 0 && hex8(s + j + 7, &a)) keep_addr(a);
        }
        // after leaving unconditionally; prologues
        unsigned a;
        const char* mn;
        size_t mlen;
        if (insn_line(l, &a, &mn, &mlen)) {
            if (mn_is(mn, mlen, "blr") || mn_is(mn, mlen, "b") || mn_is(mn, mlen, "bctr") || mn_is(mn, mlen, "rfi") ||
                mn_is(mn, mlen, "sc") || mn_is(mn, mlen, "tw") || mn_is(mn, mlen, "twi") || mn_is(mn, mlen, "bl") ||
                mn_is(mn, mlen, "bctrl") || mn_is(mn, mlen, "blrl") || mn_is(mn, mlen, "bla") || mn_is(mn, mlen, "ba"))
                keep_addr(a + 4);
            if (mn_is(mn, mlen, "stwu") && find_mem(mn, (size_t)(s + n - mn), "r1, -", 5)) keep_addr(a);
            if (mn_is(mn, mlen, "mflr")) keep_addr(a);
            // (an instruction whose code returns at the top level: DolRecomp's fallback)
            for (size_t k = i + 1; k < to; k++) {
                unsigned b;
                const char* m2;
                size_t l2;
                if (label_line(&L[k], &b) || insn_line(&L[k], &b, &m2, &l2)) break;
                if (L[k].n >= 11 && memcmp(L[k].s, "    return", 10) == 0) { keep_addr(a + 4); break; }
            }
        }
    }
}

// --- --regcache: the game's integer registers in local variables ---------------------------
// DolRecomp's code keeps the PowerPC registers in the CPU state (ctx->gpr[N])
// and reads/writes them there for every instruction; any guest memory write
// (a byte store) might change them as far as the compiler knows, so it can't
// keep them in PC registers. With --regcache each piece copies the registers
// it uses into locals (sg_N) on entry, works on those, and writes them back
// before it leaves (every return) and around anything that may look at or
// change them through ctx (calls other than the memory and float helpers);
// after such a call they are read back. A piece with anything else that
// touches them (a register number that isn't a constant, its address taken,
// a jump on the same line as such a call) keeps the plain code.
static int g_regcache = 0;   // 1: --regcache (integer registers), 2: --regcache2 (and the special ones)
static int g_sm_base = 0;    // --membase: game memory through a base pointer read once per piece
// --probe lo-hi (hex, repeatable): an execution counter before every
// instruction in that range (debugging: which code runs, which branch is
// taken; runtime/main.c dumps them with SPLAT_PROBE_DUMP=1)
static unsigned g_probe_lo[16], g_probe_hi[16];
static int g_nprobe = 0;
static int probed(unsigned a) {
    for (int k = 0; k < g_nprobe; k++) if (a >= g_probe_lo[k] && a < g_probe_hi[k]) return 1;
    return 0;
}
static size_t g_rc_pieces, g_rc_bad_pieces;
static unsigned g_rc_mask;    // registers cached in the piece being written (0: none)
static unsigned g_rc_wmask;   // ... of those, the ones the piece writes (only those need writing back)
static unsigned g_rc_scan_w;  // (rc_scan's result for that)

// The other registers the code works on in ctx, kept in locals the same way:
// the condition register, XER, link and count registers, the cycle counter,
// and the address of the next instruction -- DolRecomp writes ctx->pc before
// most instructions; in a local, the compiler drops the writes nobody reads.
// (Only one helper the pieces call with ctx, ppc_fcmp, touches any of them:
// the condition register, written back before it and read back after.)
static const struct { const char* field; const char* local; const char* type; } g_sx[] = {
    { "cr", "sx_cr", "u32" }, { "xer", "sx_xer", "u32" }, { "lr", "sx_lr", "u32" },
    { "ctr", "sx_ctr", "u32" }, { "pc", "sx_pc", "u32" }, { "downcount", "sx_dc", "s64" },
};
enum { SX_CR = 0, SX_PC = 4, SX_DC = 5, NSX = 6 };
static unsigned g_sx_mask, g_sx_wmask, g_sx_scan_w;
static int rc_on(void) { return g_rc_mask || g_sx_mask; }
// "ctx->FIELD" (not a longer name) at s: which special register (-1: none); *len: its length
static int sx_at(const char* s, size_t n, size_t* len) {
    if (n < 7 || memcmp(s, "ctx->", 5) != 0) return -1;
    for (int k = 0; k < NSX; k++) {
        size_t fl = strlen(g_sx[k].field);
        if (n >= 5 + fl && memcmp(s + 5, g_sx[k].field, fl) == 0 &&
            (n == 5 + fl || !(isalnum((unsigned char)s[5 + fl]) || s[5 + fl] == '_'))) {
            *len = 5 + fl;
            return k;
        }
    }
    return -1;
}
// The name the code uses for special register k in the piece being written
static const char* sx_name(int k) {
    static char b[NSX][16];
    if (g_sx_mask >> k & 1) return g_sx[k].local;
    snprintf(b[k], sizeof(b[k]), "ctx->%s", g_sx[k].field);
    return b[k];
}
// A call that reads or writes the condition register in ctx (but no other register)?
static int sx_cr_call(const char* s, size_t n) {
    for (size_t i = 0; i + 14 <= n; i++)
        if (memcmp(s + i, "ppc_fcmp(ctx,", 13) == 0) return 1;
    return 0;
}

// --regcache3: the condition register's field 0 in a local of its own (sx_c0,
// 4 bits; sx_cr keeps the rest). Every integer compare in this game sets
// field 0 and the next branch tests one of its bits: kept in one 32-bit
// local, each compare was a read-modify-write of the whole register that
// the compiler couldn't see through; on its own, a compare result nobody
// reads goes away, and "compare, branch" becomes a compare and a branch.
static int g_cr_split = 0;
#define CR_FULL "((sx_cr & 0x0FFFFFFFu) | (sx_c0 << 28))"
// How a line uses the condition register: 0 not at all, 1 "field 0 =
// cr_bits" (DolRecomp's compares and record forms), 2 a branch's test of
// one bit (*mask, *want: "true"/"false"), 3 anything else (done on ctx->cr
// itself: written there before the line, read back after it).
static int cr_line_kind(const char* s, size_t n, unsigned* mask, char* want) {
    int has = 0;
    for (size_t j = 0; j + 7 <= n && !has; j++) {
        size_t fl;
        has = sx_at(s + j, n - j, &fl) == SX_CR && (j == 0 || !(isalnum((unsigned char)s[j - 1]) || s[j - 1] == '_'));
    }
    if (!has) return 0;
    char t[256];
    while (n && *s == ' ') { s++; n--; }
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
    if (n >= sizeof(t)) return 3;
    memcpy(t, s, n);
    t[n] = 0;
    if (!strcmp(t, "ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_bits << 28);") ||
        !strcmp(t, "ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (cr_bits << 28);"))
        return 1;
    int used = 0;
    if (sscanf(t, "bool cr_ok = (((ctx->cr & 0x%xu) != 0) == %7[a-z]);%n", mask, want, &used) == 2 && used == (int)n &&
        (!strcmp(want, "true") || !strcmp(want, "false")))
        return 2;
    return 3;
}

static void rc_flush(Buf* o) {
    for (int r = 0; r < 32; r++) if (g_rc_mask >> r & g_rc_wmask >> r & 1) printf_(o, "ctx->gpr[%d] = sg_%d; ", r, r);
    for (int k = 0; k < NSX; k++) {
        if (!(g_sx_mask >> k & g_sx_wmask >> k & 1)) continue;
        if (k == SX_CR && g_cr_split) puts_(o, "ctx->cr = " CR_FULL "; ");
        else printf_(o, "ctx->%s = %s; ", g_sx[k].field, g_sx[k].local);
    }
}
static void rc_reload(Buf* o) {
    for (int r = 0; r < 32; r++) if (g_rc_mask >> r & 1) printf_(o, "sg_%d = ctx->gpr[%d]; ", r, r);
    for (int k = 0; k < NSX; k++) {
        if (!(g_sx_mask >> k & 1)) continue;
        printf_(o, "%s = ctx->%s; ", g_sx[k].local, g_sx[k].field);
        if (k == SX_CR && g_cr_split) puts_(o, "sx_c0 = sx_cr >> 28; ");
    }
}

// After a call: abi = only the registers the callee may change (see --abi)
static void rc_reload_call(Buf* o, int abi) {
    for (int r = 0; r < 32; r++) {
        if (!(g_rc_mask >> r & 1)) continue;
        if (abi && !(r == 0 || (r >= 3 && r <= 12))) continue;
        printf_(o, "sg_%d = ctx->gpr[%d]; ", r, r);
    }
    for (int k = 0; k < NSX; k++) {
        if (!(g_sx_mask >> k & 1)) continue;
        printf_(o, "%s = ctx->%s; ", g_sx[k].local, g_sx[k].field);
        if (k == SX_CR && g_cr_split) puts_(o, "sx_c0 = sx_cr >> 28; ");
    }
}

// A call on this line that takes ctx and may read or write the registers there?
static int rc_barrier(const char* s, size_t n) {
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(s + i, "(ctx", 4) != 0) continue;
        if (i + 4 < n && (isalnum((unsigned char)s[i + 4]) || s[i + 4] == '_' || s[i + 4] == '-')) continue;   // (ctx->x, ctxy
        size_t b = i;
        while (b > 0 && (isalnum((unsigned char)s[b - 1]) || s[b - 1] == '_')) b--;
        if (b == i) continue;   // a bracket, not a call
        const char* nm = s + b;
        size_t len = i - b;
        if (len == 24 && memcmp(nm, "ppc_fallback_instruction", 24) == 0) return 1;
        if ((len >= 8 && memcmp(nm, "mem_read", 8) == 0) || (len >= 9 && memcmp(nm, "mem_write", 9) == 0) ||
            (len >= 5 && memcmp(nm, "ppc_f", 5) == 0) || (len >= 6 && memcmp(nm, "ppc_ps", 6) == 0))
            continue;   // memory and float helpers: they don't touch the integer registers
        return 1;
    }
    for (size_t i = 0; i + 6 <= n; i++)   // ctx handed over further along an argument list
        if ((memcmp(s + i, ", ctx,", 6) == 0 || memcmp(s + i, ", ctx)", 6) == 0)) return 1;
    return 0;
}

// lmw / stmw as DolRecomp writes them (a loop over a constant range):
// unrolled, so each register has a constant number. 0: not one of those.
static int rc_unroll(const char* s, size_t n, Buf* out) {
    while (n && *s == ' ') { s++; n--; }
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
    char t[200];
    if (n >= sizeof(t)) return 0;
    memcpy(t, s, n);
    t[n] = 0;
    unsigned a, b;
    int used = 0;
    if (sscanf(t, "for (u32 r = %u; r < %u; r++, ea += 4) ctx->gpr[r] = mem_read32(ctx, ea);%n", &a, &b, &used) == 2 && used == (int)n && a < b && b <= 32) {
        for (unsigned r = a; r < b; r++) printf_(out, "ctx->gpr[%u] = mem_read32(ctx, ea + %uu); ", r, (r - a) * 4);
        return 1;
    }
    used = 0;
    if (sscanf(t, "for (u32 r = %u; r < %u; r++, ea += 4) mem_write32(ctx, ea, ctx->gpr[r]);%n", &a, &b, &used) == 2 && used == (int)n && a < b && b <= 32) {
        for (unsigned r = a; r < b; r++) printf_(out, "mem_write32(ctx, ea + %uu, ctx->gpr[%u]); ", (r - a) * 4, r);
        return 1;
    }
    return 0;
}

// Is the register named at s[j..k] written there? ("=", "+=", "|=", "++", ... but not "==")
static int rc_written(const char* s, size_t n, size_t j, size_t k) {
    size_t q = k + 1;
    while (q < n && s[q] == ' ') q++;
    if (q < n && s[q] == '=' && !(q + 1 < n && s[q + 1] == '=')) return 1;
    if (q + 1 < n && s[q + 1] == '=' && strchr("+-*/%&|^", s[q])) return 1;
    if (q + 2 < n && s[q + 2] == '=' && ((s[q] == '<' && s[q + 1] == '<') || (s[q] == '>' && s[q + 1] == '>'))) return 1;
    if (q + 1 < n && ((s[q] == '+' && s[q + 1] == '+') || (s[q] == '-' && s[q + 1] == '-'))) return 1;
    if (j >= 2 && ((s[j - 1] == '+' && s[j - 2] == '+') || (s[j - 1] == '-' && s[j - 2] == '-'))) return 1;
    return 0;
}
static unsigned g_sx_scan;    // (rc_scan: the special registers the piece uses)

// Registers a piece's lines use (constant numbers); *bad: something the
// cache can't handle.
static unsigned rc_scan(const Line* L, size_t from, size_t to, int* bad) {
    unsigned mask = 0;
    for (size_t i = from; i < to; i++) {
        const char* s = L[i].s;
        size_t n = L[i].n;
        Buf u = { 0 };
        if (rc_unroll(s, n, &u)) { s = u.p; n = u.n; }
        for (size_t j = 0; j + 9 <= n; j++) {
            if (memcmp(s + j, "ctx->gpr[", 9) != 0) continue;
            if (j > 0 && s[j - 1] == '&') *bad = 1;
            size_t k = j + 9;
            unsigned r = 0;
            int digits = 0;
            while (k < n && s[k] >= '0' && s[k] <= '9') { r = r * 10 + (unsigned)(s[k] - '0'); k++; digits++; }
            if (!digits || k >= n || s[k] != ']' || r > 31) { *bad = 1; continue; }
            mask |= 1u << r;
            if (rc_written(s, n, j, k)) g_rc_scan_w |= 1u << r;
        }
        for (size_t j = 0; j + 7 <= n; j++) {   // the special registers
            size_t fl;
            int sx = sx_at(s + j, n - j, &fl);
            if (sx < 0 || (j > 0 && (isalnum((unsigned char)s[j - 1]) || s[j - 1] == '_'))) continue;
            if (j > 0 && s[j - 1] == '&') *bad = 1;
            g_sx_scan |= 1u << sx;
            if (rc_written(s, n, j, j + fl - 1)) g_sx_scan_w |= 1u << sx;
        }
        unsigned crm;
        char crw[8];
        if (rc_barrier(s, n) || sx_cr_call(s, n) || (g_cr_split && cr_line_kind(s, n, &crm, crw) == 3)) {
            for (size_t j = 0; j + 5 <= n; j++) if (memcmp(s + j, "goto ", 5) == 0) *bad = 1;
            size_t e = n;   // (must be a whole statement: reading back after it must happen on every path)
            while (e && (s[e - 1] == '\r' || s[e - 1] == ' ')) e--;
            if (!e || (s[e - 1] != ';' && s[e - 1] != '}')) *bad = 1;
        }
        free(u.p);
    }
    return mask;
}

// One line of a piece, with branches to other pieces and "return;" changed
// (and with --regcache, the registers as locals).
static void emit_line(Buf* o, const Line* l, int piece, const char* rd_name, int* uses_rd) {
    const char* s = l->s;
    size_t n = l->n, i = 0;
    Buf u = { 0 };
    int barrier = 0, crcall = 0;
    if (g_nprobe) {   // (--probe: count this instruction)
        unsigned pa;
        const char* mn;
        size_t ml;
        if (insn_line(l, &pa, &mn, &ml) && probed(pa)) printf_(o, "    SPLAT_PROBE(0x%08Xu);\n", pa);
    }
    if (rc_on()) {
        if (rc_unroll(s, n, &u)) { s = u.p; n = u.n; }
        barrier = rc_barrier(s, n);
        int crc = !barrier && (g_sx_mask >> SX_CR & 1);
        if (crc && g_cr_split) {
            unsigned m;
            char want[8];
            int kind = cr_line_kind(s, n, &m, want);
            if (kind == 1) { puts_(o, "        sx_c0 = cr_bits;\n"); free(u.p); return; }
            if (kind == 2) {
                if (!(m & 0x0FFFFFFFu)) printf_(o, "        bool cr_ok = (((sx_c0 & 0x%Xu) != 0) == %s);\n", m >> 28, want);
                else if (!(m & 0xF0000000u)) printf_(o, "        bool cr_ok = (((sx_cr & 0x%08Xu) != 0) == %s);\n", m, want);
                else printf_(o, "        bool cr_ok = ((((u32)" CR_FULL " & 0x%08Xu) != 0) == %s);\n", m, want);
                free(u.p);
                return;
            }
            crcall = kind == 3 || sx_cr_call(s, n);
        } else crcall = crc && sx_cr_call(s, n);
        if (barrier) { puts_(o, "    "); rc_flush(o); puts_(o, "\n"); }
        if (crcall) puts_(o, g_cr_split ? "    ctx->cr = " CR_FULL ";\n" : "    ctx->cr = sx_cr;\n");
    }
    int cache = rc_on() && !barrier;   // (a call line works on ctx itself: written back before, read back after)
    while (i < n) {
        if (n - i >= 16 && memcmp(s + i, "goto label_", 11) == 0) {
            unsigned a;
            if (n - i >= 20 && hex8(s + i + 11, &a) && s[i + 19] == ';') {
                int k = find_lab(a);
                if (k >= 0 && g_labs[k].piece != piece) {
                    puts_(o, "{ ");
                    if (cache) rc_flush(o);
                    printf_(o, "ctx->pc = 0x%08Xu; return 1; }", a);
                    i += 20;
                    continue;
                }
            }
        }
        if (g_sm_base && n - i >= 12 && (memcmp(s + i, "mem_read", 8) == 0 || memcmp(s + i, "mem_write", 9) == 0) &&
            (i == 0 || !(isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
            // game memory through the piece's base pointer (split_inline.h)
            size_t w = memcmp(s + i, "mem_read", 8) == 0 ? 8 : 9, d = i + w;
            while (d < n && isdigit((unsigned char)s[d])) d++;
            if (d > i + w && n - d >= 6 && memcmp(s + d, "(ctx, ", 6) == 0) {
                printf_(o, "sm_%s%.*s_b(sm_b, ", w == 8 ? "read" : "write", (int)(d - i - w), s + i + w);
                i = d + 6;
                continue;
            }
        }
        if (cache && n - i >= 7 && s[i] == 'c' && (i == 0 || !(isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
            size_t fl;
            int sx = sx_at(s + i, n - i, &fl);
            if (sx >= 0 && (g_sx_mask >> sx & 1) && !((crcall || g_cr_split) && sx == SX_CR)) { puts_(o, g_sx[sx].local); i += fl; continue; }
        }
        if (n - i >= 7 && memcmp(s + i, "return;", 7) == 0 && (i == 0 || (!isalnum((unsigned char)s[i - 1]) && s[i - 1] != '_'))) {
            if (cache) { puts_(o, "{ "); rc_flush(o); puts_(o, "return 0; }"); }
            else puts_(o, "return 0;");
            i += 7;
            continue;
        }
        if (cache && n - i >= 10 && memcmp(s + i, "ctx->gpr[", 9) == 0) {
            size_t k = i + 9;
            unsigned r = 0;
            while (k < n && s[k] >= '0' && s[k] <= '9') { r = r * 10 + (unsigned)(s[k] - '0'); k++; }
            if (k < n && s[k] == ']' && r < 32 && (g_rc_mask >> r & 1)) { printf_(o, "sg_%u", r); i = k + 1; continue; }
        }
        if (rd_name && n - i >= 5 && memcmp(s + i, "goto ", 5) == 0 && n - i - 5 >= strlen(rd_name) &&
            memcmp(s + i + 5, rd_name, strlen(rd_name)) == 0)
            *uses_rd = 1;
        put(o, s + i, 1);
        i++;
    }
    puts_(o, "\n");
    if (barrier) { puts_(o, "    "); rc_reload(o); puts_(o, "\n"); }
    if (crcall) puts_(o, g_cr_split ? "    sx_cr = ctx->cr; sx_c0 = sx_cr >> 28;\n" : "    sx_cr = ctx->cr;\n");
    free(u.p);
}

static int copy_fixed(const char* out, const Line* L, size_t nl, const char* text, size_t len) {
    Buf o = { 0 };
    for (size_t i = 0; i < nl; i++) { put_line_fixed(&o, L[i].s, L[i].n); if (i + 1 < nl || (len && text[len - 1] == '\n')) puts_(&o, "\n"); }
    int r = write_if_changed(out, o.p ? o.p : "", o.n);
    free(o.p);
    return r;
}


static int split_one(const char* in, const char* out, size_t target) {
    size_t len = 0;
    char* text = read_file(in, &len);
    if (!text) { fprintf(stderr, "chunk_split: can't read %s\n", in); return -1; }
    size_t nl = 0;
    Line* L = split_lines(text, len, &nl);

    // the file's function: "void func_XXXXXXXX(CPUState* ctx) {"
    size_t fstart = nl;
    unsigned base = 0;
    for (size_t i = 0; i < nl; i++)
        if (starts(&L[i], "void func_") && L[i].n >= 18 && hex8(L[i].s + 10, &base) && find_mem(L[i].s, L[i].n, "(CPUState* ctx) {", 17)) { fstart = i; break; }
    // its entry switch, the body (labels), the return dispatch, the end
    size_t sw_end = nl, body_end = nl, fend = nl, rd_line = nl;
    char rd_name[64];
    snprintf(rd_name, sizeof(rd_name), "return_dispatch_%08X", base);
    if (fstart < nl) {
        for (size_t i = fstart + 1; i < nl; i++)
            if (starts(&L[i], "    default: return;")) { sw_end = i + 1; break; }
        if (sw_end < nl && starts(&L[sw_end], "    }")) sw_end++;
        else sw_end = nl;
        for (size_t i = sw_end; i < nl; i++) {
            if (L[i].n >= strlen(rd_name) + 1 && memcmp(L[i].s, rd_name, strlen(rd_name)) == 0 && L[i].s[strlen(rd_name)] == ':') rd_line = i;
            if (L[i].n >= 1 && L[i].s[0] == '}' && (L[i].n == 1 || L[i].s[1] == '\r')) { fend = i; break; }
        }
        body_end = rd_line < fend ? rd_line : fend;
    }
    // labels in the body
    size_t labcap = 4096;
    g_labs = (Lab*)malloc(labcap * sizeof(Lab));
    g_nlabs = 0;
    int sorted = 1;
    if (fstart < nl && sw_end < nl && fend < nl)
        for (size_t i = sw_end; i < body_end; i++) {
            unsigned a;
            if (!label_line(&L[i], &a)) continue;
            if (g_nlabs == labcap) { labcap *= 2; g_labs = (Lab*)realloc(g_labs, labcap * sizeof(Lab)); }
            if (g_nlabs && a <= g_labs[g_nlabs - 1].addr) sorted = 0;
            g_labs[g_nlabs].addr = a;
            g_labs[g_nlabs].line = i;
            g_labs[g_nlabs].piece = 0;
            g_nlabs++;
        }
    if (fstart == nl || sw_end == nl || fend == nl || g_nlabs < 2 || !sorted || g_labs[0].addr != base) {
        // not the shape this tool knows: keep the file as it is
        int r = copy_fixed(out, L, nl, text, len);
        free(L); free(text); free(g_labs);
        return r < 0 ? -1 : 0;
    }
    // (nothing but the first label may come before it)
    for (size_t i = sw_end; i < g_labs[0].line; i++) {
        const char* s = L[i].s;
        size_t n = L[i].n;
        while (n && (s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
        if (n) { int r = copy_fixed(out, L, nl, text, len); free(L); free(text); free(g_labs); return r < 0 ? -1 : 0; }
    }

    // (--funcs: where the game's functions start)
    for (size_t k = 0; k < g_nlabs; k++) g_labs[k].func = 0;
    if (g_funcs) {
        for (size_t i = sw_end; i + 2 < body_end; i++) {   // targets of the calls in this file
            unsigned ra2, t2;
            int is_call = (call_at(L, i, body_end, &ra2) && call_target(&L[i + 1], &t2)) || local_call_at(L, i, body_end, &ra2, &t2);
            if (!is_call || t2 < base || t2 >= base + 0x4000u) continue;
            int li = find_lab(t2);
            if (li >= 0) g_labs[li].func = 1;
        }
        const char* prev_mn = NULL;   // prologues ("stwu r1,-N(r1)" / "mflr r0") right after an exit
        size_t prev_ml = 0;
        for (size_t k = 0; k < g_nlabs; k++) {
            size_t from = g_labs[k].line, to = k + 1 < g_nlabs ? g_labs[k + 1].line : body_end;
            const char* mn = NULL;
            size_t ml = 0;
            const Line* il = NULL;
            for (size_t i = from; i < to && !mn; i++) { unsigned ia; if (insn_line(&L[i], &ia, &mn, &ml)) il = &L[i]; }
            if (mn && prev_mn && (mn_is(prev_mn, prev_ml, "blr") || mn_is(prev_mn, prev_ml, "b") || mn_is(prev_mn, prev_ml, "bctr")) &&
                ((mn_is(mn, ml, "stwu") && find_mem(il->s, il->n, "r1, -", 5)) || (mn_is(mn, ml, "mflr") && find_mem(il->s, il->n, "r0", 2))))
                g_labs[k].func = 1;
            prev_mn = mn;
            prev_ml = ml;
        }
    }
    // Pieces: about `target` instructions each, cut at the start of a game
    // function if one is near (the instruction after a blr / an exit).
    int npieces = 1;
    size_t in_piece = 0;
    int last_was_end = 0;
    for (size_t k = 0; k < g_nlabs; k++) {
        if (k > 0) {
            int cut = 0;
            if (g_funcs) {
                if (g_labs[k].func) cut = 1;
                if (in_piece >= target * 4) cut = 1;
            } else {
                if (in_piece >= target && last_was_end) cut = 1;
                if (in_piece >= target * 2) cut = 1;
            }
            if (cut) { npieces++; in_piece = 0; }
        }
        g_labs[k].piece = npieces - 1;
        in_piece++;
        // does this instruction end a function (blr, or an unconditional exit)?
        size_t from = g_labs[k].line, to = k + 1 < g_nlabs ? g_labs[k + 1].line : body_end;
        last_was_end = 0;
        for (size_t i = from; i < to; i++) {
            if (find_mem(L[i].s, L[i].n, ": blr", 5) && !find_mem(L[i].s, L[i].n, ": blrl", 6)) last_was_end = 1;
        }
    }
    // the return dispatch's addresses
    unsigned* rd = NULL;
    size_t nrd = 0;
    if (rd_line < fend) {
        rd = (unsigned*)malloc((fend - rd_line) * sizeof(unsigned));
        for (size_t i = rd_line; i < fend; i++) {
            const char* p = find_mem(L[i].s, L[i].n, "case 0x", 7);
            unsigned a;
            if (p && (size_t)(p - L[i].s) + 15 <= L[i].n && hex8(p + 7, &a) && find_lab(a) >= 0) rd[nrd++] = a;
        }
    }

    for (size_t k = 0; k < g_nlabs; k++) g_labs[k].keep = 0;
    find_entry_points(L, 0, fstart);    // (the loop functions DolRecomp puts first)
    find_entry_points(L, sw_end, nl);   // (not the entry switch: it names every instruction)
    for (size_t k = 0; k < g_nlabs; k++)
        if (k == 0 || g_labs[k].piece != g_labs[k - 1].piece) g_labs[k].keep = 1;   // (a piece starts there)
    for (size_t j = 0; j < nrd; j++) keep_addr(rd[j]);
    for (size_t j = 0; j < g_nextra; j++) keep_addr(g_extra[j]);
    size_t nkept = 0;
    for (size_t k = 0; k < g_nlabs; k++) nkept += g_labs[k].keep;
    g_total_labels += g_nlabs;
    g_total_kept += nkept;

    Buf o = { 0 };
    puts_(&o, "// split into small functions by chunk_split (see SplatRecompRuntime/tools/chunk_split.c)\n");
    for (size_t i = 0; i < fstart; i++) { put_line_fixed(&o, L[i].s, L[i].n); puts_(&o, "\n"); }
    {   // (--probe: the counters, for a file that has some -- defined here, so the
        // other files don't change)
        int any = 0;
        for (int p = 0; p < g_nprobe; p++) if (g_probe_lo[p] < base + 0x4000u && g_probe_hi[p] > base) any = 1;
        if (any) puts_(&o, "extern u32 splat_probe_counts[];\n#define SPLAT_PROBE(a) (splat_probe_counts[((a) - 0x02000000u) >> 2]++)\n");
    }
    if (g_funcs) printf_(&o, "static int func_%08X_step(CPUState* ctx);\n", base);
    if (g_funcs)   // (pieces call each other directly: declared first)
        for (int p = 0; p < npieces; p++) printf_(&o, "static int func_%08X_p%d(CPUState* SPLIT_RESTRICT ctx);\n", base, p);
    size_t k = 0;
    for (int p = 0; p < npieces; p++) {
        size_t k0 = k;
        while (k < g_nlabs && g_labs[k].piece == p) k++;
        // (the entry points numbered 1.. through a table: a switch on the
        // addresses themselves, now that most are gone, became a chain of
        // compares instead of one jump)
        unsigned a0 = g_labs[k0].addr, pspan = g_labs[k - 1].addr - a0 + 4;
        printf_(&o, "static const unsigned short func_%08X_p%d_ent[%u] = {", base, p, pspan / 4);
        {
            int num = 0;
            size_t j = k0;
            for (unsigned x = 0; x < pspan / 4; x++) {
                while (j < k && g_labs[j].addr < a0 + x * 4) j++;
                int v = (j < k && g_labs[j].addr == a0 + x * 4 && g_labs[j].keep) ? ++num : 0;
                printf_(&o, "%s%d", x % 32 ? "," : (x ? ",\n    " : "\n    "), v);
            }
        }
        puts_(&o, "\n};\n");
        size_t from = g_labs[k0].line, to = k < g_nlabs ? g_labs[k].line : body_end;
        g_rc_mask = 0;
        g_sx_mask = 0;
        if (g_regcache) {
            int bad = 0;
            g_rc_scan_w = 0;
            g_sx_scan = g_sx_scan_w = 0;
            unsigned m = rc_scan(L, from, to, &bad);
            if (!bad) {
                g_rc_mask = m; g_rc_wmask = g_rc_scan_w;
                if (g_regcache > 1) { g_sx_mask = g_sx_scan; g_sx_wmask = g_sx_scan_w; }
            } else g_rc_bad_pieces++;
            g_rc_pieces++;
        }
        printf_(&o, "static int func_%08X_p%d(CPUState* SPLIT_RESTRICT ctx) {\n", base, p);
        for (int r = 0; r < 32; r++) if (g_rc_mask >> r & 1) printf_(&o, "    u32 sg_%d = ctx->gpr[%d];\n", r, r);
        for (int x = 0; x < NSX; x++) if (g_sx_mask >> x & 1) printf_(&o, "    %s %s = ctx->%s;\n", g_sx[x].type, g_sx[x].local, g_sx[x].field);
        if ((g_sx_mask >> SX_CR & 1) && g_cr_split) puts_(&o, "    u32 sx_c0 = sx_cr >> 28;\n");
        if (g_sm_base) puts_(&o, "    u8* const sm_b = wiiu_fastmem;\n    if (!sm_b) return 2;   /* (debugging options: the unsplit code, which checks every access) */\n");
        printf_(&o, "    u32 eo = ctx->pc - 0x%08Xu;\n    switch (eo < 0x%Xu && !(eo & 3) ? func_%08X_p%d_ent[eo >> 2] : 0) {\n", a0, pspan, base, p);
        {
            int num = 0;
            for (size_t j = k0; j < k; j++)
                if (g_labs[j].keep) printf_(&o, "    case %d: goto label_%08X;\n", ++num, g_labs[j].addr);
        }
        puts_(&o, g_all_labels ? "    default: return 0;\n    }\n" : "    default: return 2;   /* (not an entry point here: the unsplit code) */\n    }\n");
        int uses_rd = 0;
        for (size_t i = from; i < to; i++) {
            unsigned la;
            if (label_line(&L[i], &la)) {
                int li = find_lab(la);
                if (li >= 0 && !g_labs[li].keep) continue;
            }
            unsigned ra;
            unsigned lra, ltg;
            int lcl = (g_funcs && g_calls && rc_on()) ? local_call_at(L, i, to, &lra, &ltg) : 0;
            if (lcl) {
                // (--funcs) a call to a function in another piece of this file: call
                // that piece (the compiler may inline it); if the callee goes on in
                // other pieces of the file, step through them until it's back at the
                // return address; anything else (leaving the file, the loop budget)
                // ends in splat_call_sp, as other calls do.
                int tli = find_lab(ltg), rli = find_lab(lra);
                if (tli >= 0 && rli >= 0 && g_labs[rli].keep && g_labs[tli].piece != p &&
                    (tli == 0 || g_labs[tli - 1].piece != g_labs[tli].piece)) {
                    int here = g_labs[rli].piece == p, abi = abi_target(1, ltg);
                    emit_line(&o, &L[i], p, rd_name, &uses_rd);   // (lr = the return address)
                    printf_(&o, "            %s = 0x%08Xu;\n            ", sx_name(SX_PC), ltg);
                    rc_flush(&o);
                    printf_(&o, "\n            { const u32 sp_ = ctx->gpr[1]; ctx->downcount = 0; int r_ = func_%08X_p%d(ctx);\n", base, g_labs[tli].piece);
                    printf_(&o, "              while (r_ == 1 && !(ctx->pc == 0x%08Xu && ctx->gpr[1] == sp_)) r_ = func_%08X_step(ctx);\n", lra, base);
                    if (here) {
                        printf_(&o, "              if (ctx->pc == 0x%08Xu && ctx->gpr[1] == sp_ && !ctx->exception) { ", lra);
                        rc_reload_call(&o, abi);
                        printf_(&o, "goto label_%08X; }\n              if (splat_call_sp(ctx, 0x%08Xu, sp_)) { ", lra, lra);
                        rc_reload_call(&o, 0);
                        printf_(&o, "goto label_%08X; } }\n", lra);
                    } else
                        printf_(&o, "              if ((ctx->pc == 0x%08Xu && ctx->gpr[1] == sp_ && !ctx->exception) || splat_call_sp(ctx, 0x%08Xu, sp_)) { ctx->pc = 0x%08Xu; return 1; } }\n", lra, lra, lra);
                    puts_(&o, "            return 0;\n");
                    g_local_calls++;
                    i += (size_t)lcl - 1;
                    continue;
                }
            }
            if (g_calls && call_at(L, i, to, &ra)) {
                // a call: run the callee right here and carry on after it
                // when it comes back (see splat_call in runtime/main.c)
                emit_line(&o, &L[i], p, rd_name, &uses_rd);
                emit_line(&o, &L[i + 1], p, rd_name, &uses_rd);
                int ri = find_lab(ra);
                unsigned tgt = 0;
                int known = call_target(&L[i + 1], &tgt);
                int direct = known && direct_target(tgt), abi = abi_target(known, tgt);
                // (--direct, a call through a register -- bctrl/blrl, the game's
                // C++ virtual calls: the target's file function from a table)
                int table = g_direct && !known;
                int here = ri >= 0 && g_labs[ri].keep && g_labs[ri].piece == p;
                char invoke[512];
                int tli = (known && g_funcs && tgt >= base && tgt < base + 0x4000u) ? find_lab(tgt) : -1;
                if (direct && tli >= 0 && (tli == 0 || g_labs[tli - 1].piece != g_labs[tli].piece))
                    // (--funcs: a function in this file -- its piece, which the compiler may inline;
                    // anything it doesn't finish itself, the file's function carries on)
                    snprintf(invoke, sizeof(invoke), "ctx->downcount = 0; if (func_%08X_p%d(ctx)) func_%08X(ctx);", base, g_labs[tli].piece, base);
                else if (direct)
                    snprintf(invoke, sizeof(invoke), "ctx->downcount = 0; func_%08X(ctx);", DIRECT_LO + ((tgt - DIRECT_LO) & ~0x3FFFu));
                else if (table) {
                    char keep[160] = "";
                    for (unsigned k = 0; k < g_nkeep_call; k++) snprintf(keep + strlen(keep), sizeof(keep) - strlen(keep), " && t_ != 0x%08Xu", g_keep_call[k]);
                    snprintf(invoke, sizeof(invoke), "const u32 t_ = ctx->pc; if (t_ - 0x%08Xu < 0x%08Xu%s) { ctx->downcount = 0; splat_file_fn[(t_ - 0x%08Xu) >> 14](ctx); }",
                             DIRECT_LO, DIRECT_HI - DIRECT_LO, keep, DIRECT_LO);
                }
                if (direct || table) { direct = 1; g_direct_calls++; }
                if (rc_on()) {   // (registers to ctx for the callee, back again after it)
                    puts_(&o, "            ");
                    rc_flush(&o);
                    puts_(&o, "\n");
                    if (direct) {   // (see --direct)
                        printf_(&o, "            { const u32 sp_ = ctx->gpr[1]; %s\n", invoke);
                        if (here) {
                            printf_(&o, "              if (ctx->pc == 0x%08Xu && ctx->gpr[1] == sp_ && !ctx->exception) { ", ra);
                            rc_reload_call(&o, abi);
                            printf_(&o, "goto label_%08X; }\n              if (splat_call_sp(ctx, 0x%08Xu, sp_)) { ", ra, ra);
                            rc_reload_call(&o, 0);
                            printf_(&o, "goto label_%08X; } }\n", ra);
                        } else
                            printf_(&o, "              if ((ctx->pc == 0x%08Xu && ctx->gpr[1] == sp_ && !ctx->exception) || splat_call_sp(ctx, 0x%08Xu, sp_)) { ctx->pc = 0x%08Xu; return 1; } }\n", ra, ra, ra);
                    } else if (here) {
                        printf_(&o, "            if (splat_call(ctx, 0x%08Xu)) { ", ra);
                        rc_reload_call(&o, abi);
                        printf_(&o, "goto label_%08X; }\n", ra);
                    } else
                        printf_(&o, "            if (splat_call(ctx, 0x%08Xu)) { ctx->pc = 0x%08Xu; return 1; }\n", ra, ra);
                    puts_(&o, "            return 0;\n");   // (ctx holds them already)
                } else if (direct) {
                    printf_(&o, "            { const u32 sp_ = ctx->gpr[1]; %s\n", invoke);
                    if (here)
                        printf_(&o, "              if ((ctx->pc == 0x%08Xu && ctx->gpr[1] == sp_ && !ctx->exception) || splat_call_sp(ctx, 0x%08Xu, sp_)) goto label_%08X; }\n", ra, ra, ra);
                    else
                        printf_(&o, "              if ((ctx->pc == 0x%08Xu && ctx->gpr[1] == sp_ && !ctx->exception) || splat_call_sp(ctx, 0x%08Xu, sp_)) { ctx->pc = 0x%08Xu; return 1; } }\n", ra, ra, ra);
                    emit_line(&o, &L[i + 2], p, rd_name, &uses_rd);
                } else {
                    if (ri >= 0 && g_labs[ri].keep && g_labs[ri].piece == p)
                        printf_(&o, "            if (splat_call(ctx, 0x%08Xu)) goto label_%08X;\n", ra, ra);
                    else
                        printf_(&o, "            if (splat_call(ctx, 0x%08Xu)) { ctx->pc = 0x%08Xu; return 1; }\n", ra, ra);
                    emit_line(&o, &L[i + 2], p, rd_name, &uses_rd);
                }
                i += 2;
                continue;
            }
            if (g_direct && i > 0 && line_is(&L[i], "ppc_memory_fence();")) {
                // isync / eieio: nothing to order on x86 beyond what the
                // compiler might move (only "sync" needs the processor's fence)
                unsigned fa;
                const char* fm;
                size_t fl;
                if (insn_line(&L[i - 1], &fa, &fm, &fl) && (mn_is(fm, fl, "isync") || mn_is(fm, fl, "eieio"))) {
                    puts_(&o, "    SPLAT_COMPILER_BARRIER();\n");
                    continue;
                }
            }
            emit_line(&o, &L[i], p, rd_name, &uses_rd);
        }
        puts_(&o, "    ");
        rc_flush(&o);   // (nothing without --regcache)
        if (k < g_nlabs) printf_(&o, "\n    ctx->pc = 0x%08Xu;\n    return 1;\n", g_labs[k].addr);   // (falls into the next piece)
        else puts_(&o, "\n    return 0;\n");
        if (uses_rd) {
            printf_(&o, "%s:\n    if (%s <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) { ", rd_name, sx_name(SX_DC));
            rc_flush(&o);
            printf_(&o, "return 0; }\n    switch (%s) {\n", sx_name(SX_PC));
            for (size_t j = 0; j < nrd; j++) {
                int li = find_lab(rd[j]);
                if (li >= 0 && g_labs[li].piece == p) printf_(&o, "    case 0x%08Xu: goto label_%08X;\n", rd[j], rd[j]);
            }
            puts_(&o, "    default: ");
            rc_flush(&o);
            puts_(&o, "return 1;\n    }\n");
        }
        g_rc_mask = 0;
        g_sx_mask = 0;
        puts_(&o, "}\n\n");
    }
    // the original entry: the piece that holds ctx->pc, until one leaves
    unsigned span = g_labs[g_nlabs - 1].addr - base + 4;
    unsigned ninst = span / 4;
    printf_(&o, "static const unsigned %s func_%08X_piece[%u] = {", npieces > 255 ? "short" : "char", base, ninst);
    size_t li = 0;
    for (unsigned i = 0; i < ninst; i++) {
        unsigned a = base + i * 4;
        while (li + 1 < g_nlabs && g_labs[li + 1].addr <= a) li++;
        printf_(&o, "%s%d", i % 32 ? "," : (i ? ",\n    " : "\n    "), g_labs[li].piece);
    }
    puts_(&o, "\n};\n");
    if (g_funcs) {   // (--funcs: one piece of the file, for a call that goes on past its first piece)
        printf_(&o, "static int func_%08X_step(CPUState* ctx) {\n    u32 off = ctx->pc - 0x%08Xu;\n    if (off >= 0x%Xu || (off & 3)) return 0;\n    switch (func_%08X_piece[off >> 2]) {\n",
                base, base, span, base);
        for (int q = 0; q < npieces; q++) printf_(&o, "    case %d: return func_%08X_p%d(ctx);\n", q, base, q);
        puts_(&o, "    default: return 0;\n    }\n}\n");
    }
    if (!g_all_labels) {
        // the original function, for coming in where the pieces have no label
        // (built without optimization: it's huge, and rarely used; and put at
        // the end of the program's code, so the pieces of all the files sit
        // together instead of between these -- fewer pages of code in use)
        puts_(&o, "#if defined(__clang__)\n#pragma clang optimize off\n#elif defined(_MSC_VER)\n#pragma optimize(\"\", off)\n#endif\n"
                  "#if defined(_MSC_VER)\n#pragma code_seg(\".text$zfull\")\n#endif\n");
        printf_(&o, "static void func_%08X_full(CPUState* ctx) {\n", base);
        for (size_t i = fstart + 1; i <= fend; i++) { put(&o, L[i].s, L[i].n); puts_(&o, "\n"); }
        puts_(&o, "#if defined(_MSC_VER)\n#pragma code_seg()\n#endif\n"
                  "#if defined(__clang__)\n#pragma clang optimize on\n#elif defined(_MSC_VER)\n#pragma optimize(\"\", on)\n#endif\n");
    }
    printf_(&o, "void func_%08X(CPUState* ctx) {\n", base);
    puts_(&o, "    for (int hops = 0; hops < 256; hops++) {\n");
    printf_(&o, "        u32 off = ctx->pc - 0x%08Xu;\n", base);
    printf_(&o, "        if (off >= 0x%Xu || (off & 3)) return;\n", span);
    puts_(&o, "        int r;\n");
    printf_(&o, "        switch (func_%08X_piece[off >> 2]) {\n", base);
    for (int p = 0; p < npieces; p++) printf_(&o, "        case %d: r = func_%08X_p%d(ctx); break;\n", p, base, p);
    puts_(&o, "        default: return;\n        }\n");
    if (!g_all_labels) printf_(&o, "        if (r == 2) { splat_split_fallback(ctx->pc); func_%08X_full(ctx); return; }\n", base);
    puts_(&o, "        if (!r) return;\n    }\n}\n");
    // anything after the function (other helpers)
    for (size_t i = fend + 1; i < nl; i++) { put(&o, L[i].s, L[i].n); if (i + 1 < nl || (len && text[len - 1] == '\n')) puts_(&o, "\n"); }

    int r = write_if_changed(out, o.p, o.n);
    free(o.p); free(rd); free(L); free(text); free(g_labs);
    return r < 0 ? -1 : r;
}

int main(int argc, char** argv) {
    size_t target = 192;
    int i = 1, changed = 0, failed = 0, files = 0;
    for (;;) {
        if (i + 1 < argc && !strcmp(argv[i], "--size")) { target = (size_t)atoi(argv[i + 1]); i += 2; }
        else if (i < argc && !strcmp(argv[i], "--all-labels")) { g_all_labels = 1; i++; }
        else if (i + 1 < argc && !strcmp(argv[i], "--entries")) { read_entries(argv[i + 1]); i += 2; }
        else if (i < argc && !strcmp(argv[i], "--calls")) { g_calls = 1; i++; }
        else if (i < argc && !strcmp(argv[i], "--regcache")) { g_regcache = 1; i++; }
        else if (i < argc && !strcmp(argv[i], "--regcache2")) { g_regcache = 2; i++; }
        else if (i < argc && !strcmp(argv[i], "--regcache3")) { g_regcache = 2; g_cr_split = 1; i++; }
        else if (i < argc && !strcmp(argv[i], "--membase")) { g_sm_base = 1; i++; }
        else if (i < argc && !strcmp(argv[i], "--direct")) { g_direct = 1; i++; }
        else if (i < argc && !strcmp(argv[i], "--funcs")) { g_funcs = 1; i++; }
        else if (i < argc && !strcmp(argv[i], "--abi")) { g_abi = 1; i++; }
        else if (i + 1 < argc && !strcmp(argv[i], "--keep-call")) { if (g_nkeep_call < 8) g_keep_call[g_nkeep_call++] = (unsigned)strtoul(argv[i + 1], NULL, 16); i += 2; }
        else if (i + 1 < argc && !strcmp(argv[i], "--abi-except")) {
            unsigned lo, hi;
            if (sscanf(argv[i + 1], "%x-%x", &lo, &hi) == 2 && hi > lo && g_nabi_ex < 4) { g_abi_lo[g_nabi_ex] = lo; g_abi_hi[g_nabi_ex] = hi; g_nabi_ex++; }
            i += 2;
        }
        else if (i + 1 < argc && !strcmp(argv[i], "--probe")) {
            unsigned lo, hi;
            if (sscanf(argv[i + 1], "%x-%x", &lo, &hi) == 2 && hi > lo && g_nprobe < 16) { g_probe_lo[g_nprobe] = lo; g_probe_hi[g_nprobe] = hi; g_nprobe++; }
            i += 2;
        }
        else break;
    }
    if (target < 16) target = 16;
    if (g_all_labels) g_sm_base = 0;   // (its fallback is the unsplit code, which --all-labels leaves out)
    if (i + 1 < argc && !strcmp(argv[i], "--list")) {   // a file of "in|out" lines (long lists)
        size_t ll = 0;
        char* list = read_file(argv[i + 1], &ll);
        if (!list) { fprintf(stderr, "chunk_split: can't read %s\n", argv[i + 1]); return 2; }
        for (char* line = strtok(list, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            char* bar = strchr(line, '|');
            if (!bar) continue;
            *bar = 0;
            int r = split_one(line, bar + 1, target);
            files++;
            if (r < 0) failed++;
            else changed += r;
        }
        free(list);
        printf("chunk_split: %d files, %d rewritten, %zu of %zu instructions are entry points, registers in locals in %zu of %zu pieces, %zu direct calls, %zu calls inside files made piece calls%s\n",
               files, changed, g_total_kept, g_total_labels, g_rc_pieces - g_rc_bad_pieces, g_rc_pieces, g_direct_calls, g_local_calls, failed ? ", SOME FAILED" : "");
        return failed ? 1 : 0;
    }
    if (i + 1 >= argc) { fprintf(stderr, "usage: chunk_split [--size N] [--all-labels] [--entries file]... (--list file | in.c out.c [...])\n"); return 2; }
    for (; i + 1 < argc; i += 2) {
        int r = split_one(argv[i], argv[i + 1], target);
        files++;
        if (r < 0) failed++;
        else changed += r;
    }
    printf("chunk_split: %d files, %d rewritten, %zu of %zu instructions are entry points, registers in locals in %zu of %zu pieces, %zu direct calls, %zu calls inside files made piece calls%s\n",
           files, changed, g_total_kept, g_total_labels, g_rc_pieces - g_rc_bad_pieces, g_rc_pieces, g_direct_calls, g_local_calls, failed ? ", SOME FAILED" : "");
    return failed ? 1 : 0;
}
