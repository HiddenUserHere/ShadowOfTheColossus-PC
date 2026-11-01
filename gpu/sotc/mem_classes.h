// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: memory classes (plan step 6). The block-allocator probe (gpu/sotc/probe_alloc.cpp) feeds
// every live allocation here with its guest range, its allocator pool name and whether the pool is
// CPU-only (kind 1, CPUMemory, and not a GPU upload/staging pool). This builds a range -> class
// table that marks the ranges the port never needs to CPU-write-protect.
//
// The memtype probe (gpu/sotc/probe_memtype.cpp, SOTC_HOOK_MEMTYPE_PROBE=1 + SOTC_MEMTYPE_CLASSES=1)
// feeds a second kind of owner: the 64 KiB granules of each memory heap, named "memtype:<heap>". The
// granules have their own table and lookup (OnGranule), separate from the allocator blocks, so a granule
// and a block with the same base do not overwrite each other. A guest fault is attributed to the
// granule that holds it first, then to the allocator block.
//
// SOTC_CLASS_TRACK:
//   0  off (default): OnAlloc/OnFree/OnGranule are cheap no-ops; nothing changes.
//   2  shadow: the draw thread (BufferCache::SynchronizeDmaBuffers) counts GPU-marked pages that
//      fall inside the CPU-only ranges, per pool. It must stay ~0 over a long session; a non-zero
//      count means a range the allocator called CPU-only was written by the GPU, so untracking it
//      would be unsafe. No behaviour change. Needs SOTC_HOOK_ALLOC_PROBE=1 (the feed) and SOTC_FPS=1.
//   1  apply: the CPU-only ranges of the pools in SOTC_CLASS_POOLS are published through SotcUntrack
//      (region_manager.h) and kept unprotected, like SOTC_UNTRACK_AUTO. Unset (or "all"): every pool
//      that passes the rules below, except the per-frame arenas ("TaggedArena", GPU-read every frame and
//      kept tracked). "PageAllocator" reproduces the older default. An explicit list is used as written.
//      A granule heap is listed as "memtype:<heap>" (its granules that are CPU_ONLY are published).
//      Every candidate is re-checked on each publish: past its warm-up (SOTC_CLASS_WARMUP_S, default
//      20 s after its claim), no GPU bind or write fault ever touched any of its 64 KiB chunks, and it is
//      not an image range (the caller checks images). A bind into a published range is a hit: the range
//      is retracked and RetrackPending() turns true, so the caller publishes at once.
//      Needs SOTC_HOOK_ALLOC_PROBE=1. Refused (mode 0, one log line) with SOTC_UNTRACK_AUTO=1 or
//      SOTC_CP_AHEAD set. SotcUntrack's self-check still trips and releases everything on a bad fault.
//   Mode 2 runs the same candidate rule every 250 ms ("would-be" list) and counts the bind hits and the
//      write faults the list would have covered, without changing any tracking.
//
// Both non-zero modes count guest write faults per pool (SOTC_FPS report, "faults N/s").
//
// Classes of a granule (memtype heaps). CPU_ONLY = the draw thread sampled its GPU pages (NoteGranuleGpuPages),
// the granule is past its warm-up (SOTC_CLASS_WARMUP_S, default 20 s after its claim, so data bound only at
// area entry is seen first) and it was never excluded or GPU-bound. Excluded is sticky: any GPU page, image
// overlap, write fault (the GPU reads the page), read fault (the GPU writes it) or a second heap touching the
// granule excludes it for good. GPU-bound (NoteGpuBind) is sticky too and is reported as "bound" per heap.
// A granule nobody samples is never CPU_ONLY, so it is never published. A granule that stops being CPU_ONLY
// drops out of the published list on the next publish (the version bumps), and the self-check trips the whole
// list if an untracked granule faults anyway. The trip log names the last CPU_ONLY candidate fault.
//
// How to run it, in this order:
//   1. Look at the classes (no behaviour change):
//        SOTC_CLASS_TRACK=2 SOTC_HOOK_MEMTYPE_PROBE=1 SOTC_MEMTYPE_CLASSES=1 SOTC_HOOK_ALLOC_PROBE=1 SOTC_FPS=1
//      The report lines "SOTC CLASS heap memtype:<heap>: granules ..., CPU_ONLY ..." show the per-heap
//      classes; "SOTC CLASS pools" and "SOTC CLASS OWNER" show the allocator side.
//   2. Untrack the CPU-only granules of the heaps that qualify (check the report stays "not tripped"):
//        SOTC_CLASS_TRACK=1 SOTC_CLASS_POOLS=memtype:<heap>[,PageAllocator] SOTC_HOOK_MEMTYPE_PROBE=1
//        SOTC_MEMTYPE_CLASSES=1 SOTC_HOOK_ALLOC_PROBE=1 SOTC_FPS=1
//      The name is exactly the one the heap report prints (for example "memtype:GameSingleFrame"). A
//      label of two heap globals sharing a slot is comma-joined, so it must be quoted in the setting.
//      Note: the granule feed (SnapshotGranules and the NoteGranule* calls) must run in the draw thread;
//      see the note on SnapshotGranules below.
//   Note: MEMTYPE_ALLOC (0x12820) conflicts only with PC_RING_ALLOC (SOTC_PC_RING_DISPATCH): do not
//   enable both hooks on the same wrap.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SotcMemClasses {

