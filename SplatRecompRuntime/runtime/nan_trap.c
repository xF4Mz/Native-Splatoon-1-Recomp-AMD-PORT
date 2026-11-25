// Where do the game's numbers first turn into NaN? (SPLAT_NAN_TRAP=1)
//
// The frozen TV world (Oct 2026) was the game's own camera holding NaN
// ("not a number"): every frustum test fails, so the game draws no solid
// world, only effects and the HUD, until the next scene load. To find the
// instruction that makes the first NaN, the game threads run with the
// processor's "invalid operation" exception switched on: 0/0, inf-inf, 0*inf,
// the square root of a negative, a NaN compared with comisd, a float -> int
// conversion out of range. The handler notes where (the host instruction, the
// game's pc / lr and two return addresses up its stack), lets that one
// instruction finish with the exception masked (a single step), and switches
// it back on. The sites are printed with the [hle] reports, oldest first.
// Off (the default): nothing is switched on, no cost.
#ifdef _WIN32
#include <windows.h>
#include <xmmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include "fast_cpu.h"
#include "wiiu_mem.h"
#include "host_thread.h"

CPUState* threads_current_cpu(const char** name);
u32 gx2_frame_count(void);

#define NT_SITES 1024
#define NT_MAX_TRAPS 400000   // (after this many: off -- a NaN already spreading through the game costs two exceptions per compare)
typedef struct {
    u64 rip;
    u32 pc, lr, lr2, lr3, first_frame, last_frame, seq;
    volatile long n;
    u8 code[8];
    char thread[16];
    u8 shown;
} NtSite;
static NtSite g_nt[NT_SITES];
static volatile long g_nt_lock, g_nt_total, g_nt_seq, g_nt_full;
static int g_nt_on = -1;
static HOST_THREAD_LOCAL int tl_nt;   // 0 not armed, 1 armed, 2 stepping over the trapped instruction
static u64 g_nt_base;

static u32 nt_rd(u32 a) { return (a && wiiu_mem_ptr(a, 4)) ? wiiu_read32(a) : 0; }

static void nt_note(const CONTEXT* cx) {
    const char* name = "?";
    CPUState* cpu = threads_current_cpu(&name);
    u32 pc = 0, lr = 0, lr2 = 0, lr3 = 0;
    if (cpu) {
        pc = cpu->pc;
        lr = cpu->lr;
        u32 f1 = nt_rd(cpu->gpr[1]);
        lr2 = nt_rd(f1 + 4);
        lr3 = nt_rd(nt_rd(f1) + 4);
    }
    u64 rip = cx->Rip;
    u32 h = (u32)((rip * 0x9E3779B97F4A7C15ull) >> 54);
    while (_InterlockedCompareExchange(&g_nt_lock, 1, 0)) {}
    for (u32 k = 0; k < 64; k++) {
        NtSite* s = &g_nt[(h + k) & (NT_SITES - 1)];
        if (!s->rip) {
            s->rip = rip;
            s->pc = pc; s->lr = lr; s->lr2 = lr2; s->lr3 = lr3;
            s->first_frame = gx2_frame_count();
            s->seq = (u32)++g_nt_seq;
            memcpy(s->code, (const void*)rip, 8);
            snprintf(s->thread, sizeof(s->thread), "%s", name);
        }
        if (s->rip == rip) { s->n++; s->last_frame = gx2_frame_count(); break; }
    }
    g_nt_lock = 0;
}

