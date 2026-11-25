#include "hle.h"
#include "host_thread.h"
#include "wiiu_mem.h"
#include "guest_threads.h"
#include "fs.h"
#include "sys.h"
#include "gx2.h"
#include "audio.h"
#include "erreula.h"
#include "input.h"
#include "online.h"
#include "zlib125.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Data imports that are function pointers get pointed at thunks here.
#define HLE_THUNK_BASE  0xC1000000u
#define HLE_THUNK_MAX   4096

typedef struct {
    u32 address;
    const RpxImport* import;
    HleFunction fn;
    u32 calls;
    u64 main_ticks;         // time the main thread spent in it (rdtsc ticks) and how often (for the log)
    u32 main_calls;
    volatile long other_calls;   // (lock-taking calls by other threads: [hle] line)
    u32 main_locked;             // (lock-taking calls by the main thread: [hle] line)
    signed char dl_free;    // 0 not known yet; 1 = only writes the calling thread's display list (no lock while it records one); 2 = no
    signed char light;      // 0 not known yet; 1 = trivial (never blocks, wakes or shares state): straight call, no bookkeeping; 2 = no
    signed char own_lock;   // 0 not known yet; 1 = a thread/sync call (does its own locking); 2 = takes the system-call lock
    signed char told;       // (a not-yet-written function: reported once)
    signed char watch;      // 0 not known yet; 1 = a draw / display-list call whose callers are counted (the frozen-TV hunt); 2 = no
} Binding;

static Binding g_bindings[RPX_MAX_IMPORTS + HLE_THUNK_MAX];
static volatile u32 g_bind_seq;   // odd while the table is being re-sorted
static u32 g_binding_count = 0;
static RpxImage* g_image = NULL;
static u32 g_thunk_end = HLE_THUNK_BASE;   // next free thunk address

// ---------------------------------------------------------------------------
// Guest heap: a simple first-fit allocator over [WIIU_HEAP_BASE, END).
// Good enough to start; a real MEM heap implementation comes later.
// ---------------------------------------------------------------------------
typedef struct { u32 addr, size; bool used; } Block;
static Block* g_blocks = NULL;
static u32 g_block_count = 0, g_block_cap = 0;
static u32 g_heap_top = WIIU_HEAP_BASE;

static void add_block(u32 addr, u32 size, bool used) {
    if (g_block_count == g_block_cap) {
        g_block_cap = g_block_cap ? g_block_cap * 2 : 256;
        g_blocks = (Block*)realloc(g_blocks, g_block_cap * sizeof(Block));
    }
    g_blocks[g_block_count++] = (Block){ addr, size, used };
}

static u32 heap_alloc_locked(u32 size, u32 align) {
    if (align < 4) align = 4;
    if (size == 0) size = 4;
    size = (size + 3u) & ~3u;
    for (u32 i = 0; i < g_block_count; i++) {
        Block* b = &g_blocks[i];
        u32 start = (b->addr + align - 1) & ~(align - 1);
        if (!b->used && start + size <= b->addr + b->size && start == b->addr) {
            b->used = true;
            if (b->size > size + 64) {
                u32 rest_addr = b->addr + size, rest_size = b->size - size;
                b->size = size;
                add_block(rest_addr, rest_size, false);
            }
            memset(wiiu_mem_ptr(b->addr, b->size), 0, b->size);
            return b->addr;
        }
    }
    u32 start = (g_heap_top + align - 1) & ~(align - 1);
    if (start + size > WIIU_HEAP_END || start + size < start) return 0;
    g_heap_top = start + size;
    add_block(start, size, true);
    return start;
}

u32 hle_heap_alloc(u32 size, u32 align) {
    hle_big_lock();
    u32 a = heap_alloc_locked(size, align);
    hle_big_unlock();
    return a;
}

void hle_heap_free(u32 addr) {
    hle_big_lock();
    for (u32 i = 0; i < g_block_count; i++)
        if (g_blocks[i].addr == addr && g_blocks[i].used) { g_blocks[i].used = false; break; }
    hle_big_unlock();
}

static u32 hle_heap_size_of(u32 addr) {
    for (u32 i = 0; i < g_block_count; i++)
        if (g_blocks[i].addr == addr && g_blocks[i].used) return g_blocks[i].size;
    return 0;
}

u32 hle_heap_free_bytes(void) { return WIIU_HEAP_END - g_heap_top; }

// ---------------------------------------------------------------------------
// coreinit: debug output and process control
// ---------------------------------------------------------------------------
static void OSReport(CPUState* c) {
    char buf[2048];
    hle_format(c, 3, buf, sizeof(buf));
    fputs(buf, stdout);
    fflush(stdout);
}

static void __os_snprintf(CPUState* c) {
    char buf[2048];
    u32 dst = arg(c, 0), size = arg(c, 1);
    int n = hle_format(c, 5, buf, sizeof(buf));
    if (size) {
        u32 copy = (u32)n < size - 1 ? (u32)n : size - 1;
        for (u32 i = 0; i < copy; i++) wiiu_write8(dst + i, (u8)buf[i]);
        wiiu_write8(dst + copy, 0);
    }
    ret(c, (u32)n);
}

static void OSFatal(CPUState* c) {
    char msg[1024];
    wiiu_read_string(arg(c, 0), msg, sizeof(msg));
    fprintf(stderr, "\n[OSFatal] %s\n", msg);
    runtime_report_halt(c);
    hle_print_trail(stderr);
    runtime_exit(1);
}

static void exit_(CPUState* c) { runtime_exit((int)arg(c, 0)); }

static void return_zero(CPUState* c) { ret(c, 0); }
static void return_true(CPUState* c) { ret(c, 1); }

// ---------------------------------------------------------------------------
// coreinit: threads (single-threaded for now)
// ---------------------------------------------------------------------------
static u32 g_main_thread = 0;
static u32 g_thread_specific[32];

static void OSGetCurrentThread(CPUState* c) {
    if (!g_main_thread) g_main_thread = hle_heap_alloc(0x6A0, 8);
    ret(c, g_main_thread);
}
static void OSGetThreadSpecific(CPUState* c) {
    u32 id = arg(c, 0);
    ret(c, id < 32 ? g_thread_specific[id] : 0);
}
static void OSSetThreadSpecific(CPUState* c) {
    u32 id = arg(c, 0);
    if (id < 32) g_thread_specific[id] = arg(c, 1);
}

// ---------------------------------------------------------------------------
// coreinit: atomics
// ---------------------------------------------------------------------------
static void OSCompareAndSwapAtomicEx(CPUState* c) {
    u32 ptr = arg(c, 0), compare = arg(c, 1), value = arg(c, 2), old_ptr = arg(c, 3);
    for (;;) {
        u32 cur = wiiu_read32(ptr);
        if (cur != compare) { if (old_ptr) wiiu_write32(old_ptr, cur); ret(c, 0); return; }
        if (wiiu_cas32(ptr, compare, value)) { if (old_ptr) wiiu_write32(old_ptr, cur); ret(c, 1); return; }
    }
}

// ---------------------------------------------------------------------------
// coreinit: memory heaps
// ---------------------------------------------------------------------------
static u32 fix_align(u32 a) { s32 s = (s32)a; if (s < 0) s = -s; return s < 4 ? 4u : (u32)s; }

// Expanded heaps the game creates over its own memory (MEMCreateExpHeapEx).
// Each one hands out blocks from inside its own range and keeps its own
// accounting, like the console's MEM library: games check "how much is
// free in this heap" and decide things from the answer. Handles that aren't
// one of these (the system's base heaps) use the runtime's main heap.
typedef struct { u32 addr, size; } ExpBlock;
typedef struct {
    u32 start, end;        // usable range (after the heap's own header)
    u32 handle;            // what MEMCreateExpHeapEx returned
    ExpBlock* blocks;      // allocated blocks, sorted by address
    u32 count, cap;
} ExpHeap;
#define MAX_EXP_HEAPS 64
static ExpHeap g_exp[MAX_EXP_HEAPS];
static u32 g_exp_count = 0;
#define EXP_HEADER 0x40    // the console keeps its heap header at the start

static ExpHeap* exp_find(u32 handle) {
    for (u32 i = 0; i < g_exp_count; i++) if (g_exp[i].handle == handle) return &g_exp[i];
    return NULL;
}
static ExpHeap* exp_containing(u32 addr) {
    for (u32 i = 0; i < g_exp_count; i++) if (addr >= g_exp[i].start && addr < g_exp[i].end) return &g_exp[i];
    return NULL;
}

