/* Chunked backing of the guest memory on Windows (for runtime_memory.c, "host mappings").
 *
 * The physical guest space [0, POOL_SIZE + FLEX_SPAN) is cut into 64 MiB chunks. Each chunk is its
 * own pagefile-backed section, created on first use and closed once nothing uses it, so the commit
 * charge of a chunk goes back to the system when the guest frees all of it. The backing view (the
 * host range the runtime and the GPU read and write through) stays one contiguous host range: it is
 * reserved as placeholders, one per chunk, and a chunk's section replaces its placeholder when the
 * chunk opens. Guest views map the same sections, one view per chunk piece (chunks_view_map).
 *
 * Windows only: on other hosts this file declares and compiles nothing (the Linux memfd path is
 * unchanged).
 *
 * Lifetime rules:
 *  - A chunk is open while it has holders (refs: guest views from chunks_view_map, and
 *    chunks_acquire holders such as a GPU import) or while the guest allocator holds bytes in it
 *    (allocated: chunks_note_alloc / chunks_note_free). It closes when both reach zero.
 *  - Closing discards the chunk's bytes. A reopened chunk reads as zero, which is what freed memory
 *    reads as when reused. Freed bytes of a chunk that stays open must still be zeroed by the caller
 *    (chunks_discard).
 *  - A host pointer into an open chunk is valid until that chunk closes. Whoever keeps a pointer or a
 *    section handle beyond a lookup must hold a reference (chunks_acquire) or be invalidated first.
 *  - Every guest view covers one piece of one chunk: callers cut ranges at chunks_piece() boundaries.
 *  - All functions take one internal mutex (chunks_cs). It is recursive (a Windows critical section), so a
 *    caller that holds it (chunks_lock) may call the functions below. Lock order: the runtime's table lock
 *    first, then chunks_cs; never the reverse. Nothing here calls back into the runtime, and nothing here
 *    waits for another thread while it holds chunks_cs (see the proof in runtime_memory.c, "lazy").
 *
 * SOTC_COMMIT_CHUNKS=2 (first touch, runtime_memory.c "chunks opened on first touch"): mapping opens nothing.
 * The lazy guest views (the pieces of a mapping that wait for their first touch) are listed here, not in the
 * runtime's region table: chunks_lazy_add / chunks_lazy_cut / chunks_lazy_setprot keep the list under the table
 * lock, and chunks_lazy_fault (the exception handler) opens the chunk and makes its views under chunks_cs
 * alone, so a fault never waits for the table lock. Host access through the backing view opens the chunk with
 * chunks_open (no view). Chunk lifetime is unchanged: a chunk closes when it has no views, no holds and no
 * allocated bytes.
 *
 * Integration in runtime_memory.c (done there under SOTC_COMMIT_CHUNKS; the list is the plan it followed):
 *  pool()                      chunks_init(POOL_SIZE+FLEX_SPAN) instead of CreateFileMapping2 and the
 *                              whole-section MapViewOfFile3; backing_base = chunks_backing_base().
 *                              The single section and its SEC_COMMIT/SEC_RESERVE choice go away.
 *  view_map(r, protect)        chunks_view_map(r->start, r->phys, r->end-r->start, protect), once per
 *                              piece that lies in one chunk (see chunks_piece).
 *  host_map(), carve_host(),   cut the pieces at phys chunk boundaries as well as at the VIEW chunks
 *  map_fine()                  (2 MiB / 64 KiB virtual). The region table keeps one entry per view.
 *  carve_host() unmapping      UnmapViewOfFile2(view) becomes chunks_view_unmap(start, phys, size).
 *  direct_allocate()           chunks_note_alloc(start, size) once the block is recorded.
 *  map_flexible_locked()       chunks_note_alloc(phys, size) after flex_alloc succeeds (and
 *                              chunks_note_free on its failure path).
 *  direct_release(), flex_free() chunks_note_free(phys, size) after the views of the range are gone
 *                              (unmap_released_vmas_locked and carve_host run first).
 *  host_discard()              Windows: chunks_discard(phys, size) instead of memset of backing_base.
 *  runtime_memory_backing_view() returns chunks_backing_base(); only open chunks are readable, so
 *                              a whole-view import (SOTC_HOST_IMPORT) must import chunk by chunk.
 *  GPU host imports            chunks_acquire(phys, size) on import, chunks_release on destroy. The
 *                              chunk then stays open for as long as the GPU uses it.
 */
#ifndef BB_RUNTIME_MEMORY_CHUNKS_H
#define BB_RUNTIME_MEMORY_CHUNKS_H
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h> /* before extern "C": C++ callers include this header too */
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* Chunk size: 64 MiB (granularity of the commit charge and of guest views of one chunk). */
#define CHUNKS_CHUNK_SHIFT 26
#define CHUNKS_CHUNK_BYTES (UINT64_C(1) << CHUNKS_CHUNK_SHIFT)