// --- Memory watch (tests): SPLAT_WATCH_HEX=<bytes> SPLAT_WATCH_OFFS=a,b,... [SPLAT_WATCH_FRAME=n]
// At game frame n (default 900) the main thread looks for the byte pattern in game memory and
// watches the 4 bytes at each offset of the first matches (up to 4 places in all) with the
// processor's debug registers: every read or write by the main thread is noted -- the game's pc
// / lr / callers and the word's value -- and printed with the [hle] reports. (For finding the
// code that reads a save field.)
#define WT_SITES 256
typedef struct { u64 rip; u32 pc, lr, lr2, lr3, addr, val, seq; volatile long n; } WtSite;
static WtSite g_wt[WT_SITES];
static volatile long g_wt_lock, g_wt_seq;
static u64 g_wt_host[4];
static u32 g_wt_guest[4];
static int g_wt_n;
static int g_wt_state;   // 0 off, 1 waiting for the frame, 2 armed
#define WT_ARM_CODE 0xE0571A7Cu
static LONG CALLBACK wt_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* cx = ep->ContextRecord;
    if (code == WT_ARM_CODE) {   // (arm this thread's debug registers)
        u64 dr7 = 0;
        u64* drs[4] = { &cx->Dr0, &cx->Dr1, &cx->Dr2, &cx->Dr3 };
        for (int i = 0; i < g_wt_n; i++) {
            *drs[i] = g_wt_host[i];
            dr7 |= 1ull << (2 * i);              // local enable
            dr7 |= 3ull << (16 + 4 * i);         // read or write
            dr7 |= 3ull << (18 + 4 * i);         // 4 bytes
        }
        cx->Dr7 = dr7;
        cx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        cx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && g_wt_state == 2 && (cx->Dr6 & 0xF)) {
        int which = (cx->Dr6 & 1) ? 0 : (cx->Dr6 & 2) ? 1 : (cx->Dr6 & 4) ? 2 : 3;
        cx->Dr6 = 0;
        const char* name = "?";
        CPUState* cpu = threads_current_cpu(&name);
        u32 pc = 0, lr = 0, lr2 = 0, lr3 = 0;
        if (cpu) { pc = cpu->pc; lr = cpu->lr; u32 f1 = nt_rd(cpu->gpr[1]); lr2 = nt_rd(f1 + 4); lr3 = nt_rd(nt_rd(f1) + 4); }
        u64 rip = cx->Rip;
        u32 h = (u32)(((rip ^ which) * 0x9E3779B97F4A7C15ull) >> 56);
        while (_InterlockedCompareExchange(&g_wt_lock, 1, 0)) {}
        for (u32 k = 0; k < 64; k++) {
            WtSite* s = &g_wt[(h + k) & (WT_SITES - 1)];
            if (!s->rip) { s->rip = rip; s->pc = pc; s->lr = lr; s->lr2 = lr2; s->lr3 = lr3; s->addr = g_wt_guest[which]; s->seq = (u32)++g_wt_seq; }
            if (s->rip == rip && s->addr == g_wt_guest[which]) { s->n++; break; }   // (no read of the watched word here: it would trap again, inside the lock)
        }
        g_wt_lock = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
// Called by the main thread at every frame (gx2.c GX2SwapScanBuffers).
// (test runs: live input "WATCH:addr[,addr...]" -- up to 4 words at those game addresses, armed on the main
// thread at its next frame; "MEMDUMP" writes the game's memory 10000000-50000000 to debug_output\memdump.bin)
static volatile u32 g_wt_req[4];
static volatile int g_wt_req_n;
void watch_request(const u32* addrs, int n) {
    for (int i = 0; i < n && i < 4; i++) g_wt_req[i] = addrs[i];
    g_wt_req_n = n < 4 ? n : 4;
}
void splat_memdump(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("[watch] can't write %s\n", path); return; }
    u8* base = (u8*)wiiu_mem_ptr(0x10000000, 4);
    static u8 zero[65536];
    u32 written = 0;
    for (u32 ga = 0x10000000; ga < 0x50000000; ) {
        u8* hp = base + (ga - 0x10000000);
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(hp, &mi, sizeof(mi))) break;
        u32 len = (u32)((u8*)mi.BaseAddress + mi.RegionSize - hp);
        if (ga + (u64)len > 0x50000000u) len = 0x50000000u - ga;
        bool ok = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        for (u32 o = 0; o < len; o += 65536) {   // (pages not committed: zeros, so offsets stay addresses)
            u32 n = len - o < 65536 ? len - o : 65536;
            fwrite(ok ? hp + o : zero, 1, n, f);
        }
        if (ok) written += len;
        ga += len;
    }
    fclose(f);
    printf("[watch] game memory 10000000-50000000 written to %s (%u MB in use)\n", path, written >> 20);
}
void watch_poll(u32 frame) {
    if (g_wt_req_n > 0 && g_wt_state != 2) {   // (live request: exact addresses)
        if (g_wt_state <= 0 || g_wt_state == 3) AddVectoredExceptionHandler(1, wt_handler);
        u8* base = (u8*)wiiu_mem_ptr(0x10000000, 4);
        g_wt_n = 0;
        for (int i = 0; i < g_wt_req_n; i++) {
            g_wt_guest[g_wt_n] = g_wt_req[i];
            g_wt_host[g_wt_n] = (u64)(base + (g_wt_req[i] - 0x10000000));
            g_wt_n++;
        }
        g_wt_req_n = 0;
        g_wt_state = 2;
        RaiseException(WT_ARM_CODE, 0, 0, NULL);
        printf("[watch] watching %d words on the main thread (from frame %u)\n", g_wt_n, frame);
        return;
    }
    if (g_wt_state == 0) {
        const char* e = getenv("SPLAT_WATCH_HEX");
        g_wt_state = e ? 1 : -1;
        if (e) AddVectoredExceptionHandler(1, wt_handler);
    }
    if (g_wt_state != 1) return;
    const char* fe = getenv("SPLAT_WATCH_FRAME");
    if (frame < (u32)(fe ? atoi(fe) : 900)) return;
    g_wt_state = 3;
    u8 pat[256];
    int pn = 0;
    for (const char* p = getenv("SPLAT_WATCH_HEX"); p[0] && p[1] && pn < 256; ) {
        if (hexval(p[0]) < 0) { p++; continue; }
        pat[pn++] = (u8)(hexval(p[0]) * 16 + hexval(p[1]));
        p += 2;
    }
    int offs[4], no = 0;
    for (const char* p = getenv("SPLAT_WATCH_OFFS"); p && *p && no < 4; ) { offs[no++] = atoi(p); p = strchr(p, ','); if (p) p++; }
    if (!no) offs[no++] = 0;
    // (only memory that's there: the game's memory is committed as it's used)
    u8* base = (u8*)wiiu_mem_ptr(0x10000000, 4);
    u32 found[8], nf = 0;
    for (u32 ga = 0x10000000; ga < 0x50000000 && nf < 8; ) {
        u8* hp = base + (ga - 0x10000000);
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(hp, &mi, sizeof(mi))) break;
        u32 len = (u32)((u8*)mi.BaseAddress + mi.RegionSize - hp);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (u32 i = 0; i + (u32)pn <= len && nf < 8; i += 4)
                if (hp[i] == pat[0] && !memcmp(hp + i, pat, pn)) found[nf++] = ga + i;
        ga += len;
    }
    printf("[watch] pattern of %d bytes found at", pn);
    for (u32 i = 0; i < nf; i++) printf(" %08X", found[i]);
    printf("%s\n", nf ? "" : " no place");
    g_wt_n = 0;
    for (u32 i = 0; i < nf && g_wt_n < 4; i++)
        for (int k = 0; k < no && g_wt_n < 4; k++) {
            g_wt_guest[g_wt_n] = found[i] + offs[k];
            g_wt_host[g_wt_n] = (u64)(base + (found[i] + offs[k] - 0x10000000));
            g_wt_n++;
        }
    if (!g_wt_n) return;
    g_wt_state = 2;
    RaiseException(WT_ARM_CODE, 0, 0, NULL);
    printf("[watch] watching %d words on the main thread\n", g_wt_n);
}
void watch_report(FILE* out) {
    if (g_wt_state != 2) return;
    while (_InterlockedCompareExchange(&g_wt_lock, 1, 0)) {}
    int idx[WT_SITES], m = 0;
    for (int i = 0; i < WT_SITES; i++) if (g_wt[i].rip) idx[m++] = i;
    for (int a = 1; a < m; a++) { int v = idx[a], b = a; while (b > 0 && g_wt[idx[b - 1]].seq > g_wt[v].seq) { idx[b] = idx[b - 1]; b--; } idx[b] = v; }
    fprintf(out, "[watch] %d places touched the watched words (oldest first; game addr, value now, pc / lr / callers, count, host offset):\n", m);
    for (int k = 0; k < m && k < 60; k++) {
        WtSite* s = &g_wt[idx[k]];
        fprintf(out, "  %08X  (%08X)  pc %08X lr %08X <- %08X <- %08X  %6ld  +%08llX\n", s->addr, s->val, s->pc, s->lr, s->lr2 ? s->lr2 - 4 : 0,
                s->lr3 ? s->lr3 - 4 : 0, s->n, (unsigned long long)(s->rip - (u64)GetModuleHandleA(NULL)));
    }
    g_wt_lock = 0;
}

