/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Windows: a pagefile-backed section plays the memfd's part, and the PS4 user range is
 * reserved as one placeholder that views of the section replace (VirtualAlloc2/MapViewOfFile3,
 * as shadPS4 does); see the "host mappings" section below.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include "runtime_memory_chunks.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). A PS4 Pro (SOTC_NEO=1) gives games
 * 640 MiB more (shadPS4: 5888 vs 5248 MiB in total); with the base size the game's 64 KiB page
 * bitmap filled up when a save loaded in Pro mode and its allocator read past it (guest offset
 * 0x1a371, read of -1). */
int runtime_sotc_neo(void);
static uint64_t pool_size_mb;
static uint64_t pool_base_mb(void) { return runtime_sotc_neo() ? 5056 + 640 : 5056; }
static uint64_t pool_size_bytes(void) {
    if (!pool_size_mb) {
        const char *env = getenv("BB_DMEM_MB");
        const uint64_t base = pool_base_mb();
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < base || mb > 16384) mb = base;
        pool_size_mb = mb;
    }
    return pool_size_mb * 1024 * 1024;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define SYSTEM_LOW_MIN UINT64_C(0x0700000000)
#define SYSTEM_LOW_MAX UINT64_C(0x0800000000)
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* sotc: counts the lock_backoff iterations that reached Sleep(1); read and reset by
 * runtime_memory_lock_sleeps. Stays 0 on the Linux path, which never sleeps here. */
static uint64_t lock_sleeps;
uint64_t runtime_memory_lock_sleeps(void) { return __atomic_exchange_n(&lock_sleeps,0,__ATOMIC_RELAXED); }
/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
/* sotc: which functions readers waited in and writers held the lock in (lock sites report). */
typedef struct { const char *name; uint64_t count, ticks; } LockSite;
#define LOCK_SITES 32
static LockSite read_sites[LOCK_SITES] __attribute__((unused)), write_sites[LOCK_SITES] __attribute__((unused));
static __thread const char *lock_site_name, *lock_write_site;
__attribute__((unused)) static void lock_site_add(LockSite *sites, const char *name, uint64_t ticks) {
    if (!name) return;
    for (int i=0;i<LOCK_SITES;++i) {
        const char *n=__atomic_load_n(&sites[i].name,__ATOMIC_ACQUIRE);
        if (!n) {
            const char *expected=NULL;
            if (!__atomic_compare_exchange_n(&sites[i].name,&expected,name,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) && expected!=name) continue;
            n=name;
        }
        if (n==name) {
            __atomic_add_fetch(&sites[i].count,1,__ATOMIC_RELAXED);
            __atomic_add_fetch(&sites[i].ticks,ticks,__ATOMIC_RELAXED);
            return;
        }
    }
}
#ifdef _WIN32
/* sotc: winpthreads' rwlock queues new readers behind a waiting writer, so a nested read (a
 * reader faulting into runtime_memory_gpu_protect) deadlocked against a mapping thread
 * waiting to write: the GPU command processor, two job workers in the fault handler and
 * sceKernelBatchMap2 all stopped. Each read also took a mutex, thousands of times per frame.
 * This lock prefers readers like glibc's: a read is one atomic operation and never waits for
 * a waiting writer; a writer waits until no reader holds it (maps and unmaps are rare). */
static volatile int64_t lock_state; /* readers, or -1 while a writer holds it */
static void lock_backoff(unsigned *spins) {
    if (++*spins < 64) YieldProcessor();
    else if (*spins < 256) SwitchToThread();
    else { __atomic_add_fetch(&lock_sleeps,1,__ATOMIC_RELAXED); Sleep(1); } /* Sleep(0) only yields to equal or higher priority: a preempted lower-priority holder could wait seconds */
}
/* sotc: what the writers wait for and hold (maps, unmaps, protection changes): loading a new
 * area stalled with every thread around this table; runtime_memory_lock_stats reports it. */
static uint64_t lock_writes, lock_wait_ticks, lock_hold_ticks, lock_worst_wait_ticks, lock_worst_hold_ticks;
static uint64_t lock_read_wait_ticks, lock_read_waits;
static __thread uint64_t lock_held_since;
static uint64_t lock_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
/* sotc: SOTC_LOCK_WAKE=0 goes back to Sleep(1) for readers that find a writer. A WPR trace of an
 * area change: the draw thread waited 0.9 s of 1.9 s here, about 1.2 ms per wait for writes that
 * held the lock 0.1 ms on average (Sleep(1) sleeps a whole timer tick). Readers now block on the
 * lock word and the writer wakes them as it lets go; the 2 ms timeout only bounds a missed wake. */
static int lock_wake(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_LOCK_WAKE"); v=!(e && e[0]=='0'); }
    return v;
}
static volatile LONG lock_sleepers;
/* sotc: SOTC_LOCK_WATCH=1 (or ring modes 6, 7): the holder of the table lock, for the watch lines below.
 * Written by the exclusive holder; read without the lock (diagnostics only). */
static volatile LONG lock_writer_tid;  /* thread id of the exclusive holder, 0 when none */
static const char *lock_last_write;    /* site of the last write lock taken */
int sotc_pc_ring_full6(void) __attribute__((weak));
void runtime_memory_lock_watch_text(char *out, size_t capacity);
static int lock_watch_on(void) {
    static int v=-1;
    if (v<0) {
        const char *e=getenv("SOTC_LOCK_WATCH");
        v=(e && e[0]=='1') || (sotc_pc_ring_full6 && sotc_pc_ring_full6());
    }
    return v;
}
/* sotc: a table lock wait that passes 2 s is reported once per thread, with the holder's state. */
static __thread int lock_watch_reported;
static void lock_watch_check(uint64_t start, const char *what) {
    static uint64_t freq;
    if (lock_watch_reported) return;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq=(uint64_t)f.QuadPart; }
    if (lock_ticks()-start <= 2*freq) return;
    lock_watch_reported=1;
    char state[192];
    runtime_memory_lock_watch_text(state,sizeof state);
    fprintf(stderr,"SOTC LOCK WATCH: tid %lu waits table %s lock for 2 s; table: %s\n",
            (unsigned long)GetCurrentThreadId(),what,state);
}
static void rw_rdlock(void) {
    int64_t s=__atomic_load_n(&lock_state,__ATOMIC_RELAXED);
    if (s>=0 && __atomic_compare_exchange_n(&lock_state,&s,s+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
    const uint64_t start=lock_ticks();
    const int watch=lock_watch_on();
    for (unsigned spins=0;;) {
        s=__atomic_load_n(&lock_state,__ATOMIC_RELAXED);
        if (s>=0 && __atomic_compare_exchange_n(&lock_state,&s,s+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
        if (watch) lock_watch_check(start,"read");
        if (s<0 && spins>=64 && lock_wake()) {
            __atomic_add_fetch(&lock_sleepers,1,__ATOMIC_SEQ_CST);
            int64_t held=-1;
            WaitOnAddress((volatile void *)&lock_state,&held,sizeof(held),2);
            __atomic_sub_fetch(&lock_sleepers,1,__ATOMIC_SEQ_CST);
        } else {
            lock_backoff(&spins);
        }
    }
    const uint64_t waited=lock_ticks()-start;
    __atomic_add_fetch(&lock_read_wait_ticks,waited,__ATOMIC_RELAXED);
    __atomic_add_fetch(&lock_read_waits,1,__ATOMIC_RELAXED);
    lock_site_add(read_sites,lock_site_name,waited);
}
static void rw_rdunlock(void) { __atomic_sub_fetch(&lock_state,1,__ATOMIC_RELEASE); }
static void rw_wrlock(void) {
    const uint64_t start=lock_ticks();
    const int watch=lock_watch_on();
    for (unsigned spins=0;;lock_backoff(&spins)) {
        int64_t s=0;
        if (__atomic_compare_exchange_n(&lock_state,&s,-1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
        if (watch) lock_watch_check(start,"write");
    }
    __atomic_store_n(&lock_writer_tid,(LONG)GetCurrentThreadId(),__ATOMIC_RELAXED);
    const uint64_t now=lock_ticks(), waited=now-start;
    lock_held_since=now;
    ++lock_writes; /* under the lock */
    lock_wait_ticks+=waited;
    if (waited>lock_worst_wait_ticks) lock_worst_wait_ticks=waited;
}
static void rw_wrunlock(void) {
    const uint64_t held=lock_ticks()-lock_held_since;
    lock_hold_ticks+=held;
    if (held>lock_worst_hold_ticks) lock_worst_hold_ticks=held;
    lock_site_add(write_sites,lock_write_site,held);
    __atomic_store_n(&lock_writer_tid,0,__ATOMIC_RELAXED);
    /* seq_cst: the store must not pass the load of lock_sleepers (see rw_rdlock) */
    __atomic_store_n(&lock_state,0,__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&lock_sleepers,__ATOMIC_SEQ_CST)) WakeByAddressAll((void *)&lock_state);
}
/* Since the last call: writes, ms writers waited, ms they held it, the longest wait in ms. */
void runtime_memory_lock_stats(uint64_t *writes, double *wait_ms, double *hold_ms, double *worst_ms) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    *writes=__atomic_exchange_n(&lock_writes,0,__ATOMIC_RELAXED);
    *wait_ms=(double)__atomic_exchange_n(&lock_wait_ticks,0,__ATOMIC_RELAXED)*ms;
    *hold_ms=(double)__atomic_exchange_n(&lock_hold_ticks,0,__ATOMIC_RELAXED)*ms;
    *worst_ms=(double)__atomic_exchange_n(&lock_worst_wait_ticks,0,__ATOMIC_RELAXED)*ms;
}
/* sotc: SOTC_LOCK_WATCH. The table lock's holder as text: the exclusive holder's thread id, the number of
 * readers, and the site of the last write lock (page_manager.cpp and the watch line above read it). */
void runtime_memory_lock_watch_text(char *out, size_t capacity) {
    const char *site=__atomic_load_n(&lock_last_write,__ATOMIC_RELAXED);
    int64_t s=__atomic_load_n(&lock_state,__ATOMIC_RELAXED);
    snprintf(out,capacity,"writer tid %lu, readers %lld, last write site %s",
             (unsigned long)__atomic_load_n(&lock_writer_tid,__ATOMIC_RELAXED),
             (long long)(s>0 ? s : 0), site ? site : "none");
}
/* Since the last call, the sites readers waited longest in and writers held the lock longest in:
 * "name count/ms" by descending ms. */
static void lock_sites_format(LockSite *sites, char *out, size_t capacity, double ms) {
    LockSite copy[LOCK_SITES];
    int n=0;
    for (int i=0;i<LOCK_SITES;++i) {
        const char *name=__atomic_load_n(&sites[i].name,__ATOMIC_ACQUIRE);
        if (!name) break;
        copy[n]=(LockSite){name,__atomic_exchange_n(&sites[i].count,0,__ATOMIC_RELAXED),__atomic_exchange_n(&sites[i].ticks,0,__ATOMIC_RELAXED)};
        if (copy[n].count) ++n;
    }
    size_t used=0; out[0]=0;
    for (int k=0;k<6 && n;++k) {
        int best=0;
        for (int i=1;i<n;++i) if (copy[i].ticks>copy[best].ticks) best=i;
        int w=snprintf(out+used,capacity-used,"%s%s %llu/%.1f",k ? ", " : "",copy[best].name,(unsigned long long)copy[best].count,(double)copy[best].ticks*ms);
        if (w<0 || (size_t)w>=capacity-used) break;
        used+=(size_t)w; copy[best]=copy[--n];
    }
}
void runtime_memory_lock_sites(char *readers, char *writers, size_t capacity) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    lock_sites_format(read_sites,readers,capacity,ms);
    lock_sites_format(write_sites,writers,capacity,ms);
}
/* Since the last call: the longest hold in ms, and how often and how long readers waited. */
void runtime_memory_lock_stats2(double *worst_hold_ms, uint64_t *read_waits, double *read_wait_ms) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    *worst_hold_ms=(double)__atomic_exchange_n(&lock_worst_hold_ticks,0,__ATOMIC_RELAXED)*ms;
    *read_waits=__atomic_exchange_n(&lock_read_waits,0,__ATOMIC_RELAXED);
    *read_wait_ms=(double)__atomic_exchange_n(&lock_read_wait_ticks,0,__ATOMIC_RELAXED)*ms;
}
#else
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static void rw_rdlock(void) { pthread_rwlock_rdlock(&lock); }
static void rw_rdunlock(void) { pthread_rwlock_unlock(&lock); }
static void rw_wrlock(void) { pthread_rwlock_wrlock(&lock); }
static void rw_wrunlock(void) { pthread_rwlock_unlock(&lock); }
void runtime_memory_lock_stats(uint64_t *writes, double *wait_ms, double *hold_ms, double *worst_ms) {
    *writes=0; *wait_ms=*hold_ms=*worst_ms=0;
}
void runtime_memory_lock_stats2(double *worst_hold_ms, uint64_t *read_waits, double *read_wait_ms) {
    *worst_hold_ms=*read_wait_ms=0; *read_waits=0;
}
void runtime_memory_carve_stats(uint64_t *mib, uint64_t *runs, double *split_ms, double *worst_ms, double *other_ms) {
    *mib=*runs=0; *split_ms=*worst_ms=*other_ms=0;
}
/* sotc: no live view splits on this path (driver.cpp reads these counters unconditionally). */
uint64_t runtime_memory_carve_freeze_ns(void) { return 0; }
uint64_t runtime_memory_carve_nostop(void) { return 0; }
void runtime_memory_lock_sites(char *readers, char *writers, size_t capacity) {
    if (capacity) readers[0]=writers[0]=0;
}
void runtime_memory_lock_watch_text(char *out, size_t capacity) {
    if (capacity) snprintf(out,capacity,"not tracked on this path");
}
#endif
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
/* sotc: the VMA range each write changed (SOTC_REGION_LOG=0: none kept). An area change makes
 * ~2000 writes per second (maps, unmaps, the GPU hook queue), so caches valid for one generation
 * were always stale and every GPU lookup took the read lock, then waited for the writer. A
 * cached region stays valid while no write since it was read touched its range, even while a
 * write elsewhere is under way (dirty_lo/hi is published before each change). Slot of the
 * write that ended at even generation g: (g/2) % DIRTY_LOG; generation 0 while it is rewritten. */
#define DIRTY_LOG 64
typedef struct { uint64_t generation; uintptr_t start, end; } DirtyLogEntry; /* renamed: carve_dirty below has its own DirtyRange */
static DirtyLogEntry dirty_log[DIRTY_LOG];
static uintptr_t dirty_lo, dirty_hi; /* of the write under way */
static int region_log_on(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_REGION_LOG"); v=!(e && e[0]=='0'); }
    return v;
}
/* Before a change of the VMAs in [a,b) (under the write lock). */
static void dirty(uintptr_t a, uintptr_t b) {
    if (a<dirty_lo) __atomic_store_n(&dirty_lo,a,__ATOMIC_SEQ_CST);
    if (b>dirty_hi) __atomic_store_n(&dirty_hi,b,__ATOMIC_SEQ_CST);
}
static void write_lock_at(const char *site) {
    if (!exclusive_depth++) {
        rw_wrlock();
        lock_write_site=site;
#ifdef _WIN32
        __atomic_store_n(&lock_last_write,site,__ATOMIC_RELAXED);
#endif
        __atomic_store_n(&dirty_lo,UINTPTR_MAX,__ATOMIC_SEQ_CST);
        __atomic_store_n(&dirty_hi,0,__ATOMIC_SEQ_CST);
        __atomic_add_fetch(&table_generation,1,__ATOMIC_SEQ_CST);
    }
}
/* sotc: SOTC_CARVE_NOSTOP. Ranges a split left writable that the GPU tracker had write-protected:
 * they are reported to the GPU as written by the CPU (hook_invalidate) once the table lock is free
 * (the invalidation takes it for reading). Filled and emptied under the lock. */
#define CARVE_DIRTY_MAX 256
typedef struct { uintptr_t start, end; } DirtyRange;
static DirtyRange carve_dirty[CARVE_DIRTY_MAX];
static size_t carve_dirty_count;
static void (*carve_dirty_hook)(uintptr_t, uint64_t);
/* Ends a write under the lock: logs the VMA range it changed (dirty_lo/hi) at the next even
 * generation, then bumps the generation. Both unlock paths below call it before the lock is let go. */
static void write_publish(void) {
    const uint64_t ended=table_generation+1;
    DirtyLogEntry *d=&dirty_log[(ended/2)%DIRTY_LOG];
    const int any=dirty_lo<dirty_hi;
    __atomic_store_n(&d->generation,0,__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&d->start,any ? dirty_lo : 0,__ATOMIC_RELAXED);
    __atomic_store_n(&d->end,any ? dirty_hi : 0,__ATOMIC_RELAXED);
    __atomic_store_n(&d->generation,ended,__ATOMIC_RELEASE);
    __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE);
}
/* sotc: SOTC_COMMIT_CHUNKS bookkeeping (runtime_memory_chunks.h). The Windows definitions are below;
 * on the Linux path there are no chunks and these do nothing. */
static void chunks_alloc_note(uint64_t phys, uint64_t size);
static void chunks_free_note(uint64_t phys, uint64_t size);
static int chunks_pin(uint64_t phys, uint64_t size);   /* 1 pinned, 0 not chunked, -1 refused */
static void chunks_unpin(uint64_t phys, uint64_t size);
static void chunks_drain_after_unlock(void);
static int commit_chunks_lazy(void);                       /* SOTC_COMMIT_CHUNKS=2: chunks opened on first touch */
static void lazy_materialize_range(uintptr_t address, uint64_t size);
static int backing_ready(uint64_t offset, uint64_t size);   /* 0 readable, -1 Windows refused */
static void write_unlock(void) {
    if (exclusive_depth==1 && carve_dirty_count) {
        DirtyRange local[CARVE_DIRTY_MAX];
        const size_t n=carve_dirty_count;
        void (*hook)(uintptr_t, uint64_t)=carve_dirty_hook;
        memcpy(local,carve_dirty,n*sizeof(*local));
        carve_dirty_count=0;
        --exclusive_depth;
        write_publish();
        rw_wrunlock();
        if (hook) for (size_t i=0;i<n;++i) hook(local[i].start,local[i].end-local[i].start);
        chunks_drain_after_unlock(); /* closes of idle chunks: after the lock is let go */
        return;
    }
    if (!--exclusive_depth) { write_publish(); rw_wrunlock(); chunks_drain_after_unlock(); }
}
/* Whether a region read at even generation `since` is unchanged at generation now. */
static int region_unchanged(uintptr_t start, uintptr_t end, uint64_t since, uint64_t now) {
    const uint64_t done=now & ~(uint64_t)1;
    if (done-since>2*(DIRTY_LOG-1)) return 0;
    for (uint64_t g=since+2; g<=done; g+=2) {
        DirtyLogEntry *d=&dirty_log[(g/2)%DIRTY_LOG];
        if (__atomic_load_n(&d->generation,__ATOMIC_ACQUIRE)!=g) return 0;
        uintptr_t a=__atomic_load_n(&d->start,__ATOMIC_RELAXED), b=__atomic_load_n(&d->end,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&d->generation,__ATOMIC_RELAXED)!=g) return 0;
        if (a<end && start<b) return 0;
    }
    if (now & 1) {
        uintptr_t lo=__atomic_load_n(&dirty_lo,__ATOMIC_SEQ_CST), hi=__atomic_load_n(&dirty_hi,__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST)!=now) return 0;
        if (lo<end && start<hi) return 0;
    }
    return 1;
}
/* sotc: read locks this thread holds (the lazy-chunk fault handler cannot take the table lock again). */
static __thread unsigned read_depth;
static void read_lock_at(const char *site) { if (!exclusive_depth) { lock_site_name=site; rw_rdlock(); ++read_depth; } }
#define write_lock() write_lock_at(__func__)
#define read_lock() read_lock_at(__func__)
static void read_unlock(void) { if (!exclusive_depth) { --read_depth; rw_rdunlock(); } }
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
#ifndef _WIN32
static int pool_fd=-1;
#endif
static void region_direct_init(void); /* sotc: SOTC_REGION_DIRECT, read before any fault */
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
/* sotc: grows on demand. With 64 fixed slots, a GPU hook slowed down by an emergency texture
 * collection (video memory full in the open world) let other threads queue past the end, and
 * the exit(21) that followed hung the game instead of closing it. */
