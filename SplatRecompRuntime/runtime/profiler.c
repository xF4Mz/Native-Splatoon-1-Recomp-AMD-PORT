// A sampling profiler: about 1000 times a second it notes where the running
// game thread is (which game function, or which system call), or that no
// game thread is running at all. Every 20 seconds, and when the game exits
// (window closed or Ctrl+C), it prints where the time went, which tells us
// what to speed up next.
#include "profiler.h"
#include "guest_threads.h"
#include "hle.h"
#include "scan.h"
#include "wiiu_mem.h"
#include "host_thread.h"
#include "gx2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 g_text_start = 0, g_text_end = 0;
static u32* g_code_hits = NULL;     // one counter per instruction of .text
static u32* g_main_hits = NULL;     // the same, main thread only (the one that sets the frame rate)
static volatile u64 g_main_samples = 0;
static volatile u64 g_samples = 0, g_idle = 0, g_in_system = 0, g_other = 0;
static volatile bool g_running = false;
static u64 g_start_ticks = 0;
static u32 g_start_frames = 0;
static volatile u64 g_jumps = 0, g_start_jumps = 0;
static hmtx_t g_prof_lock;

#define REPORT_EVERY_MS 20000

static volatile u64 g_spin_yields = 0, g_start_spin_yields = 0;
void profiler_add_jumps(u32 n) { g_jumps += n; }
void profiler_note_spin_yield(void) { g_spin_yields++; }

#define MAX_SYS 512
static struct { u32 address; u64 hits; } g_sys[MAX_SYS];
static int g_sys_count = 0;

#define MAX_NAMED 64
static struct { const char* name; u64 hits; } g_threads[MAX_NAMED];
static int g_thread_names = 0;

static void count_thread(const char* name) {
    for (int i = 0; i < g_thread_names; i++)
        if (g_threads[i].name == name) { g_threads[i].hits++; return; }
    if (g_thread_names < MAX_NAMED) {
        g_threads[g_thread_names].name = name;
        g_threads[g_thread_names++].hits = 1;
    }
}

static void count_system(u32 address) {
    for (int i = 0; i < g_sys_count; i++)
        if (g_sys[i].address == address) { g_sys[i].hits++; return; }
    if (g_sys_count < MAX_SYS) {
        g_sys[g_sys_count].address = address;
        g_sys[g_sys_count++].hits = 1;
    }
}

// The main thread's call chains: where in the game's own logic its time
// goes (pc, then the return addresses found by walking its stack frames).
#define CHAIN_DEPTH 24
#define MAX_CHAINS 96
typedef struct { u32 addr[CHAIN_DEPTH]; int n; u64 hits; } Chain;
static Chain g_chains[MAX_CHAINS];
static int g_chain_count = 0;
static u64 g_chain_samples = 0;
static bool g_dumped_code = false;
static u32 g_reports = 0;

static void sample_chain(CPUState* cpu) {
    Chain c;
    memset(&c, 0, sizeof(c));
    c.addr[c.n++] = cpu->pc;
    u32 sp = cpu->gpr[1];
    while (c.n < CHAIN_DEPTH) {
        if (!wiiu_mem_ptr(sp, 8)) break;
        u32 next = wiiu_read32(sp);
        if (next <= sp || next - sp > 0x100000u || !wiiu_mem_ptr(next, 8)) break;
        u32 lr = wiiu_read32(next + 4);
        if (lr < g_text_start || lr >= g_text_end) break;
        c.addr[c.n++] = lr;
        sp = next;
    }
    // Group by the chain below the current function (pc varies within it).
    g_chain_samples++;
    for (int i = 0; i < g_chain_count; i++) {
        Chain* k = &g_chains[i];
        if (k->n == c.n && !memcmp(&k->addr[1], &c.addr[1], (size_t)(c.n - 1) * 4) &&
            scan_function_start(k->addr[0], g_text_start) == scan_function_start(c.addr[0], g_text_start)) {
            k->hits++;
            return;
        }
    }
    if (g_chain_count < MAX_CHAINS) { c.hits = 1; g_chains[g_chain_count++] = c; }
}

