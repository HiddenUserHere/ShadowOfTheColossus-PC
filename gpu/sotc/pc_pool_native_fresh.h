// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native fresh-VA allocation of the game's pool allocator (pool_alloc_region 0x19e00, mode 0, after the
// free-run search misses). Companion of pc_pool.h, pc_pool_native.h and pc_pool_native_reserve.h: the model predicts
// the call (Predict, paths FreshPlain, FreshReserve, FreshSpill, FreshPoolFix), this file performs the writes the game
// makes on that path, in the game's order, through a guest memory interface, and flushes the batch queue through a
// host interface.
//
// Order, eboot addresses (pool_alloc_region 0x19e00):
//   1. sceKernelReserveVirtualRange(&va, size, 0, max(1 MiB, align64))   0x19f8a-0x19fb2   (host)
//   2. pool_node_alloc pop (0x19660), count - 1                          0x19fbf
//   3. node fields: pool id, raw size, VA, in-use 1, owner obj, -1       0x19fce-0x19fed
//   4. owner tags, one byte per 1 MiB unit of [va, va + size)            0x1a01a-0x1a0ef
//   5. region link: node into P's list, P.region + 1, node owner = P     0x1a0f1-0x1a11a
//   6. P allocated bytes and peak                                        0x1a123-0x1a148
//   7. page loop, per iteration (0x1a1b0-0x1a627):
//      a. P.cached == 0: when P has no free VA the loop switches to the spill set F (0x1a1e4, 0x1a275); a set with no
//         cached pages takes the reserve-map step (vmem_query_reserve_map flag 1, 0x1abf0, see
//         pc_pool_native_reserve.h) on the node in rax (the sticky node) or the first searched node.
//      b. scan the bitmap for the lowest set bit (0x1a2f8-0x1a357). No set bit: the POOL_FIX patch (0x1a321): cached
//         := 0, rax := 0, the reserve-map step, then the scan again.
//      c. the run: clear its bits (words), cached -= run, batch op 0 at the cursor (and op 4 when the run came from F),
//         node backed += bytes, set backed (+ peak), cursor += bytes (0x1a357-0x1a627).
//   8. vmem_batch_map (0x1af80) at 0x1a62d: the host applies the queued entries, the count is reset; the node is
//      returned in rax.
//
// Atomicity. The whole sequence runs on an overlay: reads see the overlay, writes are recorded. It runs twice: a dry
// run with a placeholder VA decides every refusal and checks the plan against the prediction, then the host reserves
// the VA, and the real run repeats the sequence with that VA. Only then do the recorded writes reach guest memory,
// in order, with the batch flush between the queue entries and the count reset. So a refusal before the VA
// reservation writes nothing and reserves nothing. A refusal after the reservation (only a guest unmapped page in the
// owner table, since the plan does not depend on the VA) leaves the reserved range unused. A failing guest write in
// the commit leaves the earlier writes in place: the state is unknown (GuestWrite, LastWriteSet is then non-empty).
//
// Sticky node. vmem_query_reserve_map returns the node in rax when it is only partly consumed, and the loop keeps rax
// as the node of its next reserve step, without a search (0x1a1f1: test rax, rax; jne 0x1a2c0). The model's
// SearchNode (pc_pool.cpp) does not do that, so a prediction made with the search refuses here (Prediction) and the
// game runs the call.
//
// Host interface: the VA reservation and the batch flush are the port's own implementations (reserve_range and
// batch_map2 in src/runtime_memory.c, reached through sceKernelReserveVirtualRange and sceKernelBatchMap). The native
// path never calls the game's 0x1af80 or 0x1abf0 itself: after a native call the glue must apply the flushed entries
// to its shadow VA map with SotcPcPool::Model::ApplyBatchOp, as the PC_POOL_MAP hook does for the game's own flush.
//
// Not thread-safe: the glue calls it under its mutex and under the game's pool lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "pc_pool.h"
#include "pc_pool_native.h"

