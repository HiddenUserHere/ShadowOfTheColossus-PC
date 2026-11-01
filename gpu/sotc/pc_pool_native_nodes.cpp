// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: node-pool pop with refill (pool_node_alloc 0x19660), native application and model. Companion of
// pc_pool_native_nodes.h. Traced in the disassembly `tools/re/eboot_re.py dis 0x19660 -n 250` and the decompile
// out/re/decomp/0001/00019660.c (the decompile agrees with the disassembly on every store below).
//
// Node pool: sentinel S = root+0x1e0 (an element at +0x20 link positions, like the free-VA list), head link
// root+0x1f0 (= S+0x10), tail link root+0x1e8 (= S+8), count i32 root+0x200. An element E = N+0x20 holds
// E+0 = N, E+8 = prev element (N+0x28), E+0x10 = next element (N+0x30), E+0x18 = owner list (N+0x38).
//
// Store list of the pop (P1..P7; the reads are N = [E], E = [root+0x1f0], prev = [E+8], next = [E+0x10]):
//   P1  [N+0x38]     := 0                     0x1968f
//   P2  [N+0x20]     := 0 (E+0 loses N)       0x19697
//   P3  [prev+0x10]  := next                  0x196a7  (prev = S when N is the head: writes root+0x1f0)
//   P4  [next+0x8]   := prev                  0x196af  (next = S when N is the tail: writes root+0x1e8)
//   P5  [N+0x30]     := E                     0x196b3
//   P6  [N+0x28]     := E                     0x196b7
//   P7  [root+0x200] := count - 1 (i32)       0x196c3
// Refill, when the new count is 2 (0x196c9-0x196cc):
//   R0  [DAT_02dfca60] += 0x10000             0x196d2  (bookkeeping global, outside the root; passed in as an address)
//   R1  pool_alloc_region(0,0,0,0x10000,0,8,1) 0x196f8  = the exact hit of ApplyExactHit, node B
//       (B's +0x58 is the size, +0x60 the base; the carve reads both after the call)
// Carve, when size >= 0x70 (0x19722): n = size / 0x70 nodes N_k = base + k*0x70, each of the 17 stores:
//   C1  [N_k+0x00]   := 0
//   C2  [N_k+0x08]   := N_k
//   C3  [N_k+0x10]   := N_k
//   C4  [N_k+0x18]   := 0
//   C5  [N_k+0x28]   := E_k                   (dead: overwritten by C13)
//   C6  [N_k+0x69]   := 0 (byte)
//   C7  [N_k+0x50]   := 0 (16 bytes, one vmovups: N_k+0x50..0x5f)
//   C8  [N_k+0x60]   := 0
//   C9  [N_k+0x40]   := 0
//   C10 [N_k+0x48]   := -1 (i32)
//   C11 [N_k+0x20]   := N_k                   (E_k+0 = N_k)
//   C12 [N_k+0x30]   := S                     (next = S)
//   C13 [N_k+0x28]   := old tail              (old tail = [root+0x1e8] before this node; prev link)
//   C14 [root+0x1e8] := E_k                   (tail)
//   C15 [old tail+0x10] := E_k                (old tail's next)
//   C16 [root+0x200] += 1 (i32)               per node
//   C17 [N_k+0x38]   := S                     (owner list)
// The node pointer rax returns is N (the popped node), not the refill block.
//
// The nested exact hit (R1) is ApplyExactHitTo's store list (pc_pool_native.cpp, W1..W22). Its reads never touch the
// pool list (root+0x1e0..0x207 lie outside the three set headers, which end at root+0x1d8), and its writes touch only
// set fields, free-VA nodes and owner tags, so it may run before the pop stores.
#include "pc_pool_native_nodes.h"

#include <cstring>