constexpr uint32_t kMaxPools = 64; ///< distinct pool names; later ones share the last slot
// sotc: distinct owners (allocator objects) of the all-blocks table; later ones share the last slot.
constexpr uint32_t kMaxOwners = 128;
// sotc: granules of the granule table (memtype probe). A granule past this cap is skipped and counted.
constexpr uint32_t kMaxGranuleEntries = 131072; // sotc: = kMaxGranules (probe_memtype.cpp)

// 0 off, 1 apply, 2 shadow. Read once from SOTC_CLASS_TRACK. Mode 1 is refused (returns 0) when
// SOTC_UNTRACK_AUTO=1 or SOTC_CP_AHEAD is set.
int Mode();
inline bool On() {
    return Mode() != 0;
}

// Fed by the allocator probe. cpu_only: the pool is CPUMemory (kind 1) and not a GPU staging pool.
// base/size are the allocation's guest range; pool is its allocator name ("?" when unreadable).
// A range with size 0, size > 4 GiB, base >= 2^48 or base + size overflowing is skipped (the bounds
// of SotcUntrack::Parse) and counted in the report. Cheap no-op when Mode() == 0.
void OnAlloc(uint64_t base, uint64_t size, bool cpu_only, const char* pool);
void OnFree(uint64_t base);

// sotc: PC memory model, wave B (docs/re-stutters/PC-MEMORY-REWRITE.md). Every live block of every
// owner, CPU-only or not: owner = the allocator object's guest address, kind its +0x18 field,
// pool its name, slot the pool set the block came from (node +0x68). Feeds the all-class shadow
// of SOTC_CLASS_TRACK=2 (GPU-written pages, bound bytes and read faults per owner). Cheap no-op
// unless Mode() == 2. OnFree above also drops the block from this table. Allocator blocks only:
// memtype granules go through OnGranule.
void OnAllocOwner(uint64_t base, uint64_t size, uint64_t owner, int kind, const char* pool,
                  int slot);

// sotc: a memtype granule of a heap (probe_memtype.cpp, on the first claim of the granule). base is
// 64 KiB aligned, size its length, pool its name ("memtype:<heap label>"). Granules are never freed and
// live in their own table: OnFree does not touch them. Cheap no-op when Mode() == 0.
void OnGranule(uint64_t base, uint64_t size, const char* pool);
// sotc: moves a claimed granule to pool to_pool. Used when the label resolved after the claim (the heap-id
// globals were not set yet, probe_memtype.cpp): the granule keeps its class state, and the next publish
// re-checks it under the new pool. Returns true when it moved; false when the granule is unknown, the
// pool is already to_pool, or Mode() == 0. Any thread; takes g_mutex.
bool RelabelGranule(uint64_t base, const char* to_pool);
// sotc: the granule at base was touched by a second heap (first foreign touch in probe_memtype.cpp).
// It is excluded from CPU_ONLY for good. base is the value given to OnGranule.
void OnGranuleShared(uint64_t base);

// sotc: a live block of the all-blocks table and the index of its owner (< kMaxOwners).
struct OwnerRange {
    uint64_t addr;
    uint64_t size;
    uint32_t owner;
};

// sotc: every live block of the all-blocks table (not only CPU-only ones) and a version that bumps on
// every change to it. Any thread. Draw thread, mode 2.
std::vector<OwnerRange> SnapshotOwners(uint64_t& version);

