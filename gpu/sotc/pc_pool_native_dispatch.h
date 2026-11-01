// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the writer dispatch and the modelled-memory check of the native pool path, factored out of the glue
// (pc_pool.cpp, NativeHost) so they can be tested with a fake guest memory and a fake fresh host.
//
// Companion of pc_pool.h (the model), pc_pool_native*.h (the writers). Nothing here touches the game: the glue
// supplies the guest memory, the fresh host (VA reservation and batch flush), the shadow map and the switches.
//
// Three entry points, in the order the controller calls them:
//   PredictCall  : the prediction of the call (PredictNodeCall when refills are on, Model::Predict otherwise).
//   Apply        : runs the writer of the predicted path (or refuses with 0, having written nothing).
//   InModelled   : checks each guest range the writer wrote against the memory the model owns in the pre state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "pc_pool.h"
#include "pc_pool_native.h"
#include "pc_pool_native_fresh.h"

namespace SotcPcPoolDispatch {

/// Pool layout the dispatch and the glue share (root-relative offsets, set stride, batch queue).
namespace Layout {
constexpr uint64_t kSetStride = 0xa0;                 ///< root, root+0xa0, root+0x140: the three set headers
constexpr uint64_t kSetHeadersBytes = 3 * kSetStride; ///< root .. root+0x1e0
constexpr uint64_t kNodeCountOff = 0x200;             ///< node pool count (i32)
constexpr uint64_t kNodeSentinelOff = 0x1e0;          ///< node pool sentinel element
constexpr uint64_t kNodeHeadOff = 0x1f0;              ///< node pool head link
constexpr uint64_t kNodeWalkCap = 4096;               ///< bound on the walk of the node list
constexpr uint64_t kNodeExtent = 0x80;                ///< the bytes of a node the model owns (fields up to +0x68)
constexpr uint64_t kBatchCountOff = 0x2102f0;         ///< batch queue count (i32)
constexpr uint64_t kBatchEntriesOff = 0x2102f8;       ///< batch queue entries (32 bytes each)
constexpr int kBatchMax = 64;
constexpr uint64_t kBatchEntryBytes = 32;
} // namespace Layout

/// The writer that ran the call (None: refused before any writer ran, or a path with no writer here).
enum class Writer : uint8_t { None, Exact, Node, Primary, FreshPlain, FreshSpill, FreshReserve, FreshPoolFix };

/// Per-call state the dispatch keeps between Apply and InModelled / LastWriteSet / RefusalReason. The glue sets the
/// inputs (root, the refill switch and address, refill_block, map) before Apply; Apply sets the rest.
struct State {
    uint64_t root = 0;             ///< pool root (image global 0x2dfca78 holds it)
    bool refill = false;           ///< SOTC_PC_POOL_REFILL: node pops may refill (the writers get overhead_addr)
    uint64_t overhead_addr = 0;    ///< guest address of DAT_02dfca60 (SotcPcPoolNodes::OverheadAddr(image base))
    uint64_t refill_block = 0;     ///< the refill block of this call (PredictCall), 0 when none
    const SotcPcPool::Model::PhysMap* map = nullptr; ///< shadow VA map of the fresh writers (null: UnknownMap)
    Writer writer = Writer::None;  ///< the writer of the last Apply
    std::vector<uint64_t> pool_nodes;    ///< pool-list nodes the writer may store into (read before the writer)
    std::vector<uint64_t> region_links;  ///< old region-list head + 0x10 of each set, in the pre state
};

const char* WriterName(Writer writer);

/// The prediction of one call. refill false: Model::Predict. refill true: PredictNodeCall, which also predicts the
/// node pop's refill; *refill_block (may be null) gets the refill's block when the call is Predicted and refills, else 0.
SotcPcPool::Model::Prediction PredictCall(const SotcPcPool::Model::Pools& pre, const SotcPcPool::Model::Request& req,
                                          const SotcPcPool::Model::PhysMap* map, SotcPcPool::Model::Pools* post,
                                          int32_t count_pre, bool refill, uint64_t* refill_block);

/// Runs the writer of p.path (the glue's dispatch by path). Returns the node, or 0 when refused (then st->writer is
/// None or the writer that refused, and LastWriteSet is empty unless a write failed in the commit).
uint64_t Apply(SotcPcPoolNative::GuestMem& mem, SotcPcPoolNativeFresh::FreshHost& host, State* st,
               const SotcPcPool::Model::Pools& pre, const SotcPcPool::Model::Prediction& p,
               const SotcPcPool::Model::Request& req);

/// The guest ranges the last Apply wrote (the last writer's list).
std::vector<std::pair<uint64_t, size_t>> LastWriteSet(const State& st);

/// The writer's own reason for the last refusal ("" when the writer gives none).
const char* RefusalReason(const State& st);

/// True when [addr, addr + n) is memory the model owns in pre, or memory the writer of this call may store into:
/// the three set headers, the node-pool header, the batch queue, the pool nodes and touched nodes (see the .cpp),
/// the boot-seeded node array, the owner-tag tables, the free-VA nodes and bitmap words of pre, and, with refills on,
/// the refill bookkeeping qword and the refill block.
bool InModelled(const State& st, const SotcPcPool::Model::Pools& pre, uint64_t addr, size_t n);

} // namespace SotcPcPoolDispatch