namespace SotcPcPoolNativeFresh {

/// One entry of the game's batch queue (32 bytes, root+0x2102f8 + 32 * index). Same layout as the port's
/// BatchEntry (src/runtime_memory.c): u64 start, u64 offset, u64 length, u8 prot, u8 type, u16 reserved, u32 operation.
struct BatchEntry {
    uint64_t va;
    uint64_t offset;
    uint64_t length;
    uint8_t prot;
    uint8_t type;
    uint16_t reserved;
    uint32_t op;
};
static_assert(sizeof(BatchEntry) == 32, "BatchEntry must match the game's 32-byte queue entry");

/// What the native path needs from the port, and only that.
class FreshHost {
public:
    virtual ~FreshHost() = default;
    /// sceKernelReserveVirtualRange(&va, size, flags 0, align): returns the reserved VA (a multiple of align),
    /// or 0 when nothing was reserved.
    virtual uint64_t ReserveVirtual(uint64_t size, uint64_t align) = 0;
    /// sceKernelBatchMap over entries[0..count) in order, as the game's flush (0x1af80) calls it. Returns the
    /// last error (0 when every entry applied). The game skips a failing entry and goes on; so does the port.
    virtual int32_t BatchMap(const BatchEntry* entries, size_t count) = 0;
};

/// Why a call was not performed natively. None after a performed call.
enum class Refusal : uint8_t {
    None,
    Args,            ///< zero or unaligned arguments, or the root / align cannot be used
    Size,            ///< size is not a multiple of 64 KiB, or it needs more owner-tag units than kMaxUnits
    Route,           ///< the prediction's path is not the entry point's path, or the plan's path is not the prediction's
    Prediction,      ///< the prediction is not Predicted/fresh, or its runs or reserve steps disagree with the plan
    NodePool,        ///< the node pool is empty or would drop below 3 nodes (the game refills through 0x19e00)
    Batch,           ///< the batch queue is not empty at entry, or a queue entry would be the 65th (the game flushes)
    NoNode,          ///< a reserve step with no node on its free-VA list (the game dereferences null): WouldCrash
    NextSet,         ///< POOL_FIX with no free VA node: the latent crash of the patched game (counted)
    PageCap,         ///< a set bit at or past the page cap, or a bitmap with no words: the game's out-of-bounds path
    RunPastBitmap,   ///< a run would extend past the end of the bitmap
    UnknownMap,      ///< a reserve step is needed and the PhysMap is missing or has unknown ranges
    LoopBound,       ///< the page loop did not finish in the bound (a model or state bug)
    GuestMismatch,   ///< the guest state the plan reads (set fields, bitmap words, free-VA lists) is not what it should be
    GuestUnmapped,   ///< a guest address the sequence reads or writes is not mapped
    HostReserve,     ///< the host returned no VA: nothing was written
    HostVa,          ///< the host returned a VA that is not aligned: the reservation is leaked, nothing was written
    GuestWrite,      ///< a guest write failed during the commit: the earlier writes stay (state unknown)
    kCount
};
const char* RefusalName(Refusal refusal);

/// Refusal of the last call (None after a performed call).
Refusal LastRefusal();
/// The error the batch flush returned in the last performed call (0 when every entry applied). Nonzero means the
/// map entries of that flush are unknown to the caller; the call itself still returned its node, as the game does.
int32_t LastFlushError();
/// The (address, size) of every guest Write the last call committed, in order (empty after a refusal before the
/// commit). The owner-tag bytes are one entry each.
std::vector<std::pair<uint64_t, size_t>> LastWriteSet();

/// Per-process counts. Not thread-safe: the glue calls under its lock.
uint64_t RefusalCount(Refusal refusal); ///< calls refused with this reason since the last ResetCounts
uint64_t PerformedCount();              ///< calls performed natively since the last ResetCounts
void ResetCounts();

/// The fresh plain path: the page loop takes bitmap bits of the primary set P only (kind 1: S1, else S2).
/// Arguments as ApplyExactHit: size (rcx), align (r9d), kind (stack), obj (rdi); root is the pool root (0x2dfca78).
/// pre is the snapshot the prediction used; pred is Model::Predict's answer for that call. phys_base is the global
/// at 0x2dfca70. map is the VA -> phys shadow map (only the reserve steps read it; nullptr refuses a reserve step).
/// Returns the node the game returns in rax, or 0 (see LastRefusal). A node is returned with LastFlushError()
/// nonzero when the flush failed; the caller must then treat the map as unknown.
/// Each entry point performs only its own path: pred.path must be that path, and the plan must come out as it.
uint64_t ApplyFreshPlain(SotcPcPoolNative::GuestMem& mem, FreshHost& host, uint64_t root,
                         const SotcPcPool::Model::Pools& pre, const SotcPcPool::Model::Prediction& pred,
                         uint64_t size, uint32_t align, int kind, uint64_t obj,
                         uint64_t phys_base = SotcPcPool::Model::kPhysBaseOffset,
                         const SotcPcPool::Model::PhysMap* map = nullptr);

/// The fresh path whose page loop ran the reserve-map step (P had no cached pages), with no spill and no POOL_FIX.
uint64_t ApplyFreshReserve(SotcPcPoolNative::GuestMem& mem, FreshHost& host, uint64_t root,
                           const SotcPcPool::Model::Pools& pre, const SotcPcPool::Model::Prediction& pred,
                           uint64_t size, uint32_t align, int kind, uint64_t obj,
                           uint64_t phys_base = SotcPcPool::Model::kPhysBaseOffset,
                           const SotcPcPool::Model::PhysMap* map = nullptr);

/// The fresh spill path: the page loop takes bitmap bits of P, then of the spill set F once P has neither cached
/// pages nor free VA (kind 1: F = S2, kinds 0 and 2: F = S1). Runs from F get a protect op 4 after their op 0
/// (type 3 when F is S1, 0 when F is S2). Reserve steps may run before and after the switch.
uint64_t ApplyFreshSpill(SotcPcPoolNative::GuestMem& mem, FreshHost& host, uint64_t root,
                         const SotcPcPool::Model::Pools& pre, const SotcPcPool::Model::Prediction& pred,
                         uint64_t size, uint32_t align, int kind, uint64_t obj,
                         uint64_t phys_base = SotcPcPool::Model::kPhysBaseOffset,
                         const SotcPcPool::Model::PhysMap* map = nullptr);

/// The POOL_FIX path: the page loop found no set bit in the bitmap, the patch zeroed the cached count and ran the
/// reserve-map step with rax = 0 (and rescans). Any number of such steps, and possibly a spill, is allowed as long as
/// the plan ends in this path (POOL_FIX has priority in the classification, as in the model).
uint64_t ApplyFreshPoolFix(SotcPcPoolNative::GuestMem& mem, FreshHost& host, uint64_t root,
                           const SotcPcPool::Model::Pools& pre, const SotcPcPool::Model::Prediction& pred,
                           uint64_t size, uint32_t align, int kind, uint64_t obj,
                           uint64_t phys_base = SotcPcPool::Model::kPhysBaseOffset,
                           const SotcPcPool::Model::PhysMap* map = nullptr);

} // namespace SotcPcPoolNativeFresh