// Find room for `size` bytes aligned to `align`, searching from the bottom
// (from_tail false) or the top. Returns 0 if there's no room.
static u32 exp_fit(const ExpHeap* h, u32 size, u32 align, bool from_tail, u32* index) {
    if (!from_tail) {
        u32 lo = h->start;
        for (u32 i = 0; i <= h->count; i++) {
            u32 hi = i < h->count ? h->blocks[i].addr : h->end;
            u32 p = (lo + align - 1) & ~(align - 1);
            if (p >= lo && p + size <= hi && p + size >= p) { *index = i; return p; }
            if (i < h->count) lo = h->blocks[i].addr + h->blocks[i].size;
        }
    } else {
        u32 hi = h->end;
        for (u32 n = h->count + 1; n-- > 0;) {
            u32 lo = n > 0 ? h->blocks[n - 1].addr + h->blocks[n - 1].size : h->start;
            if (hi >= size) {
                u32 p = (hi - size) & ~(align - 1);
                if (p >= lo && p + size <= hi) { *index = n; return p; }
            }
            if (n > 0) hi = h->blocks[n - 1].addr;
        }
    }
    return 0;
}

static u32 exp_alloc(ExpHeap* h, u32 size, s32 align_signed) {
    bool from_tail = align_signed < 0;
    u32 align = fix_align((u32)align_signed);
    if (size == 0) size = 4;
    size = (size + 3u) & ~3u;
    u32 index = 0, p = exp_fit(h, size, align, from_tail, &index);
    if (!p) return 0;
    if (h->count == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 64;
        h->blocks = (ExpBlock*)realloc(h->blocks, h->cap * sizeof(ExpBlock));
    }
    memmove(&h->blocks[index + 1], &h->blocks[index], (h->count - index) * sizeof(ExpBlock));
    h->blocks[index] = (ExpBlock){ p, size };
    h->count++;
    return p;
}

static bool exp_free(ExpHeap* h, u32 addr) {
    for (u32 i = 0; i < h->count; i++) {
        if (h->blocks[i].addr != addr) continue;
        memmove(&h->blocks[i], &h->blocks[i + 1], (h->count - i - 1) * sizeof(ExpBlock));
        h->count--;
        return true;
    }
    return false;
}

static u32 exp_largest_free(const ExpHeap* h, u32 align) {
    u32 best = 0, lo = h->start;
    for (u32 i = 0; i <= h->count; i++) {
        u32 hi = i < h->count ? h->blocks[i].addr : h->end;
        u32 p = (lo + align - 1) & ~(align - 1);
        if (p < hi && hi - p > best) best = hi - p;
        if (i < h->count) lo = h->blocks[i].addr + h->blocks[i].size;
    }
    return best & ~3u;
}

static u32 exp_total_free(const ExpHeap* h) {
    u32 used = 0;
    for (u32 i = 0; i < h->count; i++) used += h->blocks[i].size;
    return (h->end - h->start) - used;
}

static void MEMCreateExpHeapEx(CPUState* c) {   // (start, size, flags) -> handle
    u32 start = arg(c, 0), size = arg(c, 1);
    if (!start || size <= EXP_HEADER || !wiiu_mem_ptr(start, size)) { ret(c, 0); return; }
    ExpHeap* h = exp_find(start);
    if (!h) {
        if (g_exp_count >= MAX_EXP_HEAPS) { ret(c, start); return; }   // fall back to the main heap
        h = &g_exp[g_exp_count++];
        memset(h, 0, sizeof(*h));
    }
    h->handle = start;
    h->start = (start + EXP_HEADER + 3u) & ~3u;
    h->end = (start + size) & ~3u;
    h->count = 0;
    ret(c, start);
}
static void MEMDestroyExpHeap(CPUState* c) {   // (heap) -> the heap's start
    ExpHeap* h = exp_find(arg(c, 0));
    if (h) {
        free(h->blocks);
        *h = g_exp[--g_exp_count];
    }
    ret(c, arg(c, 0));
}
static void MEMAllocFromExpHeapEx(CPUState* c) {   // (heap, size, align; negative align = from the top)
    ExpHeap* h = exp_find(arg(c, 0));
    if (h) { ret(c, exp_alloc(h, arg(c, 1), (s32)arg(c, 2))); return; }
    ret(c, hle_heap_alloc(arg(c, 1), fix_align(arg(c, 2))));
}
static void MEMFreeToExpHeap(CPUState* c) {   // (heap, block)
    ExpHeap* h = exp_find(arg(c, 0));
    if (!h) h = exp_containing(arg(c, 1));
    if (h && exp_free(h, arg(c, 1))) return;
    hle_heap_free(arg(c, 1));
}
static void MEMGetAllocatableSizeForExpHeapEx(CPUState* c) {   // (heap, align)
    ExpHeap* h = exp_find(arg(c, 0));
    if (h) { ret(c, exp_largest_free(h, fix_align(arg(c, 1)))); return; }
    // The system's main heap: games grab nearly all of it for their own
    // heaps; keep 16 MB back for the runtime's own small allocations.
    u32 free_bytes = hle_heap_free_bytes();
    ret(c, free_bytes > 0x01000000u ? free_bytes - 0x01000000u : free_bytes / 2);
}
static void MEMGetTotalFreeSizeForExpHeap(CPUState* c) {   // (heap)
    ExpHeap* h = exp_find(arg(c, 0));
    ret(c, h ? exp_total_free(h) : hle_heap_free_bytes());
}
static void MEMGetSizeForMBlockExpHeap(CPUState* c) {   // (block)
    ExpHeap* h = exp_containing(arg(c, 0));
    if (h)
        for (u32 i = 0; i < h->count; i++)
            if (h->blocks[i].addr == arg(c, 0)) { ret(c, h->blocks[i].size); return; }
    ret(c, hle_heap_size_of(arg(c, 0)));
}
static void MEMAllocFromDefaultHeap(CPUState* c) { ret(c, hle_heap_alloc(arg(c, 0), 0x40)); }
static void MEMAllocFromDefaultHeapEx(CPUState* c) { ret(c, hle_heap_alloc(arg(c, 0), fix_align(arg(c, 1)))); }
static void MEMFreeToDefaultHeap(CPUState* c) { hle_heap_free(arg(c, 0)); }

// ---------------------------------------------------------------------------
// coreinit: file system client (no files yet; just say "not available")
// ---------------------------------------------------------------------------
#define FS_ERROR_NOT_FOUND ((u32)-0x30006)
static void FSAInit(CPUState* c) { ret(c, 0); }
static void FSAAddClient(CPUState* c) { ret(c, 1); }
static void fs_not_found(CPUState* c) { ret(c, FS_ERROR_NOT_FOUND); }

// ---------------------------------------------------------------------------
// coreinit: memory copy / fill (the OS exports its own memcpy & friends)
// ---------------------------------------------------------------------------
// A copy or fill touching memory that doesn't exist means the game is
// already off the rails (usually a null pointer). Refuse it, loudly, rather
// than scribbling over everything in between.
static void bad_range(const char* what, u32 dst, u32 src, u32 n) {
    static int logged = 0;
    if (logged++ < 16)
        fprintf(stderr, "[hle] refused %s: dst=%08X src=%08X size=%08X (outside game memory)\n",
                what, dst, src, n);
}
static void copy_guest(u32 dst, u32 src, u32 n) {
    if (!n) return;
    u8* d = wiiu_mem_ptr(dst, n);
    u8* s = wiiu_mem_ptr(src, n);
    if (d && s) memmove(d, s, n);
    else bad_range("memory copy", dst, src, n);
}
static void fill_guest(u32 dst, u8 value, u32 n) {
    if (!n) return;
    u8* d = wiiu_mem_ptr(dst, n);
    if (d) memset(d, value, n);
    else bad_range("memory fill", dst, 0, n);
}
static void dc_flush(CPUState* c) { wiiu_mem_mark(arg(c, 0), arg(c, 1)); ret(c, 0); }
static void memcpy_(CPUState* c) { copy_guest(arg(c, 0), arg(c, 1), arg(c, 2)); wiiu_mem_mark(arg(c, 0), arg(c, 2)); ret(c, arg(c, 0)); }
static void memset_(CPUState* c) { fill_guest(arg(c, 0), (u8)arg(c, 1), arg(c, 2)); ret(c, arg(c, 0)); }
static void memclr_(CPUState* c) { fill_guest(arg(c, 0), 0, arg(c, 1)); ret(c, arg(c, 0)); }
static void memcmp_(CPUState* c) {
    u32 a = arg(c, 0), b = arg(c, 1), n = arg(c, 2);
    for (u32 i = 0; i < n; i++) {
        u8 x = wiiu_read8(a + i), y = wiiu_read8(b + i);
        if (x != y) { ret(c, (u32)(x < y ? -1 : 1)); return; }
    }
    ret(c, 0);
}
// OSBlockMove(dst, src, size, flush) / OSBlockSet(dst, value, size)
static void OSBlockMove(CPUState* c) { copy_guest(arg(c, 0), arg(c, 1), arg(c, 2)); wiiu_mem_mark(arg(c, 0), arg(c, 2)); ret(c, arg(c, 0)); }
static void OSBlockSet(CPUState* c) { fill_guest(arg(c, 0), (u8)arg(c, 1), arg(c, 2)); ret(c, arg(c, 0)); }

