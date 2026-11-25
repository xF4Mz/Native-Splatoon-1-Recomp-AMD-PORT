// (v0.13.3)
#include "wiiu_mem.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#endif

typedef struct {
    u32 base;
    u32 size;
    u8* host;
} Region;

static Region g_regions[] = {
    { WIIU_CODE_BASE, WIIU_CODE_SIZE, NULL },
    { WIIU_DATA_BASE, WIIU_DATA_SIZE, NULL },
    { WIIU_LOAD_BASE, WIIU_LOAD_SIZE, NULL },
    { WIIU_FG_BASE,   WIIU_FG_SIZE,   NULL },
    { WIIU_MEM1_BASE, WIIU_MEM1_SIZE, NULL },
    { WIIU_OVERLAY_BASE, WIIU_OVERLAY_SIZE, NULL },
};
#define REGION_COUNT (sizeof(g_regions) / sizeof(g_regions[0]))

// Reserve address space and let the OS hand out real memory only for the
// pages the game actually touches.
static u8* reserve_zeroed(u32 size) {
#ifdef _WIN32
    return (u8*)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : (u8*)p;
#endif
}

static void release(u8* p, u32 size) {
    if (!p) return;
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

// The whole 4 GB guest address space as one block of PC memory, so guest
// address A is simply g_space + A (see fast_cpu.h). The OS only hands out
// real memory for pages the game touches. The extra 64 KB lets an 8-byte
// access at the very top of the address space stay inside the block.
#define SPACE_SIZE (0x100000000ull + 0x10000ull)
static u8* g_space = NULL;
static bool g_force_slow = false;
u8* wiiu_fastmem = NULL;

static u8* reserve_space(void) {
#ifdef _WIN32
    // The whole 4 GB used to be committed (so any address read as zero);
    // now only the console's memory regions are, plus the 64 KB wrap at the
    // top: ~1.8 GB of commit charge instead of 4 GB. SPLAT_MEM_COMMIT_ALL=1: as before.
    const char* all = getenv("SPLAT_MEM_COMMIT_ALL");
    if (all && atoi(all)) return (u8*)VirtualAlloc(NULL, (SIZE_T)SPACE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    u8* p = (u8*)VirtualAlloc(NULL, (SIZE_T)SPACE_SIZE, MEM_RESERVE, PAGE_READWRITE);
    if (!p) return NULL;
    bool ok = true;
    for (size_t i = 0; i < REGION_COUNT && ok; i++)
        ok = VirtualAlloc(p + g_regions[i].base, g_regions[i].size, MEM_COMMIT, PAGE_READWRITE) != NULL;
    if (ok) ok = VirtualAlloc(p + 0xFFFF0000ull, 0x20000, MEM_COMMIT, PAGE_READWRITE) != NULL;
    if (ok) return p;
    VirtualFree(p, 0, MEM_RELEASE);
    fprintf(stderr, "[mem] couldn't commit the game's memory regions; committing the whole 4 GB\n");
    return (u8*)VirtualAlloc(NULL, (SIZE_T)SPACE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, (size_t)SPACE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : (u8*)p;
#endif
}

#ifdef _WIN32
// A read or write to a page of the 4 GB block outside the console's regions
// (the game reads through null pointers now and then: on the console those
// pages are zero): commit that page and carry on, as if the whole block had
// been committed from the start. Only the commit charge changes.
static volatile long g_stray_pages;
static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS* x) {
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_space || x->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    u8* at = (u8*)x->ExceptionRecord->ExceptionInformation[1];
    if (at < g_space || at >= g_space + SPACE_SIZE) return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualAlloc((void*)((uintptr_t)at & ~(uintptr_t)0xFFFF), 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    long n = _InterlockedIncrement(&g_stray_pages);
    if (n <= 4 || (n & (n - 1)) == 0) printf("[mem] game touched memory outside its regions at 0x%08X (%ld times so far): page committed\n", (u32)(at - g_space), n);
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

bool wiiu_mem_init(void) {
    g_space = reserve_space();
#ifdef _WIN32
    if (g_space) AddVectoredExceptionHandler(1, commit_on_touch);
#endif
    if (g_space) {
        for (size_t i = 0; i < REGION_COUNT; i++)
            g_regions[i].host = g_space + g_regions[i].base;
        wiiu_fastmem = g_force_slow ? NULL : g_space;
        return true;
    }
    // Couldn't get 4 GB of address space: fall back to separate regions and
    // the slower checked path.
    fprintf(stderr, "[mem] couldn't reserve the 4 GB guest address space; using slower memory access\n");
    for (size_t i = 0; i < REGION_COUNT; i++) {
        g_regions[i].host = reserve_zeroed(g_regions[i].size);
        if (!g_regions[i].host) {
            fprintf(stderr, "error: couldn't reserve %u MB of guest memory at 0x%08X\n",
                    g_regions[i].size >> 20, g_regions[i].base);
            wiiu_mem_shutdown();
            return false;
        }
    }
    return true;
}

// How much of the game's memory (the 4 GB block) is really in the PC's RAM,
// in bytes (pages the game has touched and Windows keeps in RAM). Slow-ish
// (~1M pages looked at): only for SPLAT_MEM_DETAIL=1's [mem] lines.
unsigned long long wiiu_mem_resident_bytes(void) {
#ifdef _WIN32
    if (!g_space) return 0;
    enum { CHUNK = 65536 };
    static PSAPI_WORKING_SET_EX_INFORMATION* wi;
    if (!wi) wi = (PSAPI_WORKING_SET_EX_INFORMATION*)malloc(sizeof(*wi) * CHUNK);
    if (!wi) return 0;
    unsigned long long pages = 0;
    for (unsigned long long off = 0; off < 0x100000000ull; off += (unsigned long long)CHUNK * 4096) {
        for (int i = 0; i < CHUNK; i++) wi[i].VirtualAddress = g_space + off + (unsigned long long)i * 4096;
        if (!K32QueryWorkingSetEx(GetCurrentProcess(), wi, (DWORD)(sizeof(*wi) * CHUNK))) return 0;
        for (int i = 0; i < CHUNK; i++) pages += wi[i].VirtualAttributes.Valid;
    }
    return pages * 4096;
#else
    return 0;
#endif
}

void wiiu_mem_shutdown(void) {
    if (g_space) {
#ifdef _WIN32
        VirtualFree(g_space, 0, MEM_RELEASE);
#else
        munmap(g_space, (size_t)SPACE_SIZE);
#endif
        g_space = NULL;
        wiiu_fastmem = NULL;
        for (size_t i = 0; i < REGION_COUNT; i++) g_regions[i].host = NULL;
        return;
    }
    for (size_t i = 0; i < REGION_COUNT; i++) {
        release(g_regions[i].host, g_regions[i].size);
        g_regions[i].host = NULL;
    }
}

void wiiu_mem_use_slow_path(void) { g_force_slow = true; wiiu_fastmem = NULL; }

// The original DolRecomp memory functions, for fast_cpu.h's slow path
// (this file sees the real mem_read32 etc., not fast_cpu.h's).
u64 wiiu_slow_read64(CPUState* cpu, u32 addr) { return mem_read64(cpu, addr); }
u32 wiiu_slow_read32(CPUState* cpu, u32 addr) { return mem_read32(cpu, addr); }
u16 wiiu_slow_read16(CPUState* cpu, u32 addr) { return mem_read16(cpu, addr); }
u8  wiiu_slow_read8(CPUState* cpu, u32 addr) { return mem_read8(cpu, addr); }
void wiiu_slow_write64(CPUState* cpu, u32 addr, u64 v) { mem_write64(cpu, addr, v); }
void wiiu_slow_write32(CPUState* cpu, u32 addr, u32 v) { mem_write32(cpu, addr, v); }
void wiiu_slow_write16(CPUState* cpu, u32 addr, u16 v) { mem_write16(cpu, addr, v); }
void wiiu_slow_write8(CPUState* cpu, u32 addr, u8 v) { mem_write8(cpu, addr, v); }
// (the same for the optimized game-code files, which don't hand over their
// CPU state: see split_inline.h)
CPUState* splat_current_cpu(void);   // main.c
u64 wiiu_slow_read64_cur(u32 addr) { return mem_read64(splat_current_cpu(), addr); }
u32 wiiu_slow_read32_cur(u32 addr) { return mem_read32(splat_current_cpu(), addr); }
u16 wiiu_slow_read16_cur(u32 addr) { return mem_read16(splat_current_cpu(), addr); }
u8 wiiu_slow_read8_cur(u32 addr) { return mem_read8(splat_current_cpu(), addr); }
void wiiu_slow_write64_cur(u32 addr, u64 v) { mem_write64(splat_current_cpu(), addr, v); }
void wiiu_slow_write32_cur(u32 addr, u32 v) { mem_write32(splat_current_cpu(), addr, v); }
void wiiu_slow_write16_cur(u32 addr, u16 v) { mem_write16(splat_current_cpu(), addr, v); }
void wiiu_slow_write8_cur(u32 addr, u8 v) { mem_write8(splat_current_cpu(), addr, v); }

u8* wiiu_mem_ptr(u32 addr, u32 size) {
    for (size_t i = 0; i < REGION_COUNT; i++) {
        const Region* r = &g_regions[i];
        u32 off = addr - r->base;
        if (addr >= r->base && off < r->size && size <= r->size - off)
            return r->host + off;
    }
    return NULL;
}

static u64 load_be(const u8* p, u8 size) {
    u64 v = 0;
    for (u8 i = 0; i < size; i++) v = (v << 8) | p[i];
    return v;
}

static void store_be(u8* p, u64 v, u8 size) {
    for (int i = size - 1; i >= 0; i--) { p[i] = (u8)v; v >>= 8; }
}

static u32 g_bad_reads = 0, g_bad_writes = 0;

// Watchpoints (--watch ADDR): report every write the game makes there.
static u32 g_watch[8], g_watch_end[8];
static int g_watch_count = 0;
void wiiu_mem_watch(u32 addr, u32 end) {
    wiiu_mem_use_slow_path();   // watching needs every write to go through check_watch
    if (g_watch_count < 8) { g_watch[g_watch_count] = addr; g_watch_end[g_watch_count++] = end > addr ? end : addr + 4; }
}
static void check_watch(CPUState* cpu, u32 ea, u64 value, u8 size) {
    for (int i = 0; i < g_watch_count; i++) {
        if (ea < g_watch_end[i] && ea + size > g_watch[i]) {
            static int logged = 0;
            if (logged++ < 200)
                printf("[watch] write%u %08X = %0*llX at pc=%08X lr=%08X\n", size * 8, ea, size * 2,
                       (unsigned long long)value, cpu->pc, cpu->lr);
        }
    }
}

static u64 ext_read(CPUState* cpu, u32 ea, u8 size) {
    (void)cpu;
    u8* p = wiiu_mem_ptr(ea, size);
    if (!p) {
        if (g_bad_reads++ < 32)
            fprintf(stderr, "[mem] read%u from unmapped 0x%08X (pc=%08X lr=%08X)\n",
                    size * 8, ea, cpu->pc, cpu->lr);
        return 0;
    }
    return load_be(p, size);
}

static void ext_write(CPUState* cpu, u32 ea, u64 value, u8 size) {
    if (g_watch_count) check_watch(cpu, ea, value, size);
    u8* p = wiiu_mem_ptr(ea, size);
    if (!p) {
        if (g_bad_writes++ < 32)
            fprintf(stderr, "[mem] write%u to unmapped 0x%08X (pc=%08X lr=%08X)\n",
                    size * 8, ea, cpu->pc, cpu->lr);
        return;
    }
    store_be(p, value, size);
}

void wiiu_mem_attach(CPUState* cpu) {
    // Turn off DolRecomp's GameCube RAM so every access falls through to us.
    free(cpu->ram);
    cpu->ram = NULL;
    cpu->ram_size = 0;
    cpu->external_read = ext_read;
    cpu->external_write = ext_write;
}

u32 wiiu_read32(u32 addr) { u8* p = wiiu_mem_ptr(addr, 4); return p ? (u32)load_be(p, 4) : 0; }
u16 wiiu_read16(u32 addr) { u8* p = wiiu_mem_ptr(addr, 2); return p ? (u16)load_be(p, 2) : 0; }
u8  wiiu_read8(u32 addr)  { u8* p = wiiu_mem_ptr(addr, 1); return p ? *p : 0; }
void wiiu_write32(u32 addr, u32 v) { u8* p = wiiu_mem_ptr(addr, 4); if (p) store_be(p, v, 4); }
void wiiu_write16(u32 addr, u16 v) { u8* p = wiiu_mem_ptr(addr, 2); if (p) store_be(p, v, 2); }
void wiiu_write8(u32 addr, u8 v)   { u8* p = wiiu_mem_ptr(addr, 1); if (p) *p = v; }
u32 wiiu_read32_le(u32 addr) { u8* p = wiiu_mem_ptr(addr, 4); return p ? (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24) : 0; }
void wiiu_write32_le(u32 addr, u32 v) {
    u8* p = wiiu_mem_ptr(addr, 4);
    if (p) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
}

void wiiu_read_string(u32 addr, char* out, size_t out_size) {
    size_t i = 0;
    if (!out_size) return;
    for (; i + 1 < out_size; i++) {
        u8* p = wiiu_mem_ptr(addr + (u32)i, 1);
        if (!p || !*p) break;
        out[i] = (char)*p;
    }
    out[i] = '\0';
}

// --- change tracking (see wiiu_mem.h) -------------------------------------------------
static volatile u32* g_page_gen;   // per 4 KB page: counter value when last marked
static volatile u32 g_gen = 1;
static volatile u32 g_region_gen[4096];   // per 1 MB: counter value when a page in it was last marked

void wiiu_mem_mark(u32 addr, u32 size) {
    if (!size) return;
    if (!g_page_gen) {
        u32* p = (u32*)calloc(1u << 20, sizeof(u32));
        if (!p) return;
        g_page_gen = p;
    }
#if defined(_MSC_VER)
    u32 gen = (u32)_InterlockedIncrement((volatile long*)&g_gen);   // (several cores mark at once)
#else
    u32 gen = __atomic_add_fetch(&g_gen, 1, __ATOMIC_SEQ_CST);
#endif
    u64 end = (u64)addr + size;
    // (the region first, and never lowered by a slower thread's older mark:
    // wiiu_mem_changed_since skips regions whose counter says "nothing new")
    for (u64 r = addr >> 20; r <= ((end - 1) >> 20) && r < 4096; r++) {
        for (;;) {
            u32 cur = g_region_gen[r];
            if (cur >= gen) break;
#if defined(_MSC_VER)
            if ((u32)_InterlockedCompareExchange((volatile long*)&g_region_gen[r], (long)gen, (long)cur) == cur) break;
#else
            if (__atomic_compare_exchange_n(&g_region_gen[r], &cur, gen, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
#endif
        }
    }
    for (u64 a = addr & ~0xFFFull; a < end; a += 0x1000) g_page_gen[a >> 12] = gen;
}
u32 wiiu_mem_region_generation(u32 region) { return region < 4096 ? g_region_gen[region] : 0; }
u32 wiiu_mem_generation(void) { return g_gen; }
bool wiiu_mem_changed_since(u32 addr, u32 size, u32 gen) {
    if (!g_page_gen || !size) return false;
    u64 end = (u64)addr + size;
    // (1 MB regions nothing was marked in since `gen` are skipped whole: big
    // vertex buffers asked about every draw were 256 page checks each)
    for (u64 a = addr & ~0xFFFull; a < end;) {
        u64 r = a >> 20;
        if (r < 4096 && g_region_gen[r] <= gen) { a = (r + 1) << 20; continue; }
        if (g_page_gen[a >> 12] > gen) return true;
        a += 0x1000;
    }
    return false;
}

// --- Atomics (game threads run on several PC cores at once) ----------------
#include "host_thread.h"
HOST_THREAD_LOCAL u32 splat_reserve_value = 0;

// Compare-and-swap of a big-endian word in guest memory.
bool wiiu_cas32(u32 addr, u32 expect, u32 value) {
    u8* p = wiiu_mem_ptr(addr, 4);
    if (!p || (addr & 3)) {   // (unaligned: not atomic, but still correct alone)
        if (wiiu_read32(addr) != expect) return false;
        wiiu_write32(addr, value);
        return true;
    }
    u32 e = ((expect >> 24) | ((expect >> 8) & 0xFF00u) | ((expect << 8) & 0xFF0000u) | (expect << 24));
    u32 v = ((value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24));
#if defined(_MSC_VER)
    return (u32)_InterlockedCompareExchange((volatile long*)p, (long)v, (long)e) == e;
#else
    return __atomic_compare_exchange_n((u32*)p, &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}
bool splat_cas32_slow(u32 addr, u32 expect, u32 value) { return wiiu_cas32(addr, expect, value); }
