// GX2's command writing (see gx2_cmd.h).
//
// Two places commands can go:
//  - the main stream: a ring of words in PC memory, read by the GPU thread
//    (latte_cp.c) as soon as they're committed;
//  - a display list: a buffer in the game's memory that the game recorded
//    with GX2BeginDisplayList and replays later (GX2CallDisplayList). These
//    are stored big-endian exactly like the console's, because games can
//    also patch them or ship pre-recorded ones.
// Like the console, each CPU core has its own "current target", so a
// display list being recorded on one core doesn't swallow another core's
// commands.
#include "gx2_cmd.h"
#include "latte_cp.h"
#include "guest_threads.h"
#include "host_thread.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_WORDS (1u << 23)   // 32 MB of commands in flight at most
#define RING_MASK (RING_WORDS - 1)

static u32* g_ring;
static volatile u64 g_write = 0;      // producer position (words, never wraps)
static volatile u64 g_commit = 0;     // what the GPU thread may read
static volatile u64 g_read = 0;       // consumer position
static volatile u64 g_flush_pos = 0;  // commands before this were flushed by the game: their data is final
static volatile u64 g_submitted = 0;  // fence numbers ("timestamps")
static volatile u64 g_retired = 0;

typedef struct { bool active; u32 addr, cap_words, pos; bool overflow; u32 owner; } DisplayList;
// One display list being recorded per game thread. (It used to be per CPU
// core, as on the console -- but threads that may run on any core move
// between cores here, and a thread that moved in the middle of a display
// list sent the rest of it to the screen directly (and another thread's
// commands into it): draws in the wrong place or not at all, lost ink
// checks.)
#define DL_SLOTS 32
static DisplayList g_dl[DL_SLOTS];
static DisplayList g_dl_none;   // (threads not recording one)
static volatile u32 g_dl_active;   // how many are being recorded

// (Also called on first use, for programs that draw before GX2Init.)
// The GPU thread sleeps while the game has given it nothing to do, and the
// next flush wakes it. (It used to poll in 0.1 ms steps -- and on Windows a
// wait that short is a busy loop, so an idle GPU thread kept a whole PC
// core busy, often the other half of the game thread's hyper-threaded core.)
static hmtx_t g_wake_m;
static hcnd_t g_wake_c;
static volatile u64 g_gpu_sleeping;
// A game thread waiting for a fence (GX2DrawDone) sleeps on this, and the GPU
// thread wakes it the moment the fence retires.
static hmtx_t g_retire_m;
static hcnd_t g_retire_c;
static volatile u64 g_retire_waiters;
void gx2_cmd_init(void) {
    if (g_ring) return;
    hmtx_init(&g_wake_m, false);
    hcnd_init(&g_wake_c);
    hmtx_init(&g_retire_m, false);
    hcnd_init(&g_retire_c);
    g_ring = (u32*)calloc(RING_WORDS, sizeof(u32));
    latte_cp_start();
}
static void wake_gpu_thread(void) {
    if (!hatomic_load64(&g_gpu_sleeping)) return;
    hmtx_lock(&g_wake_m);
    hcnd_signal(&g_wake_c);
    hmtx_unlock(&g_wake_m);
}
void gx2_ring_wait(u32 max_ms) {
#ifdef _WIN32
    hmtx_lock(&g_wake_m);
    hatomic_store64(&g_gpu_sleeping, 1);
    if (gx2_ring_available() == 0) SleepConditionVariableCS(&g_wake_c, &g_wake_m, max_ms);
    hatomic_store64(&g_gpu_sleeping, 0);
    hmtx_unlock(&g_wake_m);
#else
    (void)max_ms;
    hsleep_ns(100000);
#endif
}

