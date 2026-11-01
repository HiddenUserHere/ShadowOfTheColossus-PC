/* Chunked backing of the guest memory (Windows). API and integration notes: runtime_memory_chunks.h.
 * Each 64 MiB chunk of the physical guest space is a pagefile section of its own. The backing range
 * is one reserved host range split into one placeholder per chunk; a chunk's section replaces its
 * placeholder while the chunk is open, and the placeholder comes back when it closes. */
#include "runtime_memory_chunks.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#define CHUNK_SHIFT CHUNKS_CHUNK_SHIFT
#define CHUNK_BYTES CHUNKS_CHUNK_BYTES
/* 512 chunks: 32 GiB of physical guest space, far above the largest pool (BB_DMEM_MB clamp 16 GiB
 * plus the 1 GiB flexible span, 272 chunks). */
#define CHUNKS_MAX 512

typedef struct {
    HANDLE section;     /* NULL while the chunk is closed */
    uint32_t refs;      /* holders: guest views and chunks_acquire references */
    uint64_t allocated; /* bytes the guest allocator holds in this chunk */
} Chunk;

static Chunk chunk[CHUNKS_MAX];
static uint64_t nchunks;
static uint8_t *backing;
static CRITICAL_SECTION chunks_cs;
static int initialized;
static uint64_t committed, opened_total, closed_total;
static int deferred;               /* idle chunks are queued for chunks_drain instead of closed at once */
static uint8_t queued[CHUNKS_MAX]; /* idle chunk waiting for chunks_drain */
static uint32_t queued_count;
static DWORD refusal_error;        /* the last refused open (chunks_last_refusal) */
static uint64_t refusal_offset;
static uint64_t open_ns_total, open_ns_max; /* time in open_locked (chunks_open_stats) */

/* Monotonic time in ns (QueryPerformanceCounter). */
static uint64_t now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
}

/* Lazy guest views (chunks_lazy_add): sorted by start, disjoint, each inside one chunk. Guarded by chunks_cs. */
typedef struct { uintptr_t end; DWORD protect; } LazyRun; /* protection of the part from the previous run's end to end */
typedef struct { uintptr_t start, end; uint64_t phys; LazyRun *runs; size_t nruns; } LazyEntry;
static LazyEntry *lazy_list;
static size_t lazy_len, lazy_cap;

/* Whether [phys, phys+size) is non-empty and lies inside the reserved range. */
static int range_valid(uint64_t phys, uint64_t size) {
    const uint64_t limit = nchunks << CHUNK_SHIFT;
    return initialized && size && phys < limit && size <= limit - phys;
}

/* The part of [phys, phys+size) inside chunk c, as [*from, *to). */
static void chunk_part(uint64_t c, uint64_t phys, uint64_t size, uint64_t *from, uint64_t *to) {
    const uint64_t base = c << CHUNK_SHIFT;
    *from = phys > base ? phys : base;
    *to = phys + size < base + CHUNK_BYTES ? phys + size : base + CHUNK_BYTES;
}

static void cancel_close_locked(uint64_t c);

/* Opens chunk c: a fresh section (zeroed pages) mapped into its backing placeholder. */
static int open_locked(uint64_t c) {
    if (chunk[c].section) return 0;
    const uint64_t t0 = now_ns();
    int result = -1;
    DWORD error = 0;
    HANDLE s = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS, PAGE_EXECUTE_READWRITE,
                                  SEC_COMMIT, CHUNK_BYTES, NULL, NULL, 0);
    if (s) {
        if (MapViewOfFile3(s, GetCurrentProcess(), backing + (c << CHUNK_SHIFT), 0, CHUNK_BYTES,
                           MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0)) {
            __atomic_store_n(&chunk[c].section, s, __ATOMIC_RELEASE); /* read without the lock by chunks_open */
            committed += CHUNK_BYTES;
            ++opened_total;
            result = 0;
        } else {
            error = GetLastError();
            CloseHandle(s);
        }
    } else {
        error = GetLastError();
    }
    const uint64_t took = now_ns() - t0;
    open_ns_total += took;
    if (took > open_ns_max) open_ns_max = took;
    if (result) SetLastError(error); /* the caller reads GetLastError for the refusal */
    return result;
}

