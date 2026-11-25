// Tiny portable wrapper for PC threads, locks and condition variables
// (Windows API on Windows, pthreads elsewhere). We set each thread's stack
// size ourselves because recompiled code calls very deep.
#ifndef HOST_THREAD_H
#define HOST_THREAD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// Per-PC-thread variables.
#ifdef _MSC_VER
#define HOST_THREAD_LOCAL __declspec(thread)
#else
#define HOST_THREAD_LOCAL _Thread_local
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
typedef CRITICAL_SECTION hmtx_t;     // always recursive on Windows
typedef CONDITION_VARIABLE hcnd_t;
typedef HANDLE hthrd_t;
static inline void hmtx_init(hmtx_t* m, bool recursive) { (void)recursive; InitializeCriticalSection(m); }
static inline void hmtx_lock(hmtx_t* m) { EnterCriticalSection(m); }
static inline void hmtx_unlock(hmtx_t* m) { LeaveCriticalSection(m); }
static inline void hcnd_init(hcnd_t* c) { InitializeConditionVariable(c); }
static inline void hcnd_wait(hcnd_t* c, hmtx_t* m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void hcnd_signal(hcnd_t* c) { WakeConditionVariable(c); }
static inline void hcnd_broadcast(hcnd_t* c) { WakeAllConditionVariable(c); }
typedef struct { int (*fn)(void*); void* arg; } hthrd_start_;
static unsigned __stdcall hthrd_tramp_(void* p) {
    hthrd_start_ s = *(hthrd_start_*)p;
    free(p);
    return (unsigned)s.fn(s.arg);
}
static inline bool hthrd_create(hthrd_t* t, int (*fn)(void*), void* arg, size_t stack) {
    hthrd_start_* s = (hthrd_start_*)malloc(sizeof(*s));
    s->fn = fn; s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack, hthrd_tramp_, s,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    *t = (HANDLE)h;
    return h != 0;
}
// Windows' Sleep() can't wait less than a millisecond (often 1-2 ms), which
// made every short wait (the GPU thread waiting for the next command, the
// game waiting for the GPU) far too long. Short waits now spin politely.
// (SPLAT_NAP_TIMER=1: 0.15-2 ms naps on a high-resolution timer. Measured in
// the plaza: the game's 0.5 ms nap took 1.4 ms that way, 1.1 ms with the
// yield loop below -- so off by default.)
static inline int hsleep_timer_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SPLAT_NAP_TIMER") != NULL;
    return on;
}
static inline HANDLE hsleep_hr_timer_(void) {   // (this thread's high-resolution timer; NULL on older Windows)
    static __declspec(thread) HANDLE hr = NULL;
    static __declspec(thread) int hr_ok = -1;
    if (hr_ok < 0) {
        hr = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        hr_ok = hr != NULL;
    }
    return hr;
}
static inline void hsleep_ns(uint64_t ns) {
    if (ns >= 2000000u) {
        // 2 ms and up (the game's own frame nap, ~14 ms): a high-resolution
        // timer too. Sleep() follows the system's timer steps, which Windows 11
        // widens to 15.6 ms for a program it thinks nobody is watching (a
        // window on another screen, behind others): the game's 14 ms nap then
        // took up to 31 ms. SPLAT_OLD_SLEEP=1: Sleep() as before.
        static int old = -1;
        if (old < 0) old = getenv("SPLAT_OLD_SLEEP") != NULL;
        HANDLE hr = old ? NULL : hsleep_hr_timer_();
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ns / 100);
        if (hr && SetWaitableTimer(hr, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(hr, (DWORD)(ns / 1000000u) + 20); return; }
        Sleep((DWORD)(ns / 1000000u));
        return;
    }
    if (ns >= 150000u && hsleep_timer_on()) {
        // 0.15-2 ms: a high-resolution timer (wakes within ~0.1 ms). Yielding
        // in a loop (SwitchToThread) handed the core to whoever was ready and
        // got it back a time slice later: the game's 0.5 ms nap took 1.1 ms.
        HANDLE hr = hsleep_hr_timer_();
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ns / 100);
        if (hr && SetWaitableTimer(hr, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(hr, 5); return; }
    }
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    uint64_t ticks = ns * (uint64_t)f.QuadPart / 1000000000u;
    for (;;) {
        QueryPerformanceCounter(&t);
        if ((uint64_t)(t.QuadPart - t0.QuadPart) >= ticks) break;
        if (!SwitchToThread()) YieldProcessor();
    }
}
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t hmtx_t;
typedef pthread_cond_t hcnd_t;
typedef pthread_t hthrd_t;
static inline void hmtx_init(hmtx_t* m, bool recursive) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (recursive) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void hmtx_lock(hmtx_t* m) { pthread_mutex_lock(m); }
static inline void hmtx_unlock(hmtx_t* m) { pthread_mutex_unlock(m); }
static inline void hcnd_init(hcnd_t* c) { pthread_cond_init(c, NULL); }
static inline void hcnd_wait(hcnd_t* c, hmtx_t* m) { pthread_cond_wait(c, m); }
static inline void hcnd_signal(hcnd_t* c) { pthread_cond_signal(c); }
static inline void hcnd_broadcast(hcnd_t* c) { pthread_cond_broadcast(c); }
typedef struct { int (*fn)(void*); void* arg; } hthrd_start_;
static void* hthrd_tramp_(void* p) {
    hthrd_start_ s = *(hthrd_start_*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
static inline bool hthrd_create(hthrd_t* t, int (*fn)(void*), void* arg, size_t stack) {
    hthrd_start_* s = (hthrd_start_*)malloc(sizeof(*s));
    s->fn = fn; s->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(t, &a, hthrd_tramp_, s);
    pthread_attr_destroy(&a);
    return r == 0;
}
static inline void hsleep_ns(uint64_t ns) {
    struct timespec d = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
    nanosleep(&d, NULL);
}
#endif

// 64-bit atomics shared between PC threads (acquire loads, release stores).
#ifdef _MSC_VER
#include <intrin.h>
static inline uint64_t hatomic_load64(volatile uint64_t* p) {
#if defined(_M_X64) || defined(_M_AMD64)
    // (an aligned 64-bit read is whole on x64, and x64 never moves later
    // reads ahead of it: a plain read is the acquire load. The locked
    // compare-exchange this was made the reading core take the value's
    // cache line away from the writer every time.)
    uint64_t v = *p;
    _ReadWriteBarrier();
    return v;
#else
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64*)p, 0, 0);
#endif
}
static inline void hatomic_store64(volatile uint64_t* p, uint64_t v) {
    _InterlockedExchange64((volatile __int64*)p, (__int64)v);
}
static inline uint64_t hatomic_add64(volatile uint64_t* p, uint64_t v) {   // returns the old value
    return (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)p, (__int64)v);
}
static inline bool hatomic_cas64(volatile uint64_t* p, uint64_t expect, uint64_t v) {   // *p = v if it was expect
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64*)p, (__int64)v, (__int64)expect) == expect;
}
#else
static inline uint64_t hatomic_load64(volatile uint64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void hatomic_store64(volatile uint64_t* p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint64_t hatomic_add64(volatile uint64_t* p, uint64_t v) { return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL); }
static inline bool hatomic_cas64(volatile uint64_t* p, uint64_t expect, uint64_t v) {
    return __atomic_compare_exchange_n(p, &expect, v, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

// A steady clock in nanoseconds (for timing the renderer's work).
#ifdef _WIN32
static inline uint64_t hnow_ns(void) {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
static inline uint64_t hnow_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
#endif

#endif