// (the game thread's object, remembered by its PC thread: asking for it for
// every word while another thread recorded a display list was 1.5% of the
// main thread)
static HOST_THREAD_LOCAL u32 tl_me;
static inline u32 me_object(void) {
    if (tl_me) return tl_me;
    u32 fixed = threads_fixed_object();
    if (fixed) { tl_me = fixed; return fixed; }
    return threads_current_object();
}
// (the slot this PC thread found last: the game records thousands of display lists a frame, and
// looking through all 32 slots for every word written was 2% of the main thread in the busy plaza)
static HOST_THREAD_LOCAL DisplayList* tl_dl;
extern u8* wiiu_fastmem;
static DisplayList* current_dl(void) {
    if (!g_dl_active) { g_dl_none.active = false; return &g_dl_none; }   // (no display list being recorded: the usual case, looked at for every word)
    u32 me = me_object();
    DisplayList* c = tl_dl;
    if (c && c->active && c->owner == me) return c;
    for (u32 i = 0; i < DL_SLOTS; i++)
        if (g_dl[i].active && g_dl[i].owner == me) { tl_dl = &g_dl[i]; return &g_dl[i]; }
    g_dl_none.active = false;
    return &g_dl_none;
}
// (a word into the display list in game memory: big-endian, straight in when memory is directly reachable)
static inline void dl_put(DisplayList* dl, u32 w) {
    u32 a = dl->addr + dl->pos * 4;
    if (wiiu_fastmem) {
        u32 b = (w >> 24) | ((w >> 8) & 0xFF00) | ((w << 8) & 0xFF0000) | (w << 24);
        memcpy(wiiu_fastmem + a, &b, 4);
    } else wiiu_write32(a, w);
    dl->pos++;
}

// The GPU thread's read position as last looked at: looked at again only
// when the ring seems full. (Reading it for every word -- a locked read of a
// value the GPU thread keeps changing -- bounced it between the two PC cores
// about 100,000 times a frame.)
static u64 g_read_seen;
static void ring_put(u32 w) {
    if (!g_ring) gx2_cmd_init();
    u64 pos = g_write;
    // Full? Wait for the GPU thread (it runs on its own, so this ends).
    if (pos - g_read_seen >= RING_WORDS - 16) {
        g_read_seen = g_read;   // (a plain 64-bit read is whole on x64)
        while (pos - g_read_seen >= RING_WORDS - 16) {
            hatomic_store64(&g_commit, pos);
            wake_gpu_thread();
            hsleep_ns(100000);
            g_read_seen = g_read;
        }
    }
    g_ring[pos & RING_MASK] = w;
    g_write = pos + 1;
}

void gx2_emit(u32 w) {
    DisplayList* dl = current_dl();
    if (dl->active) {
        if (dl->pos >= dl->cap_words) {
            if (!dl->overflow) printf("[gx2] display list at %08X is full (%u words)\n", dl->addr, dl->cap_words);
            dl->overflow = true;
            return;
        }
        dl_put(dl, w);
        return;
    }
    ring_put(w);
}

void gx2_emit_n(const u32* w, u32 n) {
    DisplayList* dl = current_dl();   // (looked up once for all n words)
    if (dl->active) {
        if (wiiu_fastmem && dl->pos + n <= dl->cap_words) {   // (the usual case: room for all of it -- swapped and written at once)
            u8* d = wiiu_fastmem + dl->addr + dl->pos * 4;
            for (u32 i = 0; i < n; i++) {
                u32 v = w[i];
                v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
                memcpy(d + i * 4, &v, 4);
            }
            dl->pos += n;
            return;
        }
        for (u32 i = 0; i < n; i++) {
            if (dl->pos >= dl->cap_words) {
                if (!dl->overflow) printf("[gx2] display list at %08X is full (%u words)\n", dl->addr, dl->cap_words);
                dl->overflow = true;
                return;
            }
            dl_put(dl, w[i]);
        }
        return;
    }
    if (!g_ring) gx2_cmd_init();
    u64 pos = g_write;
    if (pos + n - g_read_seen < RING_WORDS - 16) {   // (room for all of it: no full-ring check per word)
        for (u32 i = 0; i < n; i++) g_ring[(pos + i) & RING_MASK] = w[i];
        g_write = pos + n;
        return;
    }
    for (u32 i = 0; i < n; i++) ring_put(w[i]);
}

void gx2_commit(void) {
    hatomic_store64(&g_commit, g_write);
    wake_gpu_thread();
}