/* Exported by runtime_memory.c on every host (no effect unless SOTC_COMMIT_CHUNKS=1 on Windows).
 * runtime_memory_commit_chunks: 1 when the chunked backing is in use.
 * runtime_memory_hold_range: keeps the chunks behind a guest range open until unhold (a GPU import
 * of guest memory). Returns 0, or -1 when the range is not mapped in one piece of direct or flexible
 * memory, or Windows refuses the commit. Off: no-op, returns 0.
 * runtime_memory_unhold_range: releases the hold taken with the same (address, size). */
int runtime_memory_commit_chunks(void);
int runtime_memory_hold_range(uint64_t guest_addr, uint64_t size);
void runtime_memory_unhold_range(uint64_t guest_addr, uint64_t size);

/* Cost of the SOTC_COMMIT_CHUNKS=2 first-touch path, cumulative since the process started (all 0 when the
 * mode is off or on Linux). Times come from QueryPerformanceCounter.
 * runtime_memory_lazy_ns / runtime_memory_lazy_max_ns: time in runtime_memory_lazy_fault (all calls, and the
 *   longest single call), in ns. runtime_memory_lazy_calls: calls that reached the lazy path.
 * runtime_memory_chunk_open_ns / runtime_memory_chunk_open_max_ns: time spent opening chunks (section creation
 *   and the backing view), all opens and the longest one, in ns.
 * runtime_memory_chunk_opens_locked: chunks opened by a fault or a backing access while another thread held
 *   the runtime's table lock. */
uint64_t runtime_memory_lazy_ns(void);
uint64_t runtime_memory_lazy_max_ns(void);
uint64_t runtime_memory_lazy_calls(void);
uint64_t runtime_memory_chunk_open_ns(void);
uint64_t runtime_memory_chunk_open_max_ns(void);
uint64_t runtime_memory_chunk_opens_locked(void);

#ifdef _WIN32
/* Reserves the backing range for [0, bytes) (rounded up to whole chunks) as chunk placeholders.
 * Must be the first call (pool() calls it under the table write lock). Idempotent: a later call
 * with a size that fits the reservation returns 0. Returns 0, or -1 if the reservation fails. */
int chunks_init(uint64_t bytes);

/* Host address of physical offset 0 of the backing range (NULL before chunks_init). Only open
 * chunks are readable: a closed chunk is a placeholder and touching it faults. */
uint8_t *chunks_backing_base(void);

/* Bytes of the backing range (whole chunks; 0 before chunks_init). */
uint64_t chunks_backing_size(void);

/* The section of the chunk holding phys, and the offset of phys inside that section. Returns 0 when
 * the chunk is open, -1 when it is closed (or phys is outside the range). The handle is valid only
 * while the caller holds a reference (guest view or chunks_acquire) on the chunk. */
int chunk_section_for(uint64_t phys, HANDLE *section, uint64_t *offset_in_section);

/* Bytes from phys to the end of its chunk, capped at size: the largest piece one guest view can
 * map from phys. Pure arithmetic, no state. */
uint64_t chunks_piece(uint64_t phys, uint64_t size);

/* Opens the chunks covering [phys, phys+size) that are closed (SEC_COMMIT section, 64 MiB charge
 * each) and takes one reference on each. Returns 0, or -1 when Windows refuses a commit (the page
 * file is too small). On -1 nothing stays opened by this call and no reference is taken. */
int chunks_acquire(uint64_t phys, uint64_t size);

/* Drops one reference on each chunk covering the range; a chunk that reaches zero references and
 * zero allocated bytes is closed (its commit charge goes back). Calls without a matching acquire
 * are ignored for chunks that have no references left. */
void chunks_release(uint64_t phys, uint64_t size);

/* Opens the closed chunks of [phys, phys+size) without a reference and without a view: the bytes become
 * readable through the backing view (SOTC_COMMIT_CHUNKS=2 opens on host access this way). Returns the
 * number of chunks it opened (0 when all were open), or -1 when Windows refuses; then nothing it opened
 * stays open. A chunk opened this way with no allocated bytes and no references stays open until the next
 * release or free of that chunk: callers open only ranges the guest has allocated. */
int chunks_open(uint64_t phys, uint64_t size);

/* The guest allocator took [phys, phys+size) (direct allocation, flexible map). Nothing is opened:
 * a chunk stays closed until it is mapped. Must be called for every allocation: a chunk with no
 * allocated bytes is closed as soon as its last reference goes, so an unannounced allocation whose
 * data is still wanted would lose it. */