// ---------------------------------------------------------------------------
// coreinit: time. The Wii U's timer ticks at 62,156,250 Hz.
// ---------------------------------------------------------------------------
#include <time.h>

static u64 host_ticks(void) { return wiiu_time_ticks(); }
// SPLAT_DETERMINISTIC=1 (benchmarks): the game's random generators that seed
// themselves from the clock (sead::Random::init() at 028FC048, 18 places in
// the game) get a seed made from which code asked (the caller's return address,
// saved in init's frame) and how many times that place asked before -- the same
// every run, whatever order the threads get there in.
#define DET_SEED_RET 0x028FC060u   // (OSGetTime's return address inside sead::Random::init())
int splat_deterministic(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPLAT_DETERMINISTIC"); on = e && atoi(e) ? 1 : 0; if (on) printf("[det] deterministic mode: clock-seeded random generators get fixed seeds\n"); }
    return on;
}
// (the engine's global random object: made once at start-up -- 028FBF40 seeds it from the clock and keeps
// it in a global pointer -- and drawn from by everything afterwards; remembered so a scene load can re-seed it)
#define DET_ENGINE_RANDOM_SITE 0x028FBF44u
static u32 g_det_engine_random, g_det_engine_seed;
static void det_random_init(u32 obj, u32 seed) {   // (the game's sead::Random::init(seed), 028FBFEC)
    u32 k = 0x6C078965u, x = k * (seed ^ (seed >> 30)) + 1, y = k * (x ^ (x >> 30)) + 2, z = k * (y ^ (y >> 30)) + 3, w = k * (z ^ (z >> 30)) + 4;
    wiiu_write32(obj, x); wiiu_write32(obj + 4, y); wiiu_write32(obj + 8, z); wiiu_write32(obj + 12, w);
}
static u32 det_seed(CPUState* c) {
    static volatile long lock;
    static struct { u32 site, n; } sites[64];
    u32 site = wiiu_read32(c->gpr[1] + 20), n = 0;
    while (_InterlockedCompareExchange(&lock, 1, 0)) {}
    for (int i = 0; i < 64; i++) {
        if (sites[i].site == site) { n = ++sites[i].n; break; }
        if (!sites[i].site) { sites[i].site = site; sites[i].n = n = 1; break; }
    }
    lock = 0;
    u32 h = site * 0x9E3779B1u ^ n * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    if (site == DET_ENGINE_RANDOM_SITE) { g_det_engine_random = c->gpr[31]; g_det_engine_seed = h; }   // (init's `this` is in r31 by then)
    static int told;
    if (told++ < 40) printf("[det] random seed for the generator made from %08X (time #%u there): %08X\n", site, n, h);
    return h;
}
// SPLAT_DET_TRACE=1: each place that reads the clock, the first time it does (with the frame number):
// the ones that read it once while a scene loads are the candidates for "random" choices
static void det_trace(CPUState* c, const char* what) {
    static int on = -1;
    if (on < 0) on = getenv("SPLAT_DET_TRACE") != NULL;
    if (!on) return;
    static volatile long lock;
    static u32 seen[1024];
    static int nseen;
    u32 lr = c->lr;
    while (_InterlockedCompareExchange(&lock, 1, 0)) {}
    bool fresh = true;
    for (int i = 0; i < nseen; i++) if (seen[i] == lr) { fresh = false; break; }
    if (fresh && nseen < 1024) seen[nseen++] = lr;
    lock = 0;
    extern u32 gx2_frame_count(void);
    if (fresh) printf("[det] %s read from %08X (caller %08X), first at frame %u\n", what, lr - 4, wiiu_read32(c->gpr[1] + 4) - 4, gx2_frame_count());
}
// (deterministic mode, a scene's map file opened: the game's global random generator -- set
// from a constant at start-up, then drawn from by everything while booting and loading, so its
// state when the scene picks its crowd depended on timing -- goes back to that constant start)
#define DET_GLOBAL_RANDOM 0x1021A25Cu   // (sead::Random state: 4 words; its getter is 02B0BC28)
void splat_det_scene_load(const char* what) {
    u32 s[4];
    for (int i = 0; i < 4; i++) s[i] = wiiu_read32(DET_GLOBAL_RANDOM + 4 * i);
    static const u32 start[4] = { 1u, 0x6C078967u, 0x714ACB41u, 0x48077044u };
    for (int i = 0; i < 4; i++) wiiu_write32(DET_GLOBAL_RANDOM + 4 * i, start[i]);
    printf("[det] %s: global random state was %08X %08X %08X %08X, reset to the start-up one\n", what, s[0], s[1], s[2], s[3]);
    if (g_det_engine_random && wiiu_mem_ptr(g_det_engine_random, 16)) {
        for (int i = 0; i < 4; i++) s[i] = wiiu_read32(g_det_engine_random + 4 * i);
        det_random_init(g_det_engine_random, g_det_engine_seed);
        printf("[det] engine random at %08X was %08X %08X %08X %08X, re-seeded with %08X\n", g_det_engine_random, s[0], s[1], s[2], s[3], g_det_engine_seed);
    }
}
static void OSGetTime(CPUState* c) {
    u64 t = host_ticks();
    if (c->lr == DET_SEED_RET && splat_deterministic()) t = (t & 0xFFFFFFFF00000000ull) | det_seed(c);
    det_trace(c, "time");
    c->gpr[3] = (u32)(t >> 32); c->gpr[4] = (u32)t;
}
static void OSGetTick(CPUState* c) { det_trace(c, "tick"); ret(c, (u32)host_ticks()); }

// ---------------------------------------------------------------------------
// coreinit: CPU cores. Everything runs on one host thread for now, which
// we present as the main core (core 1).
// ---------------------------------------------------------------------------
static void OSGetCoreId(CPUState* c) { ret(c, 1); }
static void OSGetMainCoreId(CPUState* c) { ret(c, 1); }
static void OSGetCoreCount(CPUState* c) { ret(c, 3); }

// ---------------------------------------------------------------------------
// coreinit: more heap entry points
// ---------------------------------------------------------------------------
static void MEMGetBaseHeapHandle(CPUState* c) { ret(c, 0x4D454D00u | (arg(c, 0) & 0xFF)); }
static void OSAllocFromSystem(CPUState* c) { ret(c, hle_heap_alloc(arg(c, 0), fix_align(arg(c, 1)))); }
static void OSFreeToSystem(CPUState* c) { hle_heap_free(arg(c, 0)); }

// ---------------------------------------------------------------------------
// coreinit: OSDynLoad, looking up system functions by name at run time.
// Each lookup gets a fresh thunk address that we route like an import.
// ---------------------------------------------------------------------------
#define MAX_DYNAMIC 2048
static RpxImport g_dynamic[MAX_DYNAMIC];
static u32 g_dynamic_count = 0;
static HleFunction lookup(const char* name);
static void register_thunk(const RpxImport* imp, HleFunction fn, u32 address);
static int compare_binding(const void* a, const void* b);

static void OSDynLoad_Acquire(CPUState* c) {
    char name[64];
    wiiu_read_string(arg(c, 0), name, sizeof(name));
    u32 handle = 0x44590000u | (g_dynamic_count & 0xFFFF);   // any nonzero value
    if (arg(c, 1)) wiiu_write32(arg(c, 1), handle);
    ret(c, 0);
}
static void OSDynLoad_FindExport(CPUState* c) {
    // (module, isData, name, outAddress)
    char name[96];
    wiiu_read_string(arg(c, 2), name, sizeof(name));
    u32 out = arg(c, 3);

    for (u32 i = 0; i < g_dynamic_count; i++) {   // already handed out?
        if (!strcmp(g_dynamic[i].name, name)) {
            if (out) wiiu_write32(out, g_dynamic[i].address);
            ret(c, 0);
            return;
        }
    }
    if (g_dynamic_count >= MAX_DYNAMIC || g_thunk_end >= HLE_THUNK_BASE + HLE_THUNK_MAX * 8u) {
        ret(c, (u32)-1);
        return;
    }
    RpxImport* imp = &g_dynamic[g_dynamic_count++];
    snprintf(imp->library, sizeof(imp->library), "dynload");
    snprintf(imp->name, sizeof(imp->name), "%s", name);
    imp->address = g_thunk_end;
    g_thunk_end += 8;
    register_thunk(imp, lookup(name), imp->address);
    if (out) wiiu_write32(out, imp->address);
    ret(c, 0);
}
static void OSDynLoad_Release(CPUState* c) { (void)c; }

