// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GPU region. A port-owned guest address range for engine allocations the GPU writes, so CPU-only
// engine memory never shares a 4 KiB page with GPU-written data. The engine's generic allocator is redirected
// here for listed allocation sites (the wiring lives outside this file).
//
// The range is reserved once through runtime_memory_reserve_range_export and committed lazily in 64 KiB
// granules through runtime_memory_batch_map_export (map_flexible, CPU and GPU read/write). All allocator
// metadata (size classes, free lists, side table, quarantine) is host-side: the allocator never reads or writes
// the bytes of the region itself.
//
// Blocks are released in two steps. Free() moves a block to a quarantine stamped with the current frame; it
// becomes reusable only after the release hook has run for it, once more than the quarantine frame count has
// passed. Ring blocks (kind kKindRing) are never freed by the engine: OnFrame() expires them after more than
// the ring frame count, and they then go through the same quarantine.
//
// Environment overrides (read by Init): SOTC_GPU_REGION_RING_FRAMES (default 4),
// SOTC_GPU_REGION_QUARANTINE_FRAMES (default 3).
#pragma once

#include <cstdint>
#include <string>

namespace SotcGpuRegion {

/// Bounds of the guest range (the port's user area above the 31-bit game pointers). Init checks the reservation.
constexpr uint64_t kRangeLo = 0x1000000000ull;
constexpr uint64_t kRangeHi = 0xfc00000000ull;
/// Commit and slab unit: every commit is one 64 KiB granule, and every small-class slab is one granule.
constexpr uint64_t kGranule = 64ull * 1024;
constexpr uint64_t kPage = 4096;
/// Default reservation size passed to Init by the caller.
constexpr uint64_t kDefaultReserve = 4ull << 30;
/// Largest block served from size classes; bigger blocks are page-aligned runs.
constexpr uint64_t kSmallMax = 4096;

constexpr uint32_t kKindNormal = 0; ///< freed explicitly with Free()
constexpr uint32_t kKindRing = 1;   ///< per-frame ring: expires by itself in OnFrame()

/// Host calls the allocator makes. Init installs the runtime exports; tests inject fakes.
struct Host {
    int32_t (*reserve)(void** inout, uint64_t size, int flags, uint64_t alignment);
    int32_t (*batch_map)(void* entries, int count, int* processed);
    /// Maps backing at [start, start + size) outside the game's flexible budget (runtime_memory.c). Null: batch
    /// flexible maps of one granule (tests).
    int32_t (*port_map)(uint64_t start, uint64_t size, int prot) = nullptr;
};

/// Counters for Report(). Values are read under the allocator lock.
struct Counters {
    uint64_t reserved_bytes = 0;
    uint64_t committed_bytes = 0;
    uint64_t bump_bytes = 0;
    uint64_t live_blocks = 0;
    uint64_t live_bytes = 0;
    uint64_t quarantined_blocks = 0;
    uint64_t quarantined_bytes = 0;
    uint64_t released = 0;
    uint64_t allocs_total = 0;
    uint64_t frees_total = 0;
    uint64_t ring_expired = 0;
    uint64_t alloc_failures = 0;
    uint64_t unknown_frees = 0;
    uint64_t frame = 0;
};

/// Reserves one guest range of reserve_bytes (0 means kDefaultReserve), rounded up to a granule. Returns false
/// when the reservation fails or the returned address is not granule-aligned or does not lie inside
/// [kRangeLo, kRangeHi). A rejected reservation cannot be returned (no unreserve export), so it stays reserved.
bool Init(uint64_t reserve_bytes);

/// Returns a guest address, or 0 (not initialised, bad arguments, out of range, or a failed commit).
/// size 0 is treated as 16; align below 16 is raised to 16 and rounded up to a power of two. kind must be
/// kKindNormal or kKindRing. The address is never 0 for a successful call.
uint64_t Alloc(uint64_t size, uint64_t align, uint32_t slot, uint32_t kind);

/// Lock-free: true when addr lies inside the reserved range (allocated or not).
bool Contains(uint64_t addr);

/// Rounded usable size of the live block that starts at addr; 0 when addr is not a live block start
/// (including a block that is quarantined or was never allocated).
uint64_t SizeOf(uint64_t addr);

/// Slot of the live block that starts at addr. False when addr is not a live block start.
bool SlotOf(uint64_t addr, uint32_t* slot);

/// Moves a live block to quarantine, stamped with the current frame. Unknown addresses only count.
void Free(uint64_t addr);

/// Called once per frame. Expires ring blocks older than the ring frame count into quarantine, then releases
/// quarantined blocks older than the quarantine frame count: adjacent released ranges are merged, the release
/// hook is called for each merged range, and only then do the blocks become reusable.
void OnFrame(uint64_t frame);

/// Release callback, called synchronously from OnFrame with the lock not held. fn may call Contains and SizeOf
/// but must not call Alloc or Free. A null fn disables the callback.
void SetReleaseHook(void (*fn)(void* ctx, uint64_t addr, uint64_t size), void* ctx);
/// Called when a block enters the quarantine (Free, or a ring block's expiry in OnFrame), with the lock released. The
/// block cannot be handed out again for the quarantine frames, so work the hook queues for later (the buffer cache's
/// ordered forget of the old contents) runs before a new owner writes the block.
void SetQuarantineHook(void (*fn)(void* ctx, uint64_t addr, uint64_t size), void* ctx);

/// Appends a multi-line counter summary to out, and resets the per-window alloc and free counts.
void Report(std::string& out);

/// Snapshot of the counters (for tests and Report).
Counters GetCounters();

/// Test hooks. SetHostForTest replaces the host calls; ResetForTest returns the allocator to its
/// uninitialised state with the default host and no release hook. The reserved range of a previous Init is
/// abandoned, not released.
void SetHostForTest(const Host& host);
void ResetForTest();

}  // namespace SotcGpuRegion
