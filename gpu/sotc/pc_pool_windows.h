// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-owner VA windows of the native pool path (docs/re-stutters/native/allocators.md, sections 5-7).
//
// Three windows are reserved once from the host and never returned to it:
//   A  GPU arena (kind 2, 128 MiB): the 32 MiB data blocks of the per-frame GPU arena (0x15ee0 -> 0x479a0).
//   B  XPR mask heap (kind 1, 16 MiB): the 8 MiB blocks of the CPU arena that serve memtype 17 (RenderSingleFrameCPU),
//      keyed by the heap slot the dispatcher 0x12820 is given.
//   C  render targets (kind 2, 352 MiB): the one render-target block of the area (0x9b778). Its size is 0x14280000
//      (322.5 MiB), not 320 MiB as allocators.md says.
// A block of a bound owner is served by WindowFreshHost, the VA source of the fresh writers (pc_pool_native_fresh.h),
// so its VA is fixed for the session. Its release is parked (0x476f0 skips the VA return 0x1b0e0; the pool keeps the
// block in use), and the next block of that owner and size takes the parked node back (0x479a0), with no pool call.
//
// Limits, checked when a window is reserved:
//  - base + size <= kWindowLimit. The owner-tag table at root+0x111908 (one byte per MiB, index VA >> 20) is modelled
//    only up to root+0x20d918 (pc_pool_native_dispatch.cpp InModelled): a window above 0xfc010 MiB writes outside it.
//    That is below the 2^40 limit of the descriptor base field (allocators.md section 4, item 3).
//  - windows are 1 MiB aligned: the primary sets round the alignment to 1 MiB (pc_pool.cpp Predict).
//
// Capacity: each window holds the blocks of its owner (A: 4, B: 2, C: 1). A call that finds no free run in its
// window takes the normal path (no forced fresh route) and is counted as a full window. A window that cannot be
// reserved or bound stays inactive, and its calls take the normal path too.
//
// Not thread-safe: the glue calls every function here under its g_mutex (pc_pool.cpp) and the game's pool lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pc_pool_native_fresh.h"

namespace SotcPcPoolWindows {

constexpr uint64_t kMiB = uint64_t{1} << 20;
constexpr uint64_t kPage = uint64_t{1} << 16;
/// The owner-tag table (InModelled, root+0x20d918) covers VA < 0xfc010 MiB; a window must end below it.
constexpr uint64_t kOwnerTagLimit = uint64_t{0xfc010} << 20;
/// The 40-bit base field of a descriptor (0x58d4e0): the window must end below 2^40 as well.
constexpr uint64_t kVaFieldLimit = uint64_t{1} << 40;
constexpr uint64_t kWindowLimit = kOwnerTagLimit < kVaFieldLimit ? kOwnerTagLimit : kVaFieldLimit;

/// The heap slot (memtype) of RenderSingleFrameCPU, the key of window B (allocators.md section 6).
constexpr int64_t kMaskKey = 17;
/// Window key meaning "any heap slot" (a binding without a key).
constexpr int64_t kAnyKey = -1;

enum class Slot : int { GpuArena = 0, MaskHeap = 1, RenderTargets = 2 };
constexpr int kSlotCount = 3;

struct SlotSpec {
    const char* name;  ///< the window's name (RegisterWindow and the report)
    uint64_t size;      ///< the reserved size
    uint64_t block;     ///< the one block size the window serves (a request of another size takes the normal path)
    int32_t kind;      ///< the pool kind of the request (1 CPU, 2 GPU)
};
const SlotSpec& Spec(int slot);

/// The VA alignment of a fresh block: pool_alloc_region rounds align up to 64 KiB, and the reserve takes max(1 MiB,
/// that) (0x19f8a). Window and writer must use the same value.
uint64_t ReserveAlign(uint64_t align);

/// One window: its own address-ordered free list (start -> end) and its live blocks (start -> bytes). Pure, no host.
class Window {
public:
    /// base and size must be 1 MiB aligned, size non-zero, and base + size within kWindowLimit.
    bool Init(uint64_t base, uint64_t size, std::string* why);
    bool active() const { return size_ != 0; }
    uint64_t base() const { return base_; }
    uint64_t size() const { return size_; }
    uint64_t end() const { return base_ + size_; }
    bool Contains(uint64_t va, uint64_t bytes) const;

