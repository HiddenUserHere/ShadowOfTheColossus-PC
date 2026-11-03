/* sotc: SOTC_DET_CLOCK, the guest's virtual clock for deterministic runs.
 *
 * Virtual time = frame * frame_ns + intra. frame is the guest flip count (bbgpu_flip_count, the
 * same counter the flip pacing and the frame reports use). intra advances by DET_STEP_NS on each
 * guest read and by the length of each sleep, and is clamped below the next frame boundary, so a
 * frame never ends early however fast the host runs. The clamp is released only when the flip
 * count has not moved for SOTC_DET_STALL_MS of host time (a thread that polls the clock for the
 * next frame would otherwise spin forever: only a flip ends the frame).
 *
 * Timed waits (condvars, event flags, semaphores, mutex and rwlock timeouts) expire on the
 * virtual deadline, so the outcome of a wait follows the frames, not the host scheduler. They are
 * re-checked every DET_WAIT_SLICE_NS of host time. SOTC_DET_WAIT_CAP_MS bounds each wait in host
 * time: a wait whose frames stopped (the thread that flips is itself blocked) expires there
 * instead of hanging. Forced expiries are counted and reported.
 *
 * Off (the default): every function here returns at once from one cached flag test or is not called. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Wall clock of the guest starts at 2015-01-01 00:00:00 UTC, so every run reads the same dates. */
#define DET_EPOCH_SEC INT64_C(1420070400)
#define DET_EPOCH_NS (DET_EPOCH_SEC * INT64_C(1000000000))
#define DET_STEP_NS UINT64_C(1000)
#define DET_WAIT_SLICE_NS UINT64_C(1000000)
#define DET_NS_PER_SEC UINT64_C(1000000000)

static pthread_once_t config_once_flag = PTHREAD_ONCE_INIT;
static _Atomic int det_on = -1;
static uint64_t frame_ns = 1000000000 / 30;
static uint64_t stall_ns = 50 * 1000000;
static uint64_t wait_cap_ns = 10000ull * 1000000;

static _Atomic uint64_t det_last;           /* last virtual time handed out, global and monotonic */
static _Atomic uint64_t seen_frame = UINT64_MAX;
static _Atomic uint64_t seen_real;          /* host time when seen_frame was first observed */
static _Atomic uint64_t forced_expiries, stall_releases;

static uint64_t env_u64(const char *name, uint64_t fallback, uint64_t lo, uint64_t hi) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    if (end == v || n < lo) return fallback;
    return n > hi ? hi : (uint64_t)n;
}
static void config_read(void) {
    const char *on = getenv("SOTC_DET_CLOCK");
    const int enabled = on && *on && strcmp(on, "0") != 0;
    if (enabled) {
        frame_ns = DET_NS_PER_SEC / env_u64("SOTC_DET_CLOCK_HZ", 30, 1, 1000);
        stall_ns = env_u64("SOTC_DET_STALL_MS", 50, 1, 60000) * 1000000;
        wait_cap_ns = env_u64("SOTC_DET_WAIT_CAP_MS", 10000, 1, 3600000) * 1000000;
        printf("Runtime: SOTC_DET_CLOCK: guest time is virtual, %llu Hz frames\n",
               (unsigned long long)(DET_NS_PER_SEC / frame_ns));
    }
    atomic_store_explicit(&det_on, enabled ? 1 : 0, memory_order_release);
}
int runtime_det_clock_on(void) {
    int v = atomic_load_explicit(&det_on, memory_order_acquire);
    if (v < 0) {
        pthread_once(&config_once_flag, config_read);
        v = atomic_load_explicit(&det_on, memory_order_acquire);
    }
    return v;
}

static uint64_t host_mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * DET_NS_PER_SEC + (uint64_t)t.tv_nsec;
}
static uint64_t frame_index(void) { return bbgpu_flip_count(); }

/* One step of the clock: a guest read (amount DET_STEP_NS) or a sleep (amount = its length).
 * Returns the new virtual time. Lock free; never moves backwards. */
static uint64_t det_advance(uint64_t amount) {
    const uint64_t frame = frame_index();
    const uint64_t base = frame * frame_ns;
    const uint64_t limit = base + frame_ns - 1;
    const uint64_t real = host_mono_ns();
    if (atomic_load_explicit(&seen_frame, memory_order_relaxed) != frame) {
        atomic_store_explicit(&seen_real, real, memory_order_relaxed);
        atomic_store_explicit(&seen_frame, frame, memory_order_relaxed);
    }
    const uint64_t since = atomic_load_explicit(&seen_real, memory_order_relaxed);
    const int escape = real > since && real - since > stall_ns;
    uint64_t t = atomic_load_explicit(&det_last, memory_order_relaxed), next;
    do {
        const uint64_t cur = t < base ? base : t; /* a new frame starts at its own boundary */
        uint64_t want = cur + amount;
        if (want > limit && !escape) want = cur < limit ? limit : cur;
        else if (want > limit && cur < limit) atomic_fetch_add_explicit(&stall_releases, 1, memory_order_relaxed);
        next = want;
    } while (!atomic_compare_exchange_weak_explicit(&det_last, &t, next, memory_order_acq_rel, memory_order_relaxed));
    return next;
}
static uint64_t det_peek(void) {
    const uint64_t base = frame_index() * frame_ns;
    const uint64_t t = atomic_load_explicit(&det_last, memory_order_acquire);
    return t < base ? base : t;
}