static PendingHook *pending; static size_t pending_count, pending_capacity;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#ifdef _WIN32
/* ---- host mappings (Windows) ----
 * [USER_MIN,USER_MAX) is covered by regions, each one placeholder or one view of the
 * section. Mapping a range carves it out as a single placeholder first: views overlapping
 * it are unmapped, placeholders coalesced and split, and the parts of views outside the
 * range mapped again with the page protections they had (GPU page tracking included). */
typedef struct { uintptr_t start, end; uint64_t phys; int mapped; } HostRegion; /* mapped: a view, or a lazy range (lazy_region) */
typedef struct { uintptr_t start, end; DWORD protect; } ProtRun;
/* SOTC_COMMIT_CHUNKS=2: a region is lazy while the chunk module's lazy list holds its range (no view yet). */
static int lazy_region(const HostRegion *r);
static HostRegion *regions; static size_t region_count, region_capacity;
static HANDLE section;
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void carve_resume_on_fail(void);
static void win_fail(const char *what, uintptr_t address, uint64_t size) {
    carve_resume_on_fail(); /* sotc: no stdio while other guest threads are stopped */
    fprintf(stderr,"STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n",what,(void *)address,size,GetLastError());
    exit(21);
}
static size_t region_index(uintptr_t a) { /* first region with end > a */
    size_t lo=0, hi=region_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (regions[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static void region_reserve(size_t extra) {
    while (region_count+extra>region_capacity) {
        size_t capacity=region_capacity ? region_capacity*2 : 256;
        HostRegion *next=realloc(regions,capacity*sizeof(*regions));
        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        regions=next; region_capacity=capacity;
    }
}
static void region_insert(size_t at, HostRegion r) {
    region_reserve(1);
    memmove(regions+at+1,regions+at,(region_count-at)*sizeof(*regions));
    regions[at]=r; ++region_count;
}
static void region_erase(size_t at, size_t n) {
    memmove(regions+at,regions+at+n,(region_count-at-n)*sizeof(*regions)); region_count-=n;
}
/* The guest range is reserved before anything else can be placed there (runtime_start). */
void runtime_memory_reserve(void) {
    if (region_count) return;
    if (!VirtualAlloc2(NULL,(void *)USER_MIN,USER_MAX-USER_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        win_fail("reserving the PS4 address range (link with --disable-high-entropy-va)",USER_MIN,USER_MAX-USER_MIN);
    region_insert(0,(HostRegion){USER_MIN,USER_MAX,0,0});
    /* sotc: Shadow of the Colossus maps fixed ranges below the user area (0x7f0000000),
     * in the PS4 system-managed window. A second placeholder covers it; mappings never
     * span both reservations because the host's low maps lie between them. */
    if (!VirtualAlloc2(NULL,(void *)SYSTEM_LOW_MIN,SYSTEM_LOW_MAX-SYSTEM_LOW_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        win_fail("reserving the PS4 system-managed window",SYSTEM_LOW_MIN,SYSTEM_LOW_MAX-SYSTEM_LOW_MIN);
    region_insert(0,(HostRegion){SYSTEM_LOW_MIN,SYSTEM_LOW_MAX,0,0});
}
static int commit_chunks_on(void); /* sotc: SOTC_COMMIT_CHUNKS, defined with the commit switches */
/* A view covers one piece of one physical chunk (host_map and carve_host cut them so). With chunks
 * the view takes a reference on its chunk (chunks_view_map). */
/* sotc: SOTC_MAP_LOG=1 (B1, BPE heap crash). The mapping history of the guest range: every map, unmap,
 * protection change and placeholder change of a range that overlaps [MAP_LOG_LO,MAP_LOG_HI) (the
 * game's heap mapping) is recorded in a ring of MAP_LOG_SIZE entries. Nothing is printed on the hot
 * path. probe.c calls runtime_memory_map_log_dump when a guest read fault reaches the fatal path: the
 * entries that overlap the fault page, newest first, and the last entries of its 1 MiB block.
 * Lock-free: a writer sets seq to 0 while it fills an entry and to its index + 1 when done; a reader
 * copies an entry only when seq is the same before and after. The caller is the address the host
 * function returns to (the runtime function that called the map, unmap or protect). */
#define MAP_LOG_SIZE 4096u
#define MAP_LOG_LO UINT64_C(0x10dc000000)
#define MAP_LOG_HI UINT64_C(0x110d000000)
enum { MAP_LOG_MAP=1, MAP_LOG_UNMAP, MAP_LOG_PROTECT, MAP_LOG_PLACEHOLDER };
typedef struct {
    LONG seq;
    uint32_t op, tid, prot;
    uint64_t start, end, ns;
    uintptr_t caller;
} MapLogEntry;
static MapLogEntry map_log[MAP_LOG_SIZE];
static volatile LONG map_log_next;
static int map_log_on(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_MAP_LOG"); v=e && e[0]=='1'; }
    return v;
}
static uint64_t map_log_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart*1e9/(double)freq.QuadPart);
}
static void map_log_note(uint32_t op, uintptr_t start, uintptr_t end, DWORD prot, uintptr_t caller) {
    if ((uint64_t)end<=MAP_LOG_LO || (uint64_t)start>=MAP_LOG_HI) return;
    const LONG i=InterlockedIncrement(&map_log_next)-1;
    MapLogEntry *e=&map_log[(uint32_t)i & (MAP_LOG_SIZE-1)];
    InterlockedExchange(&e->seq,0);
    e->op=op; e->tid=GetCurrentThreadId(); e->prot=prot;
    e->start=start; e->end=end; e->ns=map_log_ns(); e->caller=caller;
    InterlockedExchange(&e->seq,i+1);
}
/* MAP_LOG(op, start, end, prot): records a change made by the function that expands it. */
#define MAP_LOG(op,s,e,p) do { if (map_log_on()) map_log_note((op),(uintptr_t)(s),(uintptr_t)(e),(DWORD)(p),(uintptr_t)__builtin_return_address(0)); } while (0)
uint64_t runtime_memory_map_log_now_ns(void) { return map_log_ns(); }
/* Name of a host thread for the dump: GetThreadDescription (the guest thread names are set with it). */
int runtime_memory_thread_name(DWORD tid, char *out, size_t size) {
    typedef HRESULT (WINAPI *GetDescFn)(HANDLE, PWSTR *);
    GetDescFn get_description=(GetDescFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetThreadDescription");
    int ok=0, own=tid==GetCurrentThreadId();
    if (size) out[0]=0;
    if (!get_description || !size) return 0;
    HANDLE h=own ? GetCurrentThread() : OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,tid);
    if (!h) return 0;
    PWSTR wide=NULL;
    if (SUCCEEDED(get_description(h,&wide)) && wide) {
        ok=WideCharToMultiByte(CP_UTF8,0,wide,-1,out,(int)size,NULL,NULL)>0;
        LocalFree(wide);
    }
    if (!own) CloseHandle(h);
    return ok;
}
/* Copies entry k of the ring when it is complete and still that entry; 0 otherwise. */
static int map_log_copy(LONG k, MapLogEntry *out) {
    MapLogEntry *e=&map_log[(uint32_t)k & (MAP_LOG_SIZE-1)];
    if (InterlockedCompareExchange(&e->seq,0,0)!=k+1) return 0;
    out->op=e->op; out->tid=e->tid; out->prot=e->prot; out->start=e->start; out->end=e->end; out->ns=e->ns; out->caller=e->caller;
    return InterlockedCompareExchange(&e->seq,0,0)==k+1;
}
static const char *map_log_op_name(uint32_t op) {
    switch (op) {
    case MAP_LOG_MAP: return "map";
    case MAP_LOG_UNMAP: return "unmap";
    case MAP_LOG_PROTECT: return "protect";
    case MAP_LOG_PLACEHOLDER: return "placeholder";
    }
    return "?";
}
/* Prints the entries overlapping [page,page+4096), newest first (at most 32), then, when block_scan is
 * set, the last 16 entries overlapping the 1 MiB block of the page. Ages are relative to fault_ns (the
 * fault time from runtime_memory_map_log_now_ns); a positive age is before the fault. */
void runtime_memory_map_log_dump(uintptr_t page, uint64_t fault_ns, int block_scan) {
    if (!map_log_on()) return;
    const LONG last=map_log_next;
    const uintptr_t block=page & ~(uintptr_t)((UINT64_C(1)<<20)-1);
    const struct { const char *title; uintptr_t lo, hi; int max; } scans[2]={
        {"overlapping the fault page",page,page+4096,32},
        {"last entries of the 1 MiB block of the fault page",block,block+((uintptr_t)1<<20),16}};
    for (int s=0; s<(block_scan ? 2 : 1); ++s) {
        int printed=0;
        fprintf(stderr,"SOTC MAP LOG (%s), newest first:\n",scans[s].title);
        for (LONG k=last-1; k>=0 && last-k<=(LONG)MAP_LOG_SIZE && printed<scans[s].max; --k) {
            MapLogEntry e;
            if (!map_log_copy(k,&e)) continue;
            if ((uint64_t)e.end<=scans[s].lo || (uint64_t)e.start>=scans[s].hi) continue;
            char name[64];
            runtime_memory_thread_name((DWORD)e.tid,name,sizeof(name));
            const double age_us=((double)fault_ns-(double)e.ns)/1000.0;
            fprintf(stderr,"  age %+.1f us op %s tid %lu (%s) range [%#llx,%#llx) prot 0x%lx caller %p\n",
                    age_us,map_log_op_name(e.op),(unsigned long)e.tid,name[0] ? name : "unnamed",
                    (unsigned long long)e.start,(unsigned long long)e.end,(unsigned long)e.prot,(void *)e.caller);
            ++printed;
        }
        if (!printed) fprintf(stderr,"  (none)\n");
    }
    fflush(stderr);
}
static void view_map(const HostRegion *r, DWORD protect) {
    MAP_LOG(MAP_LOG_MAP,r->start,r->end,protect);
    if (commit_chunks_on()) {
        if (chunks_view_map((void *)r->start,r->phys,r->end-r->start,protect))
            win_fail("mapping a guest chunk view",r->start,r->end-r->start);
        return;
    }
    DWORD initial=protect==PAGE_NOACCESS ? PAGE_READWRITE : protect, old;
    if (!MapViewOfFile3(section,GetCurrentProcess(),(void *)r->start,r->phys,r->end-r->start,MEM_REPLACE_PLACEHOLDER,initial,NULL,0))
        win_fail("MapViewOfFile3",r->start,r->end-r->start);
    if (initial!=protect) {
        MAP_LOG(MAP_LOG_PROTECT,r->start,r->end,protect);
        if (!VirtualProtect((void *)r->start,r->end-r->start,protect,&old))
            win_fail("VirtualProtect",r->start,r->end-r->start);
    }
}
/* Unmaps a view made by view_map: its range becomes a placeholder again. With chunks the chunk's
 * reference goes too (the chunk closes after the table lock is dropped, if nothing else holds it). */
static void view_unmap(const HostRegion *r) {
    MAP_LOG(MAP_LOG_UNMAP,r->start,r->end,0);
    if (commit_chunks_on()) {
        if (chunks_view_unmap((void *)r->start,r->phys,r->end-r->start))
            win_fail("unmapping a guest chunk view",r->start,r->end-r->start);
        return;
    }
    if (!UnmapViewOfFile2(GetCurrentProcess(),(void *)r->start,MEM_PRESERVE_PLACEHOLDER))
        win_fail("UnmapViewOfFile2",r->start,r->end-r->start);
}
static size_t save_protections(uintptr_t start, uintptr_t end, ProtRun **runs, size_t count, size_t *capacity) {
    for (uintptr_t at=start; at<end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at,&m,sizeof(m))) win_fail("VirtualQuery",at,0);
        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;
        if (next>end) next=end;
        if (count==*capacity) { *capacity=*capacity ? *capacity*2 : 16; *runs=realloc(*runs,*capacity*sizeof(**runs)); }
        (*runs)[count++]=(ProtRun){at,next,m.Protect};
        at=next;
    }
    return count;
}
#ifdef _WIN32
size_t runtime_thread_suspend_others(void **out, size_t capacity); /* runtime_thread.c */
void runtime_thread_resume(void **handles, size_t n);
/* sotc: SOTC_CARVE_SUSPEND=0 keeps the other guest threads running while live views are split. */
static int carve_suspend_enabled(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_CARVE_SUSPEND"); v=!(e && e[0]=='0'); }
    return v;
}
static uint64_t carve_splits, carve_freeze_ns;
uint64_t runtime_memory_carve_splits(void) { return __atomic_exchange_n(&carve_splits,0,__ATOMIC_RELAXED); }
/* sotc: time spent splitting live views (other guest threads stopped meanwhile), read and reset. */
uint64_t runtime_memory_carve_freeze_ns(void) { return __atomic_exchange_n(&carve_freeze_ns,0,__ATOMIC_RELAXED); }
static uint64_t qpc_ns(void) {
    static LARGE_INTEGER freq; LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart*1e9/(double)freq.QuadPart);
}
/* sotc: the view chunk, one definition for both sides' switches (upstream 0.70 and ours).
 * SOTC_VIEW_CHUNK_KB=N (multiple of 64, takes precedence when set): views of N KiB. With 64, a
 * remap of the game's 64 KiB pages inside a large mapping (sceKernelBatchMap while an area streams)
 * replaces whole views and never splits a live one, so the other guest threads are never stopped
 * for it. Costs one view per 64 KiB (more host protection calls on wide ranges, and the boot maps
 * ~150k views).
 * SOTC_VIEW_CHUNK_MB=N (used when KB is not set): views of at most N MiB aligned to N MiB. Unset
 * means 2 MiB, the upstream 0.70 default; 0 means one view per mapping. Unmapping part of a range
 * then splits only the chunk(s) at its edges, so the freeze of carve_host covers at most two
 * chunks instead of the whole mapping (a split only remaps the chunks at its two ends). */
static uint64_t view_chunk(void) {
    static uint64_t v=UINT64_MAX;
    if (v==UINT64_MAX) {
        const char *kb=getenv("SOTC_VIEW_CHUNK_KB");
        const char *e=getenv("SOTC_VIEW_CHUNK_MB");
        if (kb && strtoull(kb,NULL,10)) {
            v=strtoull(kb,NULL,10)<<10;
            v=v<(UINT64_C(64)<<10) ? (UINT64_C(64)<<10) : v-v%(UINT64_C(64)<<10); /* granularity */
        } else {
            v=e ? (uint64_t)strtoull(e,NULL,10)<<20 : UINT64_C(2)<<20;
        }
    }
    return v;
}
/* sotc: the cost of carving live views (under the write lock): splits, MiB of views mapped
 * again, protection runs restored, ms they took and the longest one; ms of all other carves. */
static uint64_t carve_split_bytes, carve_split_runs, carve_split_ticks, carve_split_worst, carve_other_ticks;
void runtime_memory_carve_stats(uint64_t *mib, uint64_t *runs, double *split_ms, double *worst_ms, double *other_ms) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    *mib=__atomic_exchange_n(&carve_split_bytes,0,__ATOMIC_RELAXED)>>20;
    *runs=__atomic_exchange_n(&carve_split_runs,0,__ATOMIC_RELAXED);
    *split_ms=(double)__atomic_exchange_n(&carve_split_ticks,0,__ATOMIC_RELAXED)*ms;
    *worst_ms=(double)__atomic_exchange_n(&carve_split_worst,0,__ATOMIC_RELAXED)*ms;
    *other_ms=(double)__atomic_exchange_n(&carve_other_ticks,0,__ATOMIC_RELAXED)*ms;
}
/* The threads carve_host stopped, so win_fail can resume them before reporting. */
static void *carve_stopped[256];
static size_t carve_nstopped;
static int carve_stopping;
#endif
static void carve_resume_on_fail(void) {
#ifdef _WIN32
    if (carve_stopping) { carve_stopping=0; runtime_thread_resume(carve_stopped,carve_nstopped); }
#endif
}
/* sotc: SOTC_VIEW_SPLIT_FINE=1 (off by default). When a live view has to be split (the guest
 * unmapped part of it), the parts that stay mapped are mapped again as views of 64 KiB, aligned:
 * later unmaps of 64 KiB pages there (the game's allocator page) remove whole views and split
 * nothing, so the other guest threads are stopped at most once per area instead of at every
 * unmap. Only split areas get small views, so protection changes elsewhere stay one call. */
#define FINE_VIEW (UINT64_C(64) << 10)
static int view_split_fine(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_VIEW_SPLIT_FINE"); v=e && e[0]=='1'; }
    return v;
}
/* Maps the unmapped placeholder regions[at] from phys as FINE_VIEW views (its first piece ends at
 * the next 64 KiB boundary). Returns the index of its last piece. The table must have room for
 * them (region_reserve before stopping the other threads: no heap call meanwhile). */
static size_t map_fine(size_t at, uint64_t phys) {
    const uintptr_t start=regions[at].start, end=regions[at].end;
    for (uintptr_t x=start; x<end;) {
        uintptr_t y=(x/FINE_VIEW+1)*FINE_VIEW;
        if (y>end) y=end;
        if (y<end) {
            MAP_LOG(MAP_LOG_PLACEHOLDER,x,y,0);
            if (!VirtualFree((void *)x,y-x,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",x,y-x);
            regions[at].end=y;
            region_insert(at+1,(HostRegion){y,end,0,0});
        }
        regions[at].phys=phys+(x-start); regions[at].mapped=1;
        view_map(&regions[at],PAGE_READWRITE);
        if (y<end) ++at;
        x=y;
    }
    return at;
}
/* Sets the protection of [start,end) view by view (VirtualProtect does not cross views). */
static void protect_views(uintptr_t start, uintptr_t end, DWORD protect) {
    DWORD old;
    for (size_t i=region_index(start); i<region_count && regions[i].start<end; ++i) {
        if (!regions[i].mapped || lazy_region(&regions[i])) continue;
        uintptr_t a=regions[i].start>start ? regions[i].start : start, b=regions[i].end<end ? regions[i].end : end;
        MAP_LOG(MAP_LOG_PROTECT,a,b,protect);
        if (!VirtualProtect((void *)a,b-a,protect,&old)) win_fail("restoring page protection",a,b-a);
    }
}
/* sotc: SOTC_CARVE_NOSTOP=1 (off by default). A live view whose kept parts are all plain
 * read-write (no page the GPU tracker protects) and hold no guest stack is split without stopping
 * the other guest threads: one touching those parts while they are briefly unmapped faults, the
 * exception handler waits for the split to end (runtime_memory_carve_retry) and the instruction
 * runs again. Kept parts with protected pages still stop everything: they come back read-write
 * before their protection is restored, and a write in between would escape the tracker. The
 * windows of the last carves are kept for the handler (a fault raised during one may be handled
 * after it ended). */
int bbgpu_guest_stack_in_range(uintptr_t address, uint64_t size) __attribute__((weak));
int bbgpu_readbacks_mode(void) __attribute__((weak));
void bbgpu_upload_wait_phys(uint64_t phys, uint64_t size) __attribute__((weak));
static size_t vma_index(uintptr_t a); /* below: first VMA with end > a */
/* Why the last split stopped the threads, "" when it did not (SOTC_CARVE_LOG). */
static const char *carve_stop_reason="";
static int carve_nostop_enabled(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_CARVE_NOSTOP"); v=e && e[0]=='1'; }
    return v;
}
#define CARVE_WINDOWS 8
static uintptr_t carve_win_lo[CARVE_WINDOWS], carve_win_hi[CARVE_WINDOWS];
static unsigned carve_win_next;
static uint64_t carve_gen; /* odd while a no-stop split runs */
static uint64_t carve_nostop_count;
uint64_t runtime_memory_carve_nostop(void) { return __atomic_exchange_n(&carve_nostop_count,0,__ATOMIC_RELAXED); }
/* From the exception handler: 1 when the access at address hit a view being (or just) split
 * without stopping the threads and can run again now. */
int runtime_memory_carve_retry(uintptr_t address, int write) {
    if (!carve_nostop_enabled()) return 0;
    int hit=0;
    for (unsigned i=0;i<CARVE_WINDOWS && !hit;++i)
        hit=address>=__atomic_load_n(&carve_win_lo[i],__ATOMIC_ACQUIRE) &&
            address<__atomic_load_n(&carve_win_hi[i],__ATOMIC_ACQUIRE);
    if (!hit) return 0;
    const uint64_t gen=__atomic_load_n(&carve_gen,__ATOMIC_ACQUIRE);
    if (gen&1) while (__atomic_load_n(&carve_gen,__ATOMIC_ACQUIRE)==gen) YieldProcessor();
    /* Run it again only if the page now allows the access: a real fault (the range the guest
     * unmapped, a page the tracker protects) goes on to the other handlers. */
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)address,&m,sizeof(m)) || m.State!=MEM_COMMIT || (m.Protect&PAGE_GUARD)) return 0;
    const DWORD p=m.Protect&0xff;
    if (write) return p==PAGE_READWRITE || p==PAGE_EXECUTE_READWRITE || p==PAGE_WRITECOPY || p==PAGE_EXECUTE_WRITECOPY;
    return p!=PAGE_NOACCESS;
}
/* sotc: SOTC_CARVE_LOG=1: each split of a live view is reported (what asked for it, the range,
 * the view it split, its alignment and the time the other guest threads stayed stopped), the
 * first 200 and then every 100th, with running totals. carve_reason: set by the caller (the
 * table lock is held, so one carve at a time). */
