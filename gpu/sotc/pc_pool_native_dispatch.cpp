// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: writer dispatch and modelled-memory check of the native pool path (see pc_pool_native_dispatch.h). The logic is
// the glue's NativeHost code moved here unchanged, except for the refill terms (SOTC_PC_POOL_REFILL) and the overhead
// address, which is an image global and not a root field.
#include "pc_pool_native_dispatch.h"

#include "pc_pool_native_nodes.h"
#include "pc_pool_native_primary.h"

namespace SotcPcPoolDispatch {

using namespace Layout;
using SotcPcPool::Model::Path;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::PhysMap;
using SotcPcPool::Model::Prediction;
using SotcPcPool::Model::Request;
using SotcPcPool::Model::Set;
using SotcPcPool::Model::Run;
using SotcPcPoolNative::GuestMem;

namespace {

// The element link of the node pool list: element E holds the node [E] and its next element at E+0x10.
constexpr uint64_t kNextOff = 0x10;

bool ReadU64(GuestMem& mem, uint64_t addr, uint64_t* out) { return mem.Read(addr, out, 8); }

// The node pool list as it is before the writer runs: each element E = [head], then E+0x10, bounded by the count at
// root+0x200. The node of E is [E]. A broken walk leaves the list short, and then the writes to its nodes read as
// outside the model: a kill.
void CollectPoolNodes(GuestMem& mem, State* st) {
    st->pool_nodes.clear();
    st->region_links.clear();
    const uint64_t root = st->root;
    if (root == 0) {
        return;
    }
    // The region-list heads before the writer runs: [S+0x8] of each set (the fresh writer's old head), so the link
    // store old head + 0x10 is known as the game's own. The head element is E = [S+0x8].
    for (uint64_t s = 0; s < 3; ++s) {
        uint64_t head = 0;
        if (ReadU64(mem, root + s * kSetStride + 0x8, &head) && head != 0) {
            st->region_links.push_back(head + kNextOff);
        }
    }
    int32_t count = 0;
    uint64_t elem = 0;
    if (!mem.Read(root + kNodeCountOff, &count, sizeof count) || count < 0 ||
        !ReadU64(mem, root + kNodeHeadOff, &elem)) {
        return;
    }
    const uint64_t sentinel = root + kNodeSentinelOff;
    for (uint64_t i = 0; i < static_cast<uint64_t>(count) && i < kNodeWalkCap && elem != 0 && elem != sentinel; ++i) {
        uint64_t node = 0;
        uint64_t next = 0;
        if (!ReadU64(mem, elem, &node) || !ReadU64(mem, elem + kNextOff, &next)) {
            return;
        }
        st->pool_nodes.push_back(node);
        elem = next;
    }
}

// The overhead address a node writer receives: the refill needs it, so it is passed only with refills on (0 refuses a
// refilling pop, which is the default).
uint64_t OverheadArg(const State& st) { return st.refill ? st.overhead_addr : 0; }

} // namespace

const char* WriterName(Writer writer) {
    switch (writer) {
    case Writer::None:
        return "none";
    case Writer::Exact:
        return "exact";
    case Writer::Node:
        return "flex node";
    case Writer::Primary:
        return "primary node";
    case Writer::FreshPlain:
        return "fresh plain";
    case Writer::FreshSpill:
        return "fresh spill";
    case Writer::FreshReserve:
        return "fresh reserve";
    case Writer::FreshPoolFix:
        return "fresh pool fix";
    }
    return "none";
}

Prediction PredictCall(const Pools& pre, const Request& req, const PhysMap* map, Pools* post, int32_t count_pre,
                       bool refill, uint64_t* refill_block) {
    if (refill_block != nullptr) {
        *refill_block = 0;
    }
    if (!refill) {
        return SotcPcPool::Model::Predict(pre, req, map, post);
    }
    SotcPcPoolNodes::PopPrediction pop;
    const Prediction p = SotcPcPoolNodes::PredictNodeCall(pre, req, map, post, count_pre, &pop);
    if (refill_block != nullptr && p.outcome == SotcPcPool::Model::Outcome::Predicted && pop.ok && pop.refill) {
        *refill_block = pop.block_va;
    }
    return p;
}

uint64_t Apply(GuestMem& mem, SotcPcPoolNativeFresh::FreshHost& host, State* st, const Pools& pre, const Prediction& p,
               const Request& req) {
    const uint32_t align = static_cast<uint32_t>(req.align);
    const uint64_t root = st->root;
    st->writer = Writer::None;
    st->pool_nodes.clear();
    st->region_links.clear();
    switch (p.path) {
    case Path::FlexHitExact:
    case Path::PrimaryHitExact:
        st->writer = Writer::Exact;
        return SotcPcPoolNative::ApplyExactHit(mem, root, pre, p, req.size, align, req.kind, req.obj);
    case Path::FlexHitNode:
        st->writer = Writer::Node;
        CollectPoolNodes(mem, st);
        return SotcPcPoolNative::ApplyNodeHit(mem, root, pre, p, req.size, align, req.kind, req.obj, OverheadArg(*st));
    case Path::PrimaryHitNode:
        st->writer = Writer::Primary;
        CollectPoolNodes(mem, st);
        return SotcPcPoolNative::ApplyPrimaryNodeHit(mem, root, pre, p, req.size, align, req.kind, req.obj,
                                                     OverheadArg(*st));
    case Path::FreshPlain:
        st->writer = Writer::FreshPlain;
        CollectPoolNodes(mem, st);
        return SotcPcPoolNativeFresh::ApplyFreshPlain(mem, host, root, pre, p, req.size, align, req.kind, req.obj,
                                                      SotcPcPool::Model::kPhysBaseOffset, st->map);
    case Path::FreshSpill:
        st->writer = Writer::FreshSpill;
        CollectPoolNodes(mem, st);
        return SotcPcPoolNativeFresh::ApplyFreshSpill(mem, host, root, pre, p, req.size, align, req.kind, req.obj,
                                                      SotcPcPool::Model::kPhysBaseOffset, st->map);
    case Path::FreshReserve:
        st->writer = Writer::FreshReserve;
        CollectPoolNodes(mem, st);
        return SotcPcPoolNativeFresh::ApplyFreshReserve(mem, host, root, pre, p, req.size, align, req.kind, req.obj,
                                                        SotcPcPool::Model::kPhysBaseOffset, st->map);
    case Path::FreshPoolFix:
        st->writer = Writer::FreshPoolFix;
        CollectPoolNodes(mem, st);
        return SotcPcPoolNativeFresh::ApplyFreshPoolFix(mem, host, root, pre, p, req.size, align, req.kind, req.obj,
                                                        SotcPcPool::Model::kPhysBaseOffset, st->map);
    default:
        return 0;
    }
}

std::vector<std::pair<uint64_t, size_t>> LastWriteSet(const State& st) {
    switch (st.writer) {
    case Writer::Exact:
    case Writer::Node:
        return SotcPcPoolNative::LastWriteSet();
    case Writer::Primary:
        return SotcPcPoolNative::PrimaryLastWriteSet();
    case Writer::FreshPlain:
    case Writer::FreshSpill:
    case Writer::FreshReserve:
    case Writer::FreshPoolFix:
        return SotcPcPoolNativeFresh::LastWriteSet();
    case Writer::None:
        break;
    }
    return {};
}

const char* RefusalReason(const State& st) {
    switch (st.writer) {
    case Writer::FreshPlain:
    case Writer::FreshSpill:
    case Writer::FreshReserve:
    case Writer::FreshPoolFix:
        return SotcPcPoolNativeFresh::RefusalName(SotcPcPoolNativeFresh::LastRefusal());
    case Writer::Exact:
    case Writer::Node:
    case Writer::Primary:
    case Writer::None:
        break;
    }
    return "";
}

bool InModelled(const State& st, const Pools& pre, uint64_t addr, size_t n) {
    const uint64_t root = st.root;
    auto inside = [addr, n](uint64_t base, uint64_t len) {
        return addr >= base && n <= len && addr - base <= len - n;
    };
    if (root != 0 && inside(root, kSetHeadersBytes)) {
        return true;
    }
    // sotc: the node pool header the node pop writes (pool_node_alloc 0x19660): sentinel root+0x1e0, tail link
    // +0x1e8, head link +0x1f0, count i32 +0x200.
    if (root != 0 && inside(root + kNodeSentinelOff, 0x28)) {
        return true;
    }
    if (root != 0 && inside(root + kBatchCountOff, (kBatchEntriesOff - kBatchCountOff) + kBatchMax * kBatchEntryBytes)) {
        return true;
    }
    for (uint64_t node : st.pool_nodes) {
        if (inside(node, kNodeExtent)) {
            return true;
        }
    }
    // sotc: the nodes the writer read and validated (pc_pool_native.h TouchedNodes): the boot code seeds the
    // pool nodes inside the root struct (root+0x208 + k*0x70, eboot 0x18aa0), and a pop's next-link store can
    // land in a node the walk above did not collect (root+0x310 = N+0x28 of a seeded node in the game kill).
    for (uint64_t node : SotcPcPoolNative::TouchedNodes()) {
        if (inside(node, kNodeExtent)) {
            return true;
        }
    }
    // sotc: the boot-seeded node array (root+0x208 + k*0x70 up to the owner-tag table at root+0x111908,
    // eboot 0x18aa0). The touched list missed a pop's neighbour link here in game (root+0x310, 1 Nov); every
    // native call is still verified against its prediction afterwards.
    if (root != 0 && inside(root + 0x208, 0x111908 - 0x208)) {
        return true;
    }
    // sotc: the owner-tag tables the exact hit fills (pc_pool_native.cpp W21): 1 MiB units indexed
    // by VA >> 20 at root+0x111908, and 64 KiB units of the flexible span at root+0x20d918 (they end
    // where the timing block at root+0x20f280 starts).
    if (root != 0 && (inside(root + 0x111908, 0x20d918 - 0x111908) || inside(root + 0x20d918, 0x20f280 - 0x20d918))) {
        return true;
    }
    // sotc: refills (SOTC_PC_POOL_REFILL). The bookkeeping qword DAT_02dfca60 grows by 0x10000 (pool_node_alloc), and
    // the refill's 64 KiB block is carved into nodes and takes its own exact hit (the block node, its owner tag). Both
    // are allowed only for a call that refills; the block is the one PredictCall named for this call.
    if (st.refill) {
        if (st.overhead_addr != 0 && inside(st.overhead_addr, 8)) {
            return true;
        }
        if (st.refill_block != 0 && inside(st.refill_block, SotcPcPoolNodes::kRefillSize)) {
            return true;
        }
    }
    // sotc: the region-list link the fresh writer makes (0x1a0f1-0x1a11a, decompile 00019e00.c lines 146-148): the
    // old head element's next (old head + 0x10) takes the new element. The old heads are collected in the pre
    // phase (CollectPoolNodes), so the check is on the snapshot, not on the state after the write.
    if (n == 8) {
        for (uint64_t link : st.region_links) {
            if (addr == link) {
                return true;
            }
        }
    }
    for (const Set& s : pre.set) {
        for (const Run& run : s.free_va) {
            if (run.node != 0 && inside(run.node, kNodeExtent)) {
                return true;
            }
        }
        if (s.has_bitmap && s.words_addr != 0 && inside(s.words_addr, s.words.size() * sizeof(uint64_t))) {
            return true;
        }
    }
    return false;
}

} // namespace SotcPcPoolDispatch