// ---------------------------------------------------------------------------
// coreinit: system info, interrupts, debug text
// ---------------------------------------------------------------------------
static void OSGetSystemInfo(CPUState* c) {
    static u32 info = 0;
    if (!info) {
        info = hle_heap_alloc(0x20, 8);
        wiiu_write32(info + 0x00, 248625000u);    // bus clock
        wiiu_write32(info + 0x04, 1243125000u);   // core clock
        // 0x08: 64-bit base time (left 0), 0x10..: L2 sizes
        wiiu_write32(info + 0x10, 0x80000u);
        wiiu_write32(info + 0x14, 0x200000u);
        wiiu_write32(info + 0x18, 0x80000u);
    }
    ret(c, info);
}
static bool g_interrupts = true;
static void OSDisableInterrupts(CPUState* c) { ret(c, g_interrupts); g_interrupts = false; }
static void OSRestoreInterrupts(CPUState* c) { u32 prev = g_interrupts; g_interrupts = arg(c, 0) != 0; ret(c, prev); }

static void print_guest_string(u32 addr, u32 max_len) {
    char buf[1024];
    wiiu_read_string(addr, buf, max_len && max_len < sizeof(buf) ? max_len + 1 : sizeof(buf));
    fputs(buf, stdout);
}
static void OSConsoleWrite(CPUState* c) { print_guest_string(arg(c, 0), arg(c, 1)); }
static void OSVReport(CPUState* c) {
    // The arguments arrive as a va_list we don't decode yet: print the format.
    print_guest_string(arg(c, 0), 0);
}
static void COSWarn(CPUState* c) {
    char buf[2048];
    hle_format(c, 4, buf, sizeof(buf));   // (module, fmt, ...)
    printf("[COSWarn] %s", buf);
}
// A failed self-check in the game (a sead "assert") lands in OSPanic. Real
// retail builds keep these checks, but most are about missing extras (for
// example Mii data from system files we don't have), so by default we note
// it and let the game carry on. --strict-asserts stops at the first one.
static bool g_strict_asserts = false;
void hle_set_strict_asserts(bool on) { g_strict_asserts = on; }
bool hle_strict(void) { return g_strict_asserts; }

static void OSPanic(CPUState* c) {
    char file[256], buf[2048];
    wiiu_read_string(arg(c, 0), file, sizeof(file));
    hle_format(c, 5, buf, sizeof(buf));   // (file, line, fmt, ...)
    if (!g_strict_asserts && strstr(file, "Assert")) {
        static u32 skipped = 0;
        skipped++;
        if (skipped <= 20) {
            fprintf(stderr, "[assert] game self-check #%u failed, continuing anyway; called from:\n", skipped);
            runtime_print_chain(c, stderr, 4, false);
        } else if ((skipped & (skipped - 1)) == 0) {
            fprintf(stderr, "[assert] %u game self-checks failed so far\n", skipped);
        }
        return;   // seadAssertCafe returns normally after OSPanic
    }
    fprintf(stderr, "\n[OSPanic] %s:%u: %s\n", file, arg(c, 1), buf);
    runtime_report_halt(c);
    hle_print_trail(stderr);
    runtime_exit(1);
}
static void ENVGetEnvironmentVariable(CPUState* c) { ret(c, 1); }   // "not set"
static void bspGetHardwareVersion(CPUState* c) {
    if (arg(c, 0)) wiiu_write32(arg(c, 0), 0x26100010u);   // a retail Wii U revision
    ret(c, 0);
}

// Frame heaps. The base heaps for MEM1 (arena 0) and the foreground bucket
// (arena 8) are real frame heaps over their own memory areas: allocations
// come from the bottom (positive alignment) or the top (negative), and
// "free all" resets them. Other frame heaps use our general allocator.
typedef struct { u32 handle, base, size, head, tail, saved_head, saved_tail; } Arena;
static Arena g_arenas[2] = {
    { 0x4D454D00u, WIIU_MEM1_BASE, WIIU_MEM1_SIZE, WIIU_MEM1_BASE, WIIU_MEM1_BASE + WIIU_MEM1_SIZE, 0, 0 },
    { 0x4D454D08u, WIIU_FG_BASE,   WIIU_FG_SIZE,   WIIU_FG_BASE,   WIIU_FG_BASE + WIIU_FG_SIZE,     0, 0 },
};
static Arena* arena_of(u32 handle) {
    for (int i = 0; i < 2; i++) if (g_arenas[i].handle == handle) return &g_arenas[i];
    return NULL;
}
static void MEMCreateFrmHeapEx(CPUState* c) { ret(c, arg(c, 0)); }
static void MEMAllocFromFrmHeapEx(CPUState* c) {   // (heap, size, align)
    Arena* a = arena_of(arg(c, 0));
    u32 size = arg(c, 1);
    s32 align = (s32)arg(c, 2);
    if (!a) { ret(c, hle_heap_alloc(size, fix_align(arg(c, 2)))); return; }
    u32 al = fix_align((u32)align);
    size = (size + 3u) & ~3u;
    if (align >= 0) {
        u32 p = (a->head + al - 1) & ~(al - 1);
        if (p + size > a->tail || p + size < p) { ret(c, 0); return; }
        a->head = p + size;
        ret(c, p);
    } else {
        u32 p = (a->tail - size) & ~(al - 1);
        if (p < a->head || size > a->tail) { ret(c, 0); return; }
        a->tail = p;
        ret(c, p);
    }
}
static void MEMGetAllocatableSizeForFrmHeapEx(CPUState* c) {   // (heap, align)
    Arena* a = arena_of(arg(c, 0));
    if (!a) { ret(c, hle_heap_free_bytes() / 4); return; }
    u32 al = fix_align(arg(c, 1));
    u32 p = (a->head + al - 1) & ~(al - 1);
    ret(c, p < a->tail ? ((a->tail - p) & ~3u) : 0);
}
static void MEMFreeToFrmHeap(CPUState* c) {   // (heap, mode: 1 = head, 2 = tail, 3 = all)
    Arena* a = arena_of(arg(c, 0));
    if (!a) return;
    u32 mode = arg(c, 1);
    if (mode & 1) a->head = a->base;
    if (mode & 2) a->tail = a->base + a->size;
}
static void MEMRecordStateForFrmHeap(CPUState* c) {
    Arena* a = arena_of(arg(c, 0));
    if (a) { a->saved_head = a->head; a->saved_tail = a->tail; }
    ret(c, 1);
}
static void MEMFreeByStateToFrmHeap(CPUState* c) {
    Arena* a = arena_of(arg(c, 0));
    if (a && a->saved_head) { a->head = a->saved_head; a->tail = a->saved_tail; }
    ret(c, 1);
}

