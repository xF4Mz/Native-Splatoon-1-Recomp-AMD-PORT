// (v0.13.3)
// Guest threads, sync objects and alarms (see guest_threads.h for the design).
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700   // for recursive mutexes and nanosleep
#endif
#include "guest_threads.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_thread.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <time.h>

extern unsigned dolrecomp_call_depth;   // DolRecomp's per-thread call depth
static HOST_THREAD_LOCAL bool tl_in_alarm = false;   // running an alarm callback

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// The console's clock runs on local time.
static long long local_offset_seconds(void) {
    static bool done = false;
    static long long offset = 0;
    if (!done) {
        time_t now = time(NULL);
        struct tm lt = *localtime(&now);
        struct tm gt = *gmtime(&now);
        gt.tm_isdst = lt.tm_isdst;
        offset = (long long)difftime(mktime(&lt), mktime(&gt));
        done = true;
    }
    return offset;
}

u64 wiiu_time_ticks(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    // Wii U time counts from 2000-01-01 00:00:00.
    s64 sec = (s64)ts.tv_sec + local_offset_seconds() - 946684800LL;
    if (sec < 0) sec = 0;
    u64 t = (u64)sec * WIIU_TIMER_HZ + (u64)ts.tv_nsec * WIIU_TIMER_HZ / 1000000000ull;
    {   // SPLAT_DETERMINISTIC=1: the clock starts at the same date and time every run (2026-06-01 12:00,
        // then runs as usual): the date's shop stock, stage rotation and time of day are the same each run
        extern int splat_deterministic(void);
        static u64 base_pc;
        static int det = -1;
        if (det < 0) { det = splat_deterministic(); base_pc = t; }
        if (det) t = (u64)833630400ull * WIIU_TIMER_HZ + (t - base_pc);
    }
    return t;
}

static u64 arg64(CPUState* c, int first_reg) {   // 64-bit arg in a register pair
    return ((u64)c->gpr[first_reg] << 32) | c->gpr[first_reg + 1];
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
enum { T_READY, T_RUNNING, T_WAITING, T_SUSPENDED, T_DONE };
static const char* state_name[] = { "ready", "running", "waiting", "suspended", "done" };

typedef struct GThread {
    u32 obj;              // the game's OSThread
    CPUState* cpu;
    int state;
    s32 prio;
    u32 core;
    int group;            // which core it may run on (3 = any core)
    u64 wake_at;          // timeout (0 = none)
    u32 wait_key;         // what it's waiting on
    const char* wait_what;
    u32 wait_lr;          // the game code that made the blocking call
    bool timed_out;
    hcnd_t turn;
    bool started;
    hthrd_t host;
    u32 entry, argc, argv, stack_top;
    u32 exit_value;
    unsigned depth;
    s32 suspend;
    u64 last_run;
    bool interrupts_off;
    u32 specific[32];
    char name[48];
    int slot;             // (native threads: its own place in g_run)
} GThread;

#define MAX_THREADS 256
// PC stack for each game thread (reserved, not all used): recompiled code
// calls deep, like the main thread's 256 MB.
#define GUEST_HOST_STACK ((size_t)128 * 1024 * 1024)
static GThread* g_threads[MAX_THREADS];
static int g_thread_count = 0;
// What runs on each of the three Wii U cores right now (NULL = that core is
// idle). Every core runs its own game thread at the same time, on its own PC
// thread; within one core threads take turns by priority like on the console.
// With native threads (SPLAT_NATIVE_THREADS=1) every game thread is a PC thread of its
// own that runs whenever it isn't waiting -- no Wii U cores to take turns on
// (each thread has a place of its own here, after the three cores'). The
// game still sees the core numbers it asked for (OSGetCoreId).
// Default: the Wii U's three cores, threads taking turns on them.
#define RUN_SLOTS (MAX_THREADS + 3)
static GThread* g_run[RUN_SLOTS];
static bool g_native = false;
static GThread* g_main_thread;
static HOST_THREAD_LOCAL GThread* tl_self = NULL;   // the game thread this PC thread runs
// Set when a system call made some thread ready (woke a waiter, resumed a
// thread), so the check after each system call only does work then.
static volatile bool g_woke_someone = false;
static hmtx_t g_lock;
// (g_lock taken by the game's main thread: how long it waited for it, for the [threads] line)
static HOST_THREAD_LOCAL int tl_is_main_host = -1;
volatile long long g_glock_main_wait_ns, g_glock_main_waits, g_turn_main_wait_ns, g_turn_main_waits;
static void glock(void) {
#ifdef _WIN32
    if (TryEnterCriticalSection(&g_lock)) return;
    if (tl_is_main_host == 1) {
        u64 t0 = hnow_ns();
        EnterCriticalSection(&g_lock);
        g_glock_main_wait_ns += (long long)(hnow_ns() - t0);
        g_glock_main_waits++;
        return;
    }
    EnterCriticalSection(&g_lock);
#else
    hmtx_lock(&g_lock);
#endif
}
static u64 g_switches = 0;
static u32 g_main_r2, g_main_r13;
static bool g_one_core = false;    // everything on one core (the old way; one_core.txt)
static GThread g_alarm_thread;     // stands in for "the current thread" inside alarm callbacks

// Which core a thread runs on.
static int tcore(const GThread* t) {
    if (g_one_core) return 1;
    if (g_native) return t->slot;
    int g = t->group & 3;
    return g < 3 ? g : (int)(t->core < 3 ? t->core : 1);
}

// The core the game thinks a thread is on (even when everything really runs
// on one PC core): per-core state like GX2 display lists is kept by this.
static int nominal_core(const GThread* t) {
    int g = t->group & 3;
    return g < 3 ? g : (int)(t->core < 3 ? t->core : 1);
}

// The calling game thread (the alarm stand-in on the timer thread).
static GThread* self_t(void) {
    if (tl_self) return tl_self;
    if (g_native) return g_main_thread && g_main_thread->state == T_RUNNING ? g_main_thread : &g_alarm_thread;
    if (g_run[1]) return g_run[1];
    return &g_alarm_thread;
}

// --- The system-call lock ---------------------------------------------------
// System calls (graphics, files, memory, ...) were written for one thread at a
// time; with the cores running at once they take turns through this lock.
// Thread/sync calls (below) have their own lock and don't take it.
static hmtx_t g_big;
static bool g_big_ready = false;
static HOST_THREAD_LOCAL int tl_big_depth = 0;
void hle_big_init(void) { if (!g_big_ready) { hmtx_init(&g_big, true); g_big_ready = true; } }
volatile long g_big_taken, g_big_waited;   // (how often the lock was taken / found held by another thread: [hle] line)
volatile long long g_big_main_wait_ns, g_big_main_waits;   // (the main thread's waits for it: [hle] line)
void hle_big_lock(void) {
    if (!g_big_ready) hle_big_init();
#ifdef _WIN32
    if (!TryEnterCriticalSection(&g_big)) {
        _InterlockedIncrement(&g_big_waited);
        if (threads_is_main()) {
            u64 t0 = hnow_ns();
            EnterCriticalSection(&g_big);
            g_big_main_wait_ns += (long long)(hnow_ns() - t0);
            g_big_main_waits++;
        } else EnterCriticalSection(&g_big);
    }
    if (threads_is_main()) g_big_taken++;
#else
    hmtx_lock(&g_big);
#endif
    tl_big_depth++;
}
void hle_big_unlock(void) { tl_big_depth--; hmtx_unlock(&g_big); }
int hle_big_release_all(void) {
    int d = tl_big_depth;
    for (int i = 0; i < d; i++) hmtx_unlock(&g_big);
    tl_big_depth = 0;
    return d;
}
void hle_big_reacquire(int d) {
    for (int i = 0; i < d; i++) hmtx_lock(&g_big);
    tl_big_depth = d;
}

// The system-call table (looked up on every call, rarely added to).
static hmtx_t g_table_lock;
static bool g_table_lock_ready = false;
void hle_table_lock(void) { if (!g_table_lock_ready) { hmtx_init(&g_table_lock, false); g_table_lock_ready = true; } hmtx_lock(&g_table_lock); }
void hle_table_unlock(void) { hmtx_unlock(&g_table_lock); }

static GThread* owner_t(void) { return tl_self ? tl_self : &g_alarm_thread; }

static GThread* find_thread(u32 obj) {
    for (int i = 0; i < g_thread_count; i++)
        if (g_threads[i]->obj == obj) return g_threads[i];
    return NULL;
}

static GThread* new_thread(u32 obj) {
    if (g_thread_count >= MAX_THREADS) return NULL;
    GThread* t = (GThread*)calloc(1, sizeof(GThread));
    t->obj = obj;
    t->prio = 16;
    t->core = 1;
    t->group = 1;
    hcnd_init(&t->turn);
    t->slot = 3 + g_thread_count;
    g_threads[g_thread_count++] = t;
    return t;
}

// Alarms are fired by the timer thread.
static u64 alarms_next_due(void);
static void alarms_fire_due(void);

void threads_report_all(FILE* out) {
    fprintf(out, "  all game threads:\n");
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        fprintf(out, "    %08X %-22s prio %2d core %d %-9s", t->obj, t->name[0] ? t->name : "(unnamed)",
                t->prio, nominal_core(t), state_name[t->state]);
        if (t->state == T_WAITING)
            fprintf(out, " on %s %08X", t->wait_what ? t->wait_what : "?", t->wait_key);
        CPUState* c = t->cpu;
        if (c) fprintf(out, "  pc=%08X lr=%08X", c->pc, c->lr);
        if (t->wait_lr) fprintf(out, " (blocked by call from %08X)", t->wait_lr);
        fprintf(out, "\n");
    }
}