// Fence timing for the [sync] log line: when the game flushed each fence,
// when the GPU thread reached it, and when it retired.
#define FT_SLOTS 256
static u64 g_ft_emit[FT_SLOTS], g_ft_reach[FT_SLOTS], g_ft_retire[FT_SLOTS], g_ft_ahead[FT_SLOTS];
static u8 g_ft_waited[FT_SLOTS];
// (a slow GX2DrawDone, gx2.c: when the GPU thread reached this fence and retired it, and how many ring words were queued ahead of it at its flush)
void gx2_cmd_fence_times(u64 ts, u64* emit, u64* reach, u64* retire, u64* ahead) {
    *emit = g_ft_emit[ts % FT_SLOTS]; *reach = g_ft_reach[ts % FT_SLOTS]; *retire = g_ft_retire[ts % FT_SLOTS]; *ahead = g_ft_ahead[ts % FT_SLOTS];
}
bool gx2_cmd_gpu_sleeping(void) { return hatomic_load64(&g_gpu_sleeping) != 0; }
static GX2FenceStats g_ft;
static volatile u64 g_wait_pressure_until;
bool gx2_cmd_wait_pressure(void) { return hnow_ns() < g_wait_pressure_until; }
void gx2_cmd_note_wait(u64 ts) { g_ft_waited[ts % FT_SLOTS] = 1; }
void gx2_cmd_note_fence_reached(u64 ts) {
    u64 now = hnow_ns(), e = g_ft_emit[ts % FT_SLOTS];
    g_ft_reach[ts % FT_SLOTS] = now;
    if (e && now > e) {
        u64 d = now - e;
        g_ft.reach_sum += d;
        if (d > g_ft.reach_max) g_ft.reach_max = d;
        if (g_ft_waited[ts % FT_SLOTS]) { g_ft.wn++; g_ft.wreach_sum += d; }
    }
}
void gx2_cmd_fence_stats(GX2FenceStats* out) {
    *out = g_ft;
    memset(&g_ft, 0, sizeof(g_ft));
}

u64 gx2_flush(void) {
    DisplayList* dl = current_dl();
    if (dl->active) return hatomic_load64(&g_submitted);
    u64 ts = hatomic_add64(&g_submitted, 1) + 1;
    g_ft_emit[ts % FT_SLOTS] = hnow_ns();
    g_ft_reach[ts % FT_SLOTS] = 0;
    g_ft_retire[ts % FT_SLOTS] = 0;
    g_ft_ahead[ts % FT_SLOTS] = g_write - g_read;
    g_ft_waited[ts % FT_SLOTS] = 0;
    hatomic_store64(&g_flush_pos, g_write);
    ring_put(PM4_TYPE3(IT_HLE_FENCE, 2));
    ring_put((u32)(ts >> 32));
    ring_put((u32)ts);
    gx2_commit();
    return ts;
}

u64 gx2_last_submitted(void) { return hatomic_load64(&g_submitted); }
u64 gx2_flush_pos(void) { return hatomic_load64(&g_flush_pos); }
u64 gx2_read_pos(void) { return hatomic_load64(&g_read); }
u64 gx2_retired(void) { return hatomic_load64(&g_retired); }
void gx2_cmd_set_retired(u64 ts) {
    {
        u64 now = hnow_ns(), r = g_ft_reach[ts % FT_SLOTS];
        g_ft_retire[ts % FT_SLOTS] = now;
        g_ft.n++;
        if (r && now > r) {
            u64 d = now - r;
            g_ft.retire_sum += d;
            if (d > g_ft.retire_max) g_ft.retire_max = d;
            if (d > 200000) g_ft.deferred++;
            if (g_ft_waited[ts % FT_SLOTS]) {
                g_ft.wretire_sum += d;
                if (d > 300000) g_wait_pressure_until = now + 3000000000ull;   // (the game waited for the card's results: keep the early submits on for 3 s)
            }
        }
        g_ft_waited[ts % FT_SLOTS] = 0;
    }
    hatomic_store64(&g_retired, ts);
#ifdef _WIN32
    if (hatomic_load64(&g_retire_waiters)) {
        hmtx_lock(&g_retire_m);
        WakeAllConditionVariable(&g_retire_c);
        hmtx_unlock(&g_retire_m);
    }
#endif
}