uint64_t runtime_det_now_ns(void) { return det_advance(DET_STEP_NS); }
/* SOTC_DET_CLOCK_HOST_RT=1 (diagnostic): the wall clock stays the host's (dates are not virtual). */
uint64_t runtime_det_realtime_ns(void) {
    static int host_rt = -1;
    if (host_rt < 0) host_rt = getenv("SOTC_DET_CLOCK_HOST_RT") != NULL;
    if (host_rt) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
    }
    return (uint64_t)DET_EPOCH_NS + det_advance(DET_STEP_NS);
}
void runtime_det_advance_ns(uint64_t ns) { det_advance(ns); }
uint64_t runtime_det_tsc(void) { return det_advance(DET_STEP_NS); }
uint64_t runtime_det_tsc_frequency(void) { return DET_NS_PER_SEC; }
uint64_t runtime_det_from_unix_ns(uint64_t unix_ns) {
    return unix_ns > (uint64_t)DET_EPOCH_NS ? unix_ns - (uint64_t)DET_EPOCH_NS : 0;
}

/* ---- timed waits ---- */
static void ts_from_ns(struct timespec *t, uint64_t ns) {
    t->tv_sec = (time_t)(ns / DET_NS_PER_SEC);
    t->tv_nsec = (long)(ns % DET_NS_PER_SEC);
}
static uint64_t ts_to_ns(const struct timespec *t) {
    return (uint64_t)t->tv_sec * DET_NS_PER_SEC + (uint64_t)t->tv_nsec;
}
static uint64_t clock_ns(clockid_t clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    return ts_to_ns(&t);
}
static void real_end_after(RuntimeWait *w, uint64_t host_ns) {
    ts_from_ns(&w->real_end, clock_ns(w->clock) + host_ns);
}
void runtime_wait_relative(RuntimeWait *w, clockid_t clock, uint64_t timeout_us) {
    w->clock = clock;
    w->det = runtime_det_clock_on();
    if (w->det) {
        w->vend_ns = det_peek() + timeout_us * 1000;
        real_end_after(w, wait_cap_ns);
    } else {
        real_end_after(w, timeout_us * 1000);
    }
}
void runtime_wait_absolute(RuntimeWait *w, clockid_t clock, const struct timespec *guest_abs) {
    w->clock = clock;
    w->det = runtime_det_clock_on();
    if (w->det) {
        w->vend_ns = runtime_det_from_unix_ns(ts_to_ns(guest_abs));
        real_end_after(w, wait_cap_ns);
    } else {
        w->real_end = *guest_abs;
    }
}
int runtime_wait_timed(RuntimeWait *w, int (*attempt)(void *ctx, const struct timespec *end), void *ctx) {
    if (!w->det) return attempt(ctx, &w->real_end);
    for (;;) {
        struct timespec slice;
        ts_from_ns(&slice, clock_ns(w->clock) + DET_WAIT_SLICE_NS);
        if (ts_to_ns(&w->real_end) < ts_to_ns(&slice)) slice = w->real_end;
        const int e = attempt(ctx, &slice);
        if (e != ETIMEDOUT) return e;
        if (det_peek() >= w->vend_ns) return ETIMEDOUT;
        if (clock_ns(w->clock) >= ts_to_ns(&w->real_end)) {
            atomic_fetch_add_explicit(&forced_expiries, 1, memory_order_relaxed);
            return ETIMEDOUT;
        }
    }
}
typedef struct { pthread_cond_t *cv; pthread_mutex_t *m; } CondAttempt;
static int attempt_cond(void *ctx, const struct timespec *end) {
    CondAttempt *c = ctx;
    return pthread_cond_timedwait(c->cv, c->m, end);
}
static int attempt_mutex(void *ctx, const struct timespec *end) {
    return pthread_mutex_timedlock(ctx, end);
}
/* Condvar wait with the mutex held. 0 on a signal (or a spurious wakeup, as POSIX allows),
 * ETIMEDOUT when the guest timeout expired. */
int runtime_wait_cond(RuntimeWait *w, pthread_cond_t *cv, pthread_mutex_t *m) {
    CondAttempt c = {cv, m};
    return runtime_wait_timed(w, attempt_cond, &c);
}
int runtime_wait_mutex(RuntimeWait *w, pthread_mutex_t *m) {
    return runtime_wait_timed(w, attempt_mutex, m);
}
uint32_t runtime_wait_remaining_us(const RuntimeWait *w) {
    uint64_t now, end;
    if (w->det) {
        now = det_peek();
        end = w->vend_ns;
    } else {
        now = clock_ns(w->clock);
        end = ts_to_ns(&w->real_end);
    }
    if (end <= now) return 0;
    const uint64_t us = (end - now) / 1000;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

void runtime_det_report(void) {
    if (!runtime_det_clock_on()) return;
    printf("Runtime: SOTC_DET_CLOCK: frames=%llu virtual=%.3f s, forced wait expiries=%llu, stall releases=%llu\n",
           (unsigned long long)frame_index(), det_peek() / 1e9,
           (unsigned long long)atomic_load(&forced_expiries), (unsigned long long)atomic_load(&stall_releases));
}