void threads_report_brief(FILE* out) {
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        if (t->state == T_DONE) continue;
        fprintf(out, "    %-26s core %d %-9s", t->name[0] ? t->name : "(unnamed)", nominal_core(t), state_name[t->state]);
        if (t->state == T_WAITING) {
            fprintf(out, " on %s %08X, called from %08X", t->wait_what ? t->wait_what : "?", t->wait_key, t->wait_lr);
            // (messages queued while someone waits for one would mean a lost wake-up)
            if (t->wait_what && !strncmp(t->wait_what, "message queue", 13)) {
                u32 q = t->wait_key & ~1u;
                fprintf(out, " [%u of %u queued]", wiiu_read32(q + 0x38), wiiu_read32(q + 0x30));
            }
        }
        if (t->cpu) fprintf(out, "  pc=%08X lr=%08X", t->cpu->pc, t->cpu->lr);
        fprintf(out, "\n");
    }
}

static void report_deadlock(void) {
    fprintf(stderr, "\n[threads] every thread is waiting and nothing will wake them (deadlock):\n");
    threads_report_all(stderr);
    hle_print_trail(stderr);
    fflush(stderr);
    _Exit(4);
}

static int thread_main(void* arg);

// Let `t` run on its core now (it was chosen by the scheduler).
static void give_core(GThread* t) {
    int c = tcore(t);
    t->state = T_RUNNING;
    t->last_run = ++g_switches;
    g_run[c] = t;
    if (!t->started) {
        t->started = true;
        if (!hthrd_create(&t->host, thread_main, t, GUEST_HOST_STACK)) {
            fprintf(stderr, "[threads] couldn't start a PC thread for %08X\n", t->obj);
            _Exit(5);
        }
    } else {
        hcnd_signal(&t->turn);
    }
}

// The thread that should run on core `c` (highest priority; equal
// priorities take turns). `self` counts as runnable if it's running.
static GThread* best_on_core(int c, GThread* self) {
    GThread* best = NULL;
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        bool runnable = t->state == T_READY || (t == self && t->state == T_RUNNING);
        if (!runnable || tcore(t) != c) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->last_run < best->last_run)) best = t;
    }
    return best;
}

// Is another thread waiting for this core?
static bool other_ready_on_core(GThread* self) {
    int c = tcore(self);
    for (int i = 0; i < g_thread_count; i++)
        if (g_threads[i] != self && g_threads[i]->state == T_READY && tcore(g_threads[i]) == c) return true;
    return false;
}

// Sleeps and timeouts that are over.
static void expire_timeouts_locked(void) {
    u64 now = 0;
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        if (t->state == T_WAITING && t->wake_at) {
            if (!now) now = wiiu_time_ticks();
            if (now >= t->wake_at) { t->state = T_READY; t->timed_out = true; t->wait_key = 0; t->wake_at = 0; }
        }
    }
}

// Start threads on cores that have nothing running. A thread that may run
// on any core moves to an idle one if its own core is busy.
static void dispatch_idle_locked(void) {
    if (g_native) {   // (every ready thread runs)
        for (int i = 0; i < g_thread_count; i++)
            if (g_threads[i]->state == T_READY && g_run[g_threads[i]->slot] == NULL) give_core(g_threads[i]);
        return;
    }
    for (int c = 0; c < 3; c++) {
        if (g_run[c]) continue;
        GThread* t = best_on_core(c, NULL);
        if (!t && !g_one_core) {
            for (int i = 0; i < g_thread_count; i++) {
                GThread* m = g_threads[i];
                if (m->state != T_READY || (m->group & 3) != 3 || !g_run[tcore(m)]) continue;
                if (!t || m->prio < t->prio || (m->prio == t->prio && m->last_run < t->last_run)) t = m;
            }
            if (t) t->core = (u32)c;
        }
        if (t) give_core(t);
    }
}

// Wait (g_lock held) until the scheduler hands this thread its core again.
static void wait_for_turn(GThread* self) {
    if (tl_is_main_host == 1 && g_run[tcore(self)] != self) {   // (the main thread waiting while another game thread has its core)
        u64 t0 = hnow_ns();
        while (g_run[tcore(self)] != self) hcnd_wait(&self->turn, &g_lock);
        g_turn_main_wait_ns += (long long)(hnow_ns() - t0);
        g_turn_main_waits++;
    }
    while (g_run[tcore(self)] != self) hcnd_wait(&self->turn, &g_lock);
    self->state = T_RUNNING;
    // (DolRecomp's call-depth counter is one shared number, made for one
    // thread. With the cores running at once it's only kept from creeping
    // up -- the PC threads' big stacks make the limit unnecessary anyway.)
    dolrecomp_call_depth = g_one_core ? self->depth : 0;
    // Other threads ran meanwhile and may have written memory this thread
    // had reserved (lwarx): make its next stwcx. fail and retry, like a
    // real context switch does.
    if (self->cpu) self->cpu->reserve_valid = false;
}

// The current thread gives up its core (it's waiting, done, or taking
// turns): run whatever should run on this core now. Called with g_lock held;
// returns (with it held) once this thread runs again.
static void schedule_locked(void) {
    GThread* self = tl_self;
    if (!self) return;   // (the timer thread never waits like this)
    int c = tcore(self);
    expire_timeouts_locked();
    GThread* best = best_on_core(c, self);
    if (best == self) { self->state = T_RUNNING; dispatch_idle_locked(); return; }
    if (self->state == T_RUNNING) self->state = T_READY;
    self->depth = dolrecomp_call_depth;
    if (best) give_core(best);
    else if (g_run[c] == self) g_run[c] = NULL;
    dispatch_idle_locked();
    if (self->state == T_DONE) return;   // a finished thread never resumes
    wait_for_turn(self);
}