    /// Whether Take(bytes, align) would succeed. Pure.
    bool CanTake(uint64_t bytes, uint64_t align) const;
    /// First fit in address order: the lowest run where the aligned start leaves bytes before the run's end. The block
    /// becomes live. Returns 0 when no run fits.
    uint64_t Take(uint64_t bytes, uint64_t align);
    /// Returns a live block to the free list, coalescing with its neighbours. False when [va, va+bytes) is not a live
    /// block of this window.
    bool Give(uint64_t va, uint64_t bytes);
    bool IsLive(uint64_t va, uint64_t bytes) const;

    uint64_t free_bytes() const;
    size_t free_runs() const { return free_.size(); }
    size_t live_blocks() const { return live_.size(); }

private:
    uint64_t base_ = 0;
    uint64_t size_ = 0;
    std::map<uint64_t, uint64_t> free_; ///< free run start -> end, in address order
    std::map<uint64_t, uint64_t> live_; ///< live block start -> bytes
};

/// The parked release of a windowed block: the node stays in use in the pool, its VA stays mapped.
struct Parked {
    uint64_t node = 0;
    uint64_t va = 0;
    uint64_t bytes = 0;
};

/// Counters per slot (cumulative; the glue prints them).
struct Stats {
    uint64_t routed[kSlotCount] = {};       ///< calls whose owner, kind, key and size match the binding
    uint64_t served[kSlotCount] = {};       ///< fresh calls whose VA came from the window (committed or pending)
    uint64_t full[kSlotCount] = {};         ///< routed calls the window could not place: the normal path
    uint64_t take_failed[kSlotCount] = {};  ///< a VA the route promised was not there (state error, counted)
    uint64_t given_back[kSlotCount] = {};   ///< VA of a refused call returned to the window
    uint64_t committed[kSlotCount] = {};    ///< windowed blocks bound to their node after a verified call
    uint64_t parked[kSlotCount] = {};      ///< releases parked (no pool VA return)
    uint64_t park_refused[kSlotCount] = {}; ///< releases in a window that were not a live block of its node
    uint64_t reused[kSlotCount] = {};       ///< allocations served by a parked node
    uint64_t reuse_miss[kSlotCount] = {};   ///< allocations of a bound owner with no parked node of that size
    uint64_t reserve_ok = 0;
    uint64_t reserve_failed = 0;            ///< the host gave no VA, or an unaligned one (leaked at boot)
    uint64_t reserve_limit = 0;             ///< the reserved range ends above kWindowLimit (leaked at boot)
    uint64_t register_failed = 0;           ///< RegisterWindow refused the range (the window still places blocks)
};

/// Registers a reserved range with the host (SotcNative::RegisterWindow). Null: not registered.
using RegisterFn = std::function<bool(uint64_t base, uint64_t size, const char* name)>;

/// The three windows, their bindings and their parked nodes. The owner of a binding is the object the pool wrapper
/// receives as rdi (the arena or the render-target allocator, the family object of the block, node+0x40).
class Table {
public:
    /// Reserves the window's VA (host.ReserveVirtual, 1 MiB aligned) and registers it. A window already active is kept.
    /// Returns false when the window stays inactive (why says which).
    bool Reserve(int slot, SotcPcPoolNativeFresh::FreshHost& host, const RegisterFn& reg, std::string* why);
    /// Binds the window to the owner obj (and the heap key, kAnyKey for any). Only an active window binds.
    bool Bind(int slot, uint64_t obj, int64_t key);

