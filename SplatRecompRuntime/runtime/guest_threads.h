// Guest threads for the Wii U runtime.
//
// Every Wii U thread the game creates gets its own host (PC) thread with its
// own stack, so recompiled code can call as deep as it likes. Only one guest
// thread runs at a time: they hand control to each other at blocking calls
// (waiting on a message, event, lock, sleep...) and every so often while
// running, like a single-core OS scheduler. That keeps things simple and
// deterministic while still letting the game's threads all make progress.
#ifndef GUEST_THREADS_H
#define GUEST_THREADS_H

#include "cpu/cpu.h"
#include "hle.h"
#include <stdio.h>

// Set up the main thread (the one that runs the game's entry point).
void threads_init_main(CPUState* main_cpu, u32 stack_top);

// Called regularly from the run loop: fires due alarms and lets other
// threads run. Must not be called while holding scheduler state.
void threads_tick(CPUState* cpu);

// The current thread seems to be busy-waiting: let other ready threads run
// now (returns true if it did).
bool threads_yield_if_spinning(CPUState* cpu);

// Cheap check (every system call / often): switch only if a higher-priority
// thread on this core needs the CPU now, like a timer interrupt would.
void threads_check_preempt(CPUState* cpu);
// After a system call: the same check, but only if the call woke a thread.
void threads_check_preempt_after_call(CPUState* cpu);

// The system-call lock: game threads run on three PC threads at once (one
// per Wii U core); system calls other than the thread/sync ones take turns
// through this (recursive) lock. Code that waits inside a system call
// releases it meanwhile (release_all / reacquire).
void hle_big_init(void);
void hle_big_lock(void);
void hle_big_unlock(void);
int hle_big_release_all(void);
void hle_big_reacquire(int depth);
void hle_table_lock(void);
void hle_table_unlock(void);
// Is this one of the thread/sync calls (which do their own locking)?
bool threads_is_own_call(void (*fn)(CPUState*));

// HLE functions for threads, sync objects and alarms.
const HleEntry* threads_hle_table(size_t* count);

// The current guest thread's OSThread address (for OSGetCurrentThread).
u32 threads_current_object(void);
u32 threads_fixed_object(void);   // the same for a game thread's own PC thread (0 elsewhere): may be cached per PC thread

// The core (0-2) the running guest thread is on.
u32 threads_current_core(void);

// Put the current guest thread to sleep for `ticks` (others run meanwhile).
void threads_sleep_ticks(u64 ticks);
void threads_sleep_ticks_precise(u64 ticks);   // the same, waking on time (the last 1.5 ms waited precisely)

// Is the calling PC thread the game's main thread?
bool threads_is_main(void);
// Is this the timer thread (alarm callbacks, the sound engine's 3 ms callback)?
bool threads_in_alarm(void);
// Log what the main thread waited on (per frame, over `frames` frames) and start over.
void threads_report_main_waits(u32 frames);
// A wait done without leaving the scheduler (the thread kept its core), for that log.
void threads_note_host_wait(const char* what, u64 ns);
// Short waits done in place, keeping the core (on unless SPLAT_SLOW_WAITS is set).
int threads_quick_waits(void);
void threads_set_quick_waits(int on);
// All the time the main thread has spent waiting (blocked) so far, in ns.
u64 threads_main_waited_ns(void);

// Call guest function `callback` every `period_ticks`, like a hardware
// interrupt would (used for the audio frame callback).
void threads_add_system_timer(u32 callback, u64 period_ticks);
// A runtime function called on the timer thread every period; inside it,
// threads_call_guest_from_timer(fn, r3, r4) calls game code (as an alarm would).
void threads_add_host_timer(void (*fn)(void), u64 period_ticks);
void threads_call_guest_from_timer(u32 fn, u32 a0, u32 a1);
void threads_call_guest_from_timer3(u32 fn, u32 a0, u32 a1, u32 a2);   // (r3, r4, r5)

// Call guest function fn(a0, a1) soon, once (asynchronous system results).
void threads_queue_callback(u32 fn, u32 a0, u32 a1);

// For the profiler: the running guest thread's CPU state and name, or NULL
// when no game thread is running (all waiting).
CPUState* threads_sample(const char** name);

// Print which guest thread is running (for crash reports).
void threads_describe_current(FILE* out);

// One line per live thread: state and what it waits on.
void threads_report_brief(FILE* out);

// List every guest thread and what it's doing (for crash reports).
void threads_report_all(FILE* out);

// Provided by main.c: prepare a fresh CPUState for a new guest thread,
// and run guest code on it until *stop is set (or it gets stuck).
void runtime_setup_cpu(CPUState* cpu);
bool runtime_run(CPUState* cpu, volatile int* stop);

// Address a thread's entry function returns to when it finishes.
#define THREAD_EXIT_ADDRESS 0xC1FFFF10u

// Wii U time base: ticks per second.
#define WIIU_TIMER_HZ 62156250ull
u64 wiiu_time_ticks(void);   // ticks since 2000-01-01 (the Wii U epoch)

#endif