static const char *carve_reason="?";
static int carve_log_enabled(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_CARVE_LOG"); v=e && e[0]=='1'; }
    return v;
}
static void carve_log(uintptr_t a, uintptr_t b, uintptr_t lo, uintptr_t hi, uint64_t freeze_ns) {
    static uint64_t total, by_map, by_unmap, unaligned;
    ++total;
    if (carve_reason[0]=='m') ++by_map; else ++by_unmap;
    const int aligned=!(a%(UINT64_C(64)<<10)) && !(b%(UINT64_C(64)<<10));
    if (!aligned) ++unaligned;
    if (total<=200 || total%100==0)
        fprintf(stderr,"SOTC carve %" PRIu64 ": %s [%p,+%#" PRIx64 ") split view [%p,+%#" PRIx64 ")%s, %.2f ms, %s%s%s"
                " (totals: map %" PRIu64 ", unmap %" PRIu64 ", not 64 KiB aligned %" PRIu64 ")\n",
                total,carve_reason,(void *)a,(uint64_t)(b-a),(void *)lo,(uint64_t)(hi-lo),
                aligned ? "" : " (not 64 KiB aligned)",freeze_ns/1e6,
                carve_stop_reason[0] ? "threads stopped (" : "threads not stopped",carve_stop_reason,
                carve_stop_reason[0] ? ")" : "",by_map,by_unmap,unaligned);
}
/* Leaves [a,b) as exactly one unmapped placeholder; returns its region index. */
static size_t carve_host(uintptr_t a, uintptr_t b) {
    size_t first=region_index(a), last=first;
    if (first==region_count || regions[first].start>a) win_fail("guest range outside the reservation",a,b-a);
    while (last<region_count && regions[last].start<b) ++last;
    if (regions[last-1].end<b) win_fail("guest range outside the reservation",a,b-a);
    uintptr_t lo=regions[first].start, hi=regions[last-1].end;
    HostRegion left=regions[first], right=regions[last-1];
    int keep_left=left.mapped && lo<a, keep_right=right.mapped && b<hi;
    if (first==last-1 && !left.mapped && lo==a && hi==b) return first;
    /* sotc: SOTC_COMMIT_CHUNKS=2. The upload wait waits for copy threads, which may fault on a closed chunk and need
     * chunks_cs: it runs before the chunk lock is taken (lock order, at "lazy"). */
    if (commit_chunks_lazy() && bbgpu_upload_wait_phys) {
        if (keep_left) bbgpu_upload_wait_phys(left.phys,a-lo);
        if (keep_right) bbgpu_upload_wait_phys(right.phys+(b-right.start),hi-b);
    }
    /* The GPU answers of the nostop checks, read before the chunk lock (a GPU thread may wait for chunks_cs). */
    int readbacks=0, stack_hit=0;
    if ((keep_left || keep_right) && carve_nostop_enabled()) {
        readbacks=bbgpu_readbacks_mode ? bbgpu_readbacks_mode() : 0;
        stack_hit=!bbgpu_guest_stack_in_range || bbgpu_guest_stack_in_range(lo,hi-lo);
    }
    /* From here to the end: the lazy list and the placeholders of the lazy ranges are read and changed under
     * chunks_cs (taken after the table lock). No wait for another thread happens inside (see "lazy"). */
    const int lazy_mode=commit_chunks_lazy();
    if (lazy_mode) chunks_lock();
    const uint64_t carve_start=lock_ticks();
    /* before the views change (see runtime_memory_gpu_protect): [a,b) and the views around it
     * that are mapped again; placeholders around it hold no view to protect */
    dirty(keep_left ? lo : a,keep_right ? hi : b);
    ProtRun *runs=NULL; size_t nruns=0, capacity=0, left_runs=0;
    const int lazy_left=keep_left && lazy_region(&left), lazy_right=keep_right && lazy_region(&right);
    if (keep_left && !lazy_left) left_runs=nruns=save_protections(lo,a,&runs,nruns,&capacity);
    if (keep_right && !lazy_right) nruns=save_protections(b,hi,&runs,nruns,&capacity);
    /* sotc: a live view is split: everything in it (fiber stacks included) disappears until it is
     * mapped again below. Other guest threads stop meanwhile (see runtime_thread_suspend_others).
     * The region table is grown first: no heap call while they are stopped (one may hold the heap). */
    /* sotc: counted with or without the suspend (SOTC_CARVE_SUSPEND=0 used to read 0 splits). */
    const uint64_t split_start=(keep_left || keep_right) ? qpc_ns() : 0;
    if (keep_left || keep_right) __atomic_add_fetch(&carve_splits,1,__ATOMIC_RELAXED);
    /* sotc: SOTC_VIEW_SPLIT_FINE: kept parts become 64 KiB views (map_fine). */
    const int fine=(keep_left || keep_right) && view_split_fine() && !commit_chunks_lazy(); /* lazy kept parts stay one region */
    if (fine) region_reserve(3+(keep_left ? (a-lo)/FINE_VIEW+2 : 0)+(keep_right ? (hi-b)/FINE_VIEW+2 : 0));
    /* sotc: SOTC_CARVE_NOSTOP (see runtime_memory_carve_retry). Kept pages that are read-write
     * are fine. Write-protected ones (the GPU tracker's clean pages) come back read-write and are
     * reported as CPU-written after the lock (carve_dirty), so a write while they are writable is
     * never lost: allowed only with precise readbacks (where GPU-written pages are inaccessible,
     * never merely write-protected) and where the guest mapping itself is writable. Inaccessible
     * pages (GPU data not read back) and guest stacks still stop the threads. */
    int nostop=(keep_left || keep_right) && carve_nostop_enabled();
    size_t dirty_runs=0;
    carve_stop_reason=nostop ? "" : "switch off";
    for (size_t r=0; nostop && r<nruns; ++r) {
        const DWORD pr=runs[r].protect&0xff;
        if (pr==PAGE_READWRITE || pr==PAGE_EXECUTE_READWRITE) continue;
        if (pr==PAGE_READONLY || pr==PAGE_EXECUTE_READ) { ++dirty_runs; continue; }
        nostop=0; carve_stop_reason="inaccessible pages (GPU data)";
    }
    if (nostop && dirty_runs) {
        if (readbacks!=2) { nostop=0; carve_stop_reason="readbacks not precise"; }
        else if (!hook_invalidate) { nostop=0; carve_stop_reason="no GPU hook"; }
        else if (carve_dirty_count+dirty_runs>CARVE_DIRTY_MAX) { nostop=0; carve_stop_reason="dirty list full"; }
        for (size_t r=0; nostop && r<nruns; ++r) {
            const DWORD pr=runs[r].protect&0xff;
            if (pr!=PAGE_READONLY && pr!=PAGE_EXECUTE_READ) continue;
            /* The guest's own protection must allow writes (not a page the game made read-only). */
            for (size_t v=vma_index(runs[r].start); nostop && v<vma_count && vmas[v].start<runs[r].end; ++v)
                if (!(host_prot(vmas[v].prot)&PROT_WRITE)) { nostop=0; carve_stop_reason="guest read-only pages"; }
        }
    }
    if (nostop && stack_hit) {
        nostop=0; carve_stop_reason="guest stack";
    }
    if (nostop) carve_dirty_hook=hook_invalidate;
    /* The copies of SOTC_ASYNC_UPLOAD rely on write faults to know when the guest writes what they
     * still read: the kept pages become writable without faulting, so their pending copies finish
     * first (the copy threads take no lock: safe with the table lock held). */
    if (nostop && dirty_runs && bbgpu_upload_wait_phys && !lazy_mode) { /* lazy: waited for before the chunk lock */
        if (keep_left) bbgpu_upload_wait_phys(left.phys,a-lo);
        if (keep_right) bbgpu_upload_wait_phys(right.phys+(b-right.start),hi-b);
    }
    if (nostop) {
        region_reserve(3);
        const unsigned w=carve_win_next++%CARVE_WINDOWS;
        __atomic_store_n(&carve_win_lo[w],0,__ATOMIC_RELEASE); /* empty while it changes */
        __atomic_store_n(&carve_win_hi[w],hi,__ATOMIC_RELEASE);
        __atomic_store_n(&carve_win_lo[w],lo,__ATOMIC_RELEASE);
        __atomic_add_fetch(&carve_gen,1,__ATOMIC_ACQ_REL); /* odd: splitting */
        __atomic_add_fetch(&carve_nostop_count,1,__ATOMIC_RELAXED);
    }
    if ((keep_left || keep_right) && !nostop && carve_suspend_enabled()) {
        region_reserve(3);
        carve_nstopped=runtime_thread_suspend_others(carve_stopped,256);
        carve_stopping=1;
    }
    for (size_t i=first;i<last;++i) if (regions[i].mapped && !lazy_region(&regions[i])) view_unmap(&regions[i]);
    if (lazy_mode) chunks_lazy_cut(a,b); /* [a,b) has no lazy range now; the kept parts stay lazy */
    if (last-first>1) MAP_LOG(MAP_LOG_PLACEHOLDER,lo,hi,0);
    if (last-first>1 && !VirtualFree((void *)lo,hi-lo,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        win_fail("coalescing placeholders",lo,hi-lo);
    region_erase(first,last-first);
    region_insert(first,(HostRegion){lo,hi,0,0});
    size_t at=first;
    if (lo<a) {
        MAP_LOG(MAP_LOG_PLACEHOLDER,lo,a,0);
        if (!VirtualFree((void *)lo,a-lo,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",lo,a-lo);
        regions[at].end=a;
        region_insert(at+1,(HostRegion){a,hi,0,0});
        if (lazy_left) {
            regions[at].phys=left.phys; regions[at].mapped=1; /* still a lazy range: no view */
        } else if (keep_left && fine) {
            at=map_fine(at,left.phys);
        } else if (keep_left) {
            regions[at].phys=left.phys; regions[at].mapped=1;
            view_map(&regions[at],PAGE_READWRITE);
        }
        ++at;
    }
    if (b<hi) {
        MAP_LOG(MAP_LOG_PLACEHOLDER,a,b,0);
        if (!VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",a,b-a);
        regions[at].end=b;
        region_insert(at+1,(HostRegion){b,hi,0,0});
        if (lazy_right) {
            regions[at+1].phys=right.phys+(b-right.start); regions[at+1].mapped=1; /* still a lazy range */
        } else if (keep_right && fine) {
            map_fine(at+1,right.phys+(b-right.start));
        } else if (keep_right) {
            regions[at+1].phys=right.phys+(b-right.start); regions[at+1].mapped=1;
            view_map(&regions[at+1],PAGE_READWRITE);
        }
    }
    for (size_t i=0;i<nruns;++i) {
        DWORD old;
        if (runs[i].protect==PAGE_READWRITE) continue;
        const DWORD pr=runs[i].protect&0xff;
        if (nostop && (pr==PAGE_READONLY || pr==PAGE_EXECUTE_READ)) {
            /* sotc: SOTC_CARVE_NOSTOP: left writable, reported as CPU-written after the lock. */
            if (pr==PAGE_EXECUTE_READ) {
                if (fine) protect_views(runs[i].start,runs[i].end,PAGE_EXECUTE_READWRITE);
                else {
                    MAP_LOG(MAP_LOG_PROTECT,runs[i].start,runs[i].end,PAGE_EXECUTE_READWRITE);
                    if (!VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,PAGE_EXECUTE_READWRITE,&old))
                        win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);
                }
            }
            if (carve_dirty_count && carve_dirty[carve_dirty_count-1].end==runs[i].start)
                carve_dirty[carve_dirty_count-1].end=runs[i].end;
            else
                carve_dirty[carve_dirty_count++]=(DirtyRange){runs[i].start,runs[i].end};
            continue;
        }
        if (fine) protect_views(runs[i].start,runs[i].end,runs[i].protect); /* may span views now */
        else {
            MAP_LOG(MAP_LOG_PROTECT,runs[i].start,runs[i].end,runs[i].protect);
            if (!VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,runs[i].protect,&old))
                win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);
        }
    }
    if (carve_stopping) { carve_stopping=0; runtime_thread_resume(carve_stopped,carve_nstopped); }
    if (nostop) __atomic_add_fetch(&carve_gen,1,__ATOMIC_ACQ_REL); /* even: done, retries go on */
    if (lazy_mode) chunks_unlock();
    if (split_start) {
        const uint64_t freeze=qpc_ns()-split_start;
        __atomic_add_fetch(&carve_freeze_ns,freeze,__ATOMIC_RELAXED);
        if (carve_log_enabled()) carve_log(a,b,lo,hi,freeze); /* threads resumed: stdio is safe */
    }
    (void)left_runs;
    free(runs);
    const uint64_t carve_took=lock_ticks()-carve_start;
    if (keep_left || keep_right) {
        carve_split_bytes+=(keep_left ? a-lo : 0)+(keep_right ? hi-b : 0); /* under the write lock */
        carve_split_runs+=nruns;
        carve_split_ticks+=carve_took;
        if (carve_took>carve_split_worst) carve_split_worst=carve_took;
    } else {
        carve_other_ticks+=carve_took;
    }
    return at;
}
/* Why views are chunked (upstream 0.70, SOTC_VIEW_CHUNK_MB): a view cannot be unmapped in part, so
 * unmapping a piece of a big mapping unmapped and mapped again everything around it (live view
 * splits): at an area change the game's munmaps remapped 60-346 GB of views per 5 s under the
 * write lock and the frame stopped. Mappings made of views that end at chunk boundaries let a
 * split remap only the chunks at its two ends. */
/* End of the view piece that starts at a: at most the view chunk (chunk, 0: none) and, with
 * SOTC_COMMIT_CHUNKS (by_chunks), the physical chunk boundary of the memory it maps. A view takes
 * one chunk (chunks_view_map), so a mapping crossing a 64 MiB boundary of the backing is cut there. */
static uintptr_t piece_end(uintptr_t address, uintptr_t a, uintptr_t end, uint64_t phys, uint64_t chunk, int by_chunks) {
    uintptr_t b=end;
    if (chunk && (a/chunk+1)*chunk<b) b=(a/chunk+1)*chunk;
    if (by_chunks) b=a+chunks_piece(phys+(a-address),b-a);
    return b;
}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    MAP_LOG(MAP_LOG_MAP,address,address+size,win_prot(prot));
    carve_reason="map";
    size_t i=carve_host(address,address+size);
    if (reserved) return 0;
    const uint64_t chunk=view_chunk();
    const int by_chunks=commit_chunks_on();
    const int lazy=commit_chunks_lazy();
    if (!by_chunks && (!chunk || size<=chunk)) {
        regions[i].phys=phys; regions[i].mapped=1;
        view_map(&regions[i],win_prot(prot));
        return 0;
    }
    /* sotc: SOTC_VIEW_CHUNK_MB/KB. The carved range is one unmapped placeholder: cutting it at chunk
     * boundaries splits only placeholders, so no live view is touched and nothing is stopped.
     * The table gets all the pieces at once (one move of its tail): inserting them one by one moved
     * the whole tail per piece, quadratic with 64 KiB pieces of a multi-GiB mapping. */
    const uintptr_t end=address+size;
    size_t pieces=0;
    for (uintptr_t a=address; a<end; ++pieces) a=piece_end(address,a,end,phys,chunk,by_chunks);
    region_reserve(pieces-1);
    memmove(regions+i+pieces,regions+i+1,(region_count-i-1)*sizeof(*regions));
    region_count+=pieces-1;
    size_t k=i;
    const DWORD protect=win_prot(prot);
    for (uintptr_t a=address; a<end; ++k) {
        const uintptr_t b=piece_end(address,a,end,phys,chunk,by_chunks);
        /* [a,end) is one placeholder here: cut [a,b) off its start. */
        if (b<end) MAP_LOG(MAP_LOG_PLACEHOLDER,a,b,0);
        if (b<end && !VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
            win_fail("splitting a placeholder",a,b-a);
        regions[k]=(HostRegion){a,b,phys+(a-address),1};
        if (lazy) chunks_lazy_add(a,b,phys+(a-address),protect); /* SOTC_COMMIT_CHUNKS=2: the view waits for the first touch */
        else view_map(&regions[k],protect);
        a=b;
    }
    return 0;
}
static int host_unmap(uintptr_t address, uint64_t size) {
    MAP_LOG(MAP_LOG_UNMAP,address,address+size,0);
    carve_reason="unmap"; carve_host(address,address+size); return 0;
}
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    MAP_LOG(MAP_LOG_PROTECT,address,end,protect);
    const int lazy=commit_chunks_lazy();
    int result=0;
    if (lazy) chunks_lock(); /* a lazy range keeps the protection it gets when viewed: the list is updated */
    for (size_t i=region_index(address); i<region_count && regions[i].start<end && !result; ++i) {
        if (!regions[i].mapped) continue;
        uintptr_t a=regions[i].start>address ? regions[i].start : address, b=regions[i].end<end ? regions[i].end : end;
        if (lazy && lazy_region(&regions[i])) { chunks_lazy_setprot(a,b,protect); continue; }
        if (!VirtualProtect((void *)a,b-a,protect,&old)) result=-1;
    }
    if (lazy) chunks_unlock();
    return result;
}
/* sotc: SOTC_COMMIT_ON_DEMAND=1 (off by default). The section is created SEC_RESERVE and the
 * backing pages under a mapping are committed before the guest sees it (place -> commit_backing),
 * in 64 KiB chunks. Until then the pool has its full size but no commit charge. Committed pages
 * are never decommitted: Windows refuses MEM_DECOMMIT on section views (error 87, measured). An
 * uncommitted page cannot be read or written (access violation), so host_discard clears only the
 * committed chunks; the others already read as zero once they are committed. */