// ---------------------------------------------------------------------------
// The table: import name -> PC implementation.
// ---------------------------------------------------------------------------
static const HleEntry g_table[] = {
    { "OSReport", OSReport },
    { "__os_snprintf", __os_snprintf },
    { "OSFatal", OSFatal },
    { "exit", exit_ },
    { "_Exit", exit_ },
    { "OSIsDebuggerInitialized", return_zero },
    { "OSIsDebuggerPresent", return_zero },

    { "OSGetCurrentThread", OSGetCurrentThread },
    { "OSGetThreadSpecific", OSGetThreadSpecific },
    { "OSSetThreadSpecific", OSSetThreadSpecific },
    { "OSSetThreadCleanupCallback", return_zero },
    { "OSInitMutex", return_zero },
    { "OSInitMutexEx", return_zero },
    { "OSLockMutex", return_zero },
    { "OSUnlockMutex", return_zero },
    { "OSTryLockMutex", return_true },
    { "OSFastMutex_Init", return_zero },
    { "OSFastMutex_Lock", return_zero },
    { "OSFastMutex_Unlock", return_zero },
    { "OSInitSpinLock", return_zero },
    { "OSUninterruptibleSpinLock_Acquire", return_true },
    { "OSUninterruptibleSpinLock_Release", return_true },
    { "OSCompareAndSwapAtomicEx", OSCompareAndSwapAtomicEx },

    { "MEMAllocFromExpHeapEx", MEMAllocFromExpHeapEx },
    { "MEMFreeToExpHeap", MEMFreeToExpHeap },
    { "MEMGetAllocatableSizeForExpHeapEx", MEMGetAllocatableSizeForExpHeapEx },
    { "MEMGetSizeForMBlockExpHeap", MEMGetSizeForMBlockExpHeap },
    { "MEMAllocFromDefaultHeap", MEMAllocFromDefaultHeap },
    { "MEMAllocFromDefaultHeapEx", MEMAllocFromDefaultHeapEx },
    { "MEMFreeToDefaultHeap", MEMFreeToDefaultHeap },

    { "FSAInit", FSAInit },
    { "FSAAddClient", FSAAddClient },

    { "memcpy", memcpy_ },
    { "memmove", memcpy_ },
    { "memset", memset_ },
    { "memclr", memclr_ },
    { "memcmp", memcmp_ },
    { "OSBlockMove", OSBlockMove },
    { "OSBlockSet", OSBlockSet },

    { "OSGetTime", OSGetTime },
    { "OSGetSystemTime", OSGetTime },
    { "OSGetTick", OSGetTick },
    { "OSGetSystemTick", OSGetTick },

    { "OSGetCoreId", OSGetCoreId },
    { "OSGetMainCoreId", OSGetMainCoreId },
    { "OSGetCoreCount", OSGetCoreCount },
    { "OSIsMainCore", return_true },

    { "MEMGetBaseHeapHandle", MEMGetBaseHeapHandle },
    { "MEMCreateExpHeapEx", MEMCreateExpHeapEx },
    { "MEMGetTotalFreeSizeForExpHeap", MEMGetTotalFreeSizeForExpHeap },
    { "OSAllocFromSystem", OSAllocFromSystem },
    { "OSFreeToSystem", OSFreeToSystem },

    { "OSGetSystemInfo", OSGetSystemInfo },
    { "OSDisableInterrupts", OSDisableInterrupts },
    { "OSRestoreInterrupts", OSRestoreInterrupts },
    { "OSConsoleWrite", OSConsoleWrite },
    { "OSVReport", OSVReport },
    { "COSWarn", COSWarn },
    { "OSPanic", OSPanic },
    { "ENVGetEnvironmentVariable", ENVGetEnvironmentVariable },
    { "bspGetHardwareVersion", bspGetHardwareVersion },
    { "OSGetSecurityLevel", return_zero },

    // Caches and barriers: the PC keeps memory coherent for us.
    { "DCFlushRange", dc_flush },          // (the renderer re-reads textures in flushed memory)
    { "DCFlushRangeNoSync", dc_flush },
    { "DCInvalidateRange", return_zero },
    { "DCStoreRange", dc_flush },
    { "DCStoreRangeNoSync", dc_flush },
    { "OSMemoryBarrier", return_zero },
    { "OSCoherencyBarrier", return_zero },
    { "OSEnforceInorderIO", return_zero },
    { "OSIsAddressRangeDCValid", return_true },

    // Locks used by the game's C library (single-threaded for now).
    { "__ghsLock", return_zero },
    { "__ghsUnlock", return_zero },
    { "__ghs_flock_file", return_zero },
    { "__ghs_funlock_file", return_zero },
    { "__ghs_flock_ptr", return_zero },
    { "__ghs_mtx_init", return_zero },
    { "__ghs_mtx_dst", return_zero },
    { "__ghs_mtx_lock", return_zero },
    { "__ghs_mtx_unlock", return_zero },
    { "__gh_set_errno", return_zero },
    { "OSFastMutex_TryLock", return_true },

    // Thread housekeeping that doesn't need real threads yet.
    { "OSSetThreadName", return_zero },
    { "OSSetThreadPriority", return_true },
    { "OSSetThreadAffinity", return_true },
    { "OSYieldThread", return_zero },
    { "OSSleepTicks", return_zero },
    { "OSBlockThreadsOnExit", return_zero },
    { "OSEnableHomeButtonMenu", return_zero },
    { "OSDynLoad_SetAllocator", return_zero },

    { "MEMCreateFrmHeapEx", MEMCreateFrmHeapEx },
    { "MEMAllocFromFrmHeapEx", MEMAllocFromFrmHeapEx },
    { "MEMGetAllocatableSizeForFrmHeapEx", MEMGetAllocatableSizeForFrmHeapEx },
    { "MEMFreeToFrmHeap", MEMFreeToFrmHeap },
    { "MEMRecordStateForFrmHeap", MEMRecordStateForFrmHeap },
    { "MEMFreeByStateToFrmHeap", MEMFreeByStateToFrmHeap },
    { "MEMDestroyExpHeap", MEMDestroyExpHeap },
    { "MEMDestroyFrmHeap", return_zero },

    { "OSDynLoad_Acquire", OSDynLoad_Acquire },
    { "OSDynLoad_FindExport", OSDynLoad_FindExport },
    { "OSDynLoad_Release", OSDynLoad_Release },
};

static HleFunction lookup(const char* name) {
    // Tables from the other modules first, then this file's.
    const HleEntry* (*modules[])(size_t*) = { threads_hle_table, fs_hle_table, sys_hle_table, gx2_hle_table, audio_hle_table,
                                                  erreula_hle_table, input_hle_table, online_net_hle_table, online_hle_table, boss_hle_table, zlib_hle_table };
    for (size_t m = 0; m < sizeof(modules) / sizeof(modules[0]); m++) {
        size_t n = 0;
        const HleEntry* t = modules[m](&n);
        for (size_t i = 0; i < n; i++)
            if (!strcmp(t[i].name, name)) return t[i].fn;
    }
    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++)
        if (!strcmp(g_table[i].name, name)) return g_table[i].fn;
    // The newer FSA* file API (not FSAddClient & co, which are the FS* API).
    if (!strncmp(name, "FSA", 3) && strncmp(name, "FSAdd", 5) != 0) return fs_not_found;
    return NULL;
}

// SPLAT_HLE_TIMING=1: time every system call the main thread makes (the "ms
// inside them" in the [hle] report). Off by default: reading the processor's
// clock twice per call was ~1.5% of the main thread's time.
int g_hle_timing = 0;

void hle_init(CPUState* cpu, RpxImage* image) {
    (void)cpu;
    g_hle_timing = getenv("SPLAT_HLE_TIMING") != NULL;
    hle_big_init();
    g_image = image;
    // The heap starts right after the program's data and bss (rounded up to
    // 1 MB, plus 1 MB of space), like the console's memory layout.
    u32 heap_start = ((image->data_end + 0xFFFFFu) & ~0xFFFFFu) + 0x100000u;
    if (heap_start < WIIU_DATA_BASE + 0x100000u) heap_start = WIIU_DATA_BASE + 0x100000u;
    if (g_block_count == 0 && g_heap_top == WIIU_HEAP_BASE) g_heap_top = heap_start;
    printf("[hle] main memory heap: %08X-%08X (%u MB)\n", g_heap_top, WIIU_HEAP_END,
           (WIIU_HEAP_END - g_heap_top) >> 20);
    u32 next_thunk = HLE_THUNK_BASE;
    u32 unimplemented = 0;

    for (u32 i = 0; i < image->import_count; i++) {
        const RpxImport* imp = &image->imports[i];
        HleFunction fn = lookup(imp->name);
        if (!fn) unimplemented++;

        Binding* b = &g_bindings[g_binding_count++];
        b->import = imp;
        b->fn = fn;
        b->address = imp->address;

        // A data import holding a function pointer: point it at a thunk.
        // (Written both to the import slot and to the object the patched
        // code reads; see rpx_imports_layout.h.)
        if (imp->is_data && fn) {
            Binding* t = &g_bindings[g_binding_count++];
            *t = *b;
            t->address = next_thunk;
            wiiu_write32(imp->address, next_thunk);
            if (imp->data_object) wiiu_write32(imp->data_object, next_thunk);
            next_thunk += 8;
        } else if (imp->is_data && imp->data_object) {
            wiiu_write32(imp->address, imp->data_object);
            if (!strcmp(imp->name, "__gh_FOPEN_MAX"))
                wiiu_write32(imp->data_object, 20);   // size of the _iob table
        }
    }
    g_thunk_end = next_thunk;
    qsort(g_bindings, g_binding_count, sizeof(Binding), compare_binding);
    printf("[hle] %u imports, %u not implemented yet\n", image->import_count, unimplemented);

    // Write the to-do list: every import the program uses that we don't
    // implement yet, grouped by library.
    FILE* f = fopen("imports_todo.txt", "w");
    if (f) {
        fprintf(f, "%u imports, %u not implemented\n\n", image->import_count, unimplemented);
        for (u32 i = 0; i < image->import_count; i++) {
            const RpxImport* imp = &image->imports[i];
            bool seen = false;   // trampolines repeat an import's name
            for (u32 j = 0; j < i && !seen; j++)
                seen = !strcmp(image->imports[j].name, imp->name);
            if (!seen && !lookup(imp->name))
                fprintf(f, "%-12s %-5s %s\n", imp->library, imp->is_data ? "data" : "func", imp->name);
        }
        fclose(f);
        printf("[hle] wrote the list of unimplemented imports to imports_todo.txt\n");
    }
}

