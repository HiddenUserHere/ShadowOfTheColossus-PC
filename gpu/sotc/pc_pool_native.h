// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native application of an exact free-run hit of the game's pool allocator (pool_alloc_region 0x19e00
// calling pool_take_free_run 0x1a800). Companion of pc_pool.h: the model predicts the call, this file applies
// the writes the game would make, in the game's order, through a guest memory interface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "pc_pool.h"

namespace SotcPcPoolNative {

/// Guest memory access. Read and Write return false when the range is not mapped. Implemented by the port
/// over the backing view (guest_mem.h) and by the test over a byte map.
struct GuestMem {
    virtual ~GuestMem() = default;
    virtual bool Read(uint64_t addr, void* out, size_t n) = 0;
    virtual bool Write(uint64_t addr, const void* in, size_t n) = 0;
};

/// True when p lies inside the pool root struct (root .. root + 0x400000). A node or a free-VA element never lives there
/// (nodes come from carved blocks), so a node pointer or link read from guest memory that points into the root is a
/// corrupt pool: the writers refuse before any write (a store there would be outside the modelled memory).
// sotc: a node or link pointer into the root's header area (set headers root..+0x1e0, node-pool header
// +0x1e0..+0x208) is corrupt: no node lives there. The rest of the root struct is not checked: the boot
// code seeds nodes inside it (a 4 MiB guard refused every node path in game, 1 Nov).
inline bool InPoolRoot(uint64_t root, uint64_t p) { return p >= root && p - root < 0x208; }

/// The pool nodes the current writer call read and validated as pool-list or free-VA nodes (the ones it may store
/// into). Cleared at the start of each writer call; the writers and SotcPcPoolNodes::PopNode record each node they
/// validate before the first store. The glue's InModelledMemory allows kNodeExtent of each (the writer's own read set,
/// instead of a separate walk of the list).
void ClearTouchedNodes();
void NoteTouchedNode(uint64_t node);
std::vector<uint64_t> TouchedNodes();

/// Applies, for an EXACT free-run hit (Path FlexHitExact or PrimaryHitExact: no pad, no split, no node taken),
/// every write that pool_alloc_region (0x19e00) and pool_take_free_run (0x1a800) make on that path, in the
/// game's order, through mem. Arguments are the wrapper's: size (rcx), align (r9d), kind (stack), and obj (rdi:
/// the family object; node+0x40 and the owner-tag byte [obj+0x1c] come from it). va_hint and mode are 0.
/// root is the pool root (image global 0x2dfca78). pre is the snapshot the prediction used, pred the model's
/// prediction for this call.
///
/// Returns the node the game returns in rax (the block node), or 0 when the call is not an exact hit or the guest
/// does not match pre. A 0 return means nothing was written, except when a Write fails in the middle of the
/// sequence: then the earlier writes stay and the caller must treat the state as unknown.
uint64_t ApplyExactHit(GuestMem& mem, uint64_t root, const SotcPcPool::Model::Pools& pre,
                       const SotcPcPool::Model::Prediction& pred, uint64_t size, uint32_t align, int kind,
                       uint64_t obj);

/// ApplyExactHit with its write list appended to *log (may be null). It keeps no global state, so a writer can call
/// it inside its own sequence (the refill's nested exact hit does). mem may be an Overlay.
uint64_t ApplyExactHitTo(GuestMem& mem, uint64_t root, const SotcPcPool::Model::Pools& pre,
                         const SotcPcPool::Model::Prediction& pred, uint64_t size, uint32_t align, int kind,
                         uint64_t obj, std::vector<std::pair<uint64_t, size_t>>* log);

/// Applies, for a flexible-set node hit (Path::FlexHitNode: kind 1, the first fit in S0 with a pad before the
/// aligned start, or a split of the block's backed bytes, or both), every write of pool_take_free_run (0x1a800)
/// and of the pool_node_alloc (0x19660) pops it makes, in the game's order. The store list is at the top of
/// pc_pool_native.cpp (section "Flexible node path"). Arguments as ApplyExactHit.
///
/// Each node pop goes through SotcPcPoolNodes::PopNode on the call's Overlay, with the set state read back from the
/// overlay at that point. A pop that leaves the pool count at 2 refills (64 KiB, nested exact hit, carve), which
/// needs overhead_addr (guest address of DAT_02dfca60: image base + 0x2dfca60; 0 refuses a refilling pop). A refill whose
/// block would overlap the call's own run is refused.
///
/// Every write goes to an overlay first and reaches mem only after the whole sequence has passed, so a refusal
/// writes nothing. A refusal is returned as 0 when: the prediction is not FlexHitNode for kind 1, the first fit is
/// an exact start (ApplyExactHit's path), a pop cannot run (empty pool, list shape, refill not an exact hit), a
/// guest read or write is unmapped, or the guest does not match pre. A failing write during the commit leaves the
/// earlier writes in place (the caller must then treat the state as unknown; LastWriteSet is non-empty).
uint64_t ApplyNodeHit(GuestMem& mem, uint64_t root, const SotcPcPool::Model::Pools& pre,
                      const SotcPcPool::Model::Prediction& pred, uint64_t size, uint32_t align, int kind,
                      uint64_t obj, uint64_t overhead_addr = 0);

/// Rebuilds the set part of a snapshot from guest memory as it is now: the three set headers, the region lists and
/// the free-VA lists (their runs). The bitmap words, page_cap and has_bitmap are copied from pre, which the node
/// writers never change. Returns false when a read fails or a list does not walk. Used to plan a refill on the state
/// the game has at that point of the call.
bool RefreshSnapshot(GuestMem& mem, uint64_t root, const SotcPcPool::Model::Pools& pre,
                     SotcPcPool::Model::Pools* out);

/// Writes are staged: reads see the staged bytes, and nothing reaches the base memory until Commit. A writer runs its
/// whole sequence on an Overlay, so a refusal anywhere leaves guest memory untouched. bad is set by a read or write
/// that the base does not map (the caller refuses on it).
class Overlay final : public GuestMem {
public:
    struct Store {
        uint64_t addr = 0;
        std::vector<uint8_t> data;
    };

    explicit Overlay(GuestMem& base) : base_(base) {}

    bool bad = false;

    bool Read(uint64_t addr, void* out, size_t n) override;
    bool Write(uint64_t addr, const void* in, size_t n) override;

    /// The staged stores, in the order the writer issued them.
    const std::vector<Store>& stores() const { return stores_; }
    /// Applies the staged stores to the base in order and appends them to *log (may be null). False when a base
    /// write fails: the earlier stores stay, and the caller must treat the state as unknown.
    bool Commit(std::vector<std::pair<uint64_t, size_t>>* log);

private:
    GuestMem& base_;
    std::map<uint64_t, uint8_t> overlay_;
    std::vector<Store> stores_;
};

/// One pool_node_alloc pop on an overlay, refill included (see SotcPcPoolNodes::PopNode). The set state is read back
/// from t with RefreshSnapshot(t, root, pre, ...), so the call must come after the writes the game makes before the
/// pop. keep_base/keep_size: the free run the call chose (keep_size 0: none). False: refused.
bool PopNodeOn(Overlay& t, uint64_t root, const SotcPcPool::Model::Pools& pre, uint64_t overhead_addr,
               uint64_t keep_base, uint64_t keep_size, uint64_t* node);

/// The (address, size) of every Write the last ApplyExactHit or ApplyNodeHit call issued, in order (empty after a
/// refusal). The owner-tag memset is one entry of its byte count.
std::vector<std::pair<uint64_t, size_t>> LastWriteSet();

} // namespace SotcPcPoolNative