#define COMMIT_CHUNK (UINT64_C(64) << 10)
#define COMMIT_MAX_BYTES ((UINT64_C(16384) + 1024) << 20) /* largest pool (BB_DMEM_MB clamp) plus flexible */
static uint64_t commit_bits[COMMIT_MAX_BYTES/COMMIT_CHUNK/64];
static uint64_t committed_bytes, commit_calls; /* written under the table write lock; read by the GPU report without it */
static int commit_refused_reported;
static void commit_report_line(void) {
    printf("Runtime: backing committed=%" PRIu64 " KiB of %" PRIu64 " MiB, in %" PRIu64 " commits (SOTC_COMMIT_ON_DEMAND)\n",
           committed_bytes>>10, (uint64_t)(POOL_SIZE+FLEX_SPAN)>>20, commit_calls);
}
/* The committed total also goes out at exit(): runtime_report() runs only on guest exit/_exit and
 * fatal paths, so a normal window close never printed it. (_exit() skips atexit handlers.) */
static void commit_report_at_exit(void) { commit_report_line(); fflush(stdout); }
/* SOTC_PC_MEMORY=1 and SOTC_HOST_IMPORT=1 import the whole backing view into Vulkan
 * (sotc_guest_memory.cpp, host_import.cpp): that touches pages not committed yet, so they
 * need the section committed up front. Same '1' test as the GPU side (vk_instance.cpp). */
static int commit_blocked_by_gpu_import(void) {
    const char *pc=getenv("SOTC_PC_MEMORY"), *hi=getenv("SOTC_HOST_IMPORT");
    return (pc && pc[0]=='1') || (hi && hi[0]=='1');
}
/* sotc: SOTC_COMMIT_CHUNKS=2 counters: faults and views under the table lock, the rest atomically. */
static uint64_t lazy_faults, lazy_views, view_opens, backing_faults, eager_opens;
static uint64_t lazy_fault_ns, lazy_fault_max_ns, lazy_fault_calls, opens_locked_others; /* cost, cumulative */
static void chunks_report_line(void) {
    uint64_t opened=0, closed=0;
    chunks_stats(&opened,&closed);
    printf("Runtime: guest chunks committed=%" PRIu64 " MiB of %" PRIu64 " MiB, opened %" PRIu64 ", closed %" PRIu64 " (SOTC_COMMIT_CHUNKS)\n",
           chunks_committed_bytes()>>20, (uint64_t)(POOL_SIZE+FLEX_SPAN)>>20, opened, closed);
    if (commit_chunks_lazy())
        printf("Runtime: first touch: %" PRIu64 " guest fault(s), %" PRIu64 " view(s), %" PRIu64 " chunk(s) opened by views, "
               "%" PRIu64 " backing fault(s), %" PRIu64 " chunk(s) opened eagerly (SOTC_COMMIT_CHUNKS=2)\n",
               lazy_faults, lazy_views, view_opens, backing_faults, eager_opens);
    if (commit_chunks_lazy()) {
        uint64_t open_ns=0, open_max=0;
        chunks_open_stats(&open_ns,&open_max);
        printf("Runtime: lazy cost: fault handler %.2f ms in %" PRIu64 " call(s), longest %.1f us; chunk opens %.2f ms, longest %.1f us; "
               "%" PRIu64 " chunk(s) opened while another thread held the table lock\n",
               (double)__atomic_load_n(&lazy_fault_ns,__ATOMIC_RELAXED)/1e6, __atomic_load_n(&lazy_fault_calls,__ATOMIC_RELAXED),
               (double)__atomic_load_n(&lazy_fault_max_ns,__ATOMIC_RELAXED)/1e3, (double)open_ns/1e6, (double)open_max/1e3,
               __atomic_load_n(&opens_locked_others,__ATOMIC_RELAXED));
    }
}
static void chunks_report_at_exit(void) { chunks_report_line(); fflush(stdout); }
/* sotc: SOTC_COMMIT_CHUNKS=1 (off by default). The guest memory is cut into 64 MiB chunks, each its
 * own pagefile section (runtime_memory_chunks.h). A chunk is committed while the guest holds bytes in
 * it or a view maps it, and closed (its commit charge returned) when neither is left. Closes run
 * after the table lock is dropped (write_unlock -> chunks_drain). Wins over SOTC_COMMIT_ON_DEMAND
 * (one line says so). Same GPU guard as COMMIT_ON_DEMAND: a whole-backing import needs one section. */
static int lazy_chunks; /* SOTC_COMMIT_CHUNKS=2: chunks open on first touch, views made on fault */
static int commit_chunks_lazy(void) { return commit_chunks_on() && lazy_chunks; }
static int commit_chunks_on(void) {
    static int v=-1;
    if (v<0) {
        const char *e=getenv("SOTC_COMMIT_CHUNKS"), *od=getenv("SOTC_COMMIT_ON_DEMAND");
        v=e && (e[0]=='1' || e[0]=='2');
        if (v && commit_blocked_by_gpu_import()) {
            v=0;
            fputs("Runtime: SOTC_COMMIT_CHUNKS refused: SOTC_PC_MEMORY=1 or SOTC_HOST_IMPORT=1 imports the whole guest memory into Vulkan, so it needs one section (SEC_COMMIT)\n",stderr);
        } else if (v) {
            lazy_chunks=e[0]=='2';
            if (lazy_chunks)
                fputs("Runtime: SOTC_COMMIT_CHUNKS=2 on: guest chunks opened on first touch (guest faults and host backing access)\n",stderr);
            else if (od && od[0]=='1')
                fputs("Runtime: SOTC_COMMIT_CHUNKS and SOTC_COMMIT_ON_DEMAND are exclusive: SOTC_COMMIT_CHUNKS used (guest memory in 64 MiB chunks)\n",stderr);
            else
                fputs("Runtime: SOTC_COMMIT_CHUNKS on: guest memory in 64 MiB chunks, committed while in use\n",stderr);
            atexit(chunks_report_at_exit);
        }
    }
    return v;
}
/* Once: the commit Windows refused for a mapping (the page file is full); that mapping fails with
 * NO_MEMORY. The game may retry the map many times, so the line is printed only the first time. */