bool gx2_wait_retired(u64 ts, u32 timeout_ms) {
    if (hatomic_load64(&g_retired) >= ts) return true;
#ifdef _WIN32
    // Wait right here first, keeping this game thread's core: the GPU thread
    // wakes us the moment the fence retires. (Napping 0.2 ms at a time handed
    // the core to other threads and took ~1.5 ms per nap to get it back.)
    // After 20 ms nap as before, so other threads on this core can run.
    if (g_ring && threads_quick_waits()) {
        u64 t0 = hnow_ns();
        int big = hle_big_release_all();
        // (most fences retire within half a millisecond: look for that first
        // without going to sleep -- waking up from the sleep below took
        // ~0.3 ms by itself. SPLAT_NO_FENCE_SPIN=1: sleep right away.)
        static int spin = -1;
        if (spin < 0) spin = getenv("SPLAT_FENCE_SPIN") != NULL;   // (measured: no gain in the plaza, the wait is the GPU thread finishing the swap)
        if (spin) {
            while (hatomic_load64(&g_retired) < ts && hnow_ns() - t0 < 600000ull) {
                for (int i = 0; i < 64; i++) YieldProcessor();
            }
        }
        if (hatomic_load64(&g_retired) < ts) {
            hatomic_add64(&g_retire_waiters, 1);
            hmtx_lock(&g_retire_m);
            while (hatomic_load64(&g_retired) < ts && hnow_ns() - t0 < 20000000ull)
                SleepConditionVariableCS(&g_retire_c, &g_retire_m, 2);
            hmtx_unlock(&g_retire_m);
            hatomic_add64(&g_retire_waiters, (u64)-1);
        }
        hle_big_reacquire(big);
        threads_note_host_wait("GPU thread", hnow_ns() - t0);
        if (hatomic_load64(&g_retired) >= ts) return true;
    }
#endif
    u64 start = wiiu_time_ticks(), limit = (u64)timeout_ms * (WIIU_TIMER_HZ / 1000);
    while (hatomic_load64(&g_retired) < ts) {
        if (timeout_ms && wiiu_time_ticks() - start > limit) return false;
        threads_sleep_ticks(WIIU_TIMER_HZ / 5000);   // 0.2 ms; other game threads may run meanwhile
    }
    return true;
}

void gx2_dl_begin(u32 addr, u32 size_bytes) {
    DisplayList* dl = current_dl();
    if (dl == &g_dl_none) {
        for (u32 i = 0; i < DL_SLOTS; i++) if (!g_dl[i].active) { dl = &g_dl[i]; break; }
        if (dl == &g_dl_none) { printf("[gx2] too many display lists at once\n"); return; }
    }
    dl->owner = threads_current_object();
    if (!dl->active) g_dl_active++;
    dl->active = true;   // (no hand-over of the main commands here: only flushes do that)
    dl->addr = addr;
    dl->cap_words = size_bytes / 4;
    dl->pos = 0;
    dl->overflow = false;
}

// Pads to 32 bytes like GX2 and returns the size in bytes.
u32 gx2_dl_end(void) {
    DisplayList* dl = current_dl();
    if (!dl->active) return 0;
    while ((dl->pos & 7) && dl->pos < dl->cap_words) gx2_emit(PM4_FILLER);
    dl->active = false;
    g_dl_active--;
    return dl->pos * 4;
}

bool gx2_dl_current(u32* addr, u32* size_bytes) {
    DisplayList* dl = current_dl();
    if (!dl->active) return false;
    if (addr) *addr = dl->addr;
    if (size_bytes) *size_bytes = dl->cap_words * 4;
    return true;
}

bool gx2_dl_active(void) { return current_dl()->active; }

// --- GPU thread side ---------------------------------------------------------------
u32 gx2_ring_available(void) {
    return (u32)(hatomic_load64(&g_commit) - g_read);
}
u32 gx2_ring_peek(u32 offset) {
    return g_ring[(g_read + offset) & RING_MASK];
}
void gx2_ring_read(u32 offset, u32 n, u32* out) {   // (n words from offset: a packet's data at once)
    u32 at = (u32)((g_read + offset) & RING_MASK);
    u32 first = n < RING_MASK + 1 - at ? n : RING_MASK + 1 - at;
    memcpy(out, &g_ring[at], first * 4);
    if (first < n) memcpy(out + first, &g_ring[0], (n - first) * 4);
}
void gx2_ring_consume(u32 words) {
    hatomic_add64(&g_read, words);
}