void chunks_note_alloc(uint64_t phys, uint64_t size);

/* The guest allocator released [phys, phys+size). Closes every chunk that now has no references
 * and no allocated bytes (its contents are gone and it reads as zero when reopened). With
 * chunks_defer_closes those are queued instead, until chunks_drain. */
void chunks_note_free(uint64_t phys, uint64_t size);

/* Deferred closes: from now on a chunk that becomes idle (no references, no allocated bytes) is
 * queued, not closed; chunks_drain closes the queued chunks that are still idle. A queued chunk
 * that is acquired again or closed another way is dropped from the queue. The runtime calls this
 * once after chunks_init and drains after dropping its table lock (closes take milliseconds each). */
void chunks_defer_closes(void);
void chunks_drain(void);

/* Error code of the last refused open (ERROR_COMMITMENT_LIMIT and the like) and the backing offset
 * of the chunk it refused; 0 when none was refused. The runtime prints the refusal once. */
DWORD chunks_last_refusal(uint64_t *offset);

/* Maps the physical range [phys, phys+size) at address, which must be an unmapped placeholder of
 * exactly size bytes (the runtime's region). The range must lie in one chunk (see chunks_piece).
 * Takes one reference; the view replaces the placeholder with the same protection rules as the
 * runtime's view_map (PAGE_NOACCESS maps read-write, then protects). Returns 0 or -1. */
int chunks_view_map(void *address, uint64_t phys, uint64_t size, DWORD protect);

/* Unmaps a view made by chunks_view_map: the address range becomes a placeholder again
 * (MEM_PRESERVE_PLACEHOLDER), so the runtime can coalesce it as before. Drops the reference;
 * the chunk closes if nothing else uses it. Returns 0 or -1. */
int chunks_view_unmap(void *address, uint64_t phys, uint64_t size);

/* Zeroes the bytes of the open chunks in [phys, phys+size). Closed chunks already read as zero. */
void chunks_discard(uint64_t phys, uint64_t size);

/* Commit charge of the open chunks, in bytes (each open chunk counts its full 64 MiB). */
uint64_t chunks_committed_bytes(void);

/* Chunks opened and closed since the process started (diagnostics). */
void chunks_stats(uint64_t *opened, uint64_t *closed);

/* Time spent in chunk opens (section creation and backing view), total and longest, in ns. */
void chunks_open_stats(uint64_t *ns_total, uint64_t *ns_max);

/* Takes chunks_cs for a caller that needs the lazy list and the placeholders of the lazy views to stay as
 * they are across several calls (runtime_memory.c carve_host). Pair every chunks_lock with chunks_unlock.
 * Never hold it across a wait for another thread (a copy or a GPU thread may need it to finish). */
void chunks_lock(void);
void chunks_unlock(void);

/* Lazy guest views (SOTC_COMMIT_CHUNKS=2). A lazy view is a guest range [start,end) mapped to physical
 * [phys, phys+end-start) that has no host view yet: its address is a placeholder. The list is sorted by start,
 * disjoint, and each entry lies in one chunk. Writers hold the runtime's table lock; the functions take
 * chunks_cs themselves.
 * chunks_lazy_add:     records a new lazy range (no view).
 * chunks_lazy_cut:     forgets [start,end) (its range was unmapped or is being carved); the rest stays.
 * chunks_lazy_setprot: sets the protection the view will take for [start,end) (guest protect).
 * chunks_lazy_at:      1 when address lies in a lazy range (no view yet), else 0.
 * chunks_lazy_fault:   exception handler: the lazy range at address is viewed, together with every lazy range
 *                      of its chunk, and the chunk opens if closed. *viewed / *opened count the views and the
 *                      chunks opened. Returns 1 (done), 0 (no lazy range at address), -1 (Windows refused the
 *                      commit or a view: nothing of this call stays). Takes no runtime lock.
 * chunks_lazy_materialize: views every lazy range that overlaps [start,end) (with their chunks). Same returns
 *                      as chunks_lazy_fault, 0 or -1; counts go to *viewed / *opened. */
void chunks_lazy_add(uintptr_t start, uintptr_t end, uint64_t phys, DWORD protect);
void chunks_lazy_cut(uintptr_t start, uintptr_t end);
void chunks_lazy_setprot(uintptr_t start, uintptr_t end, DWORD protect);
int chunks_lazy_at(uintptr_t address);
int chunks_lazy_fault(uintptr_t address, int *viewed, int *opened);
int chunks_lazy_materialize(uintptr_t start, uintptr_t end, int *viewed, int *opened);

#else
/* Linux: nothing here. The memfd path does not use chunks. */
#endif

#ifdef __cplusplus
}
#endif
#endif
