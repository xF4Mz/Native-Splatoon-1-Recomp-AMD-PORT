#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700   // for host_thread.h on Linux
#endif
// Recomp runtime entry point.
//
// 1. Load the .rpx into Wii U guest memory (data, imports, symbols).
// 2. Hook system calls so they run our PC implementations.
// 3. Run the recompiled code from the game's entry point.
#include "generated.h"      // produced by DolRecomp for this .rpx
#include "wiiu_mem.h"
#include "rpx_loader.h"
#include "hle.h"
#include "cpu_fallback.h"
#include "fs.h"
#include "scan.h"
#include "guest_threads.h"
#include "host_thread.h"
#include "display.h"

extern bool g_render_headless;
extern u32 g_render_dump_frames;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <xmmintrin.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
#include "profiler.h"

// Address we use as a fake "return here" when the host calls guest code.
#define HOST_RETURN_ADDRESS 0xC1FFFF00u

static RpxImage g_image;
static u32 g_text_start = 0, g_text_size = 0;   // the game's .text section

// Per host thread: the guest CPU it's running and its "stop" flag.
static HOST_THREAD_LOCAL CPUState* tl_cpu = NULL;
CPUState* splat_current_cpu(void) { return tl_cpu; }   // (the game thread this PC thread runs)
static HOST_THREAD_LOCAL volatile int* tl_stop = NULL;

// A profile-guided build's counters (CMake SPLAT_PGO=gen) are written by
// __llvm_profile_write_file; _Exit skips the usual at-exit write, so call it
// here. Other builds don't have it: the linker uses the stand-in below.
int splat_no_profile_write(void) { return 0; }
#if defined(_MSC_VER)
#pragma comment(linker, "/alternatename:__llvm_profile_write_file=splat_no_profile_write")
#endif
int __llvm_profile_write_file(void);

void runtime_exit(int code) {
    if (__llvm_profile_write_file() == 0 && getenv("SPLAT_PGO_NOTE")) printf("[runtime] profile written\n");
    profiler_report(stdout);
    printf("------------------------------------------------------------\n");
    printf("[runtime] game exited with code %d\n", code);
    fflush(stdout);
    fflush(stderr);
    _Exit(code);
}

// If the PC program itself crashes, say where the game was instead of
// dying silently.
static void report_crash(const char* what) {
    fprintf(stderr, "\n[runtime] the runtime crashed: %s\n", what);
    if (tl_cpu) {
        fprintf(stderr, "          game was at pc=%08X lr=%08X r1=%08X r3=%08X\n",
                tl_cpu->pc, tl_cpu->lr, tl_cpu->gpr[1], tl_cpu->gpr[3]);
        rpx_describe_address(&g_image, tl_cpu->pc, stderr);
        hle_print_trail(stderr);
    }
    fflush(stderr);
    fflush(stdout);
}