/* Closes chunk c: the backing view becomes a placeholder again, and the section (its commit
 * charge) goes once no view of it is left. Guest views of the chunk must be gone by now. */
static void close_locked(uint64_t c) {
    if (!chunk[c].section) return;
    if (!UnmapViewOfFile2(GetCurrentProcess(), backing + (c << CHUNK_SHIFT), MEM_PRESERVE_PLACEHOLDER)) {
        /* Keep the section: the backing view is still mapped, so the chunk is still open. */
        fprintf(stderr, "Runtime: closing guest chunk at offset 0x%" PRIx64 " failed (error %lu)\n",
                c << CHUNK_SHIFT, GetLastError());
        return;
    }
    const HANDLE s = chunk[c].section;
    __atomic_store_n(&chunk[c].section, (HANDLE)0, __ATOMIC_RELEASE);
    CloseHandle(s);
    committed -= CHUNK_BYTES;
    ++closed_total;
    cancel_close_locked(c);
}

/* A queued close is dropped when the chunk is opened again or closed by other means. */
static void cancel_close_locked(uint64_t c) {
    if (queued[c]) {
        queued[c] = 0;
        --queued_count;
    }
}

/* A chunk that nothing holds (no reference, no allocated bytes) is closed: at once, or queued for
 * chunks_drain when closes are deferred (the caller holds the runtime's table lock). */
static void idle_check_locked(uint64_t c) {
    if (!chunk[c].section || chunk[c].refs || chunk[c].allocated) return;
    if (!deferred) {
        close_locked(c);
    } else if (!queued[c]) {
        queued[c] = 1;
        ++queued_count;
    }
}

/* Opens the closed chunks of the range and takes a reference on each chunk. On a refused commit,
 * the chunks this call opened are closed again and nothing is referenced. The refusal is recorded
 * (chunks_last_refusal); the runtime reports it. */
static int acquire_locked(uint64_t phys, uint64_t size) {
    const uint64_t first = phys >> CHUNK_SHIFT, last = (phys + size - 1) >> CHUNK_SHIFT;
    uint64_t fresh[CHUNKS_MAX / 64] = {0};
    for (uint64_t c = first; c <= last; ++c) {
        if (chunk[c].section) continue;
        if (open_locked(c)) {
            refusal_error = GetLastError();
            refusal_offset = c << CHUNK_SHIFT;
            for (uint64_t k = first; k < c; ++k)
                if ((fresh[k / 64] >> (k % 64)) & 1) close_locked(k); /* no references yet: nothing uses them */
            return -1;
        }
        fresh[c / 64] |= UINT64_C(1) << (c % 64);
    }
    for (uint64_t c = first; c <= last; ++c) {
        ++chunk[c].refs;
        cancel_close_locked(c); /* re-acquired before its queued close: the close is cancelled */
    }
    return 0;
}

static void release_locked(uint64_t phys, uint64_t size) {
    const uint64_t first = phys >> CHUNK_SHIFT, last = (phys + size - 1) >> CHUNK_SHIFT;
    for (uint64_t c = first; c <= last; ++c)
        if (chunk[c].refs) --chunk[c].refs;
    for (uint64_t c = first; c <= last; ++c) idle_check_locked(c);
}

static void allocated_locked(uint64_t phys, uint64_t size, int freeing) {
    const uint64_t first = phys >> CHUNK_SHIFT, last = (phys + size - 1) >> CHUNK_SHIFT;
    for (uint64_t c = first; c <= last; ++c) {
        uint64_t from, to;
        chunk_part(c, phys, size, &from, &to);
        if (!freeing) {
            chunk[c].allocated += to - from;
            continue;
        }
        chunk[c].allocated = chunk[c].allocated > to - from ? chunk[c].allocated - (to - from) : 0;
        idle_check_locked(c);
    }
}