// Block the current thread on `key` until woken (or until `deadline`).
// Called with g_lock held. Returns false on timeout.
// (for the log: what the main thread spends its frame waiting on; the
// waits are told apart by reason and by where the game waited from)
#define MAIN_WAIT_SLOTS 16
static struct { const char* what; u32 lr; u64 ns; u32 n; } g_main_waits[MAIN_WAIT_SLOTS];
static u64 g_main_wait_total, g_main_wait_all;
u64 threads_main_waited_ns(void) { return g_main_wait_all; }
// (the longest single wait since the last call: a slow frame's [spike] line says what it was)
static struct { const char* what; u32 lr; u64 ns; } g_main_wait_max;
void threads_main_longest_wait(const char** what, u32* lr, u64* ns) {
    glock();
    *what = g_main_wait_max.what;
    *lr = g_main_wait_max.lr;
    *ns = g_main_wait_max.ns;
    g_main_wait_max.what = NULL;
    g_main_wait_max.ns = 0;
    hmtx_unlock(&g_lock);
}
static void note_main_wait(const char* what, u32 lr, u64 ns) {
    g_main_wait_total += ns;
    g_main_wait_all += ns;
    if (ns > g_main_wait_max.ns) { g_main_wait_max.what = what; g_main_wait_max.lr = lr; g_main_wait_max.ns = ns; }
    int free_slot = -1;
    for (int i = 0; i < MAIN_WAIT_SLOTS; i++) {
        if (g_main_waits[i].what == what && g_main_waits[i].lr == lr) { g_main_waits[i].ns += ns; g_main_waits[i].n++; return; }
        if (!g_main_waits[i].what && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) { g_main_waits[free_slot].what = what; g_main_waits[free_slot].lr = lr; g_main_waits[free_slot].ns = ns; g_main_waits[free_slot].n = 1; }
}
void threads_note_host_wait(const char* what, u64 ns) {
    GThread* self = tl_self;
    if (!self || strcmp(self->name, "main")) return;
    glock();
    note_main_wait(what, self->cpu ? self->cpu->lr : 0, ns);
    hmtx_unlock(&g_lock);
}
static volatile int g_quick_waits = -1;
int threads_quick_waits(void) {
    if (g_quick_waits < 0) g_quick_waits = getenv("SPLAT_SLOW_WAITS") == NULL;
    return g_quick_waits;
}
void threads_set_quick_waits(int on) { g_quick_waits = on ? 1 : 0; }
void threads_report_main_waits(u32 frames) {
    glock();
    if (frames) {
        printf("[threads] main thread inside the runtime, per frame: system-call lock %.2f ms (%.1f waits), scheduler lock %.2f ms (%.1f waits), its core busy with another game thread %.2f ms (%.1f times)\n",
               g_big_main_wait_ns / 1e6 / frames, (double)g_big_main_waits / frames, g_glock_main_wait_ns / 1e6 / frames, (double)g_glock_main_waits / frames,
               g_turn_main_wait_ns / 1e6 / frames, (double)g_turn_main_waits / frames);
        g_big_main_wait_ns = g_big_main_waits = g_glock_main_wait_ns = g_glock_main_waits = g_turn_main_wait_ns = g_turn_main_waits = 0;
        printf("[threads] main thread waited %.1f ms per frame:", g_main_wait_total / 1e6 / frames);
        for (int k = 0; k < 4; k++) {   // the 4 longest
            int best = -1;
            for (int i = 0; i < MAIN_WAIT_SLOTS; i++)
                if (g_main_waits[i].what && g_main_waits[i].n && (best < 0 || g_main_waits[i].ns > g_main_waits[best].ns)) best = i;
            if (best < 0) break;
            printf("%s %s from %08X %.1f ms (%.1f times)", k ? "," : "", g_main_waits[best].what, g_main_waits[best].lr,
                   g_main_waits[best].ns / 1e6 / frames, (double)g_main_waits[best].n / frames);
            g_main_waits[best].n = 0;
        }
        printf("\n");
    }
    memset(g_main_waits, 0, sizeof(g_main_waits));
    g_main_wait_total = 0;
    hmtx_unlock(&g_lock);
}

static bool block_on(u32 key, const char* what, u64 deadline) {
    GThread* self = tl_self;
    if (!self) {   // an alarm callback can't really block: wait it out
        hmtx_unlock(&g_lock);
        hsleep_ns(50000);
        glock();
        return !(deadline && wiiu_time_ticks() >= deadline);
    }
    self->state = T_WAITING;
    self->wait_key = key;
    self->wait_what = what;
    self->wait_lr = self->cpu ? self->cpu->lr : 0;
    self->wake_at = deadline;
    self->timed_out = false;
    bool is_main = !strcmp(self->name, "main");
    u64 t0 = is_main ? hnow_ns() : 0;
    schedule_locked();
    if (is_main) note_main_wait(what, self->wait_lr, hnow_ns() - t0);
    return !self->timed_out;
}

static void make_ready(GThread* t) {
    t->state = T_READY;
    t->wait_key = 0;
    t->wake_at = 0;
    g_woke_someone = true;
    dispatch_idle_locked();   // an idle core it may use runs it right away
}

static void wake_one(u32 key) {
    GThread* pick = NULL;
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        if (t->state == T_WAITING && t->wait_key == key && (!pick || t->prio < pick->prio)) pick = t;
    }
    if (pick) make_ready(pick);
}

static void wake_all(u32 key) {
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        if (t->state == T_WAITING && t->wait_key == key) make_ready(t);
    }
}

static void finish_thread(GThread* t, u32 exit_value) {
    // g_lock held.
    t->state = T_DONE;
    t->exit_value = exit_value;
    wiiu_write32(t->obj + 0x334, exit_value);
    wake_all(t->obj);   // joiners wait on the thread object
    schedule_locked();
}