static int compare_binding(const void* a, const void* b) {
    u32 x = ((const Binding*)a)->address, y = ((const Binding*)b)->address;
    return x < y ? -1 : x > y;
}

static Binding* find_binding(u32 address) {
    u32 lo = 0, hi = g_binding_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_bindings[mid].address < address) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_binding_count && g_bindings[lo].address == address ? &g_bindings[lo] : NULL;
}

const char* hle_name_at(u32 address) {
    Binding* b = find_binding(address);
    return b ? b->import->name : NULL;
}


static void register_thunk(const RpxImport* imp, HleFunction fn, u32 address) {
    if (g_binding_count >= sizeof(g_bindings) / sizeof(g_bindings[0])) return;
    hle_table_lock();
    g_bind_seq++;   // (odd: readers look again)
    Binding* b = &g_bindings[g_binding_count++];
    b->address = address;
    b->import = imp;
    b->fn = fn;
    b->calls = 0;
    b->told = 0;
    b->own_lock = 0;
    qsort(g_bindings, g_binding_count, sizeof(Binding), compare_binding);
    g_bind_seq++;
    hle_table_unlock();
}

// For the periodic speed report: which system functions the game called
// most since the last report (a game stuck waiting usually polls something).
// The main thread's system calls per frame, and the time it spent inside them
// (over `frames` frames; the busiest ones by time), then start over.
void hle_report_callers(FILE* out, const char* title, u32 frames);
void hle_report_main_calls(u32 frames) {
    {   // (who draws: every ~30 s of normal play; the freeze dump prints the frozen stretch)
        static int k = 0;
        if (++k % 3 == 0) hle_report_callers(stdout, "[hle] draw / display-list calls by caller", frames);
        extern void nan_trap_report(FILE* out);
        nan_trap_report(stdout);   // (SPLAT_NAN_TRAP=1 only)
        extern void watch_report(FILE* out);
        watch_report(stdout);   // (SPLAT_WATCH_HEX only)
    }
    static u64 tsc_start, ns_start;
    u64 tsc = __rdtsc(), ns = hnow_ns();
    if (!tsc_start) { tsc_start = tsc; ns_start = ns; }
    double ns_per_tick = (tsc > tsc_start && ns > ns_start) ? (double)(ns - ns_start) / (double)(tsc - tsc_start) : 0.3;
    if (!frames) return;
    u64 total = 0, calls = 0;
    for (u32 i = 0; i < g_binding_count; i++) { total += g_bindings[i].main_ticks; calls += g_bindings[i].main_calls; }
    extern u64 g_main_sys_round, g_main_guest_calls, g_main_dispatches;
    printf("[hle] main thread: %.0f game-code calls and %.0f run-loop trips per frame\n", (double)g_main_guest_calls / frames, (double)g_main_dispatches / frames);
    {
        extern u64 g_main_table_calls, g_main_table_bail_depth, g_main_table_bail_budget, g_main_table_same;
        printf("[hle] main thread: %.0f calls through pointers per frame, %.0f handed on for nesting (32 deep), %.0f for the turn's budget\n",
               (double)g_main_table_calls / frames, (double)g_main_table_bail_depth / frames, (double)g_main_table_bail_budget / frames);
        printf("[hle] main thread: %.0f of them to a function in the caller's own 16 KB file\n", (double)g_main_table_same / frames);
        g_main_table_same = 0;
        g_main_table_calls = g_main_table_bail_depth = g_main_table_bail_budget = 0;
    }
    {
        extern volatile long g_big_taken, g_big_waited;
        printf("[hle] system-call lock: taken %.0f times per frame by the main thread, found held by another thread %.0f times per frame (all threads)\n",
               (double)g_big_taken / frames, (double)g_big_waited / frames);
        g_big_taken = 0;
        g_big_waited = 0;
        printf("[hle] system-call lock taken by the main thread, per frame:");
        for (int k = 0; k < 14; k++) {
            Binding* best = NULL;
            for (u32 i = 0; i < g_binding_count; i++)
                if (g_bindings[i].main_locked && (!best || g_bindings[i].main_locked > best->main_locked)) best = &g_bindings[i];
            if (!best) break;
            printf("%s %s %.0f", k ? "," : "", best->import->name, (double)best->main_locked / frames);
            best->main_locked = 0;
        }
        printf("\n");
        for (u32 i = 0; i < g_binding_count; i++) g_bindings[i].main_locked = 0;
        printf("[hle] system-call lock taken by other threads, per frame:");
        for (int k = 0; k < 8; k++) {
            Binding* best = NULL;
            for (u32 i = 0; i < g_binding_count; i++)
                if (g_bindings[i].other_calls && (!best || g_bindings[i].other_calls > best->other_calls)) best = &g_bindings[i];
            if (!best) break;
            printf("%s %s %.0f", k ? "," : "", best->import->name, (double)best->other_calls / frames);
            best->other_calls = 0;
        }
        printf("\n");
        for (u32 i = 0; i < g_binding_count; i++) g_bindings[i].other_calls = 0;
    }
    printf("[hle] main thread: %.0f system calls per frame, %.2f ms per frame inside them (%.2f ms with getting there and back):",
           (double)calls / frames, total * ns_per_tick / 1e6 / frames, g_main_sys_round * ns_per_tick / 1e6 / frames);
    g_main_sys_round = g_main_guest_calls = g_main_dispatches = 0;
    for (int k = 0; k < 6; k++) {
        Binding* best = NULL;
        for (u32 i = 0; i < g_binding_count; i++)
            if (g_bindings[i].main_ticks && (!best || g_bindings[i].main_ticks > best->main_ticks)) best = &g_bindings[i];
        if (!best) break;
        printf("%s %s %.2f ms (%.0f)", k ? "," : "", best->import->name, best->main_ticks * ns_per_tick / 1e6 / frames, (double)best->main_calls / frames);
        best->main_ticks = 0;
    }
    if (!total) {   // (no timing in this build: the most called instead)
        static u32 shown_calls[RPX_MAX_IMPORTS + HLE_THUNK_MAX];
        memset(shown_calls, 0, sizeof(shown_calls));
        for (int k = 0; k < 12; k++) {
            int best = -1;
            for (u32 i = 0; i < g_binding_count; i++)
                if (!shown_calls[i] && g_bindings[i].main_calls && (best < 0 || g_bindings[i].main_calls > g_bindings[best].main_calls)) best = (int)i;
            if (best < 0) break;
            shown_calls[best] = 1;
            printf("%s %s %.0f", k ? "," : " most called:", g_bindings[best].import->name, (double)g_bindings[best].main_calls / frames);
        }
    }
    printf("\n");
    for (u32 i = 0; i < g_binding_count; i++) { g_bindings[i].main_ticks = 0; g_bindings[i].main_calls = 0; }
}

void hle_report_calls(FILE* out, double seconds) {
    static u32 last[RPX_MAX_IMPORTS + HLE_THUNK_MAX];
    int printed = 0;
    for (int pass = 0; pass < 12; pass++) {
        u32 best = UINT32_MAX, best_n = 0;
        for (u32 i = 0; i < g_binding_count; i++) {
            u32 n = g_bindings[i].calls - last[i];
            if (n > best_n) { best_n = n; best = i; }
        }
        if (best == UINT32_MAX) break;
        if (!printed++) fprintf(out, "  most-called system functions:\n");
        fprintf(out, "    %-44s %9.0f per second\n", g_bindings[best].import->name,
                seconds > 0 ? best_n / seconds : 0.0);
        last[best] = g_bindings[best].calls;   // printed: skip it in the next pass
    }
    // Functions we haven't written yet that the game keeps calling: a stub
    // answering "not ready" forever is a classic reason for a game to stall.
    printed = 0;
    for (u32 i = 0; i < g_binding_count; i++) {
        if (g_bindings[i].fn) continue;
        u32 n = g_bindings[i].calls - last[i];
        if (!n) continue;
        if (!printed++) fprintf(out, "  not-yet-written functions the game called:\n");
        if (printed <= 25)
            fprintf(out, "    %-44s %9.1f per second\n", g_bindings[i].import->name, seconds > 0 ? n / seconds : 0.0);
    }
    for (u32 i = 0; i < g_binding_count; i++) last[i] = g_bindings[i].calls;
}

// Recent system calls, printed when something goes wrong.
#define TRAIL 12
// (one per PC thread: the game's three cores call into the system thousands
// of times a second each, and one shared list made them fight over it)
static HOST_THREAD_LOCAL struct { const char* name; u32 r3, r4, lr, result; } g_trail[TRAIL];
static HOST_THREAD_LOCAL u32 g_trail_pos = 0;