    /// The window a fresh call of this owner, kind, key and size takes its VA from, or -1 (normal path). A bound window
    /// that cannot place the block counts as full (-1 and the normal path). Counts routed and full.
    int Route(uint64_t obj, int32_t kind, int64_t key, uint64_t bytes, uint64_t align);
    /// The VA of a routed fresh call: a window Take. 0 when the window cannot place it.
    uint64_t TakeVa(int slot, uint64_t bytes, uint64_t align);
    /// Returns the VA of a refused fresh call (nothing was committed) to the window.
    void GiveVa(int slot, uint64_t va, uint64_t bytes);
    /// Binds the window's block at va to its node after a verified call (node is the rax the call returned).
    void Commit(int slot, uint64_t va, uint64_t bytes, uint64_t node);

    /// The release of a block (0x476f0): true when [va, va+bytes) is the live block of node in a window and is now
    /// parked (the caller skips the VA return). False: a normal free; the caller runs the original.
    bool Release(uint64_t node, uint64_t va, uint64_t bytes);
    /// The allocation of a bound owner (0x479a0 with the owner as the arena): a parked node of that size, lowest VA
    /// first. False: none, the caller runs the original path.
    bool Reuse(uint64_t obj, int64_t key, uint64_t bytes, uint64_t* node);

    /// The window that contains va, or -1.
    int SlotOf(uint64_t va) const;
    const Window& window(int slot) const { return slots_[slot].window; }
    bool bound(int slot) const { return slots_[slot].bound; }
    uint64_t obj(int slot) const { return slots_[slot].obj; }
    const std::vector<Parked>& parked(int slot) const { return slots_[slot].parked; }
    const Stats& stats() const { return stats_; }
    /// Clears the counters (the windows, bindings and parked nodes stay).
    void ResetCounters() { stats_ = Stats(); }

private:
    struct SlotState {
        bool bound = false;
        uint64_t obj = 0;
        int64_t key = kAnyKey;
        Window window;
        std::map<uint64_t, uint64_t> node_of; ///< live block start -> node (set by Commit)
        std::vector<Parked> parked;
    };
    /// The bound slot for an owner, kind, key and size, or -1. Does not count.
    int Match(uint64_t obj, int32_t kind, int64_t key, uint64_t bytes) const;
    SlotState slots_[kSlotCount];
    Stats stats_;
};

/// The VA source of the fresh writers for the windowed calls: a window Take when a slot is selected, else the normal
/// host. BatchMap always goes to the normal host (the batch flush is the game's own map of the same pool memory).
class WindowFreshHost final : public SotcPcPoolNativeFresh::FreshHost {
public:
    WindowFreshHost(SotcPcPoolNativeFresh::FreshHost& normal, Table& table) : normal_(normal), table_(table) {}

    /// Selects the window of the next ReserveVirtual (-1: the normal host). Clears the VA of the last call.
    void Select(int slot) {
        slot_ = slot;
        last_va_ = 0;
        last_bytes_ = 0;
    }
    int slot() const { return slot_; }
    /// The VA the last ReserveVirtual took from the window (0: it came from the normal host or failed).
    uint64_t last_va() const { return last_va_; }
    uint64_t last_bytes() const { return last_bytes_; }

    uint64_t ReserveVirtual(uint64_t size, uint64_t align) override;
    int32_t BatchMap(const SotcPcPoolNativeFresh::BatchEntry* entries, size_t count) override {
        return normal_.BatchMap(entries, count);
    }

private:
    SotcPcPoolNativeFresh::FreshHost& normal_;
    Table& table_;
    int slot_ = -1;
    uint64_t last_va_ = 0;
    uint64_t last_bytes_ = 0;
};

/// The one-line-per-window report (the glue appends it to the native report).
void ReportTable(const Table& table, std::string& out);

} // namespace SotcPcPoolWindows