#ifdef _WIN32
// A stack overflow kills the program before the signal handler can run (it
// needs stack): this vectored handler writes one line straight to the log
// first, with no library calls.
static HANDLE g_log_handle = INVALID_HANDLE_VALUE;
static int log_flush_thread(void* arg) {
    (void)arg;
    for (;;) { Sleep(200); fflush(stdout); }
    return 0;
}
static LONG CALLBACK on_stack_overflow(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    char msg[120] = "\n[runtime] crash: the PC stack ran out (stack overflow) in a game thread, game pc ";
    size_t n = 0;
    while (msg[n]) n++;
    u32 pc = tl_cpu ? tl_cpu->pc : 0;
    for (int i = 7; i >= 0; i--) msg[n++] = "0123456789ABCDEF"[(pc >> (i * 4)) & 15];
    msg[n++] = '\n';
    DWORD w;
    if (g_log_handle != INVALID_HANDLE_VALUE) WriteFile(g_log_handle, msg, (DWORD)n, &w, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void on_signal(int sig) {
    report_crash(sig == SIGSEGV ? "invalid memory access (SIGSEGV)" :
                 sig == SIGFPE  ? "math error (SIGFPE)" :
                 sig == SIGILL  ? "illegal instruction (SIGILL)" : "abort");
    _Exit(3);
}

// Ctrl+C (or closing the console): still write the speed report.
static void stop_requested(void) {
    printf("\n[runtime] stopped with Ctrl+C\n");
    profiler_report(stdout);
    fflush(stdout);
    fflush(stderr);
    _Exit(0);
}
#ifdef _WIN32
static BOOL WINAPI on_console_event(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        stop_requested();
        return TRUE;
    }
    return FALSE;
}
#else
static void on_interrupt(int sig) { (void)sig; stop_requested(); }
#endif

#ifdef _WIN32
static LONG WINAPI on_windows_exception(EXCEPTION_POINTERS* info) {
    char what[128];
    DWORD code = info->ExceptionRecord->ExceptionCode;
    snprintf(what, sizeof(what), "Windows exception 0x%08lX%s", (unsigned long)code,
             code == 0xC00000FD ? " (stack overflow)" :
             code == 0xC0000005 ? " (access violation)" : "");
    report_crash(what);
    {   // where in the PC program: the faulting address, its module and the offset in it (our exe: see the .map / a debugger)
        void* at = info->ExceptionRecord->ExceptionAddress;
        HMODULE mod = NULL;
        char name[260] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &mod) && mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        fprintf(stderr, "          PC program at %p = %s + 0x%llX%s\n", at, base ? base + 1 : name, (unsigned long long)((char*)at - (char*)mod),
                code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2 ? (info->ExceptionRecord->ExceptionInformation[0] ? " (writing)" : " (reading)") : "");
        if (code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2)
            fprintf(stderr, "          address touched: 0x%llX\n", (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Extra detail for crash reports: who called whom (by walking the stack's
// saved return addresses), the code of the function that went wrong, and
// any text that registers point at (names of things being set up).
static bool printable_string(u32 addr, char* out, size_t size) {
    if (addr < 0x10000000u || addr >= 0x50000000u) return false;
    size_t n = 0;
    for (; n + 1 < size; n++) {
        u8 ch = wiiu_read8(addr + (u32)n);
        if (!ch) break;
        if (ch < 0x20 || ch > 0x7E) return false;
        out[n] = (char)ch;
    }
    out[n] = 0;
    return n >= 3;
}

static void dump_words(FILE* out, u32 from, u32 to) {
    for (u32 a = from, k = 0; a < to; a += 4, k++)
        fprintf(out, "%s%08X", (k % 8) ? " " : (k ? "\n      " : "      "), wiiu_read32(a));
    fprintf(out, "\n");
}

// --peek ADDR: at a crash, show the word at ADDR and the memory it points to.
static u32 g_peek[8];
static int g_peek_count = 0;
static void report_peeks(FILE* out) {
    for (int i = 0; i < g_peek_count; i++) {
        u32 v = wiiu_read32(g_peek[i]);
        fprintf(out, "  peek %08X = %08X; memory there:\n", g_peek[i], v);
        for (u32 a = v, k = 0; k < 0x60 && v; a += 4, k += 4) {
            u32 w = wiiu_read32(a);
            fprintf(out, "%s%08X", (k % 32) ? " " : (k ? "\n      " : "      "), w);
        }
        fprintf(out, "\n");
    }
}

// Walk the guest stack frames and print the return addresses.
void runtime_print_chain(CPUState* cpu, FILE* out, int max_depth, bool with_code) {
    fprintf(out, "    %08X\n", cpu->lr);
    u32 frame = cpu->gpr[1];
    for (int depth = 0; depth < max_depth; depth++) {
        u32 back = wiiu_read32(frame);
        if (!back || back <= frame || back >= 0x50000000u) break;
        u32 ra = wiiu_read32(back + 4);
        frame = back;
        if (ra < 0x02000000u || ra >= 0x10000000u) continue;
        if (!with_code) { fprintf(out, "    %08X\n", ra); continue; }
        fprintf(out, "    %08X  (call at %08X; code before it:", ra, ra - 4);
        for (u32 a = ra - 24; a < ra; a += 4) fprintf(out, " %08X", wiiu_read32(a));
        fprintf(out, ")\n");
    }
}

static void report_context(CPUState* cpu, FILE* out) {
    report_peeks(out);
    char text[64];
    fprintf(out, "  text the registers point at:\n");
    for (int r = 0; r < 32; r++)
        if (printable_string(cpu->gpr[r], text, sizeof(text)))
            fprintf(out, "    r%d -> \"%s\"\n", r, text);

    // The function containing the bad call: from its start (mflr r0) to lr.
    u32 start = cpu->lr - 4;
    for (u32 a = cpu->lr - 4, k = 0; k < 1024; a -= 4, k++)
        if (wiiu_read32(a) == 0x7C0802A6u) { start = a - 8; break; }
    fprintf(out, "  code of the function that made the bad call (%08X-%08X):\n", start, cpu->lr);
    dump_words(out, start, cpu->lr + 8);

    fprintf(out, "  call chain (most recent first):\n");
    runtime_print_chain(cpu, out, 24, true);
    // Stack frames often hold the objects/arguments involved.
    fprintf(out, "  stack (r1=%08X):\n", cpu->gpr[1]);
    dump_words(out, cpu->gpr[1], cpu->gpr[1] + 0x80);
}

// For a call to an empty pointer through a C++ object's function table
// (lwz rT,12(rObj) / lwz rF,slot(rT) / mtctr rF / bctrl): the slot offset,
// or 0 if the call doesn't look like that.
static u32 null_virtual_slot(CPUState* cpu) {
    u32 site = cpu->lr - 4;
    if (wiiu_read32(site) != 0x4E800421u) return 0;   // bctrl
    int ctr_reg = -1;
    for (u32 i = 1; i <= 8; i++) {
        u32 w = wiiu_read32(site - 4 * i);
        if (ctr_reg < 0) {
            if ((w & 0xFC1FFFFFu) == 0x7C0903A6u) ctr_reg = (int)((w >> 21) & 31);   // mtctr rF
            continue;
        }
        if ((w >> 26) == 32 && (int)((w >> 21) & 31) == ctr_reg) return w & 0xFFFF;   // lwz rF, slot(rT)
    }
    return 0;
}

// Details for a call to an empty function pointer: the function that made it
// (often a short "load pointer, jump" helper), the object in r3 and its
// first words (the virtual function table pointer comes first in most objects).
static void report_missing_function(CPUState* cpu, FILE* out) {
    u32 site = cpu->lr - 4;
    u32 word = wiiu_read32(site);
    fprintf(out, "  call site %08X: %08X\n", site, word);
    if ((word & 0xFC000003u) == 0x48000001u) {   // bl: show the helper it called
        s32 off = (s32)(word & 0x03FFFFFCu);
        if (off & 0x02000000) off -= 0x04000000;
        u32 target = site + (u32)off;
        fprintf(out, "  it called %08X, which jumps on to the empty pointer:\n", target);
        dump_words(out, target, target + 0x40);
    }
    fprintf(out, "  registers:");
    for (int r = 0; r < 32; r++)
        fprintf(out, "%s r%d=%08X", (r % 8) ? "" : "\n   ", r, cpu->gpr[r]);
    fprintf(out, "\n");
    u32 obj = cpu->gpr[3];
    if (wiiu_mem_ptr(obj, 4)) {
        fprintf(out, "  object in r3 (%08X):\n", obj);
        dump_words(out, obj, obj + 0x40);
        u32 vt = wiiu_read32(obj);
        if (wiiu_mem_ptr(vt, 4)) {
            fprintf(out, "  table at its first word (%08X):\n", vt);
            dump_words(out, vt, vt + 0x40);
        }
    }
    report_context(cpu, out);
    threads_describe_current(out);
}

// Called when the game stops itself (OSPanic/OSFatal): show where.
void runtime_report_halt(CPUState* cpu) {
    fprintf(stderr, "  the game stopped itself here:\n");
    threads_describe_current(stderr);
    report_context(cpu, stderr);
    threads_report_all(stderr);
}

static bool host_call_hook(CPUState* cpu, u32 address) {
    if (address == HOST_RETURN_ADDRESS || address == THREAD_EXIT_ADDRESS) {
        if (tl_stop) *tl_stop = 1;   // back to whoever started this code
        return true;
    }
    return hle_host_call(cpu, address);
}

void runtime_setup_cpu(CPUState* cpu) {
    cpu_init(cpu);
    wiiu_mem_attach(cpu);
    cpu->host_call = host_call_hook;
    cpu_fallback_attach(cpu);
    cpu->msr = 0x00002000u;            // floating point enabled
    // Turn on paired-single math (two floats per register), which the Wii U's
    // OS enables at boot, and load its default quantization formats.
    cpu->hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
    cpu->gqr[2] = 0x00040004u;   // u8
    cpu->gqr[3] = 0x00050005u;   // u16
    cpu->gqr[4] = 0x00060006u;   // s8
    cpu->gqr[5] = 0x00070007u;   // s16
}

// --- Calls that come straight back (the optimized game code, tools/chunk_split.c) ---
// A call in the game code ("bl", "bctrl") used to leave the piece of code it
// was made from: back out to runtime_run, which found the called function;
// and when that returned, runtime_run found this function again, its piece,
// and the place after the call -- two trips through the run loop and three
// look-ups for every call. Now the call runs the callee from here, and when
// it comes back (to the return address, with the stack pointer as it was),
// the piece carries on right after the call.
// Anything else -- an exception, the thread being stopped, a return past
// the caller, too long without runtime_run (it takes turns between threads),
// too deep -- goes back out to runtime_run, which simply carries on from
// wherever the code is: the same instructions in the same order either way.
static HOST_THREAD_LOCAL u32 tl_call_used;   // jumps run inside calls since runtime_run last dispatched
static HOST_THREAD_LOCAL int tl_call_bail;   // give control back to runtime_run now
static HOST_THREAD_LOCAL int tl_call_depth;
static volatile int g_fast_calls = -1;   // SPLAT_NO_FAST_CALLS=1: every call goes out through runtime_run, as before
void splat_set_fast_calls(int on) { g_fast_calls = on ? 1 : 0; }
u64 g_main_sys_round;   // main thread: rdtsc ticks from making a system call to being back (for the log)
extern int g_hle_timing;   // (hle.c: SPLAT_HLE_TIMING=1 times every system call; off, it cost ~1.5% of the main thread)
u64 g_main_guest_calls, g_main_dispatches;   // main thread: game-code calls / trips through runtime_run (for the log)
static HOST_THREAD_LOCAL int tl_count_calls = -1;   // (SPLAT_AB_FAST_CALLS in gx2.c: on/off by turns, for measuring)

// SPLAT_CALL_HIST=lo-hi (hex): where the game code goes inside [lo, hi), counted
// per thread (main / timer -- alarms and the sound engine's 3 ms callback --
// / the rest) and logged every 600 frames: for finding out what runs (and
// what never does), e.g. in the sound library.
#define HIST_N 131072
static u32 g_hist_lo, g_hist_hi;
static int g_hist_on = -1;
static volatile long g_hist_pc[HIST_N];
static u32 g_hist_cnt[HIST_N][3];
static u32 g_hist_first[HIST_N];   // the frame it was first seen in
static u32 g_hist_order[HIST_N], g_hist_thr[HIST_N];   // first-seen sequence number and guest thread
static volatile long g_hist_seq;
extern u32 gx2_frame_count(void);
static void hist_note(u32 pc) {
    if (g_hist_on < 0) {
        const char* e = getenv("SPLAT_CALL_HIST");
        unsigned lo = 0, hi = 0;
        g_hist_on = e && sscanf(e, "%x-%x", &lo, &hi) == 2 && hi > lo;
        g_hist_lo = lo; g_hist_hi = hi;
    }
    if (!g_hist_on || pc - g_hist_lo >= g_hist_hi - g_hist_lo) return;
    int t = threads_is_main() ? 0 : threads_in_alarm() ? 1 : 2;
    u32 h = ((pc >> 2) * 2654435761u) >> 15;
    for (u32 k = 0; k < HIST_N; k++, h = (h + 1) & (HIST_N - 1)) {
        long cur = g_hist_pc[h];
        if (cur == (long)pc) { g_hist_cnt[h][t]++; return; }
        if (cur == 0 && _InterlockedCompareExchange(&g_hist_pc[h], (long)pc, 0) == 0) { g_hist_first[h] = gx2_frame_count(); g_hist_order[h] = (u32)_InterlockedIncrement(&g_hist_seq); g_hist_thr[h] = threads_current_object(); g_hist_cnt[h][t]++; return; }
        if (g_hist_pc[h] == (long)pc) { g_hist_cnt[h][t]++; return; }
    }
}
// chunk_split --probe: execution counters, one per game instruction in the
// probed ranges (the split files define SPLAT_PROBE). SPLAT_PROBE_DUMP=1
// writes the ones that ran to debug_output/probe.txt ("address count") with
// each 600-frame report.
u32 splat_probe_counts[(0x02E00000u - 0x02000000u) / 4];
static void probe_dump(void) {
    static int on = -1;
    if (on < 0) on = getenv("SPLAT_PROBE_DUMP") != NULL;
    if (!on) return;
    FILE* f = fopen("debug_output/probe.txt", "w");
    if (!f) return;
    for (u32 i = 0; i < sizeof(splat_probe_counts) / 4; i++)
        if (splat_probe_counts[i]) fprintf(f, "%08X %u\n", 0x02000000u + i * 4, splat_probe_counts[i]);
    fclose(f);
}

void splat_report_call_hist(void) {
    probe_dump();
    if (g_hist_on <= 0) return;
    printf("[calls] in %08X-%08X (main / timer / other threads):\n", g_hist_lo, g_hist_hi);
    if (getenv("SPLAT_CALL_HIST_ALL")) {   // (every address seen, in address order, to a file)
        FILE* f = fopen("debug_output/call_hist.txt", "w");
        if (f) {
            for (u32 h = 0; h < HIST_N; h++)   // (table order: sort the file by address)
                if (g_hist_pc[h])
                    fprintf(f, "%08lX %u %u %u first frame %u order %u thread %08X\n", (unsigned long)g_hist_pc[h], g_hist_cnt[h][0], g_hist_cnt[h][1], g_hist_cnt[h][2], g_hist_first[h], g_hist_order[h], g_hist_thr[h]);
            fclose(f);
        }
    }
    static u8 shown[HIST_N];
    memset(shown, 0, sizeof(shown));
    for (int k = 0; k < 80; k++) {
        int best = -1;
        u32 bt = 0;
        for (int i = 0; i < HIST_N; i++) {
            u32 t = g_hist_cnt[i][0] + g_hist_cnt[i][1] + g_hist_cnt[i][2];
            if (g_hist_pc[i] && !shown[i] && t > bt) { bt = t; best = i; }
        }
        if (best < 0) break;
        shown[best] = 1;
        printf("    %08lX %8u %8u %8u\n", (unsigned long)g_hist_pc[best], g_hist_cnt[best][0], g_hist_cnt[best][1], g_hist_cnt[best][2]);
    }
}

// The sound engine is set up on its own thread (core 0) while the game's main
// thread goes on to set up the sound-archive player. On the console the main
// thread spends milliseconds loading files first, so the sound system is
// always ready by then; here file loads are instant and the main thread can
// get there first -- the player then refuses to start (it checks the sound
// system's "initialized" flag once) and the game never plays a sound. So the
// player setup waits for that flag (up to 2 s).
#define SND_PLAYER_SETUP 0x02CEDA84u     // nw::snd SoundArchivePlayer::Initialize(arc, param)
#define SND_SYSTEM_READY 0x101E8B00u     // nw::snd SoundSystem "initialized" byte
static void wait_for_sound_system(void) {
    if (wiiu_read8(SND_SYSTEM_READY)) return;
    int waited = 0;
    while (!wiiu_read8(SND_SYSTEM_READY) && waited < 2000) {
        threads_sleep_ticks(WIIU_TIMER_HZ / 1000);
        waited++;
    }
    printf("[audio] sound-archive player setup waited %d ms for the sound system (%s)\n", waited,
           wiiu_read8(SND_SYSTEM_READY) ? "ready" : "still not ready");
}

int splat_call_sp(CPUState* cpu, u32 ret, u32 sp);
int splat_call(CPUState* cpu, u32 ret) { return splat_call_sp(cpu, ret, cpu->gpr[1]); }

// The game-code file functions by (address - 0x02000020) >> 14, for calls
// through a register in the split code (chunk_split --direct). A file without
// one gets this stand-in: the call then goes through splat_call_sp.
#define SPLAT_FILE_FNS ((0x02DD0020u - 0x02000020u) >> 14)
static void splat_file_none(CPUState* ctx) { (void)ctx; }
void (*splat_file_fn[SPLAT_FILE_FNS])(CPUState* ctx);
static void (*splat_file_real[SPLAT_FILE_FNS])(CPUState* ctx);
static HOST_THREAD_LOCAL u32 tl_table_depth;
// Every call through the table comes here first: it counts toward the jumps
// budget (tl_call_used: runtime_run's checks -- other threads' turns -- keep
// their pace) and caps how deeply these calls nest on the PC's stack. Past
// either, the call isn't run here: the caller hands it to splat_call_sp,
// which gives control back to runtime_run. (Without the cap, call chains that
// never come back -- the game going round in calls through pointers -- grew
// the stack until the program died without a word.)
u64 g_main_table_calls, g_main_table_bail_depth, g_main_table_bail_budget, g_main_table_same;   // (main thread, for the log: calls through pointers and why some were handed to splat_call_sp)
static u32 g_table_cap = 0;   // (how deep; SPLAT_TABLE_DEPTH=N, 8..4096)
static void splat_file_via_table(CPUState* ctx) {
    if (tl_count_calls > 0) g_main_table_calls++;
    if (!g_table_cap) { const char* e = getenv("SPLAT_TABLE_DEPTH"); u32 v = e ? (u32)atoi(e) : 32; g_table_cap = v < 8 ? 8 : v > 4096 ? 4096 : v; }
    if (tl_table_depth >= g_table_cap) { if (tl_count_calls > 0) g_main_table_bail_depth++; return; }
    if (++tl_call_used >= 512) { if (tl_count_calls > 0) g_main_table_bail_budget++; return; }
    tl_table_depth++;
    splat_file_real[(ctx->pc - 0x02000020u) >> 14](ctx);
    tl_table_depth--;
}
static void splat_file_fns_init(void) {
    for (u32 i = 0; i < SPLAT_FILE_FNS; i++) {
        DolRecompFunction f = dolrecomp_find_original(0x02000020u + (i << 14));
        splat_file_real[i] = f ? f : splat_file_none;
        splat_file_fn[i] = splat_file_via_table;
    }
}

// The same, for a call that's already under way: the game code called the
// callee's file directly (chunk_split --direct) and it didn't come straight
// back -- carry on until it returns to `ret` with the stack pointer at `sp`
// (the caller's), as splat_call would have.
int splat_call_sp(CPUState* cpu, u32 ret, u32 sp) {
    if (g_fast_calls <= 0) {
        if (g_fast_calls < 0) g_fast_calls = getenv("SPLAT_NO_FAST_CALLS") == NULL;
        if (!g_fast_calls) return 0;
    }
    if (tl_call_bail || tl_call_depth >= 64) return 0;
    if (tl_count_calls < 0) tl_count_calls = threads_is_main();
    int ok = 0;
    tl_call_depth++;
    for (;;) {
        u32 pc = cpu->pc;
        if (pc == ret && cpu->gpr[1] == sp) { ok = 1; break; }
        if (cpu->gpr[1] > sp || cpu->exception || pc < 0x100u || (tl_stop && *tl_stop)) break;
        if (++tl_call_used >= 512) { tl_call_bail = 1; break; }
        cpu->downcount = 0;
        bool ran;
        if (pc - g_text_start < g_text_size) {
            if (tl_count_calls) g_main_guest_calls++;
            if (g_hist_on) hist_note(pc);
            if (pc == SND_PLAYER_SETUP) wait_for_sound_system();
            ran = dolrecomp_call_original(cpu, pc);
        } else if (g_hle_timing && threads_is_main()) {   // (a system call: timed, for the log -- see hle_report_main_calls; SPLAT_HLE_TIMING=1)
            u64 t0 = __rdtsc();
            ran = dolrecomp_call(cpu, pc);
            g_main_sys_round += __rdtsc() - t0;
        } else ran = dolrecomp_call(cpu, pc);
        if (!ran || tl_call_bail) break;
    }
    tl_call_depth--;
    return ok;
}

// Run recompiled code until *stop is set. Getting stuck ends the program
// with a report (on any thread).
bool runtime_run(CPUState* cpu, volatile int* stop) {
    // The console runs every game thread in the processor's non-IEEE mode (Cafe OS starts threads
    // with FPSCR = 4, the NI bit; the game never changes it): a result too small for a normal
    // float becomes 0. Without that, the game's "flatten a direction and normalize it" (02661328)
    // got a tiny x*x + z*z when moving almost straight up or down -- a kettle dive, an ink-cannon
    // launch -- passed its "> 0" check, and its square-root estimate overflowed: 0 * inf = NaN.
    // The NaN reached the TV camera, the game culled the whole world against a NaN view and drew
    // only effects and the HUD until the next scene load (the frozen TV world). The processor's
    // flush-to-zero does the same rounding for the game code here (the float instructions'
    // direct paths round through doubles; FTZ zeroes the final double -> single step too).
    // The game never reads FPSCR, so its NI bit stays clear and the fast paths stay on.
    // SPLAT_IEEE_DENORMALS=1: keep tiny results (the old behaviour).
    {
        static int ieee = -1;
        if (ieee < 0) ieee = getenv("SPLAT_IEEE_DENORMALS") != NULL;
        if (!ieee) _mm_setcsr(_mm_getcsr() | 0x8000u);
    }
    CPUState* saved_cpu = tl_cpu;
    volatile int* saved_stop = tl_stop;
    tl_cpu = cpu;
    tl_stop = stop;
    u64 steps = 0;
    // Spin-wait detection. On the console the game's threads run on three
    // cores at once, so a thread can busy-wait ("loop until the workers
    // finish") while the workers do the job. Here threads take turns, so a
    // busy-waiting thread just burns its turn while the workers sit idle.
    // A loop that keeps coming back to the run loop in exactly the same state
    // (same place, same register values) is waiting, not working: hand the
    // CPU to the other threads right away. Loops that do work change their
    // registers, so they keep running (only very long ones take turns).
    u32 spin_region = 0, spin_count = 0, idle_count = 0;
    u64 recent[4] = { 0, 0, 0, 0 };
    u32 recent_pos = 0;
    if (tl_count_calls < 0) tl_count_calls = threads_is_main();
    while (!*stop) {
        if (tl_count_calls > 0) g_main_dispatches++;
        cpu->downcount = 0;
        if (cpu->pc < 0x100u && !hle_strict()) {
            // The game called a function pointer that is empty (usually a
            // callback some system library would have filled in). Act as if
            // that function did nothing and returned 0. Unlike a real call
            // there's no stack to undo: every call goes through here.
            static u32 null_calls = 0;
            null_calls++;
            if (null_calls == 1) {
                fprintf(stderr, "\n[runtime] the game called an empty function pointer (0x%08X) from %08X;\n"
                                "          pretending it returned 0 and carrying on. details for the first one:\n",
                        cpu->pc, cpu->lr - 4);
                report_missing_function(cpu, stderr);
            } else if (null_calls <= 16 || (null_calls & (null_calls - 1)) == 0) {
                fprintf(stderr, "[runtime] empty function pointer #%u called from %08X (r3=%08X), skipped\n",
                        null_calls, cpu->lr - 4, cpu->gpr[3]);
            }
            u32 result = 0;
            u32 slot = null_virtual_slot(cpu);
            static int heap_fix = -1;   // (SPLAT_NO_WORKHEAP=1 turns this off)
            if (heap_fix < 0) heap_fix = getenv("SPLAT_NO_WORKHEAP") == NULL;
            if (heap_fix && slot == 0x34 && cpu->gpr[3] == 0) {
                // sead::Heap::alloc(size, alignment) on a heap that doesn't
                // exist (the game's "agl::WorkHeap" -- its setup failed):
                // hand out memory from the main heap instead, so the textures
                // it was for (lighting, colour grading) still get made.
                s32 al = (s32)cpu->gpr[5];
                u32 align = al < 0 ? (u32)-al : (u32)al;
                if (align < 4) align = 4;
                result = hle_heap_alloc(cpu->gpr[4], align);
                static int logged = 0;
                if (logged++ < 8)
                    fprintf(stderr, "[runtime] (missing heap) gave %u bytes at %08X to the call from %08X\n", cpu->gpr[4], result, cpu->lr - 4);
            }
            cpu->gpr[3] = result;
            cpu->pc = cpu->lr;
            continue;
        }
        // Game code goes straight to its recompiled function; anything
        // else (system calls, special addresses) takes the general path.
        tl_call_used = 0;
        tl_call_bail = 0;
        if (g_hist_on) hist_note(cpu->pc);
        if (cpu->pc == SND_PLAYER_SETUP) wait_for_sound_system();
        bool ran = (cpu->pc - g_text_start < g_text_size)
                       ? dolrecomp_call_original(cpu, cpu->pc)
                       : dolrecomp_call(cpu, cpu->pc);
        if (!ran) {
            fprintf(stderr,
                    "\n[runtime] stuck: jumped to 0x%08X, which isn't recompiled code or a known import\n"
                    "          lr=%08X ctr=%08X r3=%08X steps=%llu\n",
                    cpu->pc, cpu->lr, cpu->ctr, cpu->gpr[3], (unsigned long long)steps);
            threads_describe_current(stderr);
            rpx_describe_address(&g_image, cpu->pc, stderr);
            fprintf(stderr, "  words there: %08X %08X %08X %08X\n",
                    wiiu_read32(cpu->pc), wiiu_read32(cpu->pc + 4),
                    wiiu_read32(cpu->pc + 8), wiiu_read32(cpu->pc + 12));
            fprintf(stderr, "  called from 0x%08X:\n", cpu->lr - 4);
            rpx_describe_address(&g_image, cpu->lr - 4, stderr);
            fprintf(stderr, "  the 12 instructions leading up to the call:\n   ");
            for (u32 a = cpu->lr - 48; a < cpu->lr; a += 4)
                fprintf(stderr, " %08X", wiiu_read32(a));
            fprintf(stderr, "\n  registers:");
            for (int r = 0; r < 32; r++)
                fprintf(stderr, "%s r%d=%08X", (r % 8) ? "" : "\n   ", r, cpu->gpr[r]);
            fprintf(stderr, "\n");
            report_context(cpu, stderr);
            threads_report_all(stderr);
            hle_print_trail(stderr);
            runtime_exit(1);
        }
        if (cpu->exception) {
            u32 at = cpu->srr0;
            u32 word = wiiu_read32(at);
            bool trap = (cpu->exception & PPC_EXC_PROGRAM) && (cpu->program_exception & PPC_PROGRAM_TRAP);
            static u32 traps_skipped = 0;
            if (trap && traps_skipped < 64) {
                // A debug trap. With no debugger attached, step over it.
                if (traps_skipped++ < 8) {
                    fprintf(stderr, "[runtime] debug trap at %08X (instruction %08X), skipping it\n", at, word);
                    rpx_describe_address(&g_image, at, stderr);
                }
                cpu->exception = 0;
                cpu->program_exception = 0;
                cpu->msr = 0x00002000u;
                cpu->hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
                cpu->pc = at + 4;
                continue;
            }
            fprintf(stderr, "\n[runtime] CPU exception 0x%08X (program cause 0x%08X)\n"
                            "          at %08X, instruction %08X, lr=%08X steps=%llu\n",
                    cpu->exception, cpu->program_exception, at, word, cpu->lr,
                    (unsigned long long)steps);
            threads_describe_current(stderr);
            rpx_describe_address(&g_image, at, stderr);
            hle_print_trail(stderr);
            runtime_exit(1);
        }
        u64 prev_steps = steps;
        steps += 1 + tl_call_used;   // (+ the jumps calls made inside the game code ran: splat_call)
#ifndef NO_SPIN_DETECT
        u32 region = cpu->pc >> 8;
        if (region != spin_region) {
            spin_region = region;
            spin_count = idle_count = 0;
        } else if ((++spin_count & 7) == 0) {   // look every 8th time round (cheap)
            u64 h = 1469598103934665603ull ^ cpu->pc;   // FNV-1a over pc + registers
            for (int r = 0; r < 32; r++) h = (h ^ cpu->gpr[r]) * 1099511628211ull;
            bool seen = recent[0] == h || recent[1] == h || recent[2] == h || recent[3] == h;
            recent[recent_pos++ & 3] = h;
            idle_count = seen ? idle_count + 1 : 0;
            if (idle_count >= 2 || spin_count >= 256) {
                idle_count = spin_count = 0;
                if (threads_yield_if_spinning(cpu)) profiler_note_spin_yield();
            }
        }
#endif
        // (steps can go up by more than one: "passed a multiple of N")
        if ((steps >> 13) != (prev_steps >> 13)) {      // take turns with other threads, run alarms
            profiler_add_jumps(0x2000);
            threads_tick(cpu);
        }
        else if ((steps >> 10) != (prev_steps >> 10))   // a higher-priority thread woke up?
            threads_check_preempt(cpu);
        if ((steps >> 26) != (prev_steps >> 26)) {      // every ~67 million jumps
            static u32 last_pc = 0;
            static int same = 0;
            printf("[runtime] still running: %llu jumps, pc=%08X lr=%08X\n",
                   (unsigned long long)steps, cpu->pc, cpu->lr);
            threads_describe_current(stdout);
            same = (cpu->pc == last_pc) ? same + 1 : 0;
            last_pc = cpu->pc;
            if (same == 2) {   // stuck in the same place: explain where, once
                printf("[runtime] the game seems to be looping at %08X; details:\n", cpu->pc);
                fprintf(stdout, "  registers:");
                for (int r = 0; r < 32; r++)
                    fprintf(stdout, "%s r%d=%08X", (r % 8) ? "" : "\n   ", r, cpu->gpr[r]);
                fprintf(stdout, "\n  code around it:\n");
                dump_words(stdout, (cpu->pc & ~3u) - 0x60, (cpu->pc & ~3u) + 0x40);
                report_context(cpu, stdout);
                threads_report_all(stdout);
                hle_print_trail(stdout);
                fflush(stdout);
            }
        }
    }
    tl_cpu = saved_cpu;
    tl_stop = saved_stop;
    return true;
}

bool hle_call_guest(CPUState* cpu, u32 address) {
    u32 saved_lr = cpu->lr, saved_pc = cpu->pc;
    cpu->lr = HOST_RETURN_ADDRESS;
    cpu->pc = address;
    volatile int stop = 0;
    int big = hle_big_release_all();   // game code never runs holding the system-call lock
    bool ok = runtime_run(cpu, &stop);
    hle_big_reacquire(big);
    cpu->lr = saved_lr;
    cpu->pc = saved_pc;
    return ok;
}

#ifdef _WIN32
// Started without arguments: double-clicked as "Play Splatoon.exe" in the
// main folder (a hard link to this program, made by the build). Find the
// folders the way go.cmd / the old launcher did, work from
// SplatRecompRuntime (shader caches, debug_output), and write the log to
// splatoon_log.txt there -- keeping the last two runs' logs as
// splatoon_log_prev.txt / _prev2.txt, so a crash's log survives starting
// the game again.
static bool setup_double_clicked(char* rpx, char* game, char* saves) {
    char root[MAX_PATH], t[MAX_PATH + 64];
    GetModuleFileNameA(NULL, root, MAX_PATH);
    bool found = false;
    for (int up = 0; up < 6 && !found; up++) {
        char* s = strrchr(root, '\\');
        if (!s) break;
        *s = 0;
        snprintf(t, sizeof(t), "%s\\Gambit_v272_patched.rpx", root);
        found = GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES;
    }
    if (!found) {
        MessageBoxA(NULL, "Put \"Play Splatoon.exe\" in the folder with Gambit_v272_patched.rpx.", "Splatoon Recomp", MB_OK | MB_ICONWARNING);
        return false;
    }
    snprintf(rpx, MAX_PATH, "%s\\Gambit_v272_patched.rpx", root);
    snprintf(saves, MAX_PATH, "%s\\saves", root);
    game[0] = 0;   // (set by launcher.ini `game=` or go.cmd below; nothing is assumed)
    // the game folder: launcher.ini (game=...) or go.cmd (set "GAME=...")
    char line[1024];
    snprintf(t, sizeof(t), "%s\\launcher.ini", root);
    FILE* f = fopen(t, "rb");
    if (f) {
        while (fgets(line, sizeof(line), f))
            if (!_strnicmp(line, "game=", 5)) { line[strcspn(line, "\r\n")] = 0; if (line[5]) snprintf(game, MAX_PATH, "%s", line + 5); }
        fclose(f);
    } else {
        snprintf(t, sizeof(t), "%s\\go.cmd", root);
        f = fopen(t, "rb");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char* k = strstr(line, "set \"GAME=");
                if (!k) continue;
                char* v = k + 10;
                char* e = strchr(v, '"');
                if (e) *e = 0;
                if (*v) snprintf(game, MAX_PATH, "%s", v);
            }
            fclose(f);
        }
    }
    snprintf(t, sizeof(t), "%s\\SplatRecompRuntime", root);
    SetCurrentDirectoryA(t);
    // (no console: the log is the only place messages go; the Bot Lobby's copy keeps its own, bot_lobby.c)
    extern int splat_lobby_on(void);
    const char* lg = splat_lobby_on() ? "botlobby_log" : "splatoon_log";
    char l0[64], l1[64], l2[64];
    snprintf(l0, sizeof(l0), "%s.txt", lg); snprintf(l1, sizeof(l1), "%s_prev.txt", lg); snprintf(l2, sizeof(l2), "%s_prev2.txt", lg);
    MoveFileExA(l1, l2, MOVEFILE_REPLACE_EXISTING);
    MoveFileExA(l0, l1, MOVEFILE_REPLACE_EXISTING);
    freopen(l0, "a", stdout);
    freopen(l0, "a", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("[runtime] started from %s (game files: %s)\n", root, game);
    return true;
}
#endif

#ifdef _WIN32
// Started without arguments outside the project tree (NSR.exe from the zip, or
// the copy in an install folder): the launcher window (launcher\nsr.c, built in
// through runtime/nsr_wizard.c) installs and starts the game. Started as
// "Play Splatoon.exe" in the project (Gambit_v272_patched.rpx in a parent
// folder and no settings.ini next to the program) the old direct path is used.
int nsr_wizard(char* rpx, char* game, char* saves, char* inst);
int nsr_play_direct(char* rpx, char* game, char* saves, char* inst);   // ("NSR.exe --play": the launcher started this copy to play)
static bool in_project_tree(void) {
    char root[MAX_PATH], t[MAX_PATH + 64];
    GetModuleFileNameA(NULL, root, MAX_PATH);
    { char* s = strrchr(root, '\\'); if (s) { *s = 0; snprintf(t, sizeof(t), "%s\\settings.ini", root); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) return false; } }
    for (int up = 0; up < 6; up++) {
        snprintf(t, sizeof(t), "%s\\Gambit_v272_patched.rpx", root);
        if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) return true;
        char* s = strrchr(root, '\\');
        if (!s) break;
        *s = 0;
    }
    return false;
}
// After the launcher's Play: work from <install>\game (the shader cache is
// cache\ under it) and log to <install>\logs\session_1.txt (three sessions kept).
static void setup_from_install(const char* inst, const char* game_dir) {
    char t[MAX_PATH + 64], a[MAX_PATH + 64], b[MAX_PATH + 64];
    snprintf(t, sizeof(t), "%s\\game", inst); CreateDirectoryA(t, NULL); SetCurrentDirectoryA(t);
    CreateDirectoryA("cache", NULL);
    snprintf(t, sizeof(t), "%s\\logs", inst); CreateDirectoryA(t, NULL);
    snprintf(a, sizeof(a), "%s\\logs\\session_3.txt", inst); DeleteFileA(a);
    snprintf(a, sizeof(a), "%s\\logs\\session_2.txt", inst); snprintf(b, sizeof(b), "%s\\logs\\session_3.txt", inst); MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
    snprintf(a, sizeof(a), "%s\\logs\\session_1.txt", inst); snprintf(b, sizeof(b), "%s\\logs\\session_2.txt", inst); MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
    snprintf(t, sizeof(t), "%s\\logs\\session_1.txt", inst);
    freopen(t, "w", stdout);
    freopen(t, "a", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("[runtime] started by the NSR launcher: install %s (game files: %s)\n", inst, game_dir);
}
#endif

int main(int argc, char** argv) {
    const char* rpx_path = NULL;
    const char* game_dir = NULL;
    const char* saves_dir = "saves";
    const char* mlc_dir = NULL;
    bool no_window = false;
    u32 scan_addr[8];
    int scan_count = 0;
    const char* find_text[8];
    int text_count = 0;
    u32 dump_from[8], dump_to[8];
    int dump_count = 0;
    u32 caller_of[8];
    int caller_count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game_dir = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves_dir = argv[++i];
        else if (!strcmp(argv[i], "--mlc") && i + 1 < argc) mlc_dir = argv[++i];
        else if (!strcmp(argv[i], "--find-text") && i + 1 < argc && text_count < 8)
            find_text[text_count++] = argv[++i];
        else if (!strcmp(argv[i], "--scan") && i + 1 < argc && scan_count < 8) {
            scan_addr[scan_count] = (u32)strtoul(argv[++i], NULL, 16);
            rpx_debug_address(scan_addr[scan_count++]);
        }
        else if (!strcmp(argv[i], "--no-window")) no_window = true;
        else if (!strcmp(argv[i], "--dump-frames") && i + 1 < argc) g_render_dump_frames = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--strict-asserts") || !strcmp(argv[i], "--strict")) hle_set_strict_asserts(true);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) hle_set_trace(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc && g_peek_count < 8) g_peek[g_peek_count++] = (u32)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--callers") && i + 1 < argc && caller_count < 8)
            caller_of[caller_count++] = (u32)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc && dump_count < 8) {
            // "START-END" (hex): print the game's memory there (code or data)
            char* dash = NULL;
            dump_from[dump_count] = (u32)strtoul(argv[++i], &dash, 16) & ~3u;
            dump_to[dump_count] = (dash && *dash == '-') ? (u32)strtoul(dash + 1, NULL, 16) : dump_from[dump_count] + 0x100;
            if (dump_to[dump_count] - dump_from[dump_count] > 0x2000) dump_to[dump_count] = dump_from[dump_count] + 0x2000;
            dump_count++;
        }
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            // "ADDR" or "START-END" (hex)
            char* dash = NULL;
            u32 a = (u32)strtoul(argv[++i], &dash, 16);
            u32 e = (dash && *dash == '-') ? (u32)strtoul(dash + 1, NULL, 16) : 0;
            wiiu_mem_watch(a, e);
        }
        else if (!rpx_path) rpx_path = argv[i];
    }