void hle_print_trail(FILE* out) {
    fprintf(out, "  last system calls (oldest first):\n");
    for (u32 i = 0; i < TRAIL; i++) {
        u32 k = (g_trail_pos + i) % TRAIL;
        if (!g_trail[k].name) continue;
        fprintf(out, "    %-36s r3=%08X r4=%08X -> %08X  (from %08X)\n",
                g_trail[k].name, g_trail[k].r3, g_trail[k].r4, g_trail[k].result, g_trail[k].lr);
    }
}

// --trace N: print the first N system calls with their arguments and results.
static int g_trace_budget = 0;
void hle_set_trace(int n) { g_trace_budget = n; }

// --- Who draws: per draw / display-list call, the three return addresses up the
// game's stack, counted. Printed every ~30 s and in every frozen-TV dump: the
// callers that stop when the TV's world stops show where the game decided to.
#define CALLER_SLOTS 4096
typedef struct { const char* name; u32 lr[6]; volatile long n; } CallerSlot;
static CallerSlot g_callers[CALLER_SLOTS];
static volatile long g_callers_lock;
static void callers_note(const Binding* b, CPUState* cpu) {
    u32 lr[6] = { cpu->lr, 0, 0, 0, 0, 0 };
    u32 f = cpu->gpr[1];
    for (int k = 1; k < 6; k++) {   // (the stack's back chain: each frame's caller saved its return address at +4 of the frame above)
        f = wiiu_mem_ptr(f, 4) ? wiiu_read32(f) : 0;
        if (!f || !wiiu_mem_ptr(f, 8)) break;
        lr[k] = wiiu_read32(f + 4);
    }
    u32 h = (u32)(uintptr_t)b->import->name;
    for (int k = 0; k < 6; k++) h = (h ^ lr[k]) * 0x9E3779B1u;
    while (_InterlockedCompareExchange(&g_callers_lock, 1, 0)) {}
    for (u32 k = 0; k < 16; k++) {
        CallerSlot* c = &g_callers[(h + k) & (CALLER_SLOTS - 1)];
        if (!c->name) { c->name = b->import->name; memcpy(c->lr, lr, sizeof(lr)); }
        if (c->name == b->import->name && !memcmp(c->lr, lr, sizeof(lr))) { c->n++; break; }
    }
    g_callers_lock = 0;
}
void hle_report_callers(FILE* out, const char* title, u32 frames) {
    if (!getenv("SPLAT_CALLERS")) return;
    extern u32 gx2_frame_count(void);
    static u32 last_report;
    u32 now = gx2_frame_count();
    frames = now - last_report;
    last_report = now;
    if (!frames) frames = 1;
    while (_InterlockedCompareExchange(&g_callers_lock, 1, 0)) {}
    fprintf(out, "%s (per frame, %u frames; function: return addresses, innermost first):\n", title, frames);
    static u8 shown[CALLER_SLOTS];
    memset(shown, 0, sizeof(shown));
    for (int line = 0; line < 60; line++) {
        int best = -1;
        for (int i = 0; i < CALLER_SLOTS; i++) if (g_callers[i].name && g_callers[i].n && !shown[i] && (best < 0 || g_callers[i].n > g_callers[best].n)) best = i;
        if (best < 0) break;
        shown[best] = 1;
        CallerSlot* c = &g_callers[best];
        fprintf(out, "    %-26s %08X <- %08X <- %08X <- %08X <- %08X <- %08X  %8.2f\n", c->name, c->lr[0] - 4, c->lr[1] ? c->lr[1] - 4 : 0, c->lr[2] ? c->lr[2] - 4 : 0, c->lr[3] ? c->lr[3] - 4 : 0, c->lr[4] ? c->lr[4] - 4 : 0, c->lr[5] ? c->lr[5] - 4 : 0, (double)c->n / frames);
    }
    memset(g_callers, 0, sizeof(g_callers));
    g_callers_lock = 0;
}