static int thread_main(void* arg) {
    GThread* t = (GThread*)arg;
    tl_self = t;
    tl_is_main_host = !strcmp(t->name, "main");
    { extern void nan_trap_arm_thread(void); nan_trap_arm_thread(); }   // (SPLAT_NAN_TRAP=1 only)
#ifdef _WIN32
    // The game's main thread (its logic is the frame's longest job) ahead of
    // our own threads on PCs with few cores. SPLAT_MAIN_PRIO=0: as before.
    if (!strcmp(t->name, "main")) {
        static int on = -1;
        if (on < 0) { const char* e = getenv("SPLAT_MAIN_PRIO"); on = e ? atoi(e) : 1; }
        if (on) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
#endif
    glock();
    while (g_run[tcore(t)] != t) hcnd_wait(&t->turn, &g_lock);
    dolrecomp_call_depth = 0;
    hmtx_unlock(&g_lock);

    CPUState* c = t->cpu;
    c->pc = t->entry;
    c->lr = THREAD_EXIT_ADDRESS;
    c->gpr[1] = (t->stack_top - 0x20) & ~0xFu;
    c->gpr[2] = g_main_r2;
    c->gpr[13] = g_main_r13;
    c->gpr[3] = t->argc;
    c->gpr[4] = t->argv;
    volatile int stop = 0;
    runtime_run(c, &stop);

    glock();
    finish_thread(t, c->gpr[3]);
    hmtx_unlock(&g_lock);
    return 0;
}

// The timer: wakes sleepers on idle cores, fires alarms (the console's timer
// interrupts) and notices when every thread is stuck.
static int timer_main(void* arg) {
    (void)arg;
    tl_in_alarm = true;
    int quiet = 0;
    for (;;) {
        glock();
        expire_timeouts_locked();
        dispatch_idle_locked();
        bool any_running = false;
        for (int s = 0; s < RUN_SLOTS && !any_running; s++) any_running = g_run[s] != NULL;
        u64 next_wake = ~0ull;
        bool any_ready = false;
        for (int i = 0; i < g_thread_count; i++) {
            GThread* t = g_threads[i];
            if (t->state == T_READY) any_ready = true;
            if (t->state == T_WAITING && t->wake_at && t->wake_at < next_wake) next_wake = t->wake_at;
        }
        hmtx_unlock(&g_lock);
        alarms_fire_due();
        u64 alarm = alarms_next_due();
        if (!any_running && !any_ready && next_wake == ~0ull && alarm == ~0ull) {
            if (++quiet > 2000) report_deadlock();   // (2 s with nothing that could ever wake anyone)
        } else {
            quiet = 0;
        }
        u64 now = wiiu_time_ticks();
        u64 wake = alarm < next_wake ? alarm : next_wake;
        u64 ns = wake > now ? (wake - now) * 1000000000ull / WIIU_TIMER_HZ : 0;
        if (ns > 1000000) ns = 1000000;   // look again at least every 1 ms
        if (ns < 100000) ns = 100000;
#ifdef _WIN32
        {   // A real sleep of just `ns` (0.1-1 ms): a high-resolution timer. Sleep(1)
            // took 1-2 ms, so every game-thread sleep and timeout ended up to a
            // millisecond late -- the game's 0.5 ms nap after a slow frame took ~1 ms.
            static HANDLE hr = NULL;
            static int hr_ok = -1;
            if (hr_ok < 0) {
                hr = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
                hr_ok = hr != NULL;
            }
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(ns / 100);   // (relative, 100 ns units)
            if (hr_ok && SetWaitableTimer(hr, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(hr, 5);
            else if (ns >= 500000) Sleep(1);   // (older Windows: no high-resolution timers)
            else SwitchToThread();
        }
#else
        hsleep_ns(ns);
#endif
    }
    return 0;
}

// Written by the build (runtime/atomics_state.c) once the game code's atomic
// operations were made safe for several cores; 0 when that file is missing.
#if defined(_MSC_VER)
int splat_atomics_patched_default = 0;
#pragma comment(linker, "/alternatename:splat_atomics_patched=splat_atomics_patched_default")
extern int splat_atomics_patched;
#define ATOMICS_PATCHED() (splat_atomics_patched != 0)
#else
extern int splat_atomics_patched __attribute__((weak));
#define ATOMICS_PATCHED() (&splat_atomics_patched && splat_atomics_patched)
#endif

void threads_init_main(CPUState* main_cpu, u32 stack_top) {
    hmtx_init(&g_lock, false);
    hle_big_init();
    {
        FILE* f = fopen("one_core.txt", "r");
        if (f || getenv("SPLAT_ONE_CORE")) g_one_core = true;
        if (f) fclose(f);
    }
    if (!ATOMICS_PATCHED() && !getenv("SPLAT_FORCE_CORES")) g_one_core = true;   // (SPLAT_FORCE_CORES: tests)
    g_native = !g_one_core && getenv("SPLAT_NATIVE_THREADS") != NULL;   // (tested: 52 ms a frame instead of 22 -- the game's busy threads crowd out the main one)
    printf("[threads] %s\n", g_one_core ? (ATOMICS_PATCHED() ? "all game threads on one PC core (one_core.txt)"
                                                             : "all game threads on one PC core (game code not updated for several cores)")
                                        : g_native ? "native threads: every game thread runs on its own PC thread whenever it isn't waiting"
                                                   : "game threads run on 3 PC cores at once, like the Wii U's 3 cores");
    u32 obj = hle_heap_alloc(0x6A0, 8);
    GThread* t = new_thread(obj);
    t->cpu = main_cpu;
    t->state = T_RUNNING;
    t->started = true;
    t->stack_top = stack_top;
    snprintf(t->name, sizeof(t->name), "main");
    wiiu_write32(obj + 0x320, 0x74487244u);   // 'tHrD'
    wiiu_write32(obj + 0x32C, 16);
    g_run[tcore(t)] = t;
    g_main_thread = t;
    tl_self = t;
    { extern void nan_trap_arm_thread(void); nan_trap_arm_thread(); }   // (SPLAT_NAN_TRAP=1 only)
    g_main_r2 = main_cpu->gpr[2];
    g_main_r13 = main_cpu->gpr[13];
    snprintf(g_alarm_thread.name, sizeof(g_alarm_thread.name), "(timer)");
    g_alarm_thread.core = 1;
    g_alarm_thread.group = 1;
    g_alarm_thread.prio = 0;
    hthrd_t timer;
    hthrd_create(&timer, timer_main, NULL, (size_t)64 * 1024 * 1024);
}

CPUState* threads_sample(const char** name) {
    static unsigned k = 0;
    int slots = g_native ? 3 + g_thread_count : 3;
    for (int n = 0; n < slots; n++) {
        GThread* t = g_run[(k++) % slots];
        if (!t || t->state != T_RUNNING) continue;
        if (name) *name = t->name[0] ? t->name : "(unnamed)";
        return t->cpu;
    }
    return NULL;
}

u32 threads_current_object(void) { return self_t()->obj; }
// Each game thread has a PC thread of its own (tl_self is set once, when it
// starts): its object is the same for the PC thread's whole life. 0 for PC
// threads that stand in for different game threads (the timer).
u32 threads_fixed_object(void) { return tl_self ? tl_self->obj : 0; }
bool threads_is_main(void) { return tl_self && tl_self == g_main_thread; }
CPUState* threads_current_cpu(const char** name) {   // (the NaN finder: which game thread this is)
    if (name) *name = tl_self ? tl_self->name : "(host)";
    return tl_self ? tl_self->cpu : NULL;
}
bool threads_in_alarm(void) { return tl_in_alarm; }

void threads_describe_current(FILE* out) {
    GThread* t = self_t();
    fprintf(out, "  thread: %08X \"%s\" (entry %08X, core %d)\n", t->obj, t->name[0] ? t->name : "unnamed", t->entry, nominal_core(t));
}

// A thread that should run instead of the current one *right now*: a
// higher-priority thread on the same core that's ready (or whose sleep or
// timeout just ended). Equal priorities only take turns in threads_tick.
static bool urgent_locked(GThread* self) {
    u64 now = 0;
    int c = tcore(self);
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        if (t == self || tcore(t) != c || t->prio >= self->prio) continue;
        if (t->state == T_READY) return true;
        if (t->state == T_WAITING && t->wake_at) {
            if (!now) now = wiiu_time_ticks();
            if (now >= t->wake_at) return true;
        }
    }
    return false;
}

void threads_check_preempt_after_call(CPUState* cpu) {
    if (!g_woke_someone) return;
    g_woke_someone = false;
    threads_check_preempt(cpu);
}

void threads_check_preempt(CPUState* cpu) {
    (void)cpu;
    GThread* self = tl_self;
    if (tl_in_alarm || !self || self->interrupts_off) return;
    glock();
    if (urgent_locked(self)) schedule_locked();
    hmtx_unlock(&g_lock);
}

bool threads_yield_if_spinning(CPUState* cpu) {
    (void)cpu;
    GThread* self = tl_self;
    if (tl_in_alarm || !self || self->interrupts_off) return false;
    glock();
    // A busy-wait loop is waiting for another thread. If one is waiting for
    // this core, let it have the core for a moment (even at lower priority).
    GThread* other = NULL;
    int c = tcore(self);
    for (int i = 0; i < g_thread_count; i++) {
        GThread* t = g_threads[i];
        if (t == self || t->state != T_READY || tcore(t) != c) continue;
        if (!other || t->prio < other->prio || (t->prio == other->prio && t->last_run < other->last_run)) other = t;
    }
    if (other) {
        self->state = T_READY;
        self->depth = dolrecomp_call_depth;
        give_core(other);
        wait_for_turn(self);
    }
    hmtx_unlock(&g_lock);
    if (!other) hsleep_ns(0);   // (waiting for another core: give the PC a breather)
    return other != NULL;
}

void threads_tick(CPUState* cpu) {
    (void)cpu;
    GThread* self = tl_self;
    if (tl_in_alarm || !self) return;   // alarm callbacks run to completion
    if (!g_one_core) dolrecomp_call_depth = 0;
    glock();
    if (!self->interrupts_off) {
        expire_timeouts_locked();
        if (other_ready_on_core(self)) schedule_locked();
        else dispatch_idle_locked();
    }
    hmtx_unlock(&g_lock);
}

// Which core a thread may run on: one bit set = that core, several = any.
static int affinity_group(u32 mask, int fallback) {
    switch (mask) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 0: return fallback;
    default: return 3;
    }
}

// --- OSThread API ----------------------------------------------------------
static int g_threads_logged = 0;

static void OSCreateThread(CPUState* c) {
    // (thread, entry, argc, argv, stackTop, stackSize, priority, attributes)
    u32 obj = c->gpr[3];
    glock();
    GThread* t = find_thread(obj);
    if (!t) t = new_thread(obj);
    if (!t) { hmtx_unlock(&g_lock); ret(c, 0); return; }
    t->entry = c->gpr[4];
    t->argc = c->gpr[5];
    t->argv = c->gpr[6];
    t->stack_top = c->gpr[7];
    t->prio = (s32)c->gpr[9];
    u32 attr = c->gpr[10] & 0xFF;
    t->core = (attr & 2) ? 1 : (attr & 1) ? 0 : (attr & 4) ? 2 : self_t()->core;
    t->group = affinity_group(attr & 7, self_t()->group);
    t->state = T_SUSPENDED;
    t->suspend = 1;
    t->started = false;
    if (!t->cpu) {
        t->cpu = (CPUState*)calloc(1, sizeof(CPUState));
        runtime_setup_cpu(t->cpu);
    }
    wiiu_write32(obj + 0x320, 0x74487244u);   // 'tHrD'
    wiiu_write32(obj + 0x32C, (u32)t->prio);
    wiiu_write32(obj + 0x330, (u32)t->prio);
    if (g_threads_logged++ < 32)
        printf("[threads] created thread %08X entry=%08X args=%08X,%08X prio=%d core=%u stack=%08X\n",
               obj, t->entry, t->argc, t->argv, t->prio, t->core, t->stack_top);
    hmtx_unlock(&g_lock);
    ret(c, 1);
}

static void OSResumeThread(CPUState* c) {
    glock();
    GThread* t = find_thread(c->gpr[3]);
    s32 old = t ? t->suspend : 0;
    if (t && t->suspend > 0 && --t->suspend == 0 && t->state == T_SUSPENDED) make_ready(t);
    hmtx_unlock(&g_lock);
    ret(c, (u32)old);
}

static void OSExitThread(CPUState* c) {
    glock();
    GThread* self = tl_self;
    if (!self) { hmtx_unlock(&g_lock); return; }
    finish_thread(self, c->gpr[3]);
    // This host thread is parked forever; it never runs guest code again.
    for (;;) hcnd_wait(&self->turn, &g_lock);
}

static void OSJoinThread(CPUState* c) {
    u32 obj = c->gpr[3], out = c->gpr[4];
    glock();
    GThread* t = find_thread(obj);
    while (t && t->state != T_DONE) block_on(obj, "join", 0);
    if (t && out) wiiu_write32(out, t->exit_value);
    hmtx_unlock(&g_lock);
    ret(c, t ? 1 : 0);
}

static void OSGetCurrentThread(CPUState* c) { ret(c, self_t()->obj); }
static void OSGetCoreId(CPUState* c) { ret(c, (u32)nominal_core(self_t())); }
u32 threads_current_core(void) { return (u32)nominal_core(self_t()); }
static void OSGetThreadPriority(CPUState* c) {
    GThread* t = find_thread(c->gpr[3]);
    ret(c, t ? (u32)t->prio : 16);
}
static void OSSetThreadPriority(CPUState* c) {
    GThread* t = find_thread(c->gpr[3]);
    if (t) { t->prio = (s32)c->gpr[4]; wiiu_write32(t->obj + 0x32C, (u32)t->prio); }
    ret(c, 1);
}
static void OSSetThreadAffinity(CPUState* c) {
    GThread* t = find_thread(c->gpr[3]);
    u32 m = c->gpr[4];
    if (t && m) {
        glock();
        int before = tcore(t);
        t->core = (m & 2) ? 1 : (m & 1) ? 0 : 2;
        t->group = affinity_group(m & 7, t->group);
        if (tcore(t) != before && t->state == T_READY) dispatch_idle_locked();
        hmtx_unlock(&g_lock);
    }
    ret(c, 1);
}
static void OSSetThreadName(CPUState* c) {
    GThread* t = find_thread(c->gpr[3]);
    if (t) wiiu_read_string(c->gpr[4], t->name, sizeof(t->name));
}
static void OSGetThreadSpecific(CPUState* c) {
    u32 id = c->gpr[3];
    ret(c, id < 32 ? self_t()->specific[id] : 0);
}
static void OSSetThreadSpecific(CPUState* c) {
    u32 id = c->gpr[3];
    if (id < 32) self_t()->specific[id] = c->gpr[4];
}
static void OSYieldThread(CPUState* c) {
    (void)c;
    glock();
    schedule_locked();
    hmtx_unlock(&g_lock);
}
// Sleep the current guest thread (other threads run meanwhile).
void threads_sleep_ticks(u64 ticks) {
    int big = hle_big_release_all();   // (called from system calls: let the other cores make theirs)
    glock();
    if (ticks == 0) schedule_locked();
    else block_on(0, "sleep", wiiu_time_ticks() + ticks);
    hmtx_unlock(&g_lock);
    hle_big_reacquire(big);
}

// The same, waking up on time (the vsync wait at 60 fps): the scheduler only
// looks every ~1 ms, so a wait through it ended 0.5 ms late on average (up
// to 2.5 ms) -- time the next frame then didn't have. All but the last
// 1.5 ms go through the scheduler (other threads on this core run); the rest
// is waited right here to the microsecond when nobody else wants the core.
void threads_sleep_ticks_precise(u64 ticks) {
    u64 end = wiiu_time_ticks() + ticks, tail = WIIU_TIMER_HZ / 667;   // (1.5 ms)
    if (ticks > tail) threads_sleep_ticks(ticks - tail);
    u64 now = wiiu_time_ticks();
    if (now >= end) return;
    glock();
    GThread* self = tl_self;
    bool quick = self && threads_quick_waits() && !other_ready_on_core(self);
    hmtx_unlock(&g_lock);
    if (!quick) { threads_sleep_ticks(end - now); return; }
    int big = hle_big_release_all();
    u64 t0 = hnow_ns();
    hsleep_ns((end - now) * 1000000000ull / WIIU_TIMER_HZ);
    threads_note_host_wait("short sleep", hnow_ns() - t0);
    hle_big_reacquire(big);
}

static void OSSleepTicks(CPUState* c) {
    u64 ticks = arg64(c, 3);
    static int logged = 0;
    if (logged++ < 12)
        printf("[threads] %s sleeps %llu us (called from %08X)\n", self_t()->name[0] ? self_t()->name : "thread",
               (unsigned long long)(ticks * 1000000ull / WIIU_TIMER_HZ), c->lr);
    glock();
    GThread* self = tl_self;
    if (ticks && ticks <= WIIU_TIMER_HZ / 500 && self && threads_quick_waits() && !other_ready_on_core(self)) {
        // A short nap (the game naps 0.5 ms after a slow frame) with nobody
        // else waiting for this core: sleep right here, to the microsecond.
        // (Handing the core over meant waiting for the scheduler's timer,
        // which looks every ~1 ms: the 0.5 ms nap took 1.3.)
        hmtx_unlock(&g_lock);
        u64 t0 = hnow_ns();
        hsleep_ns(ticks * 1000000000ull / WIIU_TIMER_HZ);
        threads_note_host_wait("short sleep", hnow_ns() - t0);
        return;
    }
    if (ticks == 0) schedule_locked();
    else block_on(0, "sleep", wiiu_time_ticks() + ticks);
    hmtx_unlock(&g_lock);
}
static void OSDisableInterrupts(CPUState* c) { ret(c, !self_t()->interrupts_off); self_t()->interrupts_off = true; }
static void OSRestoreInterrupts(CPUState* c) {
    u32 prev = !self_t()->interrupts_off;
    self_t()->interrupts_off = c->gpr[3] == 0;
    ret(c, prev);
}

// --- Locks (mutexes, fast mutexes, spinlocks, the C library's locks) --------
typedef struct { u32 key; GThread* owner; s32 count; s32 value; u32 mode; } Sync;
// By address, in a hash table (the game has thousands of locks and takes
// them ~50,000 times a second: a list searched from the start each time
// was a quarter of the main thread's time). Entries never move.
#define SYNC_CHUNK 1024
static Sync** g_sync_tab = NULL;   // open addressing; NULL = empty
static u32 g_sync_mask = 0, g_sync_count = 0;
static Sync* g_sync_free = NULL;   // (allocated a chunk at a time)
static u32 g_sync_free_left = 0;
static inline u32 sync_hash(u32 key) { return (key * 2654435761u) ^ (key >> 15); }
static Sync* sync_get(u32 key) {
    if (g_sync_tab) {
        for (u32 i = sync_hash(key) & g_sync_mask;; i = (i + 1) & g_sync_mask) {
            Sync* e = g_sync_tab[i];
            if (!e) break;
            if (e->key == key) return e;
        }
    }
    if ((g_sync_count + 1) * 2 > g_sync_mask + 1) {   // grow (keep it at most half full)
        u32 cap = g_sync_mask ? (g_sync_mask + 1) * 2 : 4096;
        Sync** t = (Sync**)calloc(cap, sizeof(Sync*));
        for (u32 j = 0; g_sync_tab && j <= g_sync_mask; j++) {
            Sync* e = g_sync_tab[j];
            if (!e) continue;
            u32 i = sync_hash(e->key) & (cap - 1);
            while (t[i]) i = (i + 1) & (cap - 1);
            t[i] = e;
        }
        free(g_sync_tab);
        g_sync_tab = t;
        g_sync_mask = cap - 1;
    }
    if (!g_sync_free_left) { g_sync_free = (Sync*)calloc(SYNC_CHUNK, sizeof(Sync)); g_sync_free_left = SYNC_CHUNK; }
    Sync* s = g_sync_free++;
    g_sync_free_left--;
    memset(s, 0, sizeof(*s));
    s->key = key;
    u32 i = sync_hash(key) & g_sync_mask;
    while (g_sync_tab[i]) i = (i + 1) & g_sync_mask;
    g_sync_tab[i] = s;
    g_sync_count++;
    return s;
}

static void lock_key(u32 key, const char* what) {
    glock();
    for (;;) {
        Sync* s = sync_get(key);
        if (!s->owner || s->owner == owner_t()) { s->owner = owner_t(); s->count++; break; }
        block_on(key, what, 0);
    }
    hmtx_unlock(&g_lock);
}
static bool trylock_key(u32 key) {
    glock();
    Sync* s = sync_get(key);
    bool ok = !s->owner || s->owner == owner_t();
    if (ok) { s->owner = owner_t(); s->count++; }
    hmtx_unlock(&g_lock);
    return ok;
}
static void unlock_key(u32 key) {
    glock();
    Sync* s = sync_get(key);
    if (s->owner == owner_t() && --s->count <= 0) { s->owner = NULL; s->count = 0; wake_one(key); }
    hmtx_unlock(&g_lock);
}
static void reset_key(u32 key) {
    glock();
    Sync* s = sync_get(key);
    s->owner = NULL; s->count = 0; s->value = 0; s->mode = 0;
    hmtx_unlock(&g_lock);
}

static void OSInitMutex(CPUState* c) { reset_key(c->gpr[3]); wiiu_write32(c->gpr[3], 0x6D557458u); }
static void OSLockMutex(CPUState* c) { lock_key(c->gpr[3], "mutex"); }
static void OSTryLockMutex(CPUState* c) { ret(c, trylock_key(c->gpr[3])); }
static void OSUnlockMutex(CPUState* c) { unlock_key(c->gpr[3]); }
static void OSFastMutex_Init(CPUState* c) { reset_key(c->gpr[3]); }
static void OSInitSpinLock(CPUState* c) { reset_key(c->gpr[3]); }
static void spin_acquire(CPUState* c) { lock_key(c->gpr[3], "spinlock"); ret(c, 1); }
static void spin_release(CPUState* c) { unlock_key(c->gpr[3]); ret(c, 1); }
// The game's C library (Green Hills) locks.
#define GHS_GLOBAL_LOCK 0x00000001u
static void ghsLock(CPUState* c) { (void)c; lock_key(GHS_GLOBAL_LOCK, "C library lock"); }
static void ghsUnlock(CPUState* c) { (void)c; unlock_key(GHS_GLOBAL_LOCK); }
static void ghs_flock(CPUState* c) { lock_key(c->gpr[3] | 0x80000000u, "file lock"); }
static void ghs_funlock(CPUState* c) { unlock_key(c->gpr[3] | 0x80000000u); }
static void ghs_mtx_init(CPUState* c) { reset_key(c->gpr[3]); }
static void ghs_mtx_lock(CPUState* c) { lock_key(c->gpr[3], "C library mutex"); }
static void ghs_mtx_unlock(CPUState* c) { unlock_key(c->gpr[3]); }
static void nothing(CPUState* c) { (void)c; }

// --- Conditions -------------------------------------------------------------
static void OSWaitCond(CPUState* c) {
    u32 cond = c->gpr[3], mutex = c->gpr[4];
    glock();
    Sync* m = sync_get(mutex);
    s32 saved = m->count;
    if (m->owner == owner_t()) { m->owner = NULL; m->count = 0; wake_one(mutex); }
    block_on(cond, "condition", 0);
    for (;;) {   // take the mutex back
        m = sync_get(mutex);
        if (!m->owner || m->owner == owner_t()) { m->owner = owner_t(); m->count = saved ? saved : 1; break; }
        block_on(mutex, "mutex", 0);
    }
    hmtx_unlock(&g_lock);
}
static void OSSignalCond(CPUState* c) {
    glock();
    wake_all(c->gpr[3]);
    hmtx_unlock(&g_lock);
}

// --- Events (mode 0 = manual reset, 1 = auto reset) --------------------------
static void OSInitEvent(CPUState* c) {
    glock();
    Sync* s = sync_get(c->gpr[3]);
    s->value = c->gpr[4] != 0;
    s->mode = c->gpr[5];
    wiiu_write32(c->gpr[3], 0x65566E54u);   // 'eVnT'
    hmtx_unlock(&g_lock);
}
static void event_signal(u32 key, bool all) {
    glock();
    Sync* s = sync_get(key);
    bool waiter = false;
    for (int i = 0; i < g_thread_count; i++)
        if (g_threads[i]->state == T_WAITING && g_threads[i]->wait_key == key) waiter = true;
    if (s->mode == 1) {               // auto reset: releases one waiter (or all)
        if (!waiter) s->value = 1;
        else if (all) wake_all(key);
        else wake_one(key);
    } else {                          // manual reset: stays signaled
        s->value = 1;
        wake_all(key);
    }
    hmtx_unlock(&g_lock);
}
static void OSSignalEvent(CPUState* c) { event_signal(c->gpr[3], false); }
static void OSSignalEventAll(CPUState* c) { event_signal(c->gpr[3], true); }
static void OSResetEvent(CPUState* c) {
    glock();
    sync_get(c->gpr[3])->value = 0;
    hmtx_unlock(&g_lock);
}
static bool event_wait(u32 key, u64 deadline) {
    glock();
    Sync* s = sync_get(key);
    bool ok = true;
    if (s->value) {
        if (s->mode == 1) s->value = 0;
    } else {
        ok = block_on(key, "event", deadline);
    }
    hmtx_unlock(&g_lock);
    return ok;
}
static void OSWaitEvent(CPUState* c) { event_wait(c->gpr[3], 0); }
static void OSWaitEventWithTimeout(CPUState* c) {
    u64 timeout = arg64(c, 5);
    ret(c, event_wait(c->gpr[3], wiiu_time_ticks() + (timeout ? timeout : 1)));
}

// --- Semaphores ----------------------------------------------------------------
static void OSInitSemaphore(CPUState* c) {
    glock();
    sync_get(c->gpr[3])->value = (s32)c->gpr[4];
    wiiu_write32(c->gpr[3], 0x73537048u);   // 'sSpH'
    hmtx_unlock(&g_lock);
}
static void OSWaitSemaphore(CPUState* c) {
    u32 key = c->gpr[3];
    glock();
    while (sync_get(key)->value <= 0) block_on(key, "semaphore", 0);
    Sync* s = sync_get(key);
    s32 prev = s->value--;
    hmtx_unlock(&g_lock);
    ret(c, (u32)prev);
}
static void OSSignalSemaphore(CPUState* c) {
    u32 key = c->gpr[3];
    glock();
    Sync* s = sync_get(key);
    s32 prev = s->value++;
    wake_one(key);
    hmtx_unlock(&g_lock);
    ret(c, (u32)prev);
}

// --- Message queues (state lives in the game's own struct) --------------------
// OSMessageQueue: +0x2C messages, +0x30 size, +0x34 first, +0x38 used.
static void OSInitMessageQueue(CPUState* c) {
    u32 q = c->gpr[3];
    wiiu_write32(q + 0x00, 0x6D536751u);   // 'mSgQ'
    wiiu_write32(q + 0x2C, c->gpr[4]);
    wiiu_write32(q + 0x30, c->gpr[5]);
    wiiu_write32(q + 0x34, 0);
    wiiu_write32(q + 0x38, 0);
}
static void OSSendMessage(CPUState* c) {
    u32 q = c->gpr[3], msg = c->gpr[4], flags = c->gpr[5];
    glock();
    for (;;) {
        u32 size = wiiu_read32(q + 0x30), used = wiiu_read32(q + 0x38);
        if (size && used < size) break;
        if (!(flags & 1)) { hmtx_unlock(&g_lock); ret(c, 0); return; }
        block_on(q + 1, "message queue (full)", 0);
    }
    u32 msgs = wiiu_read32(q + 0x2C), size = wiiu_read32(q + 0x30);
    u32 first = wiiu_read32(q + 0x34), used = wiiu_read32(q + 0x38);
    u32 slot;
    if (flags & 2) { first = (first + size - 1) % size; slot = first; wiiu_write32(q + 0x34, first); }   // jam
    else slot = (first + used) % size;
    for (u32 i = 0; i < 16; i += 4) wiiu_write32(msgs + slot * 16 + i, wiiu_read32(msg + i));
    wiiu_write32(q + 0x38, used + 1);
    wake_one(q);
    hmtx_unlock(&g_lock);
    ret(c, 1);
}
static void OSReceiveMessage(CPUState* c) {
    u32 q = c->gpr[3], msg = c->gpr[4], flags = c->gpr[5];
    glock();
    while (wiiu_read32(q + 0x38) == 0) {
        if (!(flags & 1)) { hmtx_unlock(&g_lock); ret(c, 0); return; }
        block_on(q, "message queue (empty)", 0);
    }
    u32 msgs = wiiu_read32(q + 0x2C), size = wiiu_read32(q + 0x30);
    u32 first = wiiu_read32(q + 0x34), used = wiiu_read32(q + 0x38);
    for (u32 i = 0; i < 16; i += 4) wiiu_write32(msg + i, wiiu_read32(msgs + first * 16 + i));
    wiiu_write32(q + 0x34, (first + 1) % size);
    wiiu_write32(q + 0x38, used - 1);
    wake_one(q + 1);
    hmtx_unlock(&g_lock);
    ret(c, 1);
}
static void OSInitThreadQueue(CPUState* c) { for (u32 i = 0; i < 16; i += 4) wiiu_write32(c->gpr[3] + i, 0); }
static void OSWaitRendezvous(CPUState* c) { ret(c, 1); }   // cores are simulated; don't wait

// --- Alarms ------------------------------------------------------------------
typedef struct { u32 obj, callback, user; u64 fire_at, period; bool active; u32 tag; bool own_args; u32 a0, a1; void (*host_fn)(void); } Alarm;
static Alarm g_alarms[256];
static int g_alarm_count = 0;
static CPUState* g_alarm_cpu = NULL;
static hmtx_t g_alarm_lock;
static bool g_alarm_lock_ready = false;

static Alarm* alarm_get(u32 obj, bool create) {
    for (int i = 0; i < g_alarm_count; i++) if (g_alarms[i].obj == obj) return &g_alarms[i];
    if (!create || g_alarm_count >= 256) return NULL;
    Alarm* a = &g_alarms[g_alarm_count++];
    memset(a, 0, sizeof(*a));
    a->obj = obj;
    return a;
}
static void alarm_lock(void) {
    if (!g_alarm_lock_ready) { hmtx_init(&g_alarm_lock, true); g_alarm_lock_ready = true; }
    hmtx_lock(&g_alarm_lock);
}

static u64 alarms_next_due(void) {
    u64 best = ~0ull;
    for (int i = 0; i < g_alarm_count; i++)
        if (g_alarms[i].active && g_alarms[i].fire_at < best) best = g_alarms[i].fire_at;
    return best;
}

static void alarms_fire_due(void) {
    if (!g_alarm_count) return;
    // Collect what's due, then run the callbacks without holding the alarm
    // lock (a callback's system calls may need locks another core holds while
    // it sets an alarm).
    typedef struct { u32 callback, a0, a1; void (*host_fn)(void); } Due;
    Due due[64];
    int n = 0;
    static u32 context = 0;
    if (!g_alarm_cpu) {   // (only the timer thread fires alarms)
        g_alarm_cpu = (CPUState*)calloc(1, sizeof(CPUState));
        runtime_setup_cpu(g_alarm_cpu);
        g_alarm_cpu->gpr[1] = (hle_heap_alloc(0x20000, 16) + 0x20000 - 0x20) & ~0xFu;
        g_alarm_cpu->gpr[2] = g_main_r2;
        g_alarm_cpu->gpr[13] = g_main_r13;
        context = hle_heap_alloc(0x400, 16);
    }
    alarm_lock();
    u64 now = wiiu_time_ticks();
    for (int i = 0; i < g_alarm_count && n < 64; i++) {
        Alarm* a = &g_alarms[i];
        if (!a->active || a->fire_at > now) continue;
        if (a->period) {
            while (a->fire_at <= now) a->fire_at += a->period;
        } else {
            a->active = false;
        }
        due[n].callback = a->callback;
        due[n].a0 = a->own_args ? a->a0 : a->obj;
        due[n].a1 = a->own_args ? a->a1 : context;
        due[n].host_fn = a->host_fn;
        n++;
    }
    hmtx_unlock(&g_alarm_lock);
    for (int i = 0; i < n; i++) {
        // Alarm callbacks run "in interrupt context": on the side, briefly.
        unsigned depth = dolrecomp_call_depth;
        dolrecomp_call_depth = 0;
        g_alarm_cpu->gpr[3] = due[i].a0;
        g_alarm_cpu->gpr[4] = due[i].a1;
        bool was_in_alarm = tl_in_alarm;
        tl_in_alarm = true;
        if (due[i].host_fn) due[i].host_fn();   // (a runtime timer: it calls game code itself, see threads_call_guest_from_timer)
        else hle_call_guest(g_alarm_cpu, due[i].callback);
        tl_in_alarm = was_in_alarm;
        dolrecomp_call_depth = depth;
    }
}

// A runtime function called on the timer thread every period (the sound
// mixer's 3 ms frame); it may call game code with threads_call_guest_from_timer.
void threads_add_host_timer(void (*fn)(void), u64 period_ticks) {
    alarm_lock();
    static u32 next_id = 0xFFFD0000u;
    Alarm* a = alarm_get(next_id++, true);
    if (a) {
        a->callback = 0;
        a->host_fn = fn;
        a->period = period_ticks;
        a->fire_at = wiiu_time_ticks() + period_ticks;
        a->active = true;
    }
    hmtx_unlock(&g_alarm_lock);
}
void threads_call_guest_from_timer(u32 fn, u32 a0, u32 a1) {
    if (!fn || !g_alarm_cpu) return;
    unsigned depth = dolrecomp_call_depth;
    dolrecomp_call_depth = 0;
    g_alarm_cpu->gpr[3] = a0;
    g_alarm_cpu->gpr[4] = a1;
    hle_call_guest(g_alarm_cpu, fn);
    dolrecomp_call_depth = depth;
}
void threads_call_guest_from_timer3(u32 fn, u32 a0, u32 a1, u32 a2) {
    if (!fn || !g_alarm_cpu) return;
    g_alarm_cpu->gpr[5] = a2;
    threads_call_guest_from_timer(fn, a0, a1);
}

void threads_add_system_timer(u32 callback, u64 period_ticks) {
    alarm_lock();
    static u32 next_id = 0xFFFF0000u;   // not a real OSAlarm in guest memory
    Alarm* a = alarm_get(next_id++, true);
    if (a) {
        a->callback = callback;
        a->period = period_ticks;
        a->fire_at = wiiu_time_ticks() + period_ticks;
        a->active = true;
    }
    hmtx_unlock(&g_alarm_lock);
}

// One-shot "call this guest function soon with these two arguments", for
// system libraries that finish requests asynchronously (fn(result, context)).
void threads_queue_callback(u32 fn, u32 a0, u32 a1) {
    if (!fn) return;
    alarm_lock();
    Alarm* a = NULL;
    for (int i = 0; i < g_alarm_count; i++)   // reuse a finished one
        if (g_alarms[i].own_args && !g_alarms[i].active) { a = &g_alarms[i]; break; }
    if (!a) {
        static u32 next_id = 0xFFFE0000u;
        a = alarm_get(next_id++, true);
    }
    if (a) {
        a->callback = fn;
        a->period = 0;
        a->own_args = true;
        a->a0 = a0;
        a->a1 = a1;
        a->fire_at = wiiu_time_ticks();
        a->active = true;
    }
    hmtx_unlock(&g_alarm_lock);
}

static void OSCreateAlarm(CPUState* c) {
    alarm_lock();
    Alarm* a = alarm_get(c->gpr[3], true);
    if (a) { a->active = false; a->period = 0; }
    wiiu_write32(c->gpr[3], 0x614C724Du);   // 'aLrM'
    hmtx_unlock(&g_alarm_lock);
    ret(c, 1);
}
static void OSSetAlarm(CPUState* c) {   // (alarm, delay ticks, callback)
    alarm_lock();
    Alarm* a = alarm_get(c->gpr[3], true);
    if (a) {
        a->fire_at = wiiu_time_ticks() + arg64(c, 5);
        a->period = 0;
        a->callback = c->gpr[7];
        a->active = true;
    }
    hmtx_unlock(&g_alarm_lock);
    ret(c, 1);
}
static void OSSetPeriodicAlarm(CPUState* c) {   // (alarm, start time, period, callback)
    alarm_lock();
    Alarm* a = alarm_get(c->gpr[3], true);
    if (a) {
        u64 start = arg64(c, 5), period = arg64(c, 7);
        u64 now = wiiu_time_ticks();
        a->fire_at = start > now ? start : now + period;
        a->period = period ? period : 1;
        a->callback = c->gpr[9];
        a->active = true;
    }
    hmtx_unlock(&g_alarm_lock);
    ret(c, 1);
}
static void OSCancelAlarm(CPUState* c) {
    alarm_lock();
    Alarm* a = alarm_get(c->gpr[3], false);
    if (a) a->active = false;
    hmtx_unlock(&g_alarm_lock);
    ret(c, 1);
}
static void OSCancelAlarms(CPUState* c) {
    alarm_lock();
    u32 tag = c->gpr[3];
    for (int i = 0; i < g_alarm_count; i++)
        if (wiiu_read32(g_alarms[i].obj + 0x10) == tag) g_alarms[i].active = false;
    hmtx_unlock(&g_alarm_lock);
}
static void OSSetAlarmUserData(CPUState* c) { wiiu_write32(c->gpr[3] + 0x38, c->gpr[4]); }
static void OSGetAlarmUserData(CPUState* c) { ret(c, wiiu_read32(c->gpr[3] + 0x38)); }

// ---------------------------------------------------------------------------
static const HleEntry g_thread_table[] = {
    { "OSCreateThread", OSCreateThread },
    { "OSResumeThread", OSResumeThread },
    { "OSExitThread", OSExitThread },
    { "OSJoinThread", OSJoinThread },
    { "OSGetCurrentThread", OSGetCurrentThread },
    { "OSGetCoreId", OSGetCoreId },
    { "OSGetThreadPriority", OSGetThreadPriority },
    { "OSSetThreadPriority", OSSetThreadPriority },
    { "OSSetThreadAffinity", OSSetThreadAffinity },
    { "OSSetThreadName", OSSetThreadName },
    { "OSGetThreadSpecific", OSGetThreadSpecific },
    { "OSSetThreadSpecific", OSSetThreadSpecific },
    { "OSYieldThread", OSYieldThread },
    { "OSSleepTicks", OSSleepTicks },
    { "OSDisableInterrupts", OSDisableInterrupts },
    { "OSRestoreInterrupts", OSRestoreInterrupts },

    { "OSInitMutex", OSInitMutex },
    { "OSInitMutexEx", OSInitMutex },
    { "OSLockMutex", OSLockMutex },
    { "OSTryLockMutex", OSTryLockMutex },
    { "OSUnlockMutex", OSUnlockMutex },
    { "OSFastMutex_Init", OSFastMutex_Init },
    { "OSFastMutex_Lock", OSLockMutex },
    { "OSFastMutex_TryLock", OSTryLockMutex },
    { "OSFastMutex_Unlock", OSUnlockMutex },
    { "OSInitSpinLock", OSInitSpinLock },
    { "OSUninterruptibleSpinLock_Acquire", spin_acquire },
    { "OSUninterruptibleSpinLock_Release", spin_release },
    { "__ghsLock", ghsLock },
    { "__ghsUnlock", ghsUnlock },
    { "__ghs_flock_file", ghs_flock },
    { "__ghs_funlock_file", ghs_funlock },
    { "__ghs_flock_ptr", nothing },
    { "__ghs_mtx_init", ghs_mtx_init },
    { "__ghs_mtx_dst", nothing },
    { "__ghs_mtx_lock", ghs_mtx_lock },
    { "__ghs_mtx_unlock", ghs_mtx_unlock },

    { "OSInitCond", nothing },
    { "OSWaitCond", OSWaitCond },
    { "OSSignalCond", OSSignalCond },
    { "OSInitEvent", OSInitEvent },
    { "OSSignalEvent", OSSignalEvent },
    { "OSSignalEventAll", OSSignalEventAll },
    { "OSResetEvent", OSResetEvent },
    { "OSWaitEvent", OSWaitEvent },
    { "OSWaitEventWithTimeout", OSWaitEventWithTimeout },
    { "OSInitSemaphore", OSInitSemaphore },
    { "OSWaitSemaphore", OSWaitSemaphore },
    { "OSSignalSemaphore", OSSignalSemaphore },
    { "OSInitMessageQueue", OSInitMessageQueue },
    { "OSSendMessage", OSSendMessage },
    { "OSReceiveMessage", OSReceiveMessage },
    { "OSInitThreadQueue", OSInitThreadQueue },
    { "OSInitRendezvous", nothing },
    { "OSWaitRendezvous", OSWaitRendezvous },

    { "OSCreateAlarm", OSCreateAlarm },
    { "OSSetAlarm", OSSetAlarm },
    { "OSSetPeriodicAlarm", OSSetPeriodicAlarm },
    { "OSCancelAlarm", OSCancelAlarm },
    { "OSCancelAlarms", OSCancelAlarms },
    { "OSSetAlarmUserData", OSSetAlarmUserData },
    { "OSGetAlarmUserData", OSGetAlarmUserData },
};

bool threads_is_own_call(void (*fn)(CPUState*)) {
    for (size_t i = 0; i < sizeof(g_thread_table) / sizeof(g_thread_table[0]); i++)
        if ((void (*)(CPUState*))g_thread_table[i].fn == fn) return true;
    return false;
}

const HleEntry* threads_hle_table(size_t* count) {
    *count = sizeof(g_thread_table) / sizeof(g_thread_table[0]);
    return g_thread_table;
}
