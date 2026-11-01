// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native fresh-VA page loop of the game's pool allocator, the vmem_query_reserve_map (flag 1, 0x1abf0)
// step and the POOL_FIX case (eboot 0x1a321). Companion of pc_pool.h (the model) and pc_pool_native.h (the
// exact-hit path). The model's ReserveMap (pc_pool.cpp, namespace Model) is the reference; this file applies the
// same state change to guest memory through GuestMem, in one place, and refuses whatever the model cannot vouch for.
//
// Scope: one call of the reserve-map step on set q (1 = Onion, 2 = Garlic; the flexible set has no bitmap and is
// refused). ApplyReserveMap is the fresh path (cached phys is zero when the step runs). ApplyPoolFix is the
// POOL_FIX case: the bitmap has no free bit, the patch zeroes the cached count and takes the same step.
//
// Deliberate difference from the patched game (documented in pc_pool_native_reserve.cpp, counted):
//   the latent crash (no free bit AND an empty free-VA list). The patched game jumps to 0x1a1f1, skips the empty
//   list guard at 0x1a1e4 and reads [0 + 0x58]. ApplyPoolFix does not run that: it writes nothing and returns
//   Status::NextSet, the case the original unpatched guard takes (the next pool set), and counts it.
//
// The native path reads the VA -> phys map from SotcPcPool::Model::PhysMap (fed by the glue from the batch ops),
// not from the game's sceKernelVirtualQuery walk. Any unknown range in the map refuses the whole call.
//
// The unmap the game queues (batch op 1 for the consumed range) IS written here, into the batch queue in guest
// memory (root+0x2102f0), as the game does. ReserveResult::unmapped lists the same range for the glue's map state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "pc_pool.h"
#include "pc_pool_native.h"