// sotc: a memtype granule: its guest range, its index (< kMaxGranuleEntries, for NoteGranuleGpuPages and
// NoteGranuleImage) and its heap's pool index (< kMaxPools).
struct GranuleRange {
    uint64_t addr;
    uint64_t size;
    uint32_t index;
    uint32_t pool;
};

// sotc: every granule (never shrinks). Any thread. The draw thread feeds the classes with it: for each
// granule, the GPU-marked pages of its range go to NoteGranuleGpuPages and its texture-image overlap to
// NoteGranuleImage. buffer_cache.cpp (SynchronizeDmaBuffers, the SotcMemClasses block) feeds 512 granules
// every 250 ms, so one full pass takes about 16 s. Without this feed no granule is ever sampled, so none
// is CPU_ONLY and mode 1 publishes no granule ranges (the safe default).
std::vector<GranuleRange> SnapshotGranules(uint64_t& version);

// sotc: SOTC_PC_RING=8 heap serving (SOTC_PC_RING8_HEAPS, pc_ring_copy.cpp HostMaintain). The granules of
// the heap named pool_name (as OnGranule got it, e.g. "memtype:CPUMemory") that the GPU bound and that
// are not excluded (no GPU-marked pages sampled, no read fault, not shared with another heap), merged into
// runs of adjacent granules, in address order. version: g_version at the collect (the caller recollects
// only when it moved). Any thread; takes g_mutex.
std::vector<std::pair<uint64_t, uint64_t>> CollectHostServe(const char* pool_name, uint64_t& version);

// sotc: SOTC_PC_RING8_HEAPS_MODE=2 (pc_ring_copy.cpp CollectHeapRuns). Every granule of the heap named pool_name,
// in address order, with two flags: bound (the GPU bound it) and clean (never excluded: no GPU-marked page
// sampled, no read fault, no image overlap, not shared with another heap). The caller builds whole-region runs
// from them; gap joins need the tracker's GPU bits, so the caller checks those outside g_mutex.
// version: g_version at the collect. Any thread; takes g_mutex.
struct HostGranule {
    uint64_t addr;
    uint64_t size;
    bool bound;
    bool clean;
};
std::vector<HostGranule> CollectHostGranules(const char* pool_name, uint64_t& version);

// A guest range classified CPU-only (addr/size match VideoCore::SotcUntrack::Range's fields) and the
// index of its pool (< kMaxPools).
struct Range {
    uint64_t addr;
    uint64_t size;
    uint32_t pool;
};

// The current CPU-only ranges and a version that bumps on every change. Any thread.
std::vector<Range> Snapshot(uint64_t& version);

// Mode 1: the CPU-only ranges of the apply pools (SOTC_CLASS_POOLS): the allocator blocks, then the
// CPU_ONLY granules of the "memtype:" heaps listed there. Also sets the per-heap published bytes.
// Draw thread. The version bumps on every change to either table or to a granule's class.
std::vector<Range> ApplyRanges(uint64_t& version);

// The current table version (an atomic load, no lock). Draw thread, every sync.
uint64_t Version();

// Rebuilds the lock-free lookup used by NoteWriteFault when the table changed since the last
// rebuild. Cheap when nothing changed. Throttled: a rebuild runs at most once every 16 syncs and at
// most once every 100 ms. While the table is dirty and throttled, the lookup is older than the table, so
// faults in a range allocated since the last rebuild are not counted yet, and faults in a range freed
// since then still are (lag of a few syncs; the counts are for the report only, the protection logic
// does not read them). Draw thread, every sync in modes 1 and 2.
void RefreshLookup();

// Guest write fault at addr (page_manager.cpp, the write branch of HandleGuestFault). Lock-free,
// no allocation. Attribution: a granule holding addr first (its heap's pool faults, and it is excluded
// from CPU_ONLY), then the CPU-only allocator range (pool faults), then the allocator block owner
// (mode 2). Call only when Mode() != 0.
void NoteWriteFault(uint64_t addr);

// sotc: guest read fault at addr (page_manager.cpp, the read branch of HandleGuestFault). A granule
// holding addr is excluded from CPU_ONLY; otherwise the read is counted for the owner of the block.
// Lock-free, no allocation. Call only when Mode() == 2.
void NoteReadFault(uint64_t addr);