bool hle_host_call(CPUState* cpu, u32 address) {
    // Fast reject: ordinary game code isn't in the import or thunk ranges.
    bool in_imports = address >= g_image->import_min && address <= g_image->import_max;
    bool in_thunks = address >= HLE_THUNK_BASE && address < g_thunk_end;
    if (!in_imports && !in_thunks)
        return false;

    // (a new thunk may be sorted in meanwhile: look again if the table changed)
    // The last ones this thread found are remembered while the table stays
    // as it is: the game makes the same few calls thousands of times a frame.
    static HOST_THREAD_LOCAL struct { u32 addr, seq; Binding* b; } bcache[64];
    unsigned bh = (address >> 2) & 63;
    Binding* b;
    if (bcache[bh].b && bcache[bh].addr == address && bcache[bh].seq == g_bind_seq) b = bcache[bh].b;
    else {
        u32 s1;
        for (;;) {
            s1 = g_bind_seq;
            if (s1 & 1) continue;
            b = find_binding(address);
            if (g_bind_seq == s1) break;
        }
        bcache[bh].addr = address; bcache[bh].seq = s1; bcache[bh].b = b;
    }
    if (!b) return false;

    {   // (counted 8 at a time: a counter every core writes is slow to share)
        static HOST_THREAD_LOCAL u32 tick;
        if ((tick++ & 7) == 0) b->calls += 8;
    }
    // Trivial calls the game makes thousands of times a frame (OSGetCoreId alone ~2,700 on the main
    // thread in the busy plaza): no call trail, timing or wake-up check -- they never block, wake a
    // thread or touch shared state. (Still counted above.) SPLAT_HLE_LIGHT=0: off.
    if (b->light == 1) { b->fn(cpu); cpu->pc = cpu->lr; return true; }
    if (!b->light) {
        static const char* const light_calls[] = {
            "OSGetCoreId", "OSIsAddressRangeDCValid", "OSGetTick", "OSGetTime", "OSGetSystemTick", "OSGetSystemTime",
            "DCZeroRange", "DCFlushRange", "DCFlushRangeNoSync", "DCStoreRange", "DCStoreRangeNoSync",
            "DCInvalidateRange", "DCTouchRange", "OSMemoryBarrier",
        };
        static int on = -1;
        if (on < 0) on = getenv("SPLAT_HLE_LIGHT") ? atoi(getenv("SPLAT_HLE_LIGHT")) : 1;
        b->light = 2;
        for (size_t i = 0; on && b->fn && i < sizeof(light_calls) / sizeof(light_calls[0]); i++)
            if (!strcmp(b->import->name, light_calls[i])) b->light = 1;
        if (b->light == 1 && g_trace_budget <= 0) { b->fn(cpu); cpu->pc = cpu->lr; return true; }
    }
    // Graphics calls that only write the calling thread's display list (b->dl_free, set below on
    // the first call) while it records one: no call trail, timing or wake-up check either. The busy
    // plaza's main thread makes ~12,500 a frame (GX2CallDisplayList alone ~4,500), each writing the
    // call trail every core shares. SPLAT_HLE_DL_FAST=0: off.
    if (b->dl_free == 1 && g_trace_budget <= 0) {
        static int fast = -1;
        if (fast < 0) fast = getenv("SPLAT_HLE_DL_FAST") ? atoi(getenv("SPLAT_HLE_DL_FAST")) : 1;
        extern bool gx2_dl_active(void);
        if (fast && gx2_dl_active()) { b->fn(cpu); cpu->pc = cpu->lr; return true; }
    }
    u32 slot = g_trail_pos++ % TRAIL;
    g_trail[slot].name = b->import->name;
    g_trail[slot].r3 = cpu->gpr[3];
    g_trail[slot].r4 = cpu->gpr[4];
    g_trail[slot].lr = cpu->lr;
    bool on_main = threads_is_main();
    u64 tsc0 = on_main && g_hle_timing ? __rdtsc() : 0;
    bool trace_result = false;
    {   // SPLAT_AUDIO_TRACE=N: the first N system calls made on the timer thread
        // (where the sound engine's 3 ms callback runs) and on threads other
        // than main, with their names
        static int budget = -1;
        if (budget < 0) { const char* e = getenv("SPLAT_AUDIO_TRACE"); budget = e ? atoi(e) : 0; }
        // (SPLAT_TRACE_NAMES=FSOpenDir,FSReadDir,...: only calls whose names start with one of these)
        static char names[512];
        static int names_set = -1;
        if (names_set < 0) { const char* e = getenv("SPLAT_TRACE_NAMES"); names_set = e && strlen(e) < sizeof(names) - 2; if (names_set) snprintf(names, sizeof(names), "%s,", e); }
        bool want = false;
        if (budget <= 0) {}   // (tracing off: no name compares on every call)
        else if (names_set) {
            want = false;
            for (const char* p = names; *p && !want; ) {
                const char* q = strchr(p, ',');
                size_t len = q ? (size_t)(q - p) : strlen(p);
                if (len && !strncmp(b->import->name, p, len)) want = true;
                p = q ? q + 1 : p + len;
            }
        } else
            want = (!strncmp(b->import->name, "AX", 2) || !strncmp(b->import->name, "FS", 2) || !strncmp(b->import->name, "OSSendMessage", 13) || !strncmp(b->import->name, "DRCVS", 5)) && strncmp(b->import->name, "AXRmt", 5) && strncmp(b->import->name, "AXUserIs", 8);
        static int thr_set = -1;   // (SPLAT_TRACE_THREAD=hex: only calls from that guest thread)
        static u32 thr_only;
        if (thr_set < 0) { const char* e = getenv("SPLAT_TRACE_THREAD"); thr_set = e && sscanf(e, "%x", &thr_only) == 1; }
        if (want && thr_set && threads_current_object() != thr_only) want = false;
        if (budget > 0 && want) {
            budget--;
            printf("[audio-trace] %s %08X %s r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X (from %08X)\n", threads_in_alarm() ? "timer" : "thread", threads_current_object(),
                   b->import->name, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->lr - 4);
            trace_result = true;
        }
    }
    if (b->fn) {
        if (!b->own_lock) {
            // thread/sync calls lock for themselves; a few tiny calls touch no
            // shared state at all and are made thousands of times a second
            // from every core -- they skip the system-call lock too
            static const char* const free_calls[] = {
                "OSMemoryBarrier", "DCZeroRange", "DCFlushRange", "DCFlushRangeNoSync", "DCStoreRange",
                "DCStoreRangeNoSync", "DCInvalidateRange", "DCTouchRange", "OSGetTime", "OSGetTick",
                "OSGetSystemTime", "OSGetSystemTick", "memcpy", "memset", "memmove", "OSBlockMove", "OSBlockSet",
            };
            bool free_call = false;
            for (size_t i = 0; i < sizeof(free_calls) / sizeof(free_calls[0]) && !free_call; i++)
                free_call = !strcmp(b->import->name, free_calls[i]);
            b->own_lock = threads_is_own_call(b->fn) || free_call ? 1 : 2;
        }
        if (!b->dl_free) {
            // Graphics calls that only compute words and write them into the calling thread's
            // display list (gx2.c: no other shared state). The game records display lists on its
            // worker threads and the main thread at the same time; all of them taking turns at the
            // system-call lock for these was ~3,500 waits a frame in the busy plaza. While the thread
            // records a display list nothing else can see what they write; otherwise (the main
            // command ring, shared) they take the lock as before. SPLAT_DL_LOCKFREE=0: always lock.
            static const char* const dl_calls[] = {
                "GX2SetVertexUniformReg", "GX2SetPixelUniformReg", "GX2SetVertexUniformBlock", "GX2SetPixelUniformBlock",
                "GX2SetGeometryUniformBlock", "GX2SetPixelTexture", "GX2SetVertexTexture", "GX2SetGeometryTexture",
                "GX2SetAttribBuffer", "GX2SetPixelSampler", "GX2SetVertexSampler", "GX2SetGeometrySampler",
                "GX2SetBlendControl", "GX2SetBlendControlReg", "GX2SetAlphaTest", "GX2SetAlphaTestReg", "GX2SetColorControl",
                "GX2SetColorControlReg", "GX2SetDepthStencilControl", "GX2SetDepthStencilControlReg", "GX2SetPolygonControl",
                "GX2SetPolygonControlReg", "GX2SetCullOnlyControl", "GX2SetTargetChannelMasks", "GX2SetTargetChannelMasksReg",
                "GX2SetPrimitiveRestartIndex", "GX2SetFetchShader", "GX2SetVertexShader", "GX2SetPixelShader",
                "GX2GetCurrentDisplayList", "GX2DrawIndexedEx", "GX2DrawEx", "GX2CallDisplayList",
            };
            static int on = -1;
            if (on < 0) on = getenv("SPLAT_DL_LOCKFREE") ? atoi(getenv("SPLAT_DL_LOCKFREE")) : 1;
            b->dl_free = 2;
            for (size_t i = 0; on && i < sizeof(dl_calls) / sizeof(dl_calls[0]); i++)
                if (!strcmp(b->import->name, dl_calls[i])) b->dl_free = 1;
        }
        if (b->watch != 2) {   // (draw / display-list calls: by whom -- see hle_report_callers)
            if (!b->watch) {
                static const char* const watched[] = { "GX2DrawIndexedEx", "GX2DrawEx", "GX2CallDisplayList", "GX2DirectCallDisplayList",
                                                        "GX2BeginDisplayListEx", "GX2SetColorBuffer", "GX2SetDepthBuffer", "GX2QueryBegin" };
                // (off unless SPLAT_CALLERS=1 since the frozen TV world was found: walking the stack
                // for every draw call was 1.1% of the game's main thread in the busy plaza)
                static int on = -1;
                if (on < 0) on = getenv("SPLAT_CALLERS") != NULL;
                b->watch = 2;
                for (size_t i = 0; on && i < sizeof(watched) / sizeof(watched[0]); i++) if (!strcmp(b->import->name, watched[i])) b->watch = 1;
            }
            if (b->watch == 1) callers_note(b, cpu);
        }
        extern bool gx2_dl_active(void);
        if (b->own_lock == 1) b->fn(cpu);
        else if (b->dl_free == 1 && gx2_dl_active()) b->fn(cpu);
        else { if (!on_main) _InterlockedIncrement(&b->other_calls); else b->main_locked++; hle_big_lock(); b->fn(cpu); hle_big_unlock(); }
    } else {
        // C++ constructors (__ct__) hand back the object they built, and
        // the game may keep using that return value: return it unchanged.
        bool ctor = !strncmp(b->import->name, "__ct__", 6);
        if (!b->told && (b->told = 1))
            fprintf(stderr, "[hle] TODO %s::%s (r3=%08X r4=%08X r5=%08X) -> %s\n",
                    b->import->library, b->import->name,
                    cpu->gpr[3], cpu->gpr[4], cpu->gpr[5],
                    ctor ? "returning the object" : "returning 0");
        if (!ctor) ret(cpu, 0);
    }
    g_trail[slot].result = cpu->gpr[3];
    if (on_main) { if (g_hle_timing) b->main_ticks += __rdtsc() - tsc0; b->main_calls++; }
    if (trace_result) printf("[audio-trace]   -> %08X\n", cpu->gpr[3]);
    if (g_trace_budget > 0) {
        g_trace_budget--;
        printf("[call] %-36s r3=%08X r4=%08X r5=%08X r6=%08X -> %08X  (from %08X)\n", b->import->name,
               g_trail[slot].r3, g_trail[slot].r4, cpu->gpr[5], cpu->gpr[6], cpu->gpr[3], cpu->lr - 4);
    }
    // If this call woke a higher-priority thread, it takes over right away
    // (sleepers whose time is up are caught by the run loop's regular check).
    threads_check_preempt_after_call(cpu);
    cpu->pc = cpu->lr;   // return to the caller
    return true;
}

// Every system function called since the last snapshot, most-called first
// (a stuck game polls something every frame: the freeze watchdog takes a
// snapshot a second into the stillness and prints the list a second later).
static u32 g_snap[RPX_MAX_IMPORTS + HLE_THUNK_MAX];
static bool g_snap_taken;
void hle_calls_snapshot(void) {
    for (u32 i = 0; i < g_binding_count; i++) g_snap[i] = g_bindings[i].calls;
    g_snap_taken = true;
}
void hle_report_calls_since_snapshot(FILE* out, const char* title) {
    if (!g_snap_taken) { fprintf(out, "%s: (no snapshot yet)\n", title); return; }
    fprintf(out, "%s:\n", title);
    static u8 printed[RPX_MAX_IMPORTS + HLE_THUNK_MAX];
    memset(printed, 0, sizeof(printed));
    for (int line = 0; line < 400; line++) {
        u32 best = UINT32_MAX, best_n = 0;
        for (u32 i = 0; i < g_binding_count; i++) {
            if (printed[i]) continue;
            u32 n = g_bindings[i].calls - g_snap[i];
            if (n > best_n) { best_n = n; best = i; }
        }
        if (best == UINT32_MAX) break;
        printed[best] = 1;
        fprintf(out, "    %-56s %8u%s\n", g_bindings[best].import->name, best_n, g_bindings[best].fn ? "" : "   (NOT WRITTEN: returns nothing useful)");
    }
    hle_calls_snapshot();
}