static int chunk_refusal_reported;
static void chunks_refused_line(void) {
    if (chunk_refusal_reported) return;
    chunk_refusal_reported=1;
    uint64_t offset=0;
    const DWORD error=chunks_last_refusal(&offset);
    fprintf(stderr,"guest memory commit refused by Windows: %" PRIu64 " MiB committed, page file full? "
            "(error %lu, chunk at backing offset 0x%" PRIx64 ")\n",
            chunks_committed_bytes()>>20,error,offset);
    fflush(stderr);
}
static void chunks_alloc_note(uint64_t phys, uint64_t size) {
    if (commit_chunks_on()) chunks_note_alloc(phys,size);
}
static void chunks_free_note(uint64_t phys, uint64_t size) {
    if (commit_chunks_on()) chunks_note_free(phys,size);
}
static int chunks_pin(uint64_t phys, uint64_t size) {
    if (!commit_chunks_on() || commit_chunks_lazy()) return 0; /* lazy: chunks open on first touch, not at map */
    if (chunks_acquire(phys,size)) { chunks_refused_line(); return -1; }
    return 1;
}
static void chunks_unpin(uint64_t phys, uint64_t size) { chunks_release(phys,size); }
/* sotc: SOTC_COMMIT_CHUNKS=2: the backing bytes [offset, offset+size) are about to be read or written by
 * the host (GPU, runtime copies). Their chunks are opened now (no reference, no view), so host code does
 * not fault on them. 0 when readable, -1 when Windows refuses the commit. Mode 1 and off: 0, nothing done. */
static int backing_ready(uint64_t offset, uint64_t size) {
    if (!commit_chunks_lazy() || !size) return 0;
    const int opened=chunks_open(offset,size);
    if (opened<0) { chunks_refused_line(); return -1; }
    if (opened) __atomic_add_fetch(&eager_opens,(uint64_t)opened,__ATOMIC_RELAXED);
    return 0;
}
/* Closes queued by this thread's table-lock section run now, the lock released. Cheap when none. */
static void chunks_drain_after_unlock(void) { chunks_drain(); }
static int commit_on_demand(void) {
    static int v=-1;
    if (v<0) {
        const char *e=getenv("SOTC_COMMIT_ON_DEMAND");
        v=e && e[0]=='1';
        if (v && commit_chunks_on()) {
            v=0; /* SOTC_COMMIT_CHUNKS wins (it prints its own line) */
        } else if (v && commit_blocked_by_gpu_import()) {
            v=0;
            fputs("Runtime: SOTC_COMMIT_ON_DEMAND refused: SOTC_PC_MEMORY=1 or SOTC_HOST_IMPORT=1 imports the whole guest memory into Vulkan, so it stays committed (SEC_COMMIT)\n",stderr);
        } else if (v) {
            fputs("Runtime: SOTC_COMMIT_ON_DEMAND on: guest memory committed on demand\n",stderr);
            atexit(commit_report_at_exit);
        }
    }
    return v;
}
static DWORD section_commit_flag(void) { return commit_on_demand() ? SEC_RESERVE : SEC_COMMIT; }
static int chunk_committed(uint64_t c) { return (int)((commit_bits[c/64]>>(c%64))&1); }
/* Commits the backing pages under [phys,phys+size) that are not committed yet (the table lock is
 * held). Returns -1 when Windows refuses (the page file cannot back them): the mapping fails. */
static int commit_backing(uint64_t phys, uint64_t size) {
    if (!commit_on_demand() || !size) return 0;
    const uint64_t first=phys/COMMIT_CHUNK, last=(phys+size-1)/COMMIT_CHUNK;
    for (uint64_t c=first; c<=last;) {
        if (chunk_committed(c)) { ++c; continue; }
        uint64_t run=c;
        while (run<=last && !chunk_committed(run)) ++run;
        const uint64_t bytes=(run-c)*COMMIT_CHUNK;
        if (!VirtualAlloc(backing_base+c*COMMIT_CHUNK,bytes,MEM_COMMIT,PAGE_READWRITE)) {
            /* Refused: the failed run stays uncommitted and its mapping fails with NO_MEMORY (place).
             * Runs committed earlier in this call keep their commit (never decommitted) and stay
             * counted, so nothing leaks. Reported once: the game may retry the map many times. */
            if (!commit_refused_reported) {
                commit_refused_reported=1;
                fprintf(stderr,"guest memory commit refused by Windows: %" PRIu64 " MiB committed, page file full? "
                        "(error %lu, asked for %" PRIu64 " KiB at backing offset 0x%" PRIx64 ")\n",
                        committed_bytes>>20,GetLastError(),bytes>>10,c*COMMIT_CHUNK);
                fflush(stderr);
            }
            return -1;
        }
        for (uint64_t k=c;k<run;++k) commit_bits[k/64]|=UINT64_C(1)<<(k%64);
        __atomic_add_fetch(&committed_bytes,bytes,__ATOMIC_RELAXED);
        __atomic_add_fetch(&commit_calls,1,__ATOMIC_RELAXED);
        c=run;
    }
    return 0;
}
/* Released pages read as zero when reused, like the memfd's punched holes. */
/* sotc: SOTC_ASYNC_UPLOAD (GPU async_upload.h): waits for the copies still reading these
 * backing bytes. They take no runtime lock, so it is safe with the table locks held. */
void bbgpu_upload_wait_phys(uint64_t phys, uint64_t size) __attribute__((weak));
/* sotc: SOTC_PC_RING modes 6 and 7 (pc_ring.cpp, 1 in those modes). A thread holding the table write lock must
 * not wait for upload copies: a copy can fault on a GPU-tracked page, and that fault waits for the draw thread,
 * which holds the region mutex and needs the table read lock (runtime_memory_gpu_protect). In those modes the
 * direct map and release paths wait for the copies of their physical range before they take the lock
 * (upload_fence_before_lock), and the waits under the lock for that same range are skipped (upload_fence_done).
 * Other ranges and the other modes keep the wait under the lock. */