static void sample(void) {
    const char* name = NULL;
    CPUState* cpu = threads_sample(&name);
    g_samples++;
    if (!cpu) { g_idle++; return; }
    count_thread(name);
    if (name && !strcmp(name, "main") && cpu->pc >= g_text_start && cpu->pc < g_text_end)
        sample_chain(cpu);
    u32 pc = cpu->pc;
    if (pc >= g_text_start && pc < g_text_end) {
        g_code_hits[(pc - g_text_start) >> 2]++;
        if (g_main_hits && name && !strcmp(name, "main")) { g_main_hits[(pc - g_text_start) >> 2]++; g_main_samples++; }
    } else if (pc >= 0xC0000000u) {
        g_in_system++;
        count_system(pc);
    } else {
        g_other++;
    }
}


// --- Where the main thread's PC time goes, by PC code address (Windows) ---
// Every few milliseconds the main game thread's PC thread is paused for an
// instant and its instruction address noted. The report (debug_output/
// host_profile.txt) lists the busiest addresses as offsets into
// splatoon_runner.exe; the link map (splatoon_runner.map) names them: game
// code, the CPU helpers, system calls or the run loop. This shows what to
// speed up next.
#ifdef _WIN32
// [0] the game's main thread, [1] the GPU command thread
static HANDLE g_host_thread[2];
static uintptr_t g_mod_base;
#define HOST_SLOTS 32768
// SPLAT_HOST_PROFILE_ALL=1: sample every millisecond and keep the whole run
// (for finding the hot machine instructions inside game-code pieces)
static int g_host_all = 0;
static bool g_host_final = false;
static struct { u32 rva; u32 hits; } g_host[2][HOST_SLOTS];
static u64 g_host_samples[2], g_host_outside[2];
// Outside the program (Windows, the graphics driver, waiting) by address
// too: the report names them by DLL and nearest exported function
#define OUT_SLOTS 4096
static struct { uintptr_t ip; u32 hits; } g_out[2][OUT_SLOTS];
static void note_outside(int t, uintptr_t ip) {
    u32 k = (u32)(((u64)ip * 0x9E3779B97F4A7C15ull) >> 52);
    for (u32 n = 0; n < 32; n++, k = (k + 1) & (OUT_SLOTS - 1)) {
        if (g_out[t][k].ip == ip) { g_out[t][k].hits++; return; }
        if (!g_out[t][k].hits) { g_out[t][k].ip = ip; g_out[t][k].hits = 1; return; }
    }
}
// "dll!function+offset" (the nearest exported function at or before it) and the DLL's name
static void name_outside(uintptr_t ip, char* out, size_t size, char* dll, size_t dll_size) {
    HMODULE m = NULL;
    snprintf(out, size, "%p", (void*)ip);
    snprintf(dll, dll_size, "?");
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &m) || !m)
        return;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(m, path, sizeof(path));
    const char* base = path;
    for (DWORD i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
    snprintf(dll, dll_size, "%s", n ? base : "?");
    const u8* b = (const u8*)m;
    u32 rva = (u32)(ip - (uintptr_t)m);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    const IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const char* best = NULL;
    u32 best_rva = 0;
    if (dir->VirtualAddress && dir->Size) {
        const IMAGE_EXPORT_DIRECTORY* ex = (const IMAGE_EXPORT_DIRECTORY*)(b + dir->VirtualAddress);
        const u32* fns = (const u32*)(b + ex->AddressOfFunctions);
        const u32* names = (const u32*)(b + ex->AddressOfNames);
        const u16* ords = (const u16*)(b + ex->AddressOfNameOrdinals);
        for (u32 i = 0; i < ex->NumberOfNames; i++) {
            u32 f = ords[i] < ex->NumberOfFunctions ? fns[ords[i]] : 0;
            if (f >= dir->VirtualAddress && f < dir->VirtualAddress + dir->Size) continue;   // (forwarded elsewhere)
            if (f && f <= rva && f >= best_rva) { best_rva = f; best = (const char*)(b + names[i]); }
        }
    }
    if (best) snprintf(out, size, "%s!%s+0x%X", dll, best, rva - best_rva);
    else snprintf(out, size, "%s+0x%X", dll, rva);
}
static int by_out_hits(const void* a, const void* b) {
    u32 x = ((const u32*)a)[2], y = ((const u32*)b)[2];
    return x < y ? 1 : x > y ? -1 : 0;
}
// The outside samples of thread t: by DLL and the busiest addresses (file),
// the top DLLs in one log line
static void outside_report(FILE* f, int t, const char* tname) {
    struct { uintptr_t ip; u32 hits; u32 pad; } *ol = malloc(sizeof(*ol) * OUT_SLOTS);
    if (!ol) return;
    u32 n = 0;
    for (u32 i = 0; i < OUT_SLOTS; i++)
        if (g_out[t][i].hits) { ol[n].ip = g_out[t][i].ip; ol[n].hits = g_out[t][i].hits; n++; }
    // (sorted by hits: the u32 after the 8-byte address)
    qsort(ol, n, sizeof(*ol), by_out_hits);
    struct { char name[64]; u32 hits; } dlls[48];
    u32 nd = 0;
    char where[160], dll[64];
    if (f) fprintf(f, "-- %s outside the program, busiest addresses:\n", tname);
    for (u32 i = 0; i < n; i++) {
        name_outside(ol[i].ip, where, sizeof(where), dll, sizeof(dll));
        if (f && i < 80) fprintf(f, "   %6u  at %s\n", ol[i].hits, where);   // (3 fields: not an address line for sym.sh)
        u32 d = 0;
        while (d < nd && strcmp(dlls[d].name, dll)) d++;
        if (d == nd && nd < 48) { snprintf(dlls[nd].name, sizeof(dlls[nd].name), "%s", dll); dlls[nd].hits = 0; nd++; }
        if (d < nd) dlls[d].hits += ol[i].hits;
    }
    for (u32 a = 0; a < nd; a++)   // (by hits)
        for (u32 b2 = a + 1; b2 < nd; b2++)
            if (dlls[b2].hits > dlls[a].hits) { char tn[64]; u32 th = dlls[a].hits; memcpy(tn, dlls[a].name, 64); memcpy(dlls[a].name, dlls[b2].name, 64); dlls[a].hits = dlls[b2].hits; memcpy(dlls[b2].name, tn, 64); dlls[b2].hits = th; }
    double all = (double)(g_host_samples[t] ? g_host_samples[t] : 1);
    if (f) {
        fprintf(f, "-- %s outside the program, by DLL (%% of all its samples):\n", tname);
        for (u32 d = 0; d < nd; d++) fprintf(f, "   %5.1f%%  in %s\n", 100.0 * dlls[d].hits / all, dlls[d].name);
    }
    if (nd) {
        printf("[profile] %s outside the program:", tname);
        for (u32 d = 0; d < nd && d < 5; d++) printf("%s %s %.1f%%", d ? "," : "", dlls[d].name, 100.0 * dlls[d].hits / all);
        printf("\n");
    }
    memset(g_out[t], 0, sizeof(g_out[t]));
    free(ol);
}
// (outside samples: the first return address into the program found on the stack -- which of our functions
// called the system / driver code the time went to; host_profile.txt "callers of outside time")
static struct { u32 rva, hits; } g_caller[2][HOST_SLOTS];
static void note_caller(int t, u32 rva) {
    u32 k = (rva * 2654435761u) >> 17;
    for (u32 n = 0; n < 64; n++, k = (k + 1) & (HOST_SLOTS - 1)) {
        if (g_caller[t][k].rva == rva) { g_caller[t][k].hits++; return; }
        if (!g_caller[t][k].hits) { g_caller[t][k].rva = rva; g_caller[t][k].hits = 1; return; }
    }
}
static void host_sample_one(int t) {
    HANDLE h = g_host_thread[t];
    if (!h) return;
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT cx;
    memset(&cx, 0, sizeof(cx));
    cx.ContextFlags = CONTEXT_FULL;
    BOOL ok = GetThreadContext(h, &cx);
#if defined(_M_X64) || defined(__x86_64__)
    uintptr_t ip = ok ? (uintptr_t)cx.Rip : 0;
    uintptr_t caller = 0;
    if (ok && (ip < g_mod_base || ip - g_mod_base >= 0x40000000u)) {   // (unwound properly, while the thread is still stopped)
        CONTEXT u = cx;
        for (int depth = 0; depth < 12; depth++) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &base, NULL);
            if (fe) {
                PVOID hd = NULL;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, fe, &u, &hd, &est, NULL);
            } else {   // (a leaf: the return address is on top of the stack)
                if (!u.Rsp) break;
                u.Rip = *(const DWORD64*)u.Rsp;
                u.Rsp += 8;
            }
            if (!u.Rip) break;
            if (u.Rip >= g_mod_base && u.Rip - g_mod_base < 0x40000000u) { caller = (uintptr_t)u.Rip; break; }
        }
    }