#ifdef _WIN32
    static char dc_rpx[MAX_PATH], dc_game[MAX_PATH], dc_saves[MAX_PATH];
    bool play_arg = argc == 2 && !strcmp(argv[1], "--play");
    if (argc == 1 || play_arg) {
        if (play_arg) {   // the launcher (another NSR.exe) started the install folder's copy to play
            static char dc_inst[MAX_PATH];
            if (!nsr_play_direct(dc_rpx, dc_game, dc_saves, dc_inst)) return 2;
            setup_from_install(dc_inst, dc_game);
        } else if (in_project_tree()) {
            if (!setup_double_clicked(dc_rpx, dc_game, dc_saves)) return 2;
        } else {
            static char dc_inst[MAX_PATH];
            if (!nsr_wizard(dc_rpx, dc_game, dc_saves, dc_inst)) return 0;   // (window closed without Play)
            setup_from_install(dc_inst, dc_game);
        }
        rpx_path = dc_rpx;
        game_dir = dc_game;
        saves_dir = dc_saves;
    }
#endif
    if (g_render_dump_frames && (no_window || !game_dir)) g_render_headless = true;
    if (!rpx_path) {
        fprintf(stderr,
                "usage: %s <game.rpx> --game <folder with the game and its update>\n"
                "          [--saves <folder>] [--mlc <Cemu-style mlc01 folder>]\n", argv[0]);
        return 2;
    }

    // The log: buffered, written out every 0.2 s by a small thread (and at exit, and by the crash
    // handlers). Unbuffered, every line was a file write by the thread printing it -- the game's
    // main thread and the drawing thread among them: ~1% of the main thread, in bursts at the 10-s
    // reports. (stderr stays unbuffered: errors appear at once. SPLAT_LOG_UNBUFFERED=1: as before.)
    if (getenv("SPLAT_LOG_UNBUFFERED")) setvbuf(stdout, NULL, _IONBF, 0);
    else {
        setvbuf(stdout, NULL, _IOFBF, 1 << 16);
        static hthrd_t log_flusher;
        hthrd_create(&log_flusher, log_flush_thread, NULL, 64u << 10);
    }