int sotc_pc_ring_full6(void) __attribute__((weak));
static __thread uint64_t upload_pre_lo, upload_pre_hi; /* range waited for before the lock (empty: none) */
static void upload_fence_clear(void) { upload_pre_lo=upload_pre_hi=0; }
static void upload_fence_before_lock(uint64_t phys, uint64_t size) {
    upload_fence_clear();
    if (!bbgpu_upload_wait_phys || !size || !sotc_pc_ring_full6 || !sotc_pc_ring_full6()) return;
    bbgpu_upload_wait_phys(phys,size);
    upload_pre_lo=phys; upload_pre_hi=phys+size;
}
static int upload_fence_done(uint64_t phys, uint64_t size) {
    return upload_pre_hi>upload_pre_lo && phys>=upload_pre_lo && phys+size<=upload_pre_hi;
}
static void host_discard(uint64_t phys, uint64_t size) {
    if (bbgpu_upload_wait_phys && !upload_fence_done(phys,size)) bbgpu_upload_wait_phys(phys,size);
    if (commit_chunks_on()) { chunks_discard(phys,size); return; } /* closed chunks already read as zero */
    if (!commit_on_demand()) { memset(backing_base+phys,0,size); return; }
    if (!size) return;
    for (uint64_t c=phys/COMMIT_CHUNK, last=(phys+size-1)/COMMIT_CHUNK; c<=last; ++c) {
        if (!chunk_committed(c)) continue;
        const uint64_t a=c*COMMIT_CHUNK>phys ? c*COMMIT_CHUNK : phys;
        const uint64_t b=(c+1)*COMMIT_CHUNK<phys+size ? (c+1)*COMMIT_CHUNK : phys+size;
        memset(backing_base+a,0,b-a);
    }
}
/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (backing_base) return 0;
    region_direct_init();
    runtime_memory_reserve();
    uint64_t bytes=POOL_SIZE+FLEX_SPAN;
    if (commit_chunks_on()) {
        /* sotc: SOTC_COMMIT_CHUNKS: one placeholder per chunk, sections opened on map. No pool
         * fallback: a refused commit is a NO_MEMORY at map time instead. */
        if (chunks_init(bytes)) {
            fprintf(stderr,"Runtime: cannot reserve %" PRIu64 " MiB of guest memory in chunks (error %lu)\n",bytes>>20,GetLastError());
            return -1;
        }
        chunks_defer_closes();
        backing_base=chunks_backing_base();
        return backing_base ? 0 : -1;
    }
    section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,section_commit_flag(),bytes,NULL,NULL,0);
    if (!section && pool_size_mb>pool_base_mb()) {
        /* sotc: the larger pool (BB_DMEM_MB, 9152 MiB by default since 0.59) is committed at once;
         * with little RAM and a small page file Windows refuses it (error 1450) and the game
         * stopped at boot. Fall back to the console's size. */
        fprintf(stderr,"Runtime: %" PRIu64 " MiB of guest memory refused (error %lu): using %" PRIu64 " MiB; a larger page file allows more\n",
                bytes>>20,GetLastError(),pool_base_mb());
        pool_size_mb=pool_base_mb();
        bytes=POOL_SIZE+FLEX_SPAN;
        section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,section_commit_flag(),bytes,NULL,NULL,0);
    }
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);
    return backing_base ? 0 : -1;
}
#else
/* sotc: the pre-lock upload fence (SOTC_PC_RING 6/7) exists on the Windows path only (see above). */
static void upload_fence_clear(void) {}
static void upload_fence_before_lock(uint64_t phys, uint64_t size) { (void)phys; (void)size; }
static int upload_fence_done(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    region_direct_init();
    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
void runtime_memory_reserve(void) {}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    void *mapped = !reserved
        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    return mapped==MAP_FAILED ? -1 : 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { return munmap((void *)address,size); }
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
void bbgpu_upload_wait_phys(uint64_t phys, uint64_t size) __attribute__((weak));
/* sotc: SOTC_COMMIT_CHUNKS does not exist on the memfd path: these hooks do nothing. */
static void chunks_alloc_note(uint64_t phys, uint64_t size) { (void)phys; (void)size; }
static void chunks_free_note(uint64_t phys, uint64_t size) { (void)phys; (void)size; }
static int chunks_pin(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
static void chunks_unpin(uint64_t phys, uint64_t size) { (void)phys; (void)size; }
static void chunks_drain_after_unlock(void) {}
/* sotc: SOTC_COMMIT_CHUNKS=2 does not exist on the memfd path. */
static int commit_chunks_lazy(void) { return 0; }
static void lazy_materialize_range(uintptr_t address, uint64_t size) { (void)address; (void)size; }
static int backing_ready(uint64_t offset, uint64_t size) { (void)offset; (void)size; return 0; }
int runtime_memory_lazy_fault(uintptr_t address, int write) { (void)address; (void)write; return 0; }
int runtime_memory_backing_fault(uintptr_t address, int write) { (void)address; (void)write; return 0; }
/* Linux: the memfd is sparse and pages commit on first touch, so there is nothing to commit. */
static int commit_backing(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
static void host_discard(uint64_t phys, uint64_t size) {
    if (bbgpu_upload_wait_phys) bbgpu_upload_wait_phys(phys,size); /* sotc: SOTC_ASYNC_UPLOAD */
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
#endif
/* sotc: SOTC_MIRROR (GPU memory mirror, docs/re-stutters/native/ARCH-DRAW.md part 1). The events the mirror needs
 * are queued under the table lock like the GPU hooks and delivered by flush_mirror after the lock is dropped, in
 * queue order. Armed with the GPU hooks: runtime_memory_set_gpu_hooks replays the live mappings. Deliveries are
 * serialized (mirror_deliver), so a map and an unmap of one range from two threads arrive in order, and a call
 * returns only after the events it saw queued are delivered: the mirror has the event when the guest call returns. */
void bbgpu_mirror_on_map(uint64_t addr, uint64_t size) __attribute__((weak));
void bbgpu_mirror_on_unmap(uint64_t addr, uint64_t size) __attribute__((weak));
enum { MIRROR_MAP, MIRROR_UNMAP };
typedef struct { int kind; uintptr_t address; uint64_t size; } MirrorEvent;
static MirrorEvent *mirror_events; static size_t mirror_head, mirror_count, mirror_capacity;
static pthread_mutex_t mirror_queue_lock=PTHREAD_MUTEX_INITIALIZER, mirror_deliver=PTHREAD_MUTEX_INITIALIZER;
static uint64_t mirror_maps, mirror_unmaps; /* delivered, cumulative (runtime_memory_mirror_counts) */
static int mirror_mode=-1;
static int mirror_switch(void) { /* SOTC_MIRROR 1 or 2, read once (as SotcArch::SwitchMode) */
    if (mirror_mode<0) { const char *v=getenv("SOTC_MIRROR"); mirror_mode=v && (v[0]=='1' || v[0]=='2'); }
    return mirror_mode;
}
/* Mappings the GPU can reach: every direct map, and flexible maps with GPU access bits (SCE_KERNEL_PROT_GPU_ALL). */
static int mirror_gpu_visible(int kind, int prot) { return kind==KIND_DIRECT || (kind==KIND_FLEXIBLE && (prot & 0x30)); }
/* Queues an event (table lock held). Nothing is queued before the GPU hooks are installed (no mirror yet). */
static void mirror_note(int kind, uintptr_t address, uint64_t size) {
    if (!mirror_switch() || !hook_map || !bbgpu_mirror_on_map || !bbgpu_mirror_on_unmap || !size) return;
    pthread_mutex_lock(&mirror_queue_lock);
    if (mirror_count==mirror_capacity) {
        if (mirror_head) { /* delivered events in front of the head: drop them */
            memmove(mirror_events,mirror_events+mirror_head,(mirror_count-mirror_head)*sizeof(*mirror_events));
            mirror_count-=mirror_head; mirror_head=0;
        } else {
            size_t capacity=mirror_capacity ? mirror_capacity*2 : 256;
            MirrorEvent *grown=realloc(mirror_events,capacity*sizeof(*grown));
            if (!grown) { fputs("STOP: GPU mirror queue out of memory\n",stderr); fflush(stderr); _Exit(21); }
            mirror_events=grown; mirror_capacity=capacity;
        }
    }
    mirror_events[mirror_count++]=(MirrorEvent){kind,address,size};
    pthread_mutex_unlock(&mirror_queue_lock);
}
/* Delivers the queued events to the mirror. Called with no table lock held: the mirror's side may take locks of its
 * own, and those must never wait on the table lock. */
static void flush_mirror(void) {
    if (!mirror_switch()) return;
    pthread_mutex_lock(&mirror_deliver);
    for (;;) {
        pthread_mutex_lock(&mirror_queue_lock);
        if (mirror_head==mirror_count) { mirror_head=mirror_count=0; pthread_mutex_unlock(&mirror_queue_lock); break; }
        MirrorEvent e=mirror_events[mirror_head++];
        pthread_mutex_unlock(&mirror_queue_lock);
        if (e.kind==MIRROR_MAP) { bbgpu_mirror_on_map(e.address,e.size); __atomic_add_fetch(&mirror_maps,1,__ATOMIC_RELAXED); }
        else { bbgpu_mirror_on_unmap(e.address,e.size); __atomic_add_fetch(&mirror_unmaps,1,__ATOMIC_RELAXED); }
    }
    pthread_mutex_unlock(&mirror_deliver);
}
void runtime_memory_mirror_counts(uint64_t *maps, uint64_t *unmaps) {
    *maps=__atomic_load_n(&mirror_maps,__ATOMIC_RELAXED);
    *unmaps=__atomic_load_n(&mirror_unmaps,__ATOMIC_RELAXED);
}
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==pending_capacity) {
        size_t capacity = pending_capacity ? pending_capacity*2 : 64;
        PendingHook *grown = realloc(pending, capacity*sizeof(*pending));
        if (!grown) { fputs("STOP: GPU hook queue out of memory\n",stderr); fflush(stderr); _Exit(21); }
        pending=grown; pending_capacity=capacity;
        if (capacity>64) fprintf(stderr,"Runtime: GPU hook queue grew to %zu entries\n",capacity);
    }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); flush_mirror(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
/* Frees a flexible range: from drop_range (its views gone) and from a failed map_flexible_locked. */
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    host_discard(phys,size);
    chunks_free_note(phys,size);
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
/* sotc: odd while the VMA array is being changed in place (insert, erase, field writes). The
 * GPU's lookups read the array without the lock and retry when it changed meanwhile
 * (vma_lookup_fast); a grown array is never freed, so such a read never touches freed memory. */
static uint64_t vma_seq;
static void vma_seq_begin(void) { __atomic_add_fetch(&vma_seq,1,__ATOMIC_SEQ_CST); }
static void vma_seq_end(void) { __atomic_add_fetch(&vma_seq,1,__ATOMIC_RELEASE); }
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=malloc(capacity*sizeof(*vmas));
        if (!next) return -1;
        if (vma_count) memcpy(next,vmas,vma_count*sizeof(*vmas));
        vma_seq_begin();
        __atomic_store_n(&vmas,next,__ATOMIC_RELEASE); /* the old array stays: see vma_seq */
        vma_capacity=capacity;
        vma_seq_end();
    }
    dirty(v.start,v.end);
    vma_seq_begin();
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count;
    vma_seq_end();
    return 0;
}
static void vma_erase(size_t at, size_t n) {
    if (n) dirty(vmas[at].start,vmas[at+n-1].end);
    vma_seq_begin();
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
    vma_seq_end();
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT) right.phys+=a-vmas[i].start;
    dirty(vmas[i].start,vmas[i].end);
    vma_seq_begin();
    vmas[i].end=a;
    vma_seq_end();
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if (address<USER_MIN && (address<SYSTEM_LOW_MIN || address+size>SYSTEM_LOW_MAX)) {
            fprintf(stderr,"Runtime: fixed mapping outside the guest windows: %p +0x%" PRIx64 "\n",(void *)address,size);
            return NO_MEMORY;
        }
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    /* sotc: SOTC_ASYNC_UPLOAD: memory mapped again elsewhere is not write-protected there, so the
     * copies still reading it finish first. */
    /* sotc: every direct and flexible mapping (direct_map, map_flexible, sceKernelBatchMap) comes
     * through here: the backing pages are committed before the guest view can reach them. */
    if (kind!=KIND_RESERVED && commit_backing(phys,size)) return NO_MEMORY;
    /* sotc: SOTC_COMMIT_CHUNKS: the chunks are opened (pinned) before the table changes, so a refused
     * commit fails here with nothing to undo. The pin is dropped once the views hold their own. */
    const int pinned = kind!=KIND_RESERVED ? chunks_pin(phys,size) : 0;
    if (pinned < 0) return NO_MEMORY;
    if (bbgpu_upload_wait_phys && kind!=KIND_RESERVED && !upload_fence_done(phys,size)) bbgpu_upload_wait_phys(phys,size);
    const int host_failed = host_map(address,size,host_prot(prot),kind==KIND_RESERVED,phys);
    if (pinned > 0) chunks_unpin(phys,size);
    if (host_failed) return NO_MEMORY;
    void *mapped=(void *)address;
    drop_range(address,address+size);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    if (mirror_gpu_visible(kind,prot)) mirror_note(MIRROR_MAP,address,size); /* sotc: SOTC_MIRROR */
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

/* sotc: the pool is created before its size is reported: when Windows refuses the larger pool
 * the fallback shrinks it, and the game must size its heaps for the memory it really gets. */
static ABI uint64_t direct_size(void) {
    write_lock();
    pool();
    write_unlock();
    return POOL_SIZE;
}
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) { write_unlock(); return NO_SPACE; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    chunks_alloc_note(start,size);
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    /* sotc: SOTC_PC_RING 6/7: the copies of the physical range finish before the table lock is taken. */
    if (physical>=0) upload_fence_before_lock((uint64_t)physical,size);
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    upload_fence_clear();
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (host_unmap(a,b-a)) return INVALID;
        if (mirror_gpu_visible(vmas[i].kind,vmas[i].prot)) mirror_note(MIRROR_UNMAP,a,b-a); /* sotc: SOTC_MIRROR, after the host unmap */
    }
    drop_range(start,end);
    return 0;
}
/* Unmaps the direct-memory views of a released range (the lock is held). Returns how many. */
static size_t unmap_released_vmas_locked(uint64_t start, uint64_t size) {
    size_t count=0;
    /* guard: a failed host_unmap leaves the VMA in place, so bound the scan and skip a VMA that
     * did not shrink the table (no progress), or the loop spins forever holding the lock. */
    for (size_t i=0, guard=0; i<vma_count && guard<vma_count*2+16; ++guard) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            size_t before=vma_count;
            unmap_locked(v.start+(a-v.phys),b-a);
            if (vma_count<before) { i=vma_index(v.start); ++count; continue; }
            ++i; /* unmap_locked made no progress: do not revisit this VMA */
            continue;
        }
        ++i;
    }
    return count;
}
/* sotc: SOTC_RELEASE_ORDER=0 restores the old order (zeroing before the GPU unmap hook). Default on. */
static int release_order(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_RELEASE_ORDER"); v=!(e && e[0]=='0'); }
    return v;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    flush_mirror(); /* sotc: SOTC_MIRROR: the mirror events queued by unmap_locked (not discarded above) */
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    /* sotc: SOTC_PC_RING 6/7: the copies of the range finish before the table lock is taken (host_discard
     * below then skips its wait for this range). */
    upload_fence_before_lock(start,size);
    write_lock();
    for (int pass=0; ; ++pass) {
        if (!direct_range_allocated(start,size)) { upload_fence_clear(); write_unlock(); return INVALID; }
        /* Mappings of released physical pages disappear with them. */
        size_t unmapped=unmap_released_vmas_locked(start,size);
        /* Cap the passes: a thread that keeps mapping the range must not hold this loop open. */
        /* sotc: hooks queued by other threads (pending_count) must also run before the zeroing below. */
        if (!release_order() || pass>=16 || (!unmapped && !pending_count)) break;
        /* sotc: SOTC_RELEASE_ORDER: the GPU unmap hook (queued above) runs without the lock, before the
         * zeroing below. The blocks stay allocated meanwhile, so nothing reuses the range; the check
         * runs again and unmaps any view mapped during the window. */
        write_unlock();
        flush_hooks();
        write_lock();
        if (!unmapped) break;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    host_discard(start,size); /* zero on reuse */
    chunks_free_note(start,size); /* after the views are gone (unmapped above) */
    upload_fence_clear();
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    chunks_alloc_note(phys,size); /* released by flex_free if the map fails */
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    /* sotc: Shadow of the Colossus reserves thousands of ranges while streaming; BB_MEMORY_LOG=1 lists all. */
    static unsigned reserved_logged;
    static int memory_log=-1;
    if (memory_log<0) memory_log=getenv("BB_MEMORY_LOG")!=NULL;
    if (!result && (memory_log || reserved_logged<16)) {
        printf("Runtime: reserved virtual range %p, size=%" PRIu64 "%s\n", *inout, size,
               ++reserved_logged==16 && !memory_log ? " (further ones: BB_MEMORY_LOG=1)" : "");
    }
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    dirty(start,end); /* before the host protection changes: see runtime_memory_gpu_protect */
    if (host_protect(start,end-start,host_prot(prot))) return INVALID;
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    vma_seq_begin();
    for (; i<vma_count && vmas[i].start<end; ++i) {
        /* sotc: SOTC_MIRROR: a protection change that gives or takes GPU access maps or unmaps the mirror pages */
        const int was=mirror_gpu_visible(vmas[i].kind,vmas[i].prot), now=mirror_gpu_visible(vmas[i].kind,prot);
        if (was!=now) mirror_note(now ? MIRROR_MAP : MIRROR_UNMAP,vmas[i].start,vmas[i].end-vmas[i].start);
        vmas[i].prot=prot; if (type>=0) vmas[i].type=type;
    }
    vma_seq_end();
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    /* sotc: SOTC_BATCH_SPLIT=0 holds the lock for the whole batch. At an area change the game's
     * batches held it ~0.5 s per 5 s and every GPU lookup in that time waited for the batch to
     * end; one entry at a time lets them in between (each entry is whole for them). */
    static int split=-1;
    if (split<0) { const char *v=getenv("SOTC_BATCH_SPLIT"); split=!(v && v[0]=='0'); }
    write_lock();
    for (; done<count; ++done) {
        if (split && done) { write_unlock(); write_lock(); }
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
/* sotc: exports for the native pool path (gpu/sotc/pc_pool.cpp, FreshHost). They make the same calls the game makes
 * through sceKernelReserveVirtualRange (flags 0) and sceKernelBatchMap (MAP_FIXED). Plain wrappers: reserve_range and
 * batch_map are defined on every host, so no stub is needed. */
int32_t runtime_memory_reserve_range_export(void **inout, uint64_t size, int flags, uint64_t alignment) {
    return reserve_range(inout,size,flags,alignment);
}
int32_t runtime_memory_batch_map_export(void *entries, int count, int *processed) {
    return batch_map((BatchEntry *)entries,count,processed);
}
/* sotc: SOTC_GPU_PLACE (gpu/sotc/gpu_region.cpp): maps backing for the port's GPU region at a fixed address of a
 * range the port reserved. The game takes all direct memory there is and most of its FLEXIBLE_SIZE (448 MiB) budget,
 * so the region uses the flexible backing above that budget (FLEX_SPAN is 1 GiB). The bytes are not charged to the
 * game's flexible budget (flexible_bytes), so its own maps and its available-memory query are unchanged. The region
 * never unmaps. */
int32_t runtime_memory_port_flexible_export(uint64_t start, uint64_t size, int prot) {
    int32_t result = INVALID;
    write_lock();
    if (start && size && !(size % PAGE) && !(prot & ~0x37) && !pool()) {
        uint64_t phys = flex_alloc(size);
        if (phys == UINT64_MAX) {
            result = NO_MEMORY;
        } else {
            chunks_alloc_note(phys, size);
            void *at = (void *)(uintptr_t)start;
            result = place(&at, size, prot, MAP_FIXED_FLAG, PAGE, KIND_FLEXIBLE, 0, phys);
            if (result) flex_free(phys, size);
            else flexible_bytes -= size; /* place() charged it to the game's budget */
        }
    }
    write_unlock();
    flush_hooks();
    if (result) fprintf(stderr, "Runtime: port flexible map of %" PRIu64 " bytes at 0x%" PRIx64 " failed (0x%x)\n",
                        size, start, (unsigned)result);
    return result;
}
/* sotc: largest free direct-memory range in [low,high) at the given alignment. */
static ABI int32_t direct_available(int64_t low, int64_t high, uint64_t alignment, int64_t *physical, uint64_t *size) {
    if (!alignment) alignment = PAGE;
    if (!physical || !size || low < 0 || high < 0 || !valid_alignment(alignment)) return INVALID;
    write_lock();
    pool();
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE, best=0, best_start=0;
    uint64_t cursor = (uint64_t)low;
    while (cursor < end) {
        uint64_t next = end, resume = end;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (!b.used || b.start+b.size <= cursor) continue;
            if (b.start <= cursor) { next = cursor; resume = b.start+b.size; break; }
            if (b.start < next) { next = b.start; resume = b.start+b.size; }
        }
        uint64_t start = align_up(cursor, alignment);
        if (next > start && next-start > best) { best = next-start; best_start = start; }
        cursor = resume;
    }
    write_unlock();
    if (!best) return NO_MEMORY;
    *physical = (int64_t)best_start; *size = best;
    return 0;
}
static ABI int32_t flexible_available(uint64_t *size) {
    if (!size) return INVALID;
    *size = FLEXIBLE_SIZE - flexible_bytes;
    return 0;
}
static const RuntimeExport exports[]={
    {"sceKernelAvailableDirectMemorySize",direct_available}, {"sceKernelAvailableFlexibleMemorySize",flexible_available},
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
#ifdef _WIN32
    void *p=NULL;
    while (!p && low_next+size<=USER_MIN) {
        p=VirtualAlloc((void *)low_next,size,MEM_RESERVE|MEM_COMMIT,win_prot(prot));
        low_next+=size+UINT64_C(65536); /* unmapped gap catches overruns; allocation granularity */
    }
    write_unlock();
    return p;
#else
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
#endif
}
/* ---- GPU library interface (gpu/shim/bbgpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * BB_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with BB_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (BB_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (BB_COPY_THREADS),
 * 262144 with BB_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/bbport_toggles.h). */
/* sotc: bit 39 (PendingFenceWaits) is off by default. It lets a graphics WAIT_REG_MEM pass on a
 * value a fence still queued for the draw recording thread will write, but Shadow of the
 * Colossus also waits there for labels its compute queues write: the frame then went ahead
 * without the compute results and the scene was never drawn (black screen in about half of
 * the boots). A BB_TOGGLE_FILE value replaces this default. */
uint64_t runtime_disabled_optimizations=UINT64_C(1)<<39;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0;
        if (f) { if (fscanf(f,"%llu",&value)!=1) value=0; fclose(f); }
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
#ifdef _WIN32
        compat_sleep_ns(250000000);
#else
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
#endif
    }
    return NULL;
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    /* sotc: SOTC_MIRROR: the mirror is armed here (the rasterizer exists), so the live GPU-visible mappings are replayed. */
    for (size_t i=0;i<vma_count;++i) if (mirror_gpu_visible(vmas[i].kind,vmas[i].prot)) mirror_note(MIRROR_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* Writes through the backing view; 0 when part of the range has no backing. */
/* sotc: host pointer to the backing of a mapped guest address (bypasses GPU page protection),
 * valid to the end of its VMA; NULL for reserved or unmapped addresses. */

/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind, prot; const unsigned char *backing; } CachedRegion; /* backing: view of start */
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
/* sotc: plus 512 direct-mapped entries by 16 KiB of address. The command processor's buffer binds
 * touch more regions than the 8 recent ones, so most lookups took the read lock and a binary
 * search (~4.5% of that thread: libwinpthread, vma_index). SOTC_REGION_DIRECT=0: ring only. */
#define REGION_DIRECT_SLOTS 512
static __thread CachedRegion region_direct[REGION_DIRECT_SLOTS];
static int region_direct_on=-1; /* -1 until pool(): lookups only, no fills */
static void region_direct_init(void) {
    const char *v=getenv("SOTC_REGION_DIRECT");
    region_direct_on=!(v && v[0]=='0');
}
static size_t region_direct_slot(uintptr_t address) { return (address>>14)&(REGION_DIRECT_SLOTS-1); }
/* An entry read at an older generation is checked against the writes since (region_unchanged)
 * and, while no write is under way, stamped with the current one. */
/* sotc: [qa,qb) is the part of the region the caller uses (qb 0: the whole region). A write that
 * changed another part of a direct region (the game maps 64 KiB pages inside big mappings) leaves
 * the used part as it was: direct memory stays contiguous across splits (split_at). */
static int region_current(CachedRegion *c, uint64_t generation, uintptr_t qa, uintptr_t qb) {
    if (c->generation==generation) return 1;
    if (!region_log_on()) return 0;
    if (region_unchanged(c->start,c->end,c->generation,generation)) {
        if (!(generation & 1)) c->generation=generation;
        return 1;
    }
    if (!qb || c->kind!=KIND_DIRECT) return 0;
    return region_unchanged(qa,qb<c->end ? qb : c->end,c->generation,generation);
}
/* sotc: lookups that found no entry, and that found one a write had made stale (lock report) */
static uint64_t region_misses, region_stale;
static uint64_t region_fast_hits;
void runtime_memory_cache_stats(uint64_t *misses, uint64_t *stale, uint64_t *fast) {
    *misses=__atomic_exchange_n(&region_misses,0,__ATOMIC_RELAXED);
    *stale=__atomic_exchange_n(&region_stale,0,__ATOMIC_RELAXED);
    *fast=__atomic_exchange_n(&region_fast_hits,0,__ATOMIC_RELAXED);
}
/* sotc: the VMA holding address, read without the lock (0: none there, or the array kept changing:
 * take the lock). A torn read is caught by vma_seq. */
static int vma_lookup_fast(uintptr_t address, Vma *out) {
    for (int tries=0; tries<4; ++tries) {
        const uint64_t s=__atomic_load_n(&vma_seq,__ATOMIC_ACQUIRE);
        if (s & 1) continue;
        const Vma *v=__atomic_load_n(&vmas,__ATOMIC_ACQUIRE);
        const size_t n=__atomic_load_n(&vma_count,__ATOMIC_RELAXED);
        size_t lo=0, hi=n;
        while (lo<hi) {
            size_t mid=(lo+hi)/2;
            if (__atomic_load_n(&v[mid].end,__ATOMIC_RELAXED)<=address) lo=mid+1; else hi=mid;
        }
        Vma e={0};
        if (lo<n) {
            e.start=__atomic_load_n(&v[lo].start,__ATOMIC_RELAXED);
            e.end=__atomic_load_n(&v[lo].end,__ATOMIC_RELAXED);
            e.kind=__atomic_load_n(&v[lo].kind,__ATOMIC_RELAXED);
            e.prot=__atomic_load_n(&v[lo].prot,__ATOMIC_RELAXED);
            e.phys=__atomic_load_n(&v[lo].phys,__ATOMIC_RELAXED);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vma_seq,__ATOMIC_RELAXED)!=s) continue;
        if (lo==n || e.start>address) return 0;
        *out=e;
        return 1;
    }
    return 0;
}
static __thread CachedRegion region_scratch;
static const CachedRegion *cached_region_q(uintptr_t address, uintptr_t qend) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if ((generation & 1) && !region_log_on()) return NULL;
    int found=0;
    if (region_direct_on) {
        CachedRegion *d=&region_direct[region_direct_slot(address)];
        if (d->end && d->start<=address && address<d->end) {
            if (region_current(d,generation,address,qend)) return d;
            found=1;
        }
    }
    for (unsigned i=0;i<8;++i) {
        CachedRegion *c=&region_cache[i];
        if (c->end && c->start<=address && address<c->end) {
            if (region_current(c,generation,address,qend)) return c;
            found=1;
        }
    }
    __atomic_add_fetch(found ? &region_stale : &region_misses,1,__ATOMIC_RELAXED);
    /* sotc: SOTC_REGION_FAST=0: misses take the lock. The cache missed ~100k lookups per second at
     * an area change (the GPU touches more regions than it holds), and each miss waited for the
     * writer of the moment: the VMA is read without the lock instead, and used when no write
     * since the generation read first changed its range (region_unchanged, the write under way
     * included). */
    static int fast=-1;
    if (fast<0) { const char *v=getenv("SOTC_REGION_FAST"); fast=!(v && v[0]=='0'); }
    if (!fast || !region_log_on() || !backing_base) return NULL;
    Vma e;
    if (!vma_lookup_fast(address,&e)) return NULL;
    const uint64_t now=__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST);
    const uint64_t since=generation & ~(uint64_t)1;
    if (!((now==generation && !(now & 1)) || region_unchanged(e.start,e.end,since,now) ||
          (qend && e.kind==KIND_DIRECT && region_unchanged(address,qend<e.end ? qend : e.end,since,now)))) return NULL;
    const CachedRegion entry={since,e.start,e.end,e.kind,e.prot,e.kind!=KIND_RESERVED ? backing_base+e.phys : NULL};
    region_scratch=entry;
    region_cache[region_cache_next++ % 8]=entry;
    if (region_direct_on>0) region_direct[region_direct_slot(address)]=entry;
    __atomic_add_fetch(&region_fast_hits,1,__ATOMIC_RELAXED);
    return &region_scratch;
}
/* A lookup of address alone. */
static const CachedRegion *cached_region(uintptr_t address) { return cached_region_q(address,address+1); }
/* sotc: SOTC_WRITE_FAST=0 always takes the read lock. Readbacks write here thousands of times per
 * frame and at an area change waited for map batches of other ranges (~0.4 s per 5 s). Copies up
 * to 256 KiB whose regions the cache holds as unchanged (no write under way on them) are made
 * without the lock: every region is resolved before the first byte is written, else all of it
 * takes the lock. */
