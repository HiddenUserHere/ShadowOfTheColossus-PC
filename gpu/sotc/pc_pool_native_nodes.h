// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: node-pool pop of the game's pool_node_alloc (0x19660) with its refill, native and model.
//
// Companion of pc_pool.h and pc_pool_native.h. pool_node_alloc pops one 0x70-byte node from the pool list
// (root+0x1f0, count root+0x200). When the count drops from 3 to 2 it refills the list: it calls pool_alloc_region
// (0x19e00) directly for a 64 KiB block (size 0x10000, align 8, kind 1, va_hint = node = mode = obj = 0), adds
// 0x10000 to the bookkeeping global DAT_02dfca60, and carves the block into 585 nodes that it pushes on the pool
// list. The refill's own allocation is served by SotcPcPoolNative::ApplyExactHitTo (it must be an exact hit); any
// other outcome refuses the whole pop, with nothing written.
//
// Store lists (traced in out/re/decomp/0001/00019660.c and the disassembly 0x19660-0x197c5): see the top of
// pc_pool_native_nodes.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "pc_pool.h"
#include "pc_pool_native.h"

namespace SotcPcPoolNodes {

/// The refill request, as pool_node_alloc passes it to pool_alloc_region (0x19e00).
constexpr uint64_t kRefillSize = 0x10000;
constexpr uint32_t kRefillAlign = 8;
constexpr int kRefillKind = 1;
/// The pop refills when the count after the pop is this value (the count goes 3 -> 2).
constexpr int32_t kRefillCount = 2;
/// Node stride of the carve (the loop steps 0x70 and carves size / 0x70 nodes; the test checks 585 for 64 KiB).
constexpr uint64_t kStride = 0x70;

/// A guest write, as SotcPcPoolNative::LastWriteSet reports it.
using Write = std::pair<uint64_t, size_t>;

/// PopNode outcome. Refused: nothing was written. Unknown: some writes went through and the rest did not (a Write
/// failed or a read failed after the first store); the caller must treat the pool state as unknown and stop native
/// execution.
enum class Status : uint8_t { Popped, Refused, Unknown };

// ---- model (pure) -------------------------------------------------------------------------------------------

/// What the pop (and the refill, when it happens) does, predicted from the snapshot.
struct PopPrediction {
    bool ok = false;               ///< false: the pop is refused or unmodelled (reason says why)
    bool refill = false;           ///< the count went 3 -> 2: the 64 KiB refill runs
    const char* reason = "";
    int32_t count_pre = 0;         ///< root+0x200 before the pop
    int32_t count_post = 0;        ///< root+0x200 after the pop and the refill's carve
    /// The refill's exact-hit prediction (refill only). Its slot, va and size name the block the refill takes.
    SotcPcPool::Model::Prediction nested;
    uint64_t block_va = 0;         ///< refill only: the carved block's VA (the nested run's base)
    uint64_t block_nodes = 0;      ///< refill only: nodes carved from the block (585 for 64 KiB)
    uint64_t overhead_delta = 0;   ///< change of DAT_02dfca60 (0x10000 with a refill, else 0)
    /// The set state after the pop: the nested exact hit applied on refill, else pre unchanged. Node-list state is
    /// not part of Pools; it is count_post and the carve above.
    SotcPcPool::Model::Pools post;
};

/// Predicts a pop of the pool list whose root+0x200 count is count_pre. Pure: no guest access.
PopPrediction PredictPop(const SotcPcPool::Model::Pools& pre, int32_t count_pre);

/// Predicts a node-taking allocation: the call's own prediction (Predict on pre) for a FlexHitNode or PrimaryHitNode
/// path, plus the pops it makes (one for a pad, one for a split; *pop gets the count after them and the refill).
/// On such a path a pop that takes the count from 3 to 2 refills inside the call: the post state is the call's
/// prediction taken on the post-refill state (the call's decision must stay the same; otherwise Unmodelled). Unmodelled
/// as well: a refill that takes the call's own run; a second pop's refill (count 4 with a pad and a split); a split with
/// no pad whose refill takes the run after the block (the remainder is linked before it, read before the pop).
/// Other paths pop nothing and return Predict unchanged.
SotcPcPool::Model::Prediction PredictNodeCall(const SotcPcPool::Model::Pools& pre,
                                              const SotcPcPool::Model::Request& req,
                                              const SotcPcPool::Model::PhysMap* map, SotcPcPool::Model::Pools* post,
                                              int32_t count_pre, PopPrediction* pop);

/// True for the two paths that pop a node from pool_node_alloc (0x19660): FlexHitNode and PrimaryHitNode.
bool PopsNode(SotcPcPool::Model::Path path);

// ---- native ------------------------------------------------------------------------------------------------

/// The refill's bookkeeping qword DAT_02dfca60 is an image global (eboot VA 0x2dfca60), not a field of the pool
/// root: the root pointer lives at image global 0x2dfca78 and points into direct memory. So its guest address is the
/// image base plus this offset.
constexpr uint64_t kOverheadGlobal = 0x2dfca60;
constexpr uint64_t OverheadAddr(uint64_t image_base) { return image_base + kOverheadGlobal; }

/// Pops one node from the pool list at root, refilling it when the count drops to 2. pre is the snapshot the
/// prediction uses: the set state as it is at this point of the caller's sequence (a writer that has already written
/// set fields passes RefreshSnapshot of the overlay). The refill's exact hit is planned on pre.
///
/// Order of the work: every read that can refuse is done first; the refill's exact hit (ApplyExactHitTo, predicted
/// with PredictPop on pre) runs before the pop stores, then the overhead add and the carve. The exact hit's reads and
/// writes are disjoint from the pop's (set headers, free-VA nodes, owner tags versus pool-list nodes, root+0x1e0..0x207),
/// so the final image equals the game's order (pop, counter, refill, carve). The write list (*writes) is in the order
/// performed here.
///
/// overhead_addr: guest address of the qword DAT_02dfca60 (OverheadAddr(image base)). Required for a refill: 0 refuses a pop
/// that would refill. keep_base/keep_size: the free run the calling call has chosen (keep_size 0: none). A refill whose
/// block overlaps it is refused, since the game would hand the call's own run to the refill.
///
/// Returns true with *node_out = the popped node (rax of 0x19660) and status Popped. Returns false with status Refused
/// (nothing written) or Unknown (partial writes; kill native execution). writes may be null. The mem of a writer is
/// normally an Overlay, so a refusal of a later step discards the earlier ones.
bool PopNode(SotcPcPoolNative::GuestMem& mem, uint64_t root, const SotcPcPool::Model::Pools& pre,
             uint64_t* node_out, std::vector<Write>* writes, uint64_t overhead_addr, Status* status,
             uint64_t keep_base = 0, uint64_t keep_size = 0);

} // namespace SotcPcPoolNodes