int chunks_init(uint64_t bytes) {
    if (initialized) return bytes && bytes <= (nchunks << CHUNK_SHIFT) ? 0 : -1;
    if (!bytes) return -1;
    const uint64_t n = (bytes + CHUNK_BYTES - 1) >> CHUNK_SHIFT;
    if (n > CHUNKS_MAX) return -1;
    uint8_t *base = VirtualAlloc2(NULL, NULL, n * CHUNK_BYTES, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                  PAGE_NOACCESS, NULL, 0);
    if (!base) return -1;
    /* Each call splits the first chunk off the remaining placeholder: all chunks end up as placeholders. */
    for (uint64_t i = 0; i + 1 < n; ++i)
        if (!VirtualFree(base + i * CHUNK_BYTES, CHUNK_BYTES, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return -1;
    InitializeCriticalSection(&chunks_cs);
    backing = base;
    nchunks = n;
    initialized = 1;
    return 0;
}

uint8_t *chunks_backing_base(void) { return initialized ? backing : NULL; }

uint64_t chunks_backing_size(void) { return initialized ? nchunks << CHUNK_SHIFT : 0; }

int chunk_section_for(uint64_t phys, HANDLE *section, uint64_t *offset_in_section) {
    if (!range_valid(phys, 1)) return -1;
    const uint64_t c = phys >> CHUNK_SHIFT;
    EnterCriticalSection(&chunks_cs);
    const HANDLE s = chunk[c].section;
    LeaveCriticalSection(&chunks_cs);
    if (!s) return -1;
    if (section) *section = s;
    if (offset_in_section) *offset_in_section = phys & (CHUNK_BYTES - 1);
    return 0;
}

uint64_t chunks_piece(uint64_t phys, uint64_t size) {
    const uint64_t next = ((phys >> CHUNK_SHIFT) + 1) << CHUNK_SHIFT;
    return next - phys < size ? next - phys : size;
}

int chunks_acquire(uint64_t phys, uint64_t size) {
    if (!range_valid(phys, size)) return -1;
    EnterCriticalSection(&chunks_cs);
    const int result = acquire_locked(phys, size);
    LeaveCriticalSection(&chunks_cs);
    return result;
}

void chunks_release(uint64_t phys, uint64_t size) {
    if (!range_valid(phys, size)) return;
    EnterCriticalSection(&chunks_cs);
    release_locked(phys, size);
    LeaveCriticalSection(&chunks_cs);
}

int chunks_open(uint64_t phys, uint64_t size) {
    if (!range_valid(phys, size)) return -1;
    const uint64_t first = phys >> CHUNK_SHIFT, last = (phys + size - 1) >> CHUNK_SHIFT;
    uint64_t c;
    /* Lock-free when every chunk of the range is open already (the usual case once touched). */
    for (c = first; c <= last; ++c)
        if (!__atomic_load_n(&chunk[c].section, __ATOMIC_ACQUIRE)) break;
    if (c > last) return 0;
    uint64_t fresh[CHUNKS_MAX / 64] = {0};
    int opened = 0;
    EnterCriticalSection(&chunks_cs);
    for (c = first; c <= last; ++c) {
        if (chunk[c].section) continue;
        if (open_locked(c)) {
            refusal_error = GetLastError();
            refusal_offset = c << CHUNK_SHIFT;
            for (uint64_t k = first; k < c; ++k)
                if ((fresh[k / 64] >> (k % 64)) & 1) close_locked(k); /* opened here, nothing uses them yet */
            LeaveCriticalSection(&chunks_cs);
            return -1;
        }
        fresh[c / 64] |= UINT64_C(1) << (c % 64);
        ++opened;
    }
    LeaveCriticalSection(&chunks_cs);
    return opened;
}

void chunks_note_alloc(uint64_t phys, uint64_t size) {
    if (!range_valid(phys, size)) return;
    EnterCriticalSection(&chunks_cs);
    allocated_locked(phys, size, 0);
    LeaveCriticalSection(&chunks_cs);
}

void chunks_note_free(uint64_t phys, uint64_t size) {
    if (!range_valid(phys, size)) return;
    EnterCriticalSection(&chunks_cs);
    allocated_locked(phys, size, 1);
    LeaveCriticalSection(&chunks_cs);
}

int chunks_view_map(void *address, uint64_t phys, uint64_t size, DWORD protect) {
    if (!address || !range_valid(phys, size)) return -1;
    const uint64_t c = phys >> CHUNK_SHIFT;
    if (((phys + size - 1) >> CHUNK_SHIFT) != c) return -1; /* one view, one chunk */
    const DWORD initial = protect == PAGE_NOACCESS ? PAGE_READWRITE : protect;
    int result = -1;
    EnterCriticalSection(&chunks_cs);
    if (!acquire_locked(phys, size)) {
        void *view = MapViewOfFile3(chunk[c].section, GetCurrentProcess(), address, phys - (c << CHUNK_SHIFT),
                                    size, MEM_REPLACE_PLACEHOLDER, initial, NULL, 0);
        DWORD old;
        if (view && (initial == protect || VirtualProtect(address, size, protect, &old))) {
            result = 0;
        } else if (view) {
            /* The view exists: undo it, and the reference only if the placeholder came back. */
            if (UnmapViewOfFile2(GetCurrentProcess(), address, MEM_PRESERVE_PLACEHOLDER)) release_locked(phys, size);
        } else {
            release_locked(phys, size);
        }
    }
    LeaveCriticalSection(&chunks_cs);
    return result;
}

int chunks_view_unmap(void *address, uint64_t phys, uint64_t size) {
    if (!address || !range_valid(phys, size)) return -1;
    if (((phys + size - 1) >> CHUNK_SHIFT) != (phys >> CHUNK_SHIFT)) return -1;
    int result = -1;
    EnterCriticalSection(&chunks_cs);
    if (UnmapViewOfFile2(GetCurrentProcess(), address, MEM_PRESERVE_PLACEHOLDER)) {
        release_locked(phys, size);
        result = 0;
    }
    LeaveCriticalSection(&chunks_cs);
    return result;
}

void chunks_discard(uint64_t phys, uint64_t size) {
    if (!range_valid(phys, size)) return;
    const uint64_t first = phys >> CHUNK_SHIFT, last = (phys + size - 1) >> CHUNK_SHIFT;
    EnterCriticalSection(&chunks_cs);
    for (uint64_t c = first; c <= last; ++c) {
        if (!chunk[c].section) continue; /* closed chunks read as zero */
        uint64_t from, to;
        chunk_part(c, phys, size, &from, &to);
        memset(backing + from, 0, to - from);
    }
    LeaveCriticalSection(&chunks_cs);
}

uint64_t chunks_committed_bytes(void) {
    if (!initialized) return 0;
    EnterCriticalSection(&chunks_cs);
    const uint64_t bytes = committed;
    LeaveCriticalSection(&chunks_cs);
    return bytes;
}

void chunks_defer_closes(void) {
    EnterCriticalSection(&chunks_cs);
    deferred = 1;
    LeaveCriticalSection(&chunks_cs);
}

void chunks_drain(void) {
    /* Unlocked fast path: a close is queued only by a thread holding the table lock, and that
     * thread drains it itself when it lets the lock go. */
    if (!__atomic_load_n(&queued_count, __ATOMIC_RELAXED)) return;
    for (uint64_t c = 0; c < nchunks; ++c) {
        EnterCriticalSection(&chunks_cs); /* one chunk at a time: a close takes milliseconds */
        if (queued[c]) {
            cancel_close_locked(c);
            if (!chunk[c].refs && !chunk[c].allocated) close_locked(c);
        }
        LeaveCriticalSection(&chunks_cs);
    }
}

DWORD chunks_last_refusal(uint64_t *offset) {
    EnterCriticalSection(&chunks_cs);
    const DWORD error = refusal_error;
    if (offset) *offset = refusal_offset;
    LeaveCriticalSection(&chunks_cs);
    return error;
}

void chunks_stats(uint64_t *opened, uint64_t *closed) {
    if (!initialized) {
        if (opened) *opened = 0;
        if (closed) *closed = 0;
        return;
    }
    EnterCriticalSection(&chunks_cs);
    if (opened) *opened = opened_total;
    if (closed) *closed = closed_total;
    LeaveCriticalSection(&chunks_cs);
}

void chunks_open_stats(uint64_t *ns_total, uint64_t *ns_max) {
    EnterCriticalSection(&chunks_cs);
    if (ns_total) *ns_total = open_ns_total;
    if (ns_max) *ns_max = open_ns_max;
    LeaveCriticalSection(&chunks_cs);
}

void chunks_lock(void) {
    if (initialized) EnterCriticalSection(&chunks_cs);
}

void chunks_unlock(void) {
    if (initialized) LeaveCriticalSection(&chunks_cs);
}

/* ---- lazy guest views ----
 * Every function below runs with chunks_cs held (the public ones take it). None waits for another thread.
 * One entry is one lazy region: one placeholder (a range the table keeps as one region), so a view always
 * replaces exactly one placeholder. A protection change does not split an entry: it rewrites the runs of
 * the entry (the protection of each part of it, as the view takes it). The entries split only where the
 * table splits its regions (chunks_lazy_cut, from carve_host). */

/* Index of the first entry whose end is above address (the entries are sorted and disjoint). */
static size_t lazy_first_above(uintptr_t address) {
    size_t lo = 0, hi = lazy_len;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (lazy_list[mid].end <= address) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Replaces the entries [at, at+n) by the m entries in repl (their runs now belong to the list). */
static void lazy_splice(size_t at, size_t n, const LazyEntry *repl, size_t m) {
    const size_t len = lazy_len - n + m;
    if (len > lazy_cap) {
        size_t cap = lazy_cap ? lazy_cap : 64;
        while (cap < len) cap *= 2;
        LazyEntry *grown = realloc(lazy_list, cap * sizeof *grown);
        if (!grown) {
            fputs("Runtime: out of memory (lazy guest views)\n", stderr);
            exit(1);
        }
        lazy_list = grown;
        lazy_cap = cap;
    }
    memmove(lazy_list + at + m, lazy_list + at + n, (lazy_len - at - n) * sizeof *lazy_list);
    if (m) memcpy(lazy_list + at, repl, m * sizeof *repl);
    lazy_len = len;
}

static LazyRun *lazy_alloc_runs(size_t n) {
    LazyRun *runs = malloc(n * sizeof *runs);
    if (!runs) {
        fputs("Runtime: out of memory (lazy guest views)\n", stderr);
        exit(1);
    }
    return runs;
}

/* The part [a,b) of entry e as a new entry: its phys follows its start, its runs are the runs of e within [a,b). */
static LazyEntry lazy_piece(const LazyEntry *e, uintptr_t a, uintptr_t b) {
    LazyEntry p = {a, b, e->phys + (a - e->start), lazy_alloc_runs(e->nruns), 0};
    uintptr_t prev = e->start;
    for (size_t k = 0; k < e->nruns && prev < b; ++k) {
        const uintptr_t run_end = e->runs[k].end;
        if (run_end > a) {
            const uintptr_t end = run_end < b ? run_end : b;
            if (p.nruns && p.runs[p.nruns - 1].protect == e->runs[k].protect) p.runs[p.nruns - 1].end = end;
            else p.runs[p.nruns++] = (LazyRun){end, e->runs[k].protect};
        }
        prev = run_end;
    }
    return p;
}

/* Removes [a,b) from the lazy ranges: an entry that straddles a or b keeps its outer parts as entries. */
static void lazy_cut_locked(uintptr_t a, uintptr_t b) {
    const size_t i0 = lazy_first_above(a);
    size_t i1 = i0;
    while (i1 < lazy_len && lazy_list[i1].start < b) ++i1;
    const size_t n = i1 - i0;
    if (!n) return;
    LazyEntry *pieces = malloc(2 * n * sizeof *pieces); /* at most two outer parts per entry */
    if (!pieces) {
        fputs("Runtime: out of memory (lazy guest views)\n", stderr);
        exit(1);
    }
    size_t m = 0;
    for (size_t i = i0; i < i1; ++i) {
        const LazyEntry *e = &lazy_list[i];
        if (e->start < a) pieces[m++] = lazy_piece(e, e->start, a);
        if (e->end > b) pieces[m++] = lazy_piece(e, b, e->end);
    }
    for (size_t i = i0; i < i1; ++i) free(lazy_list[i].runs);
    lazy_splice(i0, n, pieces, m);
    free(pieces);
}

/* Gives [a,b) the protection `protect` in every entry it overlaps. No entry is split. */
static void lazy_setprot_locked(uintptr_t a, uintptr_t b, DWORD protect) {
    for (size_t i = lazy_first_above(a); i < lazy_len && lazy_list[i].start < b; ++i) {
        LazyEntry *e = &lazy_list[i];
        const uintptr_t ma = e->start > a ? e->start : a, mb = e->end < b ? e->end : b;
        LazyRun *out = lazy_alloc_runs(e->nruns + 2); /* the runs cut at ma and mb: two more at most */
        size_t m = 0;
        uintptr_t cur = e->start;
        for (size_t k = 0; cur < e->end;) {
            while (e->runs[k].end <= cur) ++k;
            uintptr_t seg = e->runs[k].end;
            DWORD p = e->runs[k].protect;
            if (cur < ma) {
                if (ma < seg) seg = ma;
            } else if (cur < mb) {
                p = protect;
                if (mb < seg) seg = mb;
            }
            if (m && out[m - 1].protect == p) out[m - 1].end = seg;
            else out[m++] = (LazyRun){seg, p};
            cur = seg;
        }
        free(e->runs);
        e->runs = out;
        e->nruns = m;
    }
}

void chunks_lazy_add(uintptr_t start, uintptr_t end, uint64_t phys, DWORD protect) {
    if (end <= start) return;
    EnterCriticalSection(&chunks_cs);
    LazyEntry e = {start, end, phys, lazy_alloc_runs(1), 1};
    e.runs[0] = (LazyRun){end, protect};
    lazy_splice(lazy_first_above(start), 0, &e, 1); /* disjoint from the others: the sorted insertion point */
    LeaveCriticalSection(&chunks_cs);
}

void chunks_lazy_cut(uintptr_t start, uintptr_t end) {
    if (end <= start) return;
    EnterCriticalSection(&chunks_cs);
    lazy_cut_locked(start, end);
    LeaveCriticalSection(&chunks_cs);
}

void chunks_lazy_setprot(uintptr_t start, uintptr_t end, DWORD protect) {
    if (end <= start) return;
    EnterCriticalSection(&chunks_cs);
    lazy_setprot_locked(start, end, protect);
    LeaveCriticalSection(&chunks_cs);
}

int chunks_lazy_at(uintptr_t address) {
    EnterCriticalSection(&chunks_cs);
    const size_t i = lazy_first_above(address);
    const int lazy = i < lazy_len && lazy_list[i].start <= address;
    LeaveCriticalSection(&chunks_cs);
    return lazy;
}

/* Opens chunk c and views every lazy entry of it (one view per entry, the size of its placeholder), then
 * forgets those entries. Each view takes a chunk reference (acquire_locked), as chunks_view_map does; the
 * references go when the views go. The view is made read-write and each run of it gets its protection.
 * On a refusal the views and references made by this call are undone and the entries stay. Returns 0 or -1. */
static int lazy_open_chunk_locked(uint64_t c, int *viewed, int *opened) {
    const uint64_t base = c << CHUNK_SHIFT;
    const int was_open = chunk[c].section != NULL;
    /* references first: the first one opens the chunk */
    for (size_t i = 0; i < lazy_len; ++i) {
        const LazyEntry *e = &lazy_list[i];
        if ((e->phys >> CHUNK_SHIFT) != c) continue;
        if (acquire_locked(e->phys, e->end - e->start)) {
            for (size_t j = 0; j < i; ++j) {
                const LazyEntry *d = &lazy_list[j];
                if ((d->phys >> CHUNK_SHIFT) == c) release_locked(d->phys, d->end - d->start);
            }
            return -1; /* acquire_locked recorded the refusal */
        }
    }
    size_t mapped = 0;
    int failed = 0;
    for (size_t i = 0; i < lazy_len && !failed; ++i) {
        const LazyEntry *e = &lazy_list[i];
        if ((e->phys >> CHUNK_SHIFT) != c) continue;
        void *view = MapViewOfFile3(chunk[c].section, GetCurrentProcess(), (void *)e->start, e->phys - base,
                                    e->end - e->start, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
        if (!view) {
            refusal_error = GetLastError();
            refusal_offset = base;
            failed = 1;
            break;
        }
        ++mapped;
        uintptr_t run_start = e->start;
        for (size_t k = 0; k < e->nruns && !failed; ++k) {
            DWORD old;
            if (e->runs[k].protect != PAGE_READWRITE &&
                !VirtualProtect((void *)run_start, e->runs[k].end - run_start, e->runs[k].protect, &old)) {
                refusal_error = GetLastError();
                refusal_offset = base;
                failed = 1;
            }
            run_start = e->runs[k].end;
        }
    }
    if (failed) {
        /* undo: unmap the views this call made (the first `mapped` entries of the chunk), then drop the references */
        size_t k = 0;
        for (size_t j = 0; j < lazy_len && k < mapped; ++j) {
            const LazyEntry *d = &lazy_list[j];
            if ((d->phys >> CHUNK_SHIFT) != c) continue;
            UnmapViewOfFile2(GetCurrentProcess(), (void *)d->start, MEM_PRESERVE_PLACEHOLDER);
            ++k;
        }
        for (size_t j = 0; j < lazy_len; ++j) {
            const LazyEntry *d = &lazy_list[j];
            if ((d->phys >> CHUNK_SHIFT) == c) release_locked(d->phys, d->end - d->start);
        }
        return -1;
    }
    size_t w = 0;
    for (size_t i = 0; i < lazy_len; ++i) {
        if ((lazy_list[i].phys >> CHUNK_SHIFT) == c) {
            free(lazy_list[i].runs);
            continue;
        }
        lazy_list[w++] = lazy_list[i];
    }
    lazy_len = w;
    *viewed += (int)mapped;
    if (!was_open && chunk[c].section) *opened += 1;
    return 0;
}

int chunks_lazy_fault(uintptr_t address, int *viewed, int *opened) {
    int result = 0;
    EnterCriticalSection(&chunks_cs);
    const size_t i = lazy_first_above(address);
    if (i < lazy_len && lazy_list[i].start <= address)
        result = lazy_open_chunk_locked(lazy_list[i].phys >> CHUNK_SHIFT, viewed, opened) ? -1 : 1;
    LeaveCriticalSection(&chunks_cs);
    return result;
}

int chunks_lazy_materialize(uintptr_t start, uintptr_t end, int *viewed, int *opened) {
    int result = 0;
    EnterCriticalSection(&chunks_cs);
    for (;;) {
        const size_t i = lazy_first_above(start);
        if (i >= lazy_len || lazy_list[i].start >= end) break;
        /* opening its chunk removes that entry (and the others of its chunk): the search runs again */
        if (lazy_open_chunk_locked(lazy_list[i].phys >> CHUNK_SHIFT, viewed, opened)) {
            result = -1;
            break;
        }
    }
    LeaveCriticalSection(&chunks_cs);
    return result;
}

#else
/* Linux: no chunks. Keeps the translation unit non-empty (ISO C). */
typedef int runtime_memory_chunks_linux_unused;
#endif