// sotc: mode 2, from the draw thread: the GPU-marked pages and the texture-image overlaps of one
// owner's blocks, as NoteShadowPool and NoteImageRanges do for a pool.
void NoteShadowOwner(uint32_t owner, uint64_t gpu_pages);
void NoteOwnerImageRanges(uint32_t owner, uint64_t ranges);

// sotc: from the draw thread, once per sample of a granule (see SnapshotGranules). gpu_pages: the
// GPU-marked pages inside the granule. A non-zero count excludes the granule from CPU_ONLY for good.
void NoteGranuleGpuPages(uint32_t index, uint64_t gpu_pages);
// sotc: from the draw thread, once per sample of a granule: overlaps: a texture image overlaps it.
// Excludes the granule for good (images do not set region GPU bits, so the GPU-page count cannot see them).
void NoteGranuleImage(uint32_t index, bool overlaps);

// sotc: a guest range the GPU uses that the bind calls may not see (for example a per-frame arena): its
// chunks are never published, as a bind would do. Not counted as a bind. A published range it hits is a hit.
// Lock-free; any thread; no-op when Mode() == 0.
void NoteExcluded(uint64_t addr, uint64_t size);

// sotc: true from a bind hit on a published range (mode 1) until the next ApplyRanges. The caller must
// publish at once while it is true (BufferCache::SynchronizeDmaBuffers skips its 30-sync wait then).
bool RetrackPending();

// sotc: GPU bind of a guest range [addr, addr + size): every granule overlapping it is GPU-bound. Sticky:
// a bound granule is never CPU_ONLY again and drops out of the published list at the next publish.
// cp_frame is the command-processor frame of the bind (kept per granule, the last bind wins; diagnostic).
// Call from the draw thread only: BufferCache::ObtainBuffer for every buffer bind (device_addr, size), and
// the texture cache for every image bind (its guest range). Lock-free, no allocation per call; a no-op
// when Mode() == 0. When the granule table changed since its last lookup rebuild, the lookup is rebuilt
// here once per sync, so a bind of a freshly claimed granule is not missed. Returns true when the range hit
// a published range (mode 1) or a would-be one (mode 2): the caller marks it CPU-modified (see RetrackPending).
// hit_lo/hit_hi (optional): on a hit, the union [hit_lo, hit_hi) of the FULL spans of the published entries hit,
// so the caller can mark the whole published block CPU-modified, not only the bound part.
bool NoteGpuBind(uint64_t addr, uint64_t size, uint64_t cp_frame = 0, uint64_t* hit_lo = nullptr,
                 uint64_t* hit_hi = nullptr);

// sotc: moves into out the ranges that the last ApplyRanges publish left out of the list (they were
// published before and are not now: retracked, bound, excluded, freed or warm again). Their CPU writes were
// untracked, so the caller marks each one CPU-modified. Replaces out's contents. Draw thread.
void TakeRetracked(std::vector<Range>& out);

// Mode 1 state, from the draw thread. NotePublish: the list was handed to SotcUntrack (the ranges
// and bytes it now holds). NoteApplyState: every sync, the trip state and the untracked-fault count.
// The first sync that reports a trip logs it, with the last CPU_ONLY candidate fault as the owner.
void NotePublish(uint64_t ranges, uint64_t bytes);
void NoteApplyState(bool tripped, uint64_t faults_untracked);

// Shadow results for the window, called by the draw thread: the totals, and the GPU-marked pages
// found inside one pool's ranges (should be ~0 for every pool).
void NoteShadow(uint64_t ranges, uint64_t cpu_only_bytes, uint64_t gpu_pages);
void NoteShadowPool(uint32_t pool, uint64_t gpu_pages);
// Ranges of a pool that overlap a texture-cache image, counted by the draw thread (both modes).
// Render targets and storage images do not set region GPU bits, so the GPU-page count cannot see
// their writes; mode 1 leaves such ranges tracked.
void NoteImageRanges(uint32_t pool, uint64_t ranges);

// Report lines (driver.cpp 5 s report), reset for the next window. Empty when off or idle. Per memtype
// heap (both modes): "SOTC CLASS heap <pool>: granules, CPU_ONLY, gpu pages, published (MiB and ranges of
// the last publish), trips". Trips are cumulative for the session.
void Report(std::string& out, double seconds);

} // namespace SotcMemClasses