static int write_backing_fast(uintptr_t address, const void *data, uint64_t size) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_WRITE_FAST"); on=!(v && v[0]=='0') && region_log_on(); }
    if (!on || exclusive_depth || (runtime_disabled_optimizations & 1) || !size || size>(256u<<10)) return 0;
    unsigned char *dst[4]; uint64_t len[4]; int parts=0;
    for (uintptr_t at=address, end=address+size; at<end; ++parts) {
        if (parts==4) return 0;
        const CachedRegion *c=cached_region_q(at,end);
        if (!c || c->kind==KIND_RESERVED || !c->backing) return 0;
        dst[parts]=(unsigned char *)c->backing+(at-c->start);
        len[parts]=(c->end<end ? c->end : end)-at;
        at+=len[parts];
    }
    /* sotc: SOTC_ASYNC_UPLOAD: the copies still reading these bytes finish first (they take no lock,
     * so waiting here is safe). The backing offset of each part is its pointer minus backing_base. */
    for (int i=0;i<parts;++i)
        if (backing_ready((uint64_t)((uintptr_t)dst[i]-(uintptr_t)backing_base),len[i])) return 0;
    if (bbgpu_upload_wait_phys) for (int i=0;i<parts;++i)
        bbgpu_upload_wait_phys((uint64_t)((uintptr_t)dst[i]-(uintptr_t)backing_base),len[i]);
    const unsigned char *src=data;
    for (int i=0;i<parts;++i) { memcpy(dst[i],src,len[i]); src+=len[i]; }
    return 1;
}
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    if (write_backing_fast(address,data,size)) return 1;
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        if (backing_ready(vmas[i].phys+(at-vmas[i].start),n)) { ok=0; break; }
        if (bbgpu_upload_wait_phys) bbgpu_upload_wait_phys(vmas[i].phys+(at-vmas[i].start),n); /* sotc */
        memcpy(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i, uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    const unsigned char *backing=vmas[i].kind!=KIND_RESERVED && backing_base ? backing_base+vmas[i].phys : NULL;
    const CachedRegion entry={generation,vmas[i].start,vmas[i].end,vmas[i].kind,vmas[i].prot,backing};
    region_cache[region_cache_next++ % 8]=entry;
    if (region_direct_on>0) region_direct[region_direct_slot(address)]=entry;
}
/* sotc: the VMA table generation, for caches of backing pointers outside this file: a pointer
 * obtained while it was even stays valid until it changes (the invariant cached_region relies
 * on: only write_lock/write_unlock bump it; page protection takes the read lock). Odd (never
 * cache) while the table changes or when the GPU caches are disabled. */
uint64_t runtime_memory_generation(void) {
    if (runtime_disabled_optimizations & 1) return 1;
    return __atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
}
/* sotc: the GPU reads guest memory through the backing view on every upload; the per-thread
 * region cache spares it the table lock (a winpthread rwlock, slow) and the binary search. */
/* sotc: the whole backing view (direct then flexible memory) and its size, for the GPU to import
 * it in place: SOTC_HOST_IMPORT (GPU host_import.cpp) and SOTC_PC_MEMORY
 * (docs/sotc/MODELO-MEMORIA-PC.md). NULL before the pool exists. One definition for both. */
const unsigned char *runtime_memory_backing_view(uint64_t *size) {
    if (size) *size = backing_base ? (uint64_t)(POOL_SIZE+FLEX_SPAN) : 0;
    return backing_base;
}
const unsigned char *runtime_memory_backing_pointer(uintptr_t address) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->backing) {
        /* NULL when the chunk cannot be opened (SOTC_COMMIT_CHUNKS=2 and Windows refused the commit). */
        const uint64_t offset=(uint64_t)(c->backing-backing_base)+(address-c->start);
        return backing_ready(offset,1) ? NULL : c->backing+(address-c->start);
    }
    read_lock();
    size_t i=vma_index(address);
    const unsigned char *p=NULL;
    if (i<vma_count && vmas[i].start<=address) {
        cache_region(i,address);
        if (vmas[i].kind!=KIND_RESERVED && backing_base) {
            const uint64_t offset=vmas[i].phys+(address-vmas[i].start);
            if (!backing_ready(offset,1)) p=backing_base+offset;
        }
    }
    read_unlock();
    return p;
}
/* Mapped length from address, up to size, across adjacent mappings. */
/* sotc: mapped length of [address, address+size) from the cached regions alone, walking
 * adjacent mappings (buffers that span two VMAs missed the one-region check every time and took
 * the read lock); 0 when a region on the way is not cached. */
static uint64_t cached_mapped_length(uintptr_t address, uint64_t size) {
    if (runtime_disabled_optimizations & 1) return 0;
    uint64_t length=0;
    for (unsigned hops=0; length<size && hops<4; ++hops) {
        const CachedRegion *c=cached_region_q(address+length,address+size);
        if (!c || c->kind==KIND_RESERVED) return 0;
        length=c->end-address;
    }
    return length>=size ? size : 0;
}
/* sotc: true when address lies in no VMA (a gap, or past the last one), read without the lock like
 * vma_lookup_fast; 0 also when the array kept changing (the caller takes the lock). */
static int vma_gap_fast(uintptr_t address) {
    for (int tries=0; tries<4; ++tries) {
        const uint64_t s=__atomic_load_n(&vma_seq,__ATOMIC_ACQUIRE);
        if (s & 1) continue;
        const Vma *v=__atomic_load_n(&vmas,__ATOMIC_ACQUIRE);
        const size_t n=__atomic_load_n(&vma_count,__ATOMIC_RELAXED);
        size_t lo=0, hi=n;
        while (lo<hi) {
            size_t mid=(lo+hi)/2;
            if (__atomic_load_n(&v[mid].end,__ATOMIC_RELAXED)<=address) lo=mid+1; else hi=mid;
        }
        const int gap=lo==n || __atomic_load_n(&v[lo].start,__ATOMIC_RELAXED)>address;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vma_seq,__ATOMIC_RELAXED)!=s) continue;
        return gap;
    }
    return 0;
}
/* sotc: SOTC_CLAMP_FAST=1 (default off). The GPU clamps every buffer bind to the mapped memory, and an
 * unbounded V# (a 64 MiB window) always ends past its mapping: cached_mapped_length answers only a
 * fully mapped range, so those clamps took the read lock, ~6% of the draw thread in the frames of an
 * area load, waiting for the guest's map batches (user's session, 3 Nov). The same walk as the locked
 * loop below, from the cached regions: adjacent mappings add up, and a reserved region or a gap ends
 * it with the length so far. 0: not answered (an uncached region, too many hops), take the lock. */
static int cached_clamp(uintptr_t address, uint64_t size, uint64_t *out) {
    uint64_t length=0;
    for (unsigned hops=0; length<size && hops<8; ++hops) {
        const uintptr_t at=address+length;
        const CachedRegion *c=cached_region_q(at,address+size);
        if (!c) {
            if (!vma_gap_fast(at)) return 0;
            *out=length;
            return 1;
        }
        if (c->kind==KIND_RESERVED) {
            *out=length;
            return 1;
        }
        length=c->end-address;
    }
    if (length<size) return 0;
    *out=size;
    return 1;
}
static int clamp_fast_on(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_CLAMP_FAST"); on=v && v[0]=='1'; }
    return on;
}
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    if (size && cached_mapped_length(address,size)==size) return size;
    if (size && clamp_fast_on() && !(runtime_disabled_optimizations & 1)) {
        uint64_t length;
        if (cached_clamp(address,size,&length)) return length;
    }
    read_lock();
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        cache_region(i,vmas[i].start>address ? vmas[i].start : address);
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region_q(address,0);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i,address);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* sotc: SOTC_COPY_LOCK_ONCE=1 (bbgpu CopySparseMemory): guest memory copied out through the
 * backing view in one call, for the GPU's texture and buffer uploads. They looked up the region
 * and then the backing pointer of every mapping run, and at an area change each lookup the region
 * cache could not answer waited for a map batch: 41-58 ms for 12-20 MB of textures in one frame
 * (user's route, 29 Oct). The cached regions are copied first; the rest under one read lock.
 * Reserved and unmapped bytes read as zero, as before. 0 (caller copies run by run) before the
 * backing view exists: a copy from guest addresses can fault, and the fault must not find the
 * read lock held. */
int runtime_memory_copy_out(uintptr_t source, unsigned char *dest, uint64_t size) {
    if (!backing_base) return 0;
    uintptr_t at=source;
    const uintptr_t end=source+size;
    if (!(runtime_disabled_optimizations & 1)) {
        while (at<end) {
            const CachedRegion *c=cached_region_q(at,end);
            if (!c || c->kind==KIND_RESERVED || !c->backing) break;
            const uintptr_t stop=c->end<end ? c->end : end;
            if (backing_ready((uint64_t)(c->backing-backing_base)+(at-c->start),stop-at)) break; /* slow path: zeros if refused */
            memcpy(dest+(at-source),c->backing+(at-c->start),stop-at);
            at=stop;
        }
    }
    if (at==end) return 1;
    read_lock();
    while (at<end) {
        const size_t i=vma_index(at);
        uintptr_t stop;
        if (i<vma_count && vmas[i].start<=at) {
            stop=vmas[i].end<end ? vmas[i].end : end;
            cache_region(i,at);
            if (vmas[i].kind==KIND_RESERVED || backing_ready(vmas[i].phys+(at-vmas[i].start),stop-at))
                memset(dest+(at-source),0,stop-at); /* reserved, or a chunk Windows refused to open: zeros */
            else memcpy(dest+(at-source),backing_base+vmas[i].phys+(at-vmas[i].start),stop-at);
        } else {
            stop=i<vma_count && vmas[i].start<end ? vmas[i].start : end;
            memset(dest+(at-source),0,stop-at);
        }
        at=stop;
    }
    read_unlock();
    return 1;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
/* sotc: diagnostics for page tracking problems: the runtime's view of a page (VMA kind and guest
 * protection) next to the protection the host really has. */
void runtime_memory_describe(uintptr_t address, char *out, size_t capacity) {
    read_lock();
    const size_t i=vma_index(address);
    int kind=0, prot=-1;
    uintptr_t start=0, end=0;
    if (i<vma_count && vmas[i].start<=address) { kind=vmas[i].kind; prot=vmas[i].prot; start=vmas[i].start; end=vmas[i].end; }
    read_unlock();
    unsigned long host=0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void *)address,&mbi,sizeof(mbi))) host=mbi.Protect;
#endif
    snprintf(out,capacity,"vma kind %d prot %#x [%#llx,%#llx), host protect %#lx",kind,prot,
             (unsigned long long)start,(unsigned long long)end,host);
}
/* sotc: SOTC_PROTECT_FAST=0 always takes the read lock. GPU page protection was the lock's
 * busiest reader at area changes (~0.3 s of waits per 5 s): it waited for map and unmap batches
 * of other ranges. When the range lies in one cached region that no write is changing, the host
 * protection is set without the lock, split where views can end (chunk boundaries, within one
 * VMA). A write that started meanwhile on the range (its carve or protect publishes the range
 * first, dirty()) may have saved and restored the old protection: it is then set again under the
 * lock, after that write. */
#ifdef _WIN32
/* sotc: SOTC_PROTECT_SPAN=1 (off by default). A range that crosses view chunks gets one
 * VirtualProtect call when the system accepts it, instead of one per chunk. Each call is a system
 * call that rewrites page tables and flushes TLBs, and the draw thread spends ~6% in them.
 *
 * Ordering proof. Let P be the protection computed below. Claim: each page of the range only ever
 * takes its old protection or P, and once protect_fast returns 1 every page of the range has P.
 * (a) Span accepted: the system sets every page of the range to P before the call returns, so
 *     the claim holds, and the generation check below runs after that, as it does for the loop.
 * (b) Span refused: nothing was set to anything but P, so the claim holds for the refusal too.
 *     The loop then runs as before. It sets every chunk to P, or returns 0 at the first chunk it
 *     cannot set, exactly as it did without the attempt.
 * (c) Intermediate states: the loop already exposes a mix of old and P across chunks while it
 *     runs; the attempt exposes a mix of the same two states, page by page, under the system's own
 *     locking. Any reader or faulting thread that saw a page in a state under the loop can see the
 *     same state under the attempt, and no new state appears.
 * Refusal sets state 2 for good, so the attempt is not retried. */