#ifdef _WIN32
    g_log_handle = (HANDLE)_get_osfhandle(_fileno(stdout));
    AddVectoredExceptionHandler(1, on_stack_overflow);
#endif
    signal(SIGSEGV, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGILL, on_signal);
    signal(SIGABRT, on_signal);
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    signal(SIGINT, on_interrupt);
#endif
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_windows_exception);
#endif

    if (!wiiu_mem_init()) return 1;
    splat_file_fns_init();
#ifdef _WIN32
    // Windows normally wakes sleeping threads in 15.6 ms steps; the game
    // waits for 1/60 s (16.7 ms) every frame, so ask for 1 ms steps.
    timeBeginPeriod(1);
    {   // ...and keep them (and full speed) when Windows 11 decides nobody is
        // watching the window -- on another screen, covered, in the
        // background: it then ignores the 1 ms request and may slow the
        // program down (EcoQoS). Frames came late and the music ran dry.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
        PROCESS_POWER_THROTTLING_STATE pt;
        memset(&pt, 0, sizeof(pt));
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;   // (= throttle neither)
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt)))
            printf("[runtime] couldn't opt out of Windows' power throttling (error %lu)\n", GetLastError());
    }
#endif
    if (!rpx_load_into_memory(rpx_path, &g_image)) return 1;
    printf("[runtime] runtime v0.19.1 (v0.19 without the slow fallback path)\n");
    {   // float instructions: FPSCR status bits kept exactly only when asked (see cpu_fast.c)
        extern int splat_exact_fpscr;
        if (getenv("SPLAT_EXACT_FPSCR")) { splat_exact_fpscr = 1; printf("[runtime] float status bits kept exactly (SPLAT_EXACT_FPSCR)\n"); }
    }
    printf("[runtime] loaded %s: entry=%08X sda=%08X sda2=%08X imports=%u\n",
           rpx_path, g_image.entry_point, g_image.sda_base, g_image.sda2_base,
           g_image.import_count);
    rpx_print_sections(&g_image, stdout);
    printf("[rpx] %u named symbols\n", g_image.symbol_count);
    for (int d = 0; d < dump_count; d++) {
        printf("[dump] %08X-%08X:\n", dump_from[d], dump_to[d]);
        for (u32 a = dump_from[d]; a < dump_to[d]; a += 32) {
            printf("  %08X:", a);
            for (u32 k = 0; k < 32 && a + k < dump_to[d]; k += 4) printf(" %08X", wiiu_read32(a + k));
            printf("\n");
        }
    }
    if (scan_count || dump_count || caller_count || text_count) {   // debugging: search/print the code first
        for (int i = 0; i < text_count; i++) scan_find_text(&g_image, find_text[i]);
        for (int i = 0; i < scan_count; i++) scan_for_address(&g_image, scan_addr[i]);
        for (int i = 0; i < caller_count; i++) scan_callers(&g_image, caller_of[i]);
        fflush(stdout);
        if (!game_dir) _Exit(0);   // no --game: don't run it
    }

    static CPUState cpu;
    runtime_setup_cpu(&cpu);
    tl_cpu = &cpu;
    cpu.gpr[1] = WIIU_STACK_TOP - 0x20;
    cpu.gpr[2] = g_image.sda2_base;
    cpu.gpr[13] = g_image.sda_base;

    fs_setup(game_dir, saves_dir, mlc_dir);
    hle_init(&cpu, &g_image);
    threads_init_main(&cpu, WIIU_STACK_TOP);

    // The console calls __preinit_user(&mem1, &foreground, &mem2) before the
    // entry point so the program can set up its heaps.
    if (g_image.preinit_user) {
        u32 handles = hle_heap_alloc(16, 16);
        wiiu_write32(handles + 0, 0x4D454D31u);  // fake heap handles
        wiiu_write32(handles + 4, 0x46475244u);
        wiiu_write32(handles + 8, 0x4D454D32u);
        cpu.gpr[3] = handles + 0;
        cpu.gpr[4] = handles + 4;
        cpu.gpr[5] = handles + 8;
        printf("[runtime] running __preinit_user at %08X\n", g_image.preinit_user);
        if (!hle_call_guest(&cpu, g_image.preinit_user)) return 1;
    }

    // The window only opens for a real game (--game), not for test programs.
    for (u32 i = 0; i < g_image.section_count; i++)
        if (!strcmp(g_image.sections[i].name, ".text")) {
            g_text_start = g_image.sections[i].address;
            g_text_size = g_image.sections[i].size;
        }
    // SPLAT_DUMP_CODE_PTRS=file: every word in the game's data sections that
    // points at an instruction (vtables, callbacks, switch jump tables) --
    // places the game can jump into from data, made entry points of the
    // optimized game code by the next build (see CMakeLists.txt).
    if (getenv("SPLAT_DUMP_CODE_PTRS")) {
        FILE* f = fopen(getenv("SPLAT_DUMP_CODE_PTRS"), "w");
        u32 n = 0;
        for (u32 i = 0; f && i < g_image.section_count; i++) {
            const RpxSection* s = &g_image.sections[i];
            if (strcmp(s->name, ".rodata") && strcmp(s->name, ".data")) continue;
            for (u32 a = s->address; a + 4 <= s->address + s->size; a += 4) {
                u32 v = wiiu_read32(a);
                if (v - g_text_start < g_text_size && !(v & 3)) { fprintf(f, "%08X\n", v); n++; }
            }
        }
        if (f) fclose(f);
        printf("[runtime] %u code addresses found in the game's data written to %s\n", n, getenv("SPLAT_DUMP_CODE_PTRS"));
    }

    if (game_dir) {
        display_start(no_window);
        profiler_start(&g_image);
    }

    printf("[runtime] starting game at %08X\n", g_image.entry_point);
    printf("------------------------------------------------------------\n");
    cpu.gpr[3] = 0;   // argc
    cpu.gpr[4] = 0;   // argv
    cpu.lr = HOST_RETURN_ADDRESS;
    cpu.pc = g_image.entry_point;
    volatile int stop = 0;
    runtime_run(&cpu, &stop);
    profiler_report(stdout);
    printf("------------------------------------------------------------\n");
    printf("[runtime] game returned from its entry point\n");
    fflush(stdout);
    _Exit(0);
}