#else
    uintptr_t ip = ok ? (uintptr_t)cx.Eip : 0;
    uintptr_t caller = 0;
#endif
    ResumeThread(h);
    if (!ok) return;
    g_host_samples[t]++;
    if (ip < g_mod_base || ip - g_mod_base >= 0x40000000u) {
        g_host_outside[t]++;
        note_outside(t, ip);
        if (caller) note_caller(t, (u32)(caller - g_mod_base));
        return;
    }
    u32 rva = (u32)(ip - g_mod_base);
    u32 k = (rva * 2654435761u) >> 17;
    for (u32 n = 0; n < 64; n++, k = (k + 1) & (HOST_SLOTS - 1)) {
        if (g_host[t][k].rva == rva) { g_host[t][k].hits++; return; }
        if (!g_host[t][k].hits) { g_host[t][k].rva = rva; g_host[t][k].hits = 1; return; }
    }
}
static void host_sample(void) { host_sample_one(0); host_sample_one(1); }
static int by_host_hits(const void* a, const void* b) {
    u32 x = ((const u32*)a)[1], y = ((const u32*)b)[1];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void host_report(void) {
    if (!g_host_samples[0] && !g_host_samples[1]) return;
    if (g_host_all && !g_host_final) return;   // (SPLAT_HOST_PROFILE_ALL: one report for the whole run, at the end)
    u32 (*list)[2] = (u32(*)[2])malloc(sizeof(u32) * 2 * HOST_SLOTS);
    if (!list) return;
    CreateDirectoryA("debug_output", NULL);
    FILE* f = fopen("debug_output/host_profile.txt", "w");
    static const char* names[2] = { "game main thread", "GPU command thread" };
    for (int t = 0; t < 2; t++) {
        u32 n = 0;
        for (u32 i = 0; i < HOST_SLOTS; i++)
            if (g_host[t][i].hits) { list[n][0] = g_host[t][i].rva; list[n][1] = g_host[t][i].hits; n++; }
        qsort(list, n, sizeof(list[0]), by_host_hits);
        if (f) {
            fprintf(f, "== %s: %llu samples (outside splatoon_runner.exe: %llu), exe base %p\n", names[t],
                    (unsigned long long)g_host_samples[t], (unsigned long long)g_host_outside[t], (void*)g_mod_base);
            for (u32 i = 0; i < n && i < (g_host_all ? HOST_SLOTS : 2500u); i++) fprintf(f, "%08X %u\n", list[i][0], list[i][1]);
        }
        if (g_host_samples[t])
            printf("[profile] %s: %llu PC samples, %.1f%% outside the game program (system, driver, waiting)\n", names[t],
                   (unsigned long long)g_host_samples[t], 100.0 * (double)g_host_outside[t] / (double)g_host_samples[t]);
        if (g_host_outside[t]) outside_report(f, t, names[t]);
        if (f) {
            u32 m = 0;
            for (u32 i = 0; i < HOST_SLOTS; i++)
                if (g_caller[t][i].hits) { list[m][0] = g_caller[t][i].rva; list[m][1] = g_caller[t][i].hits; m++; }
            qsort(list, m, sizeof(list[0]), by_host_hits);
            fprintf(f, "== %s callers of outside time: %llu samples outside, exe base %p\n", names[t], (unsigned long long)g_host_outside[t], (void*)g_mod_base);
            for (u32 i = 0; i < m && i < 400u; i++) fprintf(f, "%08X %u\n", list[i][0], list[i][1]);
        }
        memset(g_caller[t], 0, sizeof(g_caller[t]));
        memset(g_host[t], 0, sizeof(g_host[t]));
        g_host_samples[t] = g_host_outside[t] = 0;
    }
    if (f) fclose(f);
    free(list);
}
static void host_register(int t) {
    if (getenv("SPLAT_NO_HOST_PROFILE")) return;
    HANDLE h = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (!g_mod_base) g_mod_base = (uintptr_t)GetModuleHandleA(NULL);
    g_host_thread[t] = h;
}
void profiler_register_gpu_thread(void) { host_register(1); }
#else
static void host_sample(void) {}
static void host_report(void) {}
void profiler_register_gpu_thread(void) {}
#endif

static void report_locked(FILE* out, bool final);

static int sampler_thread(void* arg) {
    (void)arg;
    u32 n = 0;
    while (g_running) {
        hsleep_ns(1000000);
        hmtx_lock(&g_prof_lock);
        if (g_running) {
            sample();
            if ((n & 3) == 0 || g_host_all) host_sample();
            if (++n >= REPORT_EVERY_MS) { n = 0; report_locked(stdout, false); }
        }
        hmtx_unlock(&g_prof_lock);
    }
    return 0;
}

void profiler_start(const RpxImage* image) {
    g_host_all = getenv("SPLAT_HOST_PROFILE_ALL") != NULL;
    for (u32 i = 0; i < image->section_count; i++) {
        const RpxSection* s = &image->sections[i];
        if (!strcmp(s->name, ".text")) {
            g_text_start = s->address;
            g_text_end = s->address + s->size;
        }
    }
    if (g_text_end <= g_text_start) return;
    g_code_hits = (u32*)calloc((g_text_end - g_text_start) / 4 + 1, sizeof(u32));
    if (!g_code_hits) return;
    g_main_hits = (u32*)calloc((g_text_end - g_text_start) / 4 + 1, sizeof(u32));
    hmtx_init(&g_prof_lock, false);
#ifdef _WIN32
    host_register(0);   // (called on the PC thread that runs the game's main thread)
#endif
    g_running = true;
    g_start_ticks = wiiu_time_ticks();
    static hthrd_t thread;
    hthrd_create(&thread, sampler_thread, NULL, 1u << 20);
}

typedef struct { u32 start; u64 hits; u32 hot_pc; u32 hot_hits; } FnHits;

static int by_hits_fn(const void* a, const void* b) {
    u64 x = ((const FnHits*)a)->hits, y = ((const FnHits*)b)->hits;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report_functions(FILE* out, const u32* hits, u64 total, const char* title, int count) {
    u32 n = (g_text_end - g_text_start) / 4;
    FnHits* fns = (FnHits*)calloc(4096, sizeof(FnHits));
    int fn_count = 0;
    for (u32 i = 0; i < n && fns; i++) {
        if (!hits[i]) continue;
        u32 pc = g_text_start + i * 4;
        u32 start = scan_function_start(pc, g_text_start);
        int k = 0;
        while (k < fn_count && fns[k].start != start) k++;
        if (k == fn_count) {
            if (fn_count == 4096) continue;
            fns[fn_count].start = start;
            fn_count++;
        }
        fns[k].hits += hits[i];
        if (hits[i] > fns[k].hot_hits) { fns[k].hot_hits = hits[i]; fns[k].hot_pc = pc; }
    }
    if (!fns) return;
    qsort(fns, (size_t)fn_count, sizeof(FnHits), by_hits_fn);
    fprintf(out, "  %s:\n", title);
    for (int i = 0; i < fn_count && i < count; i++) {
        if (fns[i].hits * 1000 < total) break;
        fprintf(out, "    %08X  %5.1f%%   hot %08X", fns[i].start, 100.0 * fns[i].hits / total, fns[i].hot_pc);
        if (i < 3) {   // the code around the hottest spot, for the top few
            fprintf(out, "  code:");
            for (u32 a = fns[i].hot_pc - 16; a <= fns[i].hot_pc + 16; a += 4)
                fprintf(out, " %08X", wiiu_read32(a));
        }
        fprintf(out, "\n");
    }
    free(fns);
}

void profiler_report(FILE* out) {
    if (!g_running) return;
    hmtx_lock(&g_prof_lock);
    g_host_final = true;
    if (g_running) report_locked(out, true);
    g_running = false;
    hmtx_unlock(&g_prof_lock);
}
void profiler_report_now(FILE* out) {
    if (!g_running) return;
    hmtx_lock(&g_prof_lock);
    if (g_running) report_locked(out, false);
    hmtx_unlock(&g_prof_lock);
}

// Busiest game-code files (16 KB of game code each, named by start
// address), counted over the whole run. Written to
// debug_output/hot_chunks.txt: which files are worth building with full
// optimization (runtime/fast_chunks.txt).
#define CHUNK_BASE 0x02000020u
#define MAX_CHUNKS 1024
static u64 g_chunk_hits[MAX_CHUNKS];
static void chunk_report(void) {
    u32 n = (g_text_end - g_text_start) / 4;
    u64 total = 0;
    for (u32 i = 0; i < n; i++) {
        if (!g_code_hits[i]) continue;
        u32 pc = g_text_start + i * 4;
        if (pc < CHUNK_BASE) continue;
        u32 k = (pc - CHUNK_BASE) / 0x4000u;
        if (k < MAX_CHUNKS) g_chunk_hits[k] += g_code_hits[i];
    }
    static u32 order[MAX_CHUNKS];
    u32 count = 0;
    for (u32 k = 0; k < MAX_CHUNKS; k++)
        if (g_chunk_hits[k]) { order[count++] = k; total += g_chunk_hits[k]; }
    if (!total) return;
    for (u32 i = 1; i < count; i++) {   // (insertion sort, busiest first)
        u32 k = order[i], j = i;
        while (j > 0 && g_chunk_hits[order[j - 1]] < g_chunk_hits[k]) { order[j] = order[j - 1]; j--; }
        order[j] = k;
    }
#ifdef _WIN32
    CreateDirectoryA("debug_output", NULL);
#endif
    FILE* f = fopen("debug_output/hot_chunks.txt", "w");
    if (!f) return;
    fprintf(f, "# game-code file start, %% of game-code time, running total (whole run)\n");
    u64 run = 0;
    for (u32 i = 0; i < count; i++) {
        run += g_chunk_hits[order[i]];
        fprintf(f, "%08X %6.2f%% %6.2f%%\n", CHUNK_BASE + order[i] * 0x4000u, 100.0 * g_chunk_hits[order[i]] / total,
                100.0 * run / total);
    }
    fclose(f);
}

// The game came into one of the optimized game-code files where its pieces
// have no label (tools/chunk_split.c), so that file's unsplit (slower) code
// ran. Counted, and the addresses noted: debug_output/split_entries.txt,
// which the next build reads (CMakeLists.txt) and makes entry points.
#define FB_SLOTS 1024
static volatile uint64_t g_fb[FB_SLOTS];   // address << 32 | times
static volatile uint64_t g_fb_total, g_fb_new;
static uint64_t g_start_fallbacks;
void splat_split_fallback(u32 pc) {
    hatomic_add64(&g_fb_total, 1);
    u32 h = (pc >> 2) * 2654435761u;
    for (u32 i = 0; i < 32; i++) {
        volatile uint64_t* e = &g_fb[(h + i) & (FB_SLOTS - 1)];
        uint64_t v = *e;
        if (v >> 32 == pc) { hatomic_add64(e, 1); return; }
        if (v == 0 && hatomic_cas64(e, 0, ((uint64_t)pc << 32) | 1)) { hatomic_add64(&g_fb_new, 1); return; }
        if (*e >> 32 == pc) { hatomic_add64(e, 1); return; }
    }
}
static void fallbacks_report(FILE* out) {
    uint64_t total = g_fb_total;
    if (total == g_start_fallbacks) return;
    fprintf(out, "[profile] optimized game code entered off its entry points %llu times", (unsigned long long)(total - g_start_fallbacks));
    // the busiest addresses
    u32 best[4] = { 0 };
    uint64_t bn[4] = { 0 };
    for (u32 i = 0; i < FB_SLOTS; i++) {
        uint64_t v = g_fb[i];
        if (!v) continue;
        uint64_t n = v & 0xFFFFFFFFu;
        for (int k = 0; k < 4; k++)
            if (n > bn[k]) {
                for (int m = 3; m > k; m--) { bn[m] = bn[m - 1]; best[m] = best[m - 1]; }
                bn[k] = n;
                best[k] = (u32)(v >> 32);
                break;
            }
    }
    for (int k = 0; k < 4 && bn[k]; k++) fprintf(out, "%s%08X x%llu", k ? ", " : " (", best[k], (unsigned long long)bn[k]);
    fprintf(out, "%s\n", bn[0] ? ")" : "");
    // noted for the next build (only when there are new ones: the build
    // splits the game code again when this file changes)
    // (added to what earlier runs noted: those are entry points by now, so
    // they don't show up here again)
    if (g_fb_new) {
        g_fb_new = 0;
        static u32 known[8192];
        u32 nk = 0;
        FILE* f = fopen("debug_output/split_entries.txt", "r");
        if (f) {
            char line[64];
            while (nk < 8192 && fgets(line, sizeof(line), f)) {
                unsigned a;
                if (line[0] != '#' && sscanf(line, "%x", &a) == 1) known[nk++] = a;
            }
            fclose(f);
        }
        for (u32 i = 0; i < FB_SLOTS && nk < 8192; i++) {
            u32 a = (u32)(g_fb[i] >> 32);
            if (!g_fb[i]) continue;
            bool have = false;
            for (u32 k = 0; k < nk && !have; k++) have = known[k] == a;
            if (!have) known[nk++] = a;
        }
        f = fopen("debug_output/split_entries.txt", "w");
        if (f) {
            fprintf(f, "# where the game came into the optimized game code off its entry points (written by the runtime;\n"
                       "# the next build makes these entry points too)\n");
            for (u32 k = 0; k < nk; k++) fprintf(f, "%08X\n", known[k]);
            fclose(f);
        }
    }
}

static void report_locked(FILE* out, bool final) {
    if (!g_samples) return;
    u64 total = g_samples;
    u64 code = total - g_idle - g_in_system - g_other;
    u64 now = wiiu_time_ticks();
    double seconds = (double)(now - g_start_ticks) / WIIU_TIMER_HZ;
    u32 frames = gx2_frames_presented() - g_start_frames;
    u64 jumps = g_jumps - g_start_jumps;
    fprintf(out, "\n[profile] %s: %u frames in %.1f seconds = %.1f fps, %.1f million jumps/s, %llu busy-wait handoffs\n",
            final ? "since the last report" : "this period", frames, seconds,
            seconds > 0 ? frames / seconds : 0.0, seconds > 0 ? jumps / seconds / 1e6 : 0.0,
            (unsigned long long)(g_spin_yields - g_start_spin_yields));
    fallbacks_report(out);
    fprintf(out, "[profile] where the time went (%llu samples, about 1 per ms):\n", (unsigned long long)total);
    fprintf(out, "  game code      %5.1f%%\n", 100.0 * code / total);
    fprintf(out, "  system calls   %5.1f%%\n", 100.0 * g_in_system / total);
    fprintf(out, "  nothing to run %5.1f%%  (all game threads waiting or sleeping)\n", 100.0 * g_idle / total);
    if (g_other) fprintf(out, "  elsewhere      %5.1f%%\n", 100.0 * g_other / total);

    fprintf(out, "  by thread:\n");
    for (int i = 0; i < g_thread_names; i++)
        if (g_threads[i].hits * 200 >= total)
            fprintf(out, "    %-28s %5.1f%%\n", g_threads[i].name, 100.0 * g_threads[i].hits / total);

    report_functions(out, g_code_hits, total, "busiest game functions (% of all time; hottest instruction)", 20);
    if (g_main_hits && g_main_samples)
        report_functions(out, g_main_hits, g_main_samples, "main thread's busiest functions (% of main's time)", 25);

    hle_report_calls(out, seconds);

    // Main thread call chains, most common first.
    if (g_chain_samples) {
        fprintf(out, "  main thread: most common call chains (newest call first; [function start])\n");
        for (int pass = 0; pass < 4; pass++) {
            int best = -1;
            for (int i = 0; i < g_chain_count; i++)
                if (g_chains[i].hits && (best < 0 || g_chains[i].hits > g_chains[best].hits)) best = i;
            if (best < 0) break;
            Chain* k = &g_chains[best];
            fprintf(out, "    %4.1f%% of main's time:", 100.0 * k->hits / g_chain_samples);
            for (int j = 0; j < k->n; j++)
                fprintf(out, " %08X[%08X]", k->addr[j], scan_function_start(k->addr[j], g_text_start));
            fprintf(out, "\n");
            // Once, ~40 s in: the code of each function along the top chain
            // (start of function up to just past the call), to read what
            // the game checks every frame while it waits.
            if (pass == 0 && !g_dumped_code && ++g_reports >= 2) {
                g_dumped_code = true;
                fprintf(out, "  [code] functions along that chain:\n");
                for (int j = 0; j < k->n; j++) {
                    u32 start = scan_function_start(k->addr[j], g_text_start);
                    u32 from = k->addr[j] - start > 0x400 ? k->addr[j] - 0x400 : start;
                    fprintf(out, "  [code] %08X (function %08X):", from, start);
                    for (u32 a = from, col = 0; a <= k->addr[j] + 0x20; a += 4, col++) {
                        if (col % 8 == 0) fprintf(out, "\n  [code]   %08X:", a);
                        fprintf(out, " %08X", wiiu_read32(a));
                    }
                    fprintf(out, "\n");
                }
            }
            k->hits = 0;
        }
    }
    fprintf(out, "  what each thread is doing now:\n");
    threads_report_brief(out);

    // Busiest system calls.
    for (int pass = 0; pass < 10; pass++) {
        int best = -1;
        for (int i = 0; i < g_sys_count; i++)
            if (g_sys[i].hits && (best < 0 || g_sys[i].hits > g_sys[best].hits)) best = i;
        if (best < 0 || g_sys[best].hits * 500 < total) break;
        if (pass == 0) fprintf(out, "  busiest system calls:\n");
        const char* name = hle_name_at(g_sys[best].address);
        fprintf(out, "    %-40s %5.1f%%\n", name ? name : "?", 100.0 * g_sys[best].hits / total);
        g_sys[best].hits = 0;
    }
    fflush(out);

    // Start the next period from zero.
    host_report();
    chunk_report();
    memset(g_code_hits, 0, (size_t)((g_text_end - g_text_start) / 4 + 1) * sizeof(u32));
    if (g_main_hits) memset(g_main_hits, 0, (size_t)((g_text_end - g_text_start) / 4 + 1) * sizeof(u32));
    g_main_samples = 0;
    g_samples = g_idle = g_in_system = g_other = 0;
    g_sys_count = 0;
    g_thread_names = 0;
    g_chain_count = 0;
    g_chain_samples = 0;
    g_start_ticks = now;
    g_start_frames = gx2_frames_presented();
    g_start_jumps = g_jumps;
    g_start_fallbacks = g_fb_total;
    g_start_spin_yields = g_spin_yields;
}