namespace SotcPcPoolNodes {
namespace {

using SotcPcPool::Model::Outcome;
using SotcPcPool::Model::Path;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::Prediction;
using SotcPcPool::Model::Request;
using SotcPcPool::Model::Run;
using SotcPcPool::Model::Set;
using SotcPcPoolNative::GuestMem;

constexpr uint64_t kSentinel = 0x1e0;  // root-relative sentinel S
constexpr uint64_t kTailLink = 0x1e8;  // [root+0x1e8] = S+8
constexpr uint64_t kHeadLink = 0x1f0;  // [root+0x1f0] = S+0x10
constexpr uint64_t kCount = 0x200;     // i32
constexpr uint64_t kElem = 0x20;       // element offset inside a node
constexpr uint64_t kNodeSizeField = 0x58;
constexpr uint64_t kNodeBaseField = 0x60;

// Size of the refill block the model predicts and the native code carves (must match kRefillSize).
static_assert(kRefillSize / kStride == 585, "a 64 KiB block carves 585 nodes");

// The run of a set that contains va (free-VA runs of the snapshot), or -1.
int RunIndexAt(const Set& s, uint64_t va) {
    for (size_t i = 0; i < s.free_va.size(); ++i) {
        const Run& r = s.free_va[i];
        if (va >= r.base && va < r.base + r.size) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The write journal of the native pop: performs each store and records it. A failed store returns false and records
// nothing (the caller turns the failure into Refused or Unknown, depending on whether anything was written before).
class Journal {
public:
    explicit Journal(GuestMem& mem) : mem_(mem) {}
    bool Put(uint64_t addr, const void* data, size_t n) {
        if (!mem_.Write(addr, data, n)) {
            return false;
        }
        log_.push_back({addr, n});
        return true;
    }
    bool Put64(uint64_t addr, uint64_t v) { return Put(addr, &v, sizeof v); }
    bool Put32(uint64_t addr, int32_t v) { return Put(addr, &v, sizeof v); }
    bool Put8(uint64_t addr, uint8_t v) { return Put(addr, &v, sizeof v); }
    bool Get64(uint64_t addr, uint64_t* v) { return mem_.Read(addr, v, sizeof *v); }
    bool Get32(uint64_t addr, int32_t* v) { return mem_.Read(addr, v, sizeof *v); }
    std::vector<Write>& log() { return log_; }

private:
    GuestMem& mem_;
    std::vector<Write> log_;
};

} // namespace

bool PopsNode(Path path) { return path == Path::FlexHitNode || path == Path::PrimaryHitNode; }

PopPrediction PredictPop(const Pools& pre, int32_t count_pre) {
    PopPrediction p;
    p.count_pre = count_pre;
    p.count_post = count_pre - 1;
    p.post = pre;
    if (count_pre < 1) {
        p.reason = "pool list empty (count < 1): the game dereferences a null node";
        return p;
    }
    if (count_pre - 1 != kRefillCount) {
        p.ok = true;
        return p;
    }
    // Count 3 -> 2: the refill. Its request is the exact hit of pre; any other outcome refuses the whole pop.
    p.refill = true;
    Request req;
    req.size = kRefillSize;
    req.align = kRefillAlign;
    req.kind = kRefillKind;
    Pools nested_post;
    p.nested = SotcPcPool::Model::Predict(pre, req, nullptr, &nested_post);
    const Path path = p.nested.path;
    if (p.nested.outcome != Outcome::Predicted || (path != Path::FlexHitExact && path != Path::PrimaryHitExact)) {
        p.reason = "refill request is not an exact hit (refused as a whole)";
        return p;
    }
    p.ok = true;
    p.post = nested_post;
    p.block_va = p.nested.va;
    p.block_nodes = kRefillSize / kStride;
    p.count_post = (count_pre - 1) + static_cast<int32_t>(p.block_nodes);
    p.overhead_delta = kRefillSize;
    return p;
}

Prediction PredictNodeCall(const Pools& pre, const Request& req, const SotcPcPool::Model::PhysMap* map, Pools* post,
                           int32_t count_pre, PopPrediction* pop) {
    PopPrediction local;
    PopPrediction& info = pop ? *pop : local;
    info = PopPrediction();
    info.count_pre = count_pre;
    info.count_post = count_pre;

    Prediction outer = SotcPcPool::Model::Predict(pre, req, map, post);
    if (!PopsNode(outer.path)) {
        info.ok = true;
        info.post = *post;
        return outer;
    }
    // The pops of the call: one for the pad node (the start is not the run's base), one for the split remainder (the
    // block keeps more backed bytes than the request). A pop of the second count (count_pre == 4 with both pops) is the
    // second pop's refill, which is not modelled: refused here, so the game runs the call.
    const int run_idx = RunIndexAt(pre.set[outer.slot], outer.va);
    if (run_idx < 0) {
        outer.outcome = Outcome::Unmodelled;
        outer.path = Path::Unmodelled;
        outer.reason = "node path start is not in a free run of the set";
        return outer;
    }
    const Run& run = pre.set[outer.slot].free_va[static_cast<size_t>(run_idx)];
    const uint64_t pad = outer.va - run.base;
    const uint64_t test_backed = pad != 0 ? run.size - pad : run.backed;
    const bool split = test_backed > outer.size;
    const int pops = (pad != 0 ? 1 : 0) + (split ? 1 : 0);
    if (pops == 0) {
        outer.outcome = Outcome::Unmodelled;
        outer.path = Path::Unmodelled;
        outer.reason = "node path without a pad or a split";
        return outer;
    }
    if (pops == 2 && count_pre == kRefillCount + 2) {
        outer.outcome = Outcome::Unmodelled;
        outer.path = Path::Unmodelled;
        outer.reason = "the second pop refills (count 4, pad and split): not modelled";
        return outer;
    }
    // A node-taking path is Predicted by construction (Predict sets the path only on a found run).
    PopPrediction pp = PredictPop(pre, count_pre);
    if (!pp.ok) {
        outer.outcome = Outcome::Unmodelled;
        outer.path = Path::Unmodelled;
        outer.reason = "node pop refused (pool empty, or its refill is not an exact hit)";
        return outer;
    }
    Pools composed = *post;
    if (pp.refill) {
        // The refill takes the run the call would take: the call's run is already chosen on pre, so the game would use
        // a run the refill has given away. Unmodelled.
        const int outer_run = RunIndexAt(pre.set[outer.slot], outer.va);
        const int nested_run = RunIndexAt(pre.set[pp.nested.slot], pp.nested.va);
        if (outer_run >= 0 && nested_run >= 0 && outer.slot == pp.nested.slot &&
            pre.set[outer.slot].free_va[static_cast<size_t>(outer_run)].base ==
                pre.set[pp.nested.slot].free_va[static_cast<size_t>(nested_run)].base) {
            outer.outcome = Outcome::Unmodelled;
            outer.path = Path::Unmodelled;
            outer.reason = "refill takes the call's own run";
            return outer;
        }
        // A split with no pad pops its remainder after the tail, and the remainder is linked before the element that
        // followed the block, read before that pop (0x1a980). A refill that takes that element links the remainder into
        // the block; the writer does what the game does, the model does not predict it.
        if (pops == 1 && pad == 0 && outer_run >= 0 && nested_run >= 0 && outer.slot == pp.nested.slot &&
            nested_run == outer_run + 1) {
            outer.outcome = Outcome::Unmodelled;
            outer.path = Path::Unmodelled;
            outer.reason = "refill takes the run after the split block (the remainder's link)";
            return outer;
        }
        // The call's decision must be the same on the post-refill state.
        Pools redo;
        const Prediction again = SotcPcPool::Model::Predict(pp.post, req, map, &redo);
        if (again.outcome != Outcome::Predicted || again.path != outer.path || again.slot != outer.slot ||
            again.va != outer.va || again.size != outer.size) {
            outer.outcome = Outcome::Unmodelled;
            outer.path = Path::Unmodelled;
            outer.reason = "refill changes the call's decision";
            return outer;
        }
        composed = redo;
        *post = redo;
    }
    info = pp;
    info.count_pre = count_pre;
    info.count_post = count_pre - pops + (pp.refill ? static_cast<int32_t>(pp.block_nodes) : 0);
    info.post = composed;
    return outer;
}

bool PopNode(GuestMem& mem, uint64_t root, const Pools& pre, uint64_t* node_out, std::vector<Write>* writes,
             uint64_t overhead_addr, Status* status, uint64_t keep_base, uint64_t keep_size) {
    if (status) {
        *status = Status::Refused;
    }
    if (node_out) {
        *node_out = 0;
    }
    if (writes) {
        writes->clear();
    }
    if (root == 0) {
        return false;
    }
    Journal j(mem);
    // Everything here that can refuse is a read: the pool list and the node the head names, consistent in both
    // directions (E = N+0x20, prev's next and next's prev point back at E).
    int32_t count = 0;
    uint64_t head = 0;
    if (!j.Get32(root + kCount, &count) || !j.Get64(root + kHeadLink, &head)) {
        return false;
    }
    const uint64_t S = root + kSentinel;
    if (count < 1 || head == S) {
        return false;
    }
    const uint64_t E = head;
    uint64_t N = 0, prev = 0, next = 0, prev_next = 0, next_prev = 0;
    if (!j.Get64(E, &N) || N + kElem != E || !j.Get64(E + 0x8, &prev) || !j.Get64(E + 0x10, &next) ||
        !j.Get64(prev + 0x10, &prev_next) || !j.Get64(next + 0x8, &next_prev) || prev_next != E ||
        next_prev != E) {
        return false;
    }
    // A node and the links around it are pool heap, never the root struct (the sentinel itself is the one allowed link).
    if (SotcPcPoolNative::InPoolRoot(root, N) || (prev != S && SotcPcPoolNative::InPoolRoot(root, prev)) ||
        (next != S && SotcPcPoolNative::InPoolRoot(root, next))) {
        return false;
    }
    // The nodes this pop stores into (P1-P6 write N, prev's next link and next's prev link): the writer's read set.
    SotcPcPoolNative::NoteTouchedNode(N);
    if (prev != S) {
        SotcPcPoolNative::NoteTouchedNode(prev - kElem);
    }
    if (next != S) {
        SotcPcPoolNative::NoteTouchedNode(next - kElem);
    }
    const PopPrediction pp = PredictPop(pre, count);
    if (!pp.ok) {
        return false;
    }
    const bool refill = pp.refill;
    if (refill && overhead_addr == 0) {
        return false;
    }
    uint64_t overhead = 0;
    if (refill && !j.Get64(overhead_addr, &overhead)) {
        return false;
    }

    // The refill's block: probe its ends before anything is written (the carve writes the whole block; a hole inside
    // it is not probed and would leave the state unknown). A block over the caller's own run is refused: the game
    // would give the call's run to the refill.
    if (refill) {
        if (keep_size != 0 && pp.block_va < keep_base + keep_size && keep_base < pp.block_va + kRefillSize) {
            return false;
        }
        uint8_t probe = 0;
        if (!mem.Read(pp.block_va, &probe, 1) || !mem.Read(pp.block_va + kRefillSize - 1, &probe, 1)) {
            return false;
        }
    }

    // The refill's exact hit is planned on the state of this moment (the caller passes pre as it is now) and runs
    // first; its writes go to the journal. A refusal there writes nothing, a failed write in the middle leaves its
    // earlier stores (Unknown). The pop stores and the carve follow in the game's order.
    uint64_t block = 0;
    if (refill) {
        block = SotcPcPoolNative::ApplyExactHitTo(mem, root, pre, pp.nested, kRefillSize, kRefillAlign, kRefillKind, 0,
                                                  &j.log());
        if (block == 0) {
            if (status) {
                *status = j.log().empty() ? Status::Refused : Status::Unknown;
            }
            return false;
        }
    }
    auto fail = [&]() {
        if (status) {
            *status = j.log().empty() ? Status::Refused : Status::Unknown;
        }
        if (writes) {
            *writes = j.log();
        }
        return false;
    };
    // The refill block's base and size as the game reads them (the nested exact hit has just written size).
    uint64_t base = 0, size = 0;
    if (refill) {
        if (!j.Get64(block + kNodeBaseField, &base) || !j.Get64(block + kNodeSizeField, &size)) {
            return fail();
        }
        if (base != pp.block_va || size != kRefillSize) {
            return fail();
        }
    }

    // P1..P7: the pop.
    if (!j.Put64(N + 0x38, 0) || !j.Put64(N + 0x20, 0) || !j.Put64(prev + 0x10, next) || !j.Put64(next + 0x8, prev) ||
        !j.Put64(N + 0x30, E) || !j.Put64(N + 0x28, E) || !j.Put32(root + kCount, count - 1)) {
        return fail();
    }
    if (refill) {
        // R0: the bookkeeping global grows by the block (the game does this before its nested call; here it follows
        // the exact hit, because the exact hit must be able to refuse with nothing written).
        if (!j.Put64(overhead_addr, overhead + kRefillSize)) {
            return fail();
        }
        // Carve (0x19722-0x197b3): n nodes from the block base; each pushes at the pool list tail.
        int32_t cnt = count - 1;
        uint64_t tail = 0;
        if (!j.Get64(root + kTailLink, &tail)) {
            return fail();
        }
        const uint64_t n = size / kStride;
        for (uint64_t k = 0; k < n; ++k) {
            const uint64_t Nk = base + k * kStride;
            const uint64_t Ek = Nk + kElem;
            const uint8_t zero8 = 0;
            const uint8_t zero16[16] = {};
            if (!j.Put64(Nk + 0x00, 0) ||                       // C1
                !j.Put64(Nk + 0x08, Nk) ||                      // C2
                !j.Put64(Nk + 0x10, Nk) ||                      // C3
                !j.Put64(Nk + 0x18, 0) ||                       // C4
                !j.Put64(Nk + 0x28, Ek) ||                      // C5 (dead)
                !j.Put8(Nk + 0x69, zero8) ||                    // C6
                !j.Put(Nk + 0x50, zero16, sizeof zero16) ||     // C7
                !j.Put64(Nk + 0x60, 0) ||                       // C8
                !j.Put64(Nk + 0x40, 0) ||                       // C9
                !j.Put32(Nk + 0x48, -1) ||                      // C10
                !j.Put64(Nk + 0x20, Nk) ||                      // C11
                !j.Put64(Nk + 0x30, S) ||                       // C12
                !j.Put64(Nk + 0x28, tail) ||                    // C13
                !j.Put64(root + kTailLink, Ek) ||               // C14
                !j.Put64(tail + 0x10, Ek) ||                    // C15
                !j.Put32(root + kCount, cnt + 1) ||             // C16
                !j.Put64(Nk + 0x38, S)) {                       // C17
                return fail();
            }
            ++cnt;
            tail = Ek;
        }
    }

    if (node_out) {
        *node_out = N;
    }
    if (writes) {
        *writes = j.log();
    }
    if (status) {
        *status = Status::Popped;
    }
    return true;
}

} // namespace SotcPcPoolNodes