namespace SotcPcPoolNativeReserve {

using SotcPcPool::Model::PhysMap;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::Set;
using SotcPcPoolNative::GuestMem;

enum class Status : uint8_t {
    Ok,            ///< the step ran: guest memory holds the state the model's ReserveMap leaves
    NotApplicable, ///< not this call: set index not 1 or 2, rem_pages 0, ApplyReserveMap with cached != 0,
                   ///< or ApplyPoolFix with a free bit in the bitmap. Nothing written.
    UnknownMap,    ///< the PhysMap has unknown ranges: refused. Nothing written.
    Refused,       ///< the guest does not match the snapshot, a list or header check failed, a node has size 0,
                   ///< or the map maps a phys bit outside the bitmap. Nothing written.
    NoNode,        ///< ApplyReserveMap with an empty free-VA list: the game would dereference null (WouldCrash).
                   ///< Nothing written.
    NextSet,       ///< ApplyPoolFix latent crash (no free bit, empty free-VA list): nothing written, the caller
                   ///< continues in the next pool set as the original guard at 0x1a1e4 does.
    QueueFull,     ///< the batch queue holds 64 entries: the game flushes it first (0x1adea). The glue flushes and
                   ///< calls again. Nothing written.
    WriteFailed,   ///< a Write failed in the middle of the sequence: the earlier writes stay; the state is unknown.
};
const char* StatusName(Status status);

/// What one successful step did, and the set as the model would leave it.
struct ReserveResult {
    uint64_t node_base = 0;      ///< VA start of the node the step consumed from (its base before the call)
    uint64_t consumed = 0;       ///< bytes consumed from the front of that node (1 MiB rounded, capped at its size)
    bool node_removed = false;   ///< the node was used up and unlinked from the free-VA list
    std::vector<uint64_t> bits;  ///< the phys bits the walk took, in VA order (the bitmap OR and the cached gain)
    /// (VA, bytes) ranges the step unmaps (batch op 1, also written to the queue in guest memory).
    std::vector<std::pair<uint64_t, uint64_t>> unmapped;
    Set post;                    ///< set q after the step, in the model's terms (Model::ReserveMap on a copy)
    uint64_t node = 0;           ///< guest address of the node the step consumed from
    /// rax when vmem_query_reserve_map returns (eboot 0x1af07 / 0x1af2b): the node when the node is only partly
    /// consumed, 0 when it is used up. The page loop keeps it, and the next reserve step of the same call uses it
    /// as its node without a search (0x1a1f1: test rax, rax; jne 0x1a2c0). The fresh path passes it back as node_hint.
    uint64_t returned = 0;
};

/// vmem_query_reserve_map flag 1 on the node chosen from set q's free-VA list (the fresh path: cached phys is 0).
/// rem_pages is the page loop's remaining pages (r11d). Applies, in the game's order, to guest memory:
///   1. bitmap words that gain bits: u64 OR at words_addr + 8 * word (only words whose value changes)
///   2. S+0x74 cached := bits taken (i32)
///   3. node+0x50 backed -= bits * 64 KiB (u64), only when bits were taken
///   4. batch queue entry (op 1: unmap of the consumed range, prot 0, type 0) at root+0x2102f8 + 32 * count,
///      then count += 1 at root+0x2102f0 (the flag r8d is 1 on this path)
///   5. S+0x70 free VA pages -= consumed / 64 KiB (i32)
///   6a. node used up (consumed == node size): unlink it from the free-VA list, zero its backed/size/base and push
///       it onto the node pool (root+0x1e0 sentinel, head root+0x1f0, count root+0x200). The model does not
///       track the node pool; the game does this, so the native path does too.
///   6b. otherwise the node keeps its place: pool id, base += consumed, size -= consumed, in-use 0, owner 0, -1.
/// The store addresses with eboot addresses are in pc_pool_native_reserve.cpp.
/// Every guest value is read and compared with pre before the first write, and the search is the model's
/// SearchNode (the sticky first candidate included). Returns Ok, or a refusal with nothing written.
/// node_hint != 0: the node the game takes without a search (the page loop's rax, see ReserveResult::returned);
/// it must be on set q's free-VA list, else Refused. 0: SearchNode.
Status ApplyReserveMap(GuestMem& mem, uint64_t root, const Pools& pre, int q, uint64_t rem_pages,
                       const PhysMap& map, ReserveResult* out, uint64_t node_hint = 0);

/// The POOL_FIX case (spec 2.5, eboot 0x1a321) for set q: the bitmap has no free bit (the scan that the patch
/// replaces finds nothing). Cached may be above zero or zero. Patched behaviour: the cached count is zeroed
/// (mov dword [r14+0x74], 0), then the same reserve-map step runs (ApplyReserveMap above, with cached 0).
/// Exception: when the free-VA list is also empty (the latent crash), nothing is written and NextSet is returned;
/// the counter ReserveCounters::poolfix_next_set counts it. Returns NotApplicable when a free bit exists.
/// The cached-zero store is the first entry of LastWriteSet, followed by the steps' writes.
Status ApplyPoolFix(GuestMem& mem, uint64_t root, const Pools& pre, int q, uint64_t rem_pages, const PhysMap& map,
                    ReserveResult* out);

/// The (address, size) of every Write the last ApplyReserveMap or ApplyPoolFix call issued, in order (empty after
/// a refusal that happened before the first write).
std::vector<std::pair<uint64_t, size_t>> LastWriteSet();

/// Per-process counts of the outcomes. Not thread-safe: the glue calls under its lock.
struct ReserveCounters {
    uint64_t reserve_ok = 0;
    uint64_t reserve_not_applicable = 0;
    uint64_t reserve_refused = 0;     ///< any status other than Ok or NotApplicable (NoNode included)
    uint64_t poolfix_ok = 0;
    uint64_t poolfix_next_set = 0;    ///< the latent crash of the patched game, taken on the original guard path
    uint64_t poolfix_not_applicable = 0;
    uint64_t poolfix_refused = 0;
};
const ReserveCounters& Counters();
void ResetCounters();
/// Saves and restores the counters, so a caller that runs a step twice (a dry run, then the real one) counts it once.
ReserveCounters SnapshotCounters();
void RestoreCounters(const ReserveCounters& saved);

} // namespace SotcPcPoolNativeReserve