static LONG CALLBACK nt_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* cx = ep->ContextRecord;
    if (code == STATUS_SINGLE_STEP && tl_nt == 2) {
        cx->EFlags &= ~0x100u;
        if (!g_nt_full) {   // (the flags stay as the instruction left them: the game's FPSCR copies read them)
            cx->MxCsr &= ~0x80u;
            cx->FltSave.MxCsr = cx->MxCsr;
            tl_nt = 1;
        } else tl_nt = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((code == STATUS_FLOAT_MULTIPLE_TRAPS || code == STATUS_FLOAT_INVALID_OPERATION) && tl_nt == 1) {
        nt_note(cx);
        if (_InterlockedIncrement(&g_nt_total) >= NT_MAX_TRAPS) g_nt_full = 1;
        cx->MxCsr |= 0x80u;
        cx->FltSave.MxCsr = cx->MxCsr;
        cx->EFlags |= 0x100u;
        tl_nt = 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Called by each game thread when it starts.
void nan_trap_arm_thread(void) {
    if (g_nt_on < 0) {
        const char* e = getenv("SPLAT_NAN_TRAP");
        g_nt_on = e && atoi(e) > 0;
        if (g_nt_on) {
            g_nt_base = (u64)GetModuleHandleA(NULL);
            AddVectoredExceptionHandler(1, nt_handler);
            printf("[nan] NaN finder on: game threads stop at the first instruction that makes a NaN (SPLAT_NAN_TRAP)\n");
        }
    }
    if (!g_nt_on || g_nt_full) return;
    tl_nt = 1;
    _mm_setcsr(_mm_getcsr() & ~0x80u);
}

// The sites seen so far, oldest first; new ones marked. (Called with the [hle] reports.)
void nan_trap_report(FILE* out) {
    if (g_nt_on <= 0) return;
    static u32 last_total;
    u32 total = (u32)g_nt_total;
    if (total == last_total) return;
    last_total = total;
    while (_InterlockedCompareExchange(&g_nt_lock, 1, 0)) {}
    int idx[NT_SITES], m = 0;
    for (int i = 0; i < NT_SITES; i++) if (g_nt[i].rip) idx[m++] = i;
    for (int a = 1; a < m; a++) { int v = idx[a], b = a; while (b > 0 && g_nt[idx[b - 1]].seq > g_nt[v].seq) { idx[b] = idx[b - 1]; b--; } idx[b] = v; }
    fprintf(out, "[nan] %u invalid-operation traps so far%s, %d places (oldest first; host offset, code bytes, game pc / lr / callers, thread, count, frames):\n",
            total, g_nt_full ? " (limit reached: finder off)" : "", m);
    for (int k = 0; k < m && k < 80; k++) {
        NtSite* s = &g_nt[idx[k]];
        fprintf(out, "  %s#%-3u +%08llX %02X%02X%02X%02X%02X%02X  pc %08X lr %08X <- %08X <- %08X  %-10s %8ld  frames %u-%u\n",
                s->shown ? " " : "*", s->seq, (unsigned long long)(s->rip - g_nt_base),
                s->code[0], s->code[1], s->code[2], s->code[3], s->code[4], s->code[5],
                s->pc, s->lr, s->lr2 ? s->lr2 - 4 : 0, s->lr3 ? s->lr3 - 4 : 0, s->thread, s->n, s->first_frame, s->last_frame);
        s->shown = 1;
    }
    g_nt_lock = 0;
}
#else
#include <stdio.h>
void nan_trap_arm_thread(void) {}
void nan_trap_report(FILE* out) { (void)out; }
void watch_poll(unsigned frame) { (void)frame; }
void watch_report(FILE* out) { (void)out; }
void watch_request(const unsigned* addrs, int n) { (void)addrs; (void)n; }
void splat_memdump(const char* path) { (void)path; }
#endif