static int protect_span_on(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_PROTECT_SPAN"); on=v && v[0]=='1'; }
    return on;
}
static int protect_span_state; /* 0 untried, 1 accepted, 2 refused (sticky) */
static int protect_fast(uintptr_t address, uint64_t size, int read, int write) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_PROTECT_FAST"); on=!(v && v[0]=='0') && region_log_on(); }
    if (!on || exclusive_depth || (runtime_disabled_optimizations & 1) || !size || commit_chunks_lazy()) return 0;
    const uint64_t before=__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST);
    const CachedRegion *c=cached_region_q(address,address+size);
    if (!c || c->kind==KIND_RESERVED || address+size>c->end) return 0;
    const int guest=host_prot(c->prot);
    const int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
    const DWORD protect=win_prot(guest & want);
    const uint64_t chunk=view_chunk();
    int spanned=0;
    if (chunk && protect_span_on() && __atomic_load_n(&protect_span_state,__ATOMIC_RELAXED)!=2 &&
        address/chunk != (address+size-1)/chunk) {
        DWORD old;
        if (VirtualProtect((void *)address,(SIZE_T)size,protect,&old)) {
            __atomic_store_n(&protect_span_state,1,__ATOMIC_RELAXED);
            spanned=1;
        } else {
            __atomic_store_n(&protect_span_state,2,__ATOMIC_RELAXED);
        }
    }
    for (uintptr_t at=address, end=address+size; !spanned && at<end;) {
        uintptr_t next=chunk ? (at/chunk+1)*chunk : end;
        if (next>end) next=end;
        DWORD old;
        if (!VirtualProtect((void *)at,next-at,protect,&old)) return 0;
        at=next;
    }
    /* an odd generation is a write under way, which may have reached this range since */
    const uint64_t now=__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST);
    return (now==before && !(now & 1)) || region_unchanged(address,address+size,before & ~(uint64_t)1,now);
}
#else
static int protect_fast(uintptr_t address, uint64_t size, int read, int write) {
    (void)address; (void)size; (void)read; (void)write;
    return 0;
}
#endif
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    lazy_materialize_range(address,size); /* SOTC_COMMIT_CHUNKS=2: page protection needs the views */
    if (protect_fast(address,size,read,write)) return;
    read_lock();
    /* sotc: adjacent VMAs that end up with the same protection share one host_protect call
     * (VirtualProtect is a system call; the draw thread re-protects ranges per binding). */
    uintptr_t run_a=0, run_b=0; int run_prot=-1;
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        int prot=guest & want;
        if (run_prot==prot && run_b==a) { run_b=b; continue; }
        if (run_prot>=0) host_protect(run_a,run_b-run_a,run_prot);
        run_a=a; run_b=b; run_prot=prot;
    }
    if (run_prot>=0) host_protect(run_a,run_b-run_a,run_prot);
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    if (size && cached_mapped_length(address,size)==size) return 1;
    read_lock();
    int result=covered(address,address+size,0);
    if (result) {
        uintptr_t at=address;
        for (size_t i=vma_index(address); i<vma_count && at<address+size; ++i) {
            cache_region(i,at);
            at=vmas[i].end;
        }
    }
    read_unlock();
    return result;
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
#ifdef _WIN32
    /* sotc: SOTC_COMMIT_ON_DEMAND: backing committed so far (commit_backing), of the whole section. */
    if (commit_on_demand()) commit_report_line();
    if (commit_chunks_on()) chunks_report_line();
#endif
}
/* sotc: for the GPU's periodic report (driver.cpp), which runs while the game is up: the exit
 * line above never prints when the window is closed. Plain C linkage: declare them extern "C".
 * Linux (memfd, nothing committed on demand): 0. */
uint64_t runtime_memory_committed_bytes(void) {
#ifdef _WIN32
    if (commit_chunks_on()) return chunks_committed_bytes(); /* SOTC_COMMIT_CHUNKS: open chunks */
    return __atomic_load_n(&committed_bytes,__ATOMIC_RELAXED);
#else
    return 0;
#endif
}
uint64_t runtime_memory_commit_count(void) {
#ifdef _WIN32
    return __atomic_load_n(&commit_calls,__ATOMIC_RELAXED);
#else
    return 0;
#endif
}
/* 1 when SOTC_COMMIT_ON_DEMAND is in effect (not refused by SOTC_PC_MEMORY / SOTC_HOST_IMPORT). */
int runtime_memory_commit_on_demand(void) {
#ifdef _WIN32
    return commit_on_demand();
#else
    return 0;
#endif
}
/* sotc: SOTC_COMMIT_CHUNKS in use (not refused, not overridden by SOTC_COMMIT_ON_DEMAND). */
int runtime_memory_commit_chunks(void) {
#ifdef _WIN32
    return commit_chunks_on();
#else
    return 0;
#endif
}
#ifdef _WIN32
/* sotc: holds for GPU imports of guest memory (runtime_memory.h in runtime_memory_chunks.h). A hold
 * takes a chunk reference per physical piece of the range, recorded here so the release does not
 * depend on the guest mapping still existing (the GPU may keep an import after the guest unmapped). */
#define HOLDS_MAX 64
#define HOLD_PIECES 8
typedef struct { uintptr_t address; uint64_t size; int used, n; uint64_t phys[HOLD_PIECES], length[HOLD_PIECES]; } Hold;
static Hold holds[HOLDS_MAX];
int runtime_memory_hold_range(uint64_t guest_addr, uint64_t size) {
    if (!commit_chunks_on()) return 0; /* one section: always open */
    if (!size) return -1;
    Hold h;
    memset(&h,0,sizeof h);
    write_lock();
    int ok=1;
    for (uintptr_t at=guest_addr, end=guest_addr+size; at<end && ok;) {
        const size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED || h.n==HOLD_PIECES) { ok=0; break; }
        const uintptr_t stop=vmas[i].end<end ? vmas[i].end : end;
        h.phys[h.n]=vmas[i].phys+(at-vmas[i].start);
        h.length[h.n]=stop-at;
        ++h.n;
        at=stop;
    }
    size_t slot=HOLDS_MAX;
    for (size_t s=0; s<HOLDS_MAX; ++s) if (!holds[s].used) { slot=s; break; }
    int result=-1;
    if (ok && slot<HOLDS_MAX) {
        int taken=0;
        while (taken<h.n && !chunks_acquire(h.phys[taken],h.length[taken])) ++taken;
        if (taken==h.n) {
            h.address=(uintptr_t)guest_addr;
            h.size=size;
            h.used=1;
            holds[slot]=h;
            result=0;
        } else {
            for (int k=0;k<taken;++k) chunks_release(h.phys[k],h.length[k]); /* all or nothing */
        }
    }
    write_unlock();
    return result;
}
void runtime_memory_unhold_range(uint64_t guest_addr, uint64_t size) {
    if (!commit_chunks_on()) return;
    write_lock();
    for (size_t s=0; s<HOLDS_MAX; ++s) {
        Hold *h=&holds[s];
        if (!h->used || h->address!=(uintptr_t)guest_addr || h->size!=size) continue;
        for (int k=0;k<h->n;++k) chunks_release(h->phys[k],h->length[k]);
        h->used=0;
        break;
    }
    write_unlock();
}
/* ---- SOTC_COMMIT_CHUNKS=2: chunks opened on first touch ----
 * A mapping in this mode is a set of lazy ranges (runtime_memory_chunks.c, chunks_lazy_*): each one is a placeholder
 * in the guest range with its physical chunk and no host view. The first access to it faults into
 * runtime_memory_lazy_fault, which opens the chunk and views every lazy range of that chunk at once. The lazy list
 * is guarded by chunks_cs; the region table keeps no lazy flag of its own: lazy_region asks the list. Protection
 * changes update the list (host_protect), and GPU page protection views the ranges it restricts first
 * (lazy_materialize_range).
 *
 * LOCK ORDER. Locks: T, the table lock (exclusive for writers, shared for readers: write_lock, read_lock), and
 * C, chunks_cs (a recursive critical section). "Waits" are waits for another thread to finish.
 *  1. Order: T, then C. Table code takes C inside its T section: carve_host for its whole change phase, host_map,
 *     host_protect and the lazy list calls for their short sections. No code takes T while holding C.
 *  2. The exception handler and the backing fault take C only. runtime_memory_lazy_fault takes no table lock (the
 *     fault of a thread that holds T is refused, as before); runtime_memory_backing_fault calls chunks_open. The
 *     chunk module takes no lock but C and never calls into the runtime.
 *  3. A thread holding C never waits for another thread. The Windows calls made under C (sections, views,
 *     VirtualProtect, placeholders, and SuspendThread/GetThreadContext in carve_host) do not wait on a guest or copy
 *     thread. The waits that do (bbgpu_upload_wait_phys: copy threads, which may fault on a closed chunk and need C)
 *     run before C is taken: carve_host performs them first, host_discard performs them holding T only.
 *  4. carve_host takes C before it suspends the other guest threads, so no suspended thread can hold C: a thread
 *     inside the handler holds C only until it returns, and carve_host waits for that.
 * The graph therefore has the edge T -> C and no edge out of C. The cycle that existed before (T held by a thread
 * waiting for a copy thread, the copy thread faulting and waiting for T in the handler) cannot form: the handler
 * never takes T, and nothing that holds C waits. A fault taken while another thread holds T needs only C, which
 * that holder takes in short sections, so the fault completes and the holder's wait ends. */

/* Cumulative counters of the first-touch path (atomic; read by runtime_memory_lazy_* and the report). */
static void stat_max(uint64_t *max, uint64_t v) {
    uint64_t cur = __atomic_load_n(max, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(max, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* Whether another thread holds the table lock: a writer, or readers besides this thread (read_depth). */
static int table_held_by_others(void) {
    if (exclusive_depth) return 0;
    const int64_t state = __atomic_load_n(&lock_state, __ATOMIC_RELAXED);
    return state < 0 || state > (int64_t)read_depth;
}

/* A region is lazy while the lazy list holds its range (mapped, no host view yet). */
static int lazy_region(const HostRegion *r) { return commit_chunks_lazy() && chunks_lazy_at(r->start); }

/* Lazy list not usable from a thread that is changing the table (its own operation may be half done): faults
 * there are refused, as they always were. Reported once. */
static void lazy_refused_by_own_table(uintptr_t address) {
    static int reported;
    if (reported) return;
    reported = 1;
    fprintf(stderr, "Runtime: guest address %p is in a chunk not open yet and this thread holds the memory table lock: "
            "hard fault (SOTC_COMMIT_CHUNKS=2)\n", (void *)address);
    fflush(stderr);
}

/* sotc: an access that faulted and was resolved runs again. The same address failing over and over
 * (a fault no handler resolves) ends in the normal fault path after a few hundred tries, not in a hang. */
static int fault_retry_ok(uintptr_t address) {
    static __thread uintptr_t last;
    static __thread unsigned repeats;
    if (address == last) return ++repeats <= 512;
    last = address;
    repeats = 1;
    return 1;
}

/* Whether a page with this protection lets an access of this kind (ExceptionInformation[0]: 0 read,
 * 1 write, 8 execute) through. A guard page never does. */
static int access_allowed(DWORD protect, uintptr_t kind) {
    const DWORD p = protect & 0xff;
    if (protect & PAGE_GUARD) return 0;
    if (p == PAGE_NOACCESS) return 0;
    if (kind == 1) return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
    if (kind == 8) return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    return 1;
}

/* Exception handler path of an access to a placeholder: the lazy range at address is viewed (its chunk opens),
 * or the access is not ours. 1: runs again. Takes no table lock. */
static int lazy_fault_slow(uintptr_t address, int write) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)address, &m, sizeof(m))) return 0;
    if (m.State != MEM_RESERVE)
        return access_allowed(m.Protect, (uintptr_t)write) && fault_retry_ok(address); /* a view there now */
    if (exclusive_depth || read_depth) {
        lazy_refused_by_own_table(address);
        return 0;
    }
    const int others = table_held_by_others();
    int viewed = 0, opened = 0;
    const int r = chunks_lazy_fault(address, &viewed, &opened);
    if (r) __atomic_add_fetch(&lazy_faults, 1, __ATOMIC_RELAXED);
    if (viewed) __atomic_add_fetch(&lazy_views, (uint64_t)viewed, __ATOMIC_RELAXED);
    if (opened) {
        __atomic_add_fetch(&view_opens, (uint64_t)opened, __ATOMIC_RELAXED);
        if (others) __atomic_add_fetch(&opens_locked_others, (uint64_t)opened, __ATOMIC_RELAXED);
    }
    if (r > 0) return fault_retry_ok(address);
    if (r < 0) {
        chunks_refused_line();
        return 0;
    }
    /* No lazy range at address now: another thread viewed it meanwhile (the page allows the access now),
     * or it was never a guest mapping. */
    if (VirtualQuery((void *)address, &m, sizeof(m)) && m.State != MEM_RESERVE && access_allowed(m.Protect, (uintptr_t)write))
        return fault_retry_ok(address);
    return 0;
}

/* From the vectored exception handler, before page tracking: an access violation on a guest address whose range
 * has no view yet (its chunk is closed or was never viewed). The chunk is opened and every lazy range of it gets
 * its view; 1 means the instruction runs again. 0: not ours (a real fault), or the thread holds the table lock
 * itself (a hard fault). Takes chunks_cs only, never the table lock: a thread may fault while another holds the
 * table lock and waits for it (lock order, above). */
int runtime_memory_lazy_fault(uintptr_t address, int write) {
    if (!commit_chunks_lazy()) return 0;
    if (backing_base && (uintptr_t)address - (uintptr_t)backing_base < chunks_backing_size()) return 0; /* a host address */
    const uint64_t t0 = qpc_ns();
    __atomic_add_fetch(&lazy_fault_calls, 1, __ATOMIC_RELAXED);
    const int handled = lazy_fault_slow(address, write);
    const uint64_t took = qpc_ns() - t0;
    __atomic_add_fetch(&lazy_fault_ns, took, __ATOMIC_RELAXED);
    stat_max(&lazy_fault_max_ns, took);
    return handled;
}

/* GPU page protection of [address,address+size): the lazy ranges there are viewed first, so the protection applies
 * to them. A thread holding the table lock cannot do it (the views would change under its own operation): the
 * ranges stay lazy and the protection misses them (reported once). */
static void lazy_materialize_range(uintptr_t address, uint64_t size) {
    if (!commit_chunks_lazy() || !size) return;
    if (exclusive_depth || read_depth) {
        static int reported;
        if (!reported) {
            reported = 1;
            fprintf(stderr, "Runtime: GPU protection of %p on a thread holding the memory table lock: lazy chunks not viewed for it\n", (void *)address);
        }
        return;
    }
    int viewed = 0, opened = 0;
    const int r = chunks_lazy_materialize(address, address + size, &viewed, &opened);
    if (viewed) __atomic_add_fetch(&lazy_views, (uint64_t)viewed, __ATOMIC_RELAXED);
    if (opened) __atomic_add_fetch(&view_opens, (uint64_t)opened, __ATOMIC_RELAXED);
    if (r < 0) chunks_refused_line();
}

/* From the vectored exception handler: a host access (GPU, runtime copy) through the backing view to a chunk that
 * is not open. The chunk is opened (no views: chunks_cs only, so it is safe under any lock); 1 means the access runs
 * again. A chunk another thread opened meanwhile runs again the same way. */
int runtime_memory_backing_fault(uintptr_t address, int write) {
    (void)write; /* the backing view is read-write whenever its chunk is open */
    if (!commit_chunks_lazy() || !backing_base) return 0;
    const uintptr_t base = (uintptr_t)backing_base;
    if (address < base || address - base >= chunks_backing_size()) return 0;
    const int others = table_held_by_others();
    const int opened = chunks_open(address - base, 1);
    if (opened < 0) {
        chunks_refused_line();
        return 0;
    }
    if (opened) {
        __atomic_add_fetch(&backing_faults, 1, __ATOMIC_RELAXED);
        if (others) __atomic_add_fetch(&opens_locked_others, (uint64_t)opened, __ATOMIC_RELAXED);
        return 1;
    }
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery((void *)address, &m, sizeof(m)) && m.State == MEM_COMMIT && access_allowed(m.Protect, (uintptr_t)write))
        return fault_retry_ok(address);
    return 0;
}

/* Cost of the first-touch path (runtime_memory_chunks.h). Cumulative. */
uint64_t runtime_memory_lazy_ns(void) { return __atomic_load_n(&lazy_fault_ns, __ATOMIC_RELAXED); }
uint64_t runtime_memory_lazy_max_ns(void) { return __atomic_load_n(&lazy_fault_max_ns, __ATOMIC_RELAXED); }
uint64_t runtime_memory_lazy_calls(void) { return __atomic_load_n(&lazy_fault_calls, __ATOMIC_RELAXED); }
uint64_t runtime_memory_chunk_open_ns(void) {
    uint64_t total = 0, longest = 0;
    chunks_open_stats(&total, &longest);
    return total;
}
uint64_t runtime_memory_chunk_open_max_ns(void) {
    uint64_t total = 0, longest = 0;
    chunks_open_stats(&total, &longest);
    return longest;
}
uint64_t runtime_memory_chunk_opens_locked(void) { return __atomic_load_n(&opens_locked_others, __ATOMIC_RELAXED); }
#else
int runtime_memory_hold_range(uint64_t guest_addr, uint64_t size) { (void)guest_addr; (void)size; return 0; }
void runtime_memory_unhold_range(uint64_t guest_addr, uint64_t size) { (void)guest_addr; (void)size; }
uint64_t runtime_memory_lazy_ns(void) { return 0; }
uint64_t runtime_memory_lazy_max_ns(void) { return 0; }
uint64_t runtime_memory_lazy_calls(void) { return 0; }
uint64_t runtime_memory_chunk_open_ns(void) { return 0; }
uint64_t runtime_memory_chunk_open_max_ns(void) { return 0; }
uint64_t runtime_memory_chunk_opens_locked(void) { return 0; }
#endif

