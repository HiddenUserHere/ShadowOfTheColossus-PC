// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native fresh-VA path of the game's pool allocator (eboot 0x19e00, mode 0, after the free-run search misses).
// See pc_pool_native_fresh.h for the order, the atomicity contract and the sticky node. The writes of the reserve-map
// step and of the POOL_FIX case come from pc_pool_native_reserve.cpp (ApplyReserveMap, ApplyPoolFix), run on the same
// overlay, so their guest checks see the state the fresh prefix and the earlier runs left.
#include "pc_pool_native_fresh.h"

#include <algorithm>
#include <array>
#include <map>

#include "pc_pool_native_reserve.h"

namespace SotcPcPoolNativeFresh {
namespace {

using SotcPcPool::Model::Path;
using SotcPcPool::Model::PhysMap;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::Prediction;
using SotcPcPool::Model::Set;
using SotcPcPoolNative::GuestMem;
namespace NR = SotcPcPoolNativeReserve;

constexpr uint64_t kPage = 0x10000;
constexpr uint64_t kMiB = 0x100000;
constexpr uint64_t kSetStride = 0xa0;   // S0 = root, S1 = root+0xa0, S2 = root+0x140
constexpr uint64_t kSetSentinel = 0x28; // free-VA list sentinel inside a set (S+0x28)
constexpr uint64_t kNodeSentinel = 0x1e0;
constexpr uint64_t kNodeHead = 0x1f0;
constexpr uint64_t kNodeCount = 0x200;
constexpr uint64_t kBatchCount = 0x2102f0;
constexpr uint64_t kBatchEntries = 0x2102f8;
constexpr uint64_t kOwnerMiB = 0x111908; // owner-tag table, one byte per 1 MiB unit (index = VA >> 20)
constexpr int kBatchMax = 64;
constexpr uint8_t kProtMap = 0x33;
constexpr uint64_t kMaxUnits = 4096;     // sanity bound on the owner-tag span (4 GiB)
constexpr uint64_t kMaxBitmapWords = uint64_t{1} << 20;
constexpr int kMaxLoop = 1 << 16;        // bound on page-loop and POOL_FIX iterations

Refusal g_refusal = Refusal::None;
int32_t g_flush_error = 0;
std::vector<std::pair<uint64_t, size_t>> g_writes;
std::array<uint64_t, static_cast<size_t>(Refusal::kCount)> g_refusal_counts{};
uint64_t g_performed = 0;

bool IsPow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

uint64_t AlignUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

template <typename T>
bool GetV(GuestMem& m, uint64_t addr, T* out) {
    return m.Read(addr, out, sizeof(T));
}

template <typename T>
bool PutV(GuestMem& m, uint64_t addr, T value) {
    return m.Write(addr, &value, sizeof(T));
}

// The guest as the page loop sees it: reads come from the base memory with the recorded writes applied on top; writes
// are recorded in order and reach the base only at the commit. In probe mode a write first checks that its target
// is readable in the base, so the commit has fewer failures to find.
struct Op {
    bool flush = false; // the batch flush (vmem_batch_map) at this point of the sequence
    uint64_t addr = 0;
    std::vector<uint8_t> data;
};

class FreshOverlay final : public GuestMem {
public:
    FreshOverlay(GuestMem& base, bool probe) : base_(base), probe_(probe) {}

    bool Read(uint64_t addr, void* out, size_t n) override {
        if (!base_.Read(addr, out, n)) {
            return false;
        }
        auto* o = static_cast<uint8_t*>(out);
        for (auto it = bytes_.lower_bound(addr); it != bytes_.end() && it->first < addr + n; ++it) {
            o[it->first - addr] = it->second;
        }
        return true;
    }

    bool Write(uint64_t addr, const void* in, size_t n) override {
        const auto* src = static_cast<const uint8_t*>(in);
        if (probe_) {
            std::vector<uint8_t> scratch(n);
            if (!base_.Read(addr, scratch.data(), n)) {
                return false;
            }
        }
        Op op;
        op.addr = addr;
        op.data.assign(src, src + n);
        ops_.push_back(std::move(op));
        for (size_t i = 0; i < n; ++i) {
            bytes_[addr + i] = src[i];
        }
        return true;
    }

    void Flush() {
        Op op;
        op.flush = true;
        ops_.push_back(std::move(op));
    }

    const std::vector<Op>& ops() const { return ops_; }

private:
    GuestMem& base_;
    bool probe_;
    std::map<uint64_t, uint8_t> bytes_;
    std::vector<Op> ops_;
};

// What one run of the sequence needs from the call.
struct Ctx {
    uint64_t root = 0;
    const Pools* pre = nullptr;
    const PhysMap* map = nullptr;
    int P = 1;
    int F = 2;
    uint64_t va = 0;   ///< the block VA (the dry run uses a placeholder: the plan does not depend on it)
    uint64_t size = 0;
    uint64_t obj = 0;
    uint8_t tag = 0;   ///< obj+0x1c
    uint64_t phys_base = 0;
};

struct PlanRun {
    int set = 0;
    uint64_t first = 0;
    uint64_t pages = 0;
    int32_t protect = -1; ///< op 4 type when the run came from the spill set (-1: no op 4)
};

// What one run decided: the path, the runs, the reserve steps (base, bytes), the queue entries, the node.
struct Plan {
    Path path = Path::Unmodelled;
    std::vector<PlanRun> runs;
    std::vector<std::pair<uint64_t, uint64_t>> unmapped;
    std::vector<SotcPcPoolNativeFresh::BatchEntry> entries;
    uint64_t node = 0;
};

// The reserve-map step and POOL_FIX return the same statuses. Anything but Ok leaves the overlay as it was.
Refusal FromReserve(NR::Status st) {
    switch (st) {
    case NR::Status::Ok:
        return Refusal::None;
    case NR::Status::NotApplicable:
        return Refusal::GuestMismatch;
    case NR::Status::UnknownMap:
        return Refusal::UnknownMap;
    case NR::Status::Refused:
        return Refusal::GuestMismatch;
    case NR::Status::NoNode:
        return Refusal::NoNode;
    case NR::Status::NextSet:
        return Refusal::NextSet;
    case NR::Status::QueueFull:
        return Refusal::Batch;
    case NR::Status::WriteFailed:
        return Refusal::GuestUnmapped;
    }
    return Refusal::GuestMismatch;
}

// One batch queue entry at root+0x2102f8 + 32 * count, then the count (0x1a4df-0x1a553). The game flushes at 64
// entries before it writes the 65th; the native path refuses that case instead (Batch). The reserved bytes 0x1a-0x1b
// are not written, as the game does not.
Refusal QueueOp(FreshOverlay& ov, uint64_t root, Plan* plan, uint64_t va, uint64_t offset, uint64_t length, uint8_t type,
                uint32_t op) {
    int32_t count = 0;
    if (!GetV(ov, root + kBatchCount, &count)) {
        return Refusal::GuestUnmapped;
    }
    if (count < 0 || count >= kBatchMax) {
        return Refusal::Batch;
    }
    const uint64_t e = root + kBatchEntries + 32 * static_cast<uint64_t>(count);
    if (!PutV(ov, e, va) || !PutV(ov, e + 0x8, offset) || !PutV(ov, e + 0x10, length) ||
        !PutV<uint8_t>(ov, e + 0x18, kProtMap) || !PutV<uint8_t>(ov, e + 0x19, type) ||
        !PutV<uint32_t>(ov, e + 0x1c, op) || !PutV<int32_t>(ov, root + kBatchCount, count + 1)) {
        return Refusal::GuestUnmapped;
    }
    SotcPcPoolNativeFresh::BatchEntry be{va, offset, length, kProtMap, type, 0, op};
    plan->entries.push_back(be);
    return Refusal::None;
}

// The reserve-map step queued one unmap entry (op 1, prot 0, type 0) in the guest queue (ApplyReserveMap writes it).
// The flush applies that entry too, so the host gets it, in the same place of the queue.
void AddReserveOp(Plan* plan, const NR::ReserveResult& res) {
    plan->unmapped.emplace_back(res.node_base, res.consumed);
    plan->entries.push_back(SotcPcPoolNativeFresh::BatchEntry{res.node_base, 0, res.consumed, 0, 0, 0, 1});
}

// One run of the whole sequence of 0x19e00 (mode 0, fresh path), on the overlay. Order as in the header. The page loop
// reads the set state from the overlay each time; the reserve steps get `work`, the snapshot kept equal to the overlay
// (the set fields the steps compare), updated after each step.
Refusal Simulate(FreshOverlay& ov, const Ctx& c, Plan* plan) {
    const uint64_t root = c.root;
    const int P = c.P;
    const int F = c.F;
    const uint64_t SP = root + kSetStride * static_cast<uint64_t>(P);
    const uint64_t SF = root + kSetStride * static_cast<uint64_t>(F);
    const uint64_t pages = c.size / kPage;
    Pools work = *c.pre;

    // pool_node_alloc (0x19660): pop the head element; reads first.
    uint64_t head = 0;
    uint64_t node = 0;
    uint64_t next = 0; // node+0x30: E.next
    uint64_t prev = 0; // node+0x28: E.prev
    uint64_t node_backed = 0;
    uint64_t probe = 0;
    int32_t pool_count = 0;
    if (!GetV(ov, root + kNodeHead, &head)) {
        return Refusal::GuestUnmapped;
    }
    if (head == root + kNodeSentinel) {
        return Refusal::NodePool;
    }
    if (!GetV(ov, head, &node) || node == 0 || !GetV(ov, node + 0x30, &next) || !GetV(ov, node + 0x28, &prev) ||
        !GetV(ov, root + kNodeCount, &pool_count)) {
        return Refusal::GuestUnmapped;
    }
    if (pool_count - 1 < 3) {
        return Refusal::NodePool; // the pop would leave the refill (a nested 0x19e00) to run
    }
    if (!GetV(ov, node + 0x50, &node_backed) || !GetV(ov, prev + 0x10, &probe) || !GetV(ov, next + 0x8, &probe)) {
        return Refusal::GuestUnmapped;
    }
    // Region list (0x1a0f1): the old tail is [S+8] (the sentinel S when the list is empty); its next link is written.
    uint64_t old_tail = 0;
    int32_t region = 0;
    uint64_t alloc = 0;
    uint64_t alloc_peak = 0;
    if (!GetV(ov, SP + 0x8, &old_tail) || old_tail == 0 || !GetV(ov, old_tail + 0x10, &probe) ||
        !GetV(ov, SP + 0x20, &region) || !GetV(ov, SP + 0x80, &alloc) || !GetV(ov, SP + 0x88, &alloc_peak)) {
        return Refusal::GuestUnmapped;
    }

    // ---- writes, in the game's order ----
    // pool_node_alloc: unlink the head element (0x1968b-0x196b7), count -= 1 (0x196bb-0x196c3).
    if (!PutV<uint64_t>(ov, node + 0x38, 0) || !PutV<uint64_t>(ov, node + 0x20, 0) || !PutV(ov, prev + 0x10, next) ||
        !PutV(ov, next + 0x8, prev) || !PutV(ov, node + 0x30, node + 0x20) || !PutV(ov, node + 0x28, node + 0x20) ||
        !PutV<int32_t>(ov, root + kNodeCount, pool_count - 1)) {
        return Refusal::GuestUnmapped;
    }
    // Node fields (0x19fce-0x19fed): pool id, raw size, VA, in-use flag, owner, owner list length - 1.
    if (!PutV<uint8_t>(ov, node + 0x68, static_cast<uint8_t>(P)) || !PutV(ov, node + 0x58, c.size) ||
        !PutV(ov, node + 0x60, c.va) || !PutV<uint8_t>(ov, node + 0x69, 1) || !PutV(ov, node + 0x40, c.obj) ||
        !PutV<int32_t>(ov, node + 0x48, -1)) {
        return Refusal::GuestUnmapped;
    }
    // Owner tags (0x1a01a-0x1a0ef, pool id != 0): one byte per 1 MiB unit of [va, va + size).
    const uint64_t units = ((c.va + c.size - 1) >> 20) - (c.va >> 20) + 1;
    if (units > kMaxUnits) {
        return Refusal::Size;
    }
    for (uint64_t i = 0; i < units; ++i) {
        if (!PutV<uint8_t>(ov, root + kOwnerMiB + (c.va >> 20) + i, c.tag)) {
            return Refusal::GuestUnmapped;
        }
    }
    // Region link (0x1a0f1-0x1a11a): the node's element is the new tail; the old tail's next points at it; the
    // sentinel's +0x10 is the first element, so an empty list gets the node there.
    const uint64_t E = node + 0x20;
    if (!PutV(ov, node + 0x20, node) || !PutV(ov, node + 0x30, SP) || !PutV(ov, node + 0x28, old_tail) ||
        !PutV(ov, SP + 0x8, E) || !PutV(ov, old_tail + 0x10, E) || !PutV<int32_t>(ov, SP + 0x20, region + 1) ||
        !PutV(ov, node + 0x38, SP)) {
        return Refusal::GuestUnmapped;
    }
    work.set[P].region_count = region + 1;
    // Allocated bytes and peak of P (0x1a123-0x1a148).
    const uint64_t alloc_now = alloc + c.size;
    if (!PutV(ov, SP + 0x80, alloc_now)) {
        return Refusal::GuestUnmapped;
    }
    if (alloc_now > alloc_peak && !PutV(ov, SP + 0x88, alloc_now)) {
        return Refusal::GuestUnmapped;
    }
    work.set[P].alloc_bytes = alloc_now;

    // ---- the page loop (0x1a1b0-0x1a627) ----
    int cur = P;
    bool sw = false;             // the loop switched to F (0x1a275)
    int32_t protect = -1;        // op 4 type of the spill set's runs ([rbp-0x44])
    bool used_reserve = false;   // a reserve-map step ran
    bool poolfix = false;        // the POOL_FIX patch ran
    uint64_t rem = pages;
    uint64_t cursor = c.va;
    uint64_t sticky = 0;         // rax of the loop: the node the last reserve step returned (0 when used up)
    for (int guard = 0; rem > 0; ++guard) {
        if (guard >= kMaxLoop) {
            return Refusal::LoopBound;
        }
        uint64_t S = root + kSetStride * static_cast<uint64_t>(cur);
        int32_t cached = 0;
        if (!GetV(ov, S + 0x74, &cached)) {
            return Refusal::GuestUnmapped;
        }
        if (cached == 0) {
            // 0x1a1e0-0x1a1e8: P without free VA goes to the spill set (0x1a275). Its cached count is read there.
            uint64_t p_head = 0;
            if (!GetV(ov, SP + 0x38, &p_head)) {
                return Refusal::GuestUnmapped;
            }
            if (cur == P && p_head == SP + kSetSentinel) {
                int32_t fid = 0;
                if (!GetV(ov, SF + 0x78, &fid)) {
                    return Refusal::GuestUnmapped;
                }
                cur = F;
                sw = true;
                protect = fid != 2 ? 3 : 0; // (F.id != 2) * 3 (0x1a285-0x1a298)
                S = SF;
                if (!GetV(ov, S + 0x74, &cached)) {
                    return Refusal::GuestUnmapped;
                }
            }
            if (cached == 0) {
                // The reserve-map step (0x1a1f1, 0x1a2c0-0x1a2f0) on the sticky node or the searched one.
                if (c.map == nullptr) {
                    return Refusal::UnknownMap;
                }
                NR::ReserveResult res;
                const Refusal r = FromReserve(NR::ApplyReserveMap(ov, root, work, cur, rem, *c.map, &res, sticky));
                if (r != Refusal::None) {
                    return r;
                }
                used_reserve = true;
                AddReserveOp(plan, res);
                work.set[cur] = res.post;
                sticky = res.returned;
            }
        }

        // The scan (0x1a2f8-0x1a357). No set bit: the POOL_FIX patch (0x1a321), then rescan.
        uint64_t b = 0;
        int32_t nwords = 0;
        uint64_t words_addr = 0;
        uint32_t cap = 0;
        std::vector<uint64_t> words;
        for (int scan = 0;; ++scan) {
            if (scan >= kMaxLoop) {
                return Refusal::LoopBound;
            }
            if (!GetV(ov, S + 0x58, &nwords) || !GetV(ov, S + 0x50, &words_addr) || !GetV(ov, S + 0x60, &cap)) {
                return Refusal::GuestUnmapped;
            }
            if (nwords < 0 || static_cast<uint64_t>(nwords) > kMaxBitmapWords) {
                return Refusal::GuestMismatch;
            }
            if (nwords == 0) {
                return Refusal::PageCap; // 0x1a30a: no words, r12 stays -1 (the out-of-bounds path, not the patch)
            }
            words.assign(static_cast<size_t>(nwords), 0);
            if (!ov.Read(words_addr, words.data(), static_cast<size_t>(nwords) * 8)) {
                return Refusal::GuestUnmapped;
            }
            size_t w = 0;
            while (w < words.size() && words[w] == 0) {
                ++w;
            }
            if (w == words.size()) {
                // POOL_FIX (0x1a321 patched): cached := 0, rax := 0, the reserve-map step with no sticky node.
                poolfix = true;
                if (c.map == nullptr) {
                    return Refusal::UnknownMap;
                }
                NR::ReserveResult res;
                const Refusal r = FromReserve(NR::ApplyPoolFix(ov, root, work, cur, rem, *c.map, &res));
                if (r != Refusal::None) {
                    return r;
                }
                AddReserveOp(plan, res);
                work.set[cur] = res.post;
                sticky = res.returned;
                continue;
            }
            b = static_cast<uint64_t>(w) * 64 + static_cast<uint64_t>(__builtin_ctzll(words[w]));
            if (b >= cap) {
                return Refusal::PageCap; // a set bit at or past the cap: the game takes r12 = -1
            }
            break;
        }

        // The run (0x1a357-0x1a4b1): the lowest free bit and the set bits after it, up to the first zero bit, the cap
        // and the remaining pages.
        const uint64_t nbits = static_cast<uint64_t>(nwords) * 64;
        const uint64_t limit = std::min<uint64_t>(cap, nbits);
        uint64_t end = cap;
        for (uint64_t i = b + 1; i < limit; ++i) {
            if (((words[static_cast<size_t>(i / 64)] >> (i % 64)) & 1u) == 0) {
                end = i;
                break;
            }
        }
        if (end > nbits) {
            return Refusal::RunPastBitmap;
        }
        const uint64_t len = std::min<uint64_t>(end - b, rem);
        if (len > 0xffff) {
            return Refusal::Size; // the game keeps the run length in 16 bits (and r13, 0xffff)
        }
        for (uint64_t i = b; i < b + len; ++i) {
            words[static_cast<size_t>(i / 64)] &= ~(uint64_t{1} << (i % 64));
        }
        // Words (0x1a440-0x1a4aa), then cached -= len (0x1a4c6).
        for (uint64_t w = b / 64; w <= (b + len - 1) / 64; ++w) {
            if (!PutV(ov, words_addr + 8 * w, words[static_cast<size_t>(w)])) {
                return Refusal::GuestUnmapped;
            }
        }
        int32_t cached_now = 0;
        if (!GetV(ov, S + 0x74, &cached_now)) {
            return Refusal::GuestUnmapped;
        }
        const int32_t cached_after = cached_now - static_cast<int32_t>(len);
        if (!PutV(ov, S + 0x74, cached_after)) {
            return Refusal::GuestUnmapped;
        }
        // Batch ops: op 0 (map) for the run, op 4 (protect with type) after it for a spill run (0x1a4df-0x1a5e0).
        const uint64_t bytes = len * kPage;
        const uint64_t phys = c.phys_base + b * kPage;
        Refusal r = QueueOp(ov, root, plan, cursor, phys, bytes, 0, 0);
        if (r != Refusal::None) {
            return r;
        }
        if (sw) {
            r = QueueOp(ov, root, plan, cursor, phys, bytes, static_cast<uint8_t>(protect), 4);
            if (r != Refusal::None) {
                return r;
            }
        }
        // Node backed (0x1a5e6), set backed and its peak (0x1a5ea-0x1a608), of the set the run came from.
        uint64_t node_now = 0;
        if (!GetV(ov, node + 0x50, &node_now) || !PutV(ov, node + 0x50, node_now + bytes)) {
            return Refusal::GuestUnmapped;
        }
        uint64_t set_backed = 0;
        uint64_t set_peak = 0;
        if (!GetV(ov, S + 0x90, &set_backed) || !GetV(ov, S + 0x98, &set_peak)) {
            return Refusal::GuestUnmapped;
        }
        const uint64_t backed_now = set_backed + bytes;
        if (!PutV(ov, S + 0x90, backed_now)) {
            return Refusal::GuestUnmapped;
        }
        if (backed_now > set_peak && !PutV(ov, S + 0x98, backed_now)) {
            return Refusal::GuestUnmapped;
        }
        work.set[cur].words = words;
        work.set[cur].cached_phys = cached_after;
        work.set[cur].backed_bytes = backed_now;

        plan->runs.push_back(PlanRun{cur, static_cast<uint64_t>(b), len, sw ? protect : -1});
        cursor += bytes;
        rem -= len;
    }

    // vmem_batch_map (0x1af80) at 0x1a62d: the host applies the entries, then the count is reset (0x1b0ac).
    plan->node = node;
    if (poolfix) {
        plan->path = Path::FreshPoolFix;
    } else if (sw) {
        plan->path = Path::FreshSpill;
    } else if (used_reserve) {
        plan->path = Path::FreshReserve;
    } else {
        plan->path = Path::FreshPlain;
    }
    ov.Flush();
    if (!PutV<int32_t>(ov, root + kBatchCount, 0)) {
        return Refusal::GuestUnmapped;
    }
    return Refusal::None;
}

// The set's header and bitmap as the snapshot describes them. Any difference means the prediction is not about this
// state. Reads only.
Refusal CheckSet(GuestMem& mem, uint64_t root, int index, const Set& pre_set) {
    const uint64_t a = root + kSetStride * static_cast<uint64_t>(index);
    int32_t id = 0;
    int32_t region = 0;
    int32_t cached = 0;
    int32_t nwords = 0;
    uint32_t cap = 0;
    uint64_t head = 0;
    uint64_t words_addr = 0;
    uint64_t alloc = 0;
    uint64_t backed = 0;
    if (!GetV(mem, a + 0x78, &id) || !GetV(mem, a + 0x20, &region) || !GetV(mem, a + 0x74, &cached) ||
        !GetV(mem, a + 0x38, &head) || !GetV(mem, a + 0x50, &words_addr) || !GetV(mem, a + 0x58, &nwords) ||
        !GetV(mem, a + 0x60, &cap) || !GetV(mem, a + 0x80, &alloc) || !GetV(mem, a + 0x90, &backed)) {
        return Refusal::GuestUnmapped;
    }
    if (id != index) {
        return Refusal::GuestMismatch;
    }
    if (nwords < 0 || static_cast<uint64_t>(nwords) > kMaxBitmapWords) {
        return Refusal::GuestMismatch;
    }
    std::vector<uint64_t> words(static_cast<size_t>(nwords), 0);
    if (nwords > 0 && !mem.Read(words_addr, words.data(), static_cast<size_t>(nwords) * 8)) {
        return Refusal::GuestUnmapped;
    }
    const bool free_empty = head == a + kSetSentinel;
    if (region != pre_set.region_count || cached != pre_set.cached_phys || alloc != pre_set.alloc_bytes ||
        backed != pre_set.backed_bytes || free_empty != pre_set.free_va.empty() || cap != pre_set.page_cap ||
        words != pre_set.words) {
        return Refusal::GuestMismatch;
    }
    return Refusal::None;
}

// The prediction's runs and reserve steps against the plan.
bool SameShape(const Plan& plan, const Prediction& pred) {
    if (plan.runs.size() != pred.runs.size() || plan.unmapped.size() != pred.unmapped.size()) {
        return false;
    }
    for (size_t i = 0; i < plan.runs.size(); ++i) {
        const SotcPcPool::Model::PhysRun& pr = pred.runs[i];
        const PlanRun& run = plan.runs[i];
        if (pr.set != run.set || pr.first_page != run.first || pr.pages != run.pages ||
            pr.protect_type != run.protect) {
            return false;
        }
    }
    for (size_t i = 0; i < plan.unmapped.size(); ++i) {
        if (pred.unmapped[i].base != plan.unmapped[i].first || pred.unmapped[i].size != plan.unmapped[i].second) {
            return false;
        }
    }
    return true;
}

// Two runs of the same state (dry run, real run) must give the same plan; only the VA differs.
bool SamePlan(const Plan& a, const Plan& b) {
    if (a.path != b.path || a.node != b.node || a.entries.size() != b.entries.size() ||
        a.runs.size() != b.runs.size() || a.unmapped != b.unmapped) {
        return false;
    }
    for (size_t i = 0; i < a.runs.size(); ++i) {
        if (a.runs[i].set != b.runs[i].set || a.runs[i].first != b.runs[i].first ||
            a.runs[i].pages != b.runs[i].pages || a.runs[i].protect != b.runs[i].protect) {
            return false;
        }
    }
    return true;
}

// Shared body of the four entry points. want is the path the entry point performs.
uint64_t ApplyFresh(Path want, GuestMem& mem, FreshHost& host, uint64_t root, const Pools& pre,
                    const Prediction& pred, uint64_t size, uint32_t align, int kind, uint64_t obj, uint64_t phys_base,
                    const PhysMap* map) {
    g_writes.clear();
    g_refusal = Refusal::None;
    g_flush_error = 0;
    auto refuse = [](Refusal r) -> uint64_t {
        g_refusal = r;
        ++g_refusal_counts[static_cast<size_t>(r)];
        return 0;
    };
    if (root == 0 || size == 0 || align == 0) {
        return refuse(Refusal::Args);
    }
    if (size % kPage != 0) {
        return refuse(Refusal::Size);
    }
    if (pred.outcome != SotcPcPool::Model::Outcome::Predicted || !pred.fresh || pred.va != 0 || pred.size != size) {
        return refuse(Refusal::Prediction);
    }
    if (pred.path != want) {
        return refuse(Refusal::Route);
    }
    // pool_alloc_region rounds align up to 64 KiB, then the reserve takes max(1 MiB, align64) (0x19f8a).
    const uint64_t align64 = AlignUp(align, kPage);
    if (align64 > 0x80000000u) {
        return refuse(Refusal::Args);
    }
    const uint64_t ra = std::max<uint64_t>(kMiB, align64);
    if (!IsPow2(ra) || pred.va_align != ra) {
        return refuse(Refusal::Prediction);
    }
    if (size > kMaxUnits * kMiB) {
        return refuse(Refusal::Size);
    }
    const int P = kind == 1 ? 1 : 2; // (kind == 1) selects S1 (0x19e8c)
    const int F = 3 - P;

    // Phase A: reads only. The snapshot must describe P and F, the batch queue must be empty, and obj+0x1c is read.
    Refusal r = CheckSet(mem, root, P, pre.set[P]);
    if (r != Refusal::None) {
        return refuse(r);
    }
    r = CheckSet(mem, root, F, pre.set[F]);
    if (r != Refusal::None) {
        return refuse(r);
    }
    int32_t batch = 0;
    if (!GetV(mem, root + kBatchCount, &batch)) {
        return refuse(Refusal::GuestUnmapped);
    }
    if (batch != 0) {
        return refuse(Refusal::Batch);
    }
    uint8_t tag = 0;
    if (obj != 0 && !GetV(mem, obj + 0x1c, &tag)) {
        return refuse(Refusal::GuestUnmapped);
    }

    Ctx c;
    c.root = root;
    c.pre = &pre;
    c.map = map;
    c.P = P;
    c.F = F;
    c.size = size;
    c.obj = obj;
    c.tag = tag;
    c.phys_base = phys_base;

    // Dry run with a placeholder VA: every refusal is decided here, before the host is called. The reserve counters
    // are restored, so the real run is the one counted.
    const NR::ReserveCounters saved = NR::SnapshotCounters();
    FreshOverlay dry(mem, false);
    Plan dplan;
    c.va = ra;
    r = Simulate(dry, c, &dplan);
    NR::RestoreCounters(saved);
    if (r != Refusal::None) {
        return refuse(r);
    }
    if (dplan.path != want) {
        return refuse(Refusal::Route);
    }
    if (!SameShape(dplan, pred)) {
        return refuse(Refusal::Prediction);
    }

    // The VA (sceKernelReserveVirtualRange, 0x19f8a-0x19fb2). A failure writes nothing (0x1a636).
    const uint64_t va = host.ReserveVirtual(size, ra);
    if (va == 0) {
        return refuse(Refusal::HostReserve);
    }
    if (va % ra != 0) {
        return refuse(Refusal::HostVa);
    }

    // Real run with that VA, on a fresh overlay that probes its writes against the guest.
    c.va = va;
    FreshOverlay real(mem, true);
    Plan rplan;
    r = Simulate(real, c, &rplan);
    if (r != Refusal::None) {
        return refuse(r);
    }
    if (!SamePlan(dplan, rplan)) {
        return refuse(Refusal::GuestMismatch);
    }

    // Commit: the recorded writes in order, the batch flush where the game makes it, then the guest is final.
    for (const Op& op : real.ops()) {
        if (op.flush) {
            g_flush_error = host.BatchMap(rplan.entries.data(), rplan.entries.size());
            continue;
        }
        if (!mem.Write(op.addr, op.data.data(), op.data.size())) {
            return refuse(Refusal::GuestWrite);
        }
        g_writes.emplace_back(op.addr, op.data.size());
    }
    ++g_performed;
    return rplan.node;
}

} // namespace

const char* RefusalName(Refusal refusal) {
    static const char* const kNames[static_cast<size_t>(Refusal::kCount)] = {
        "none",           "args",           "size",           "route",           "prediction",
        "node-pool",      "batch",          "no-node",        "next-set",        "page-cap",
        "run-past-bitmap", "unknown-map",   "loop-bound",     "guest-mismatch",  "guest-unmapped",
        "host-reserve",   "host-va",        "guest-write",
    };
    const size_t i = static_cast<size_t>(refusal);
    return i < static_cast<size_t>(Refusal::kCount) ? kNames[i] : "?";
}

Refusal LastRefusal() {
    return g_refusal;
}

int32_t LastFlushError() {
    return g_flush_error;
}

std::vector<std::pair<uint64_t, size_t>> LastWriteSet() {
    return g_writes;
}

uint64_t RefusalCount(Refusal refusal) {
    return g_refusal_counts[static_cast<size_t>(refusal)];
}

uint64_t PerformedCount() {
    return g_performed;
}

void ResetCounts() {
    g_refusal_counts.fill(0);
    g_performed = 0;
}

uint64_t ApplyFreshPlain(GuestMem& mem, FreshHost& host, uint64_t root, const Pools& pre, const Prediction& pred,
                         uint64_t size, uint32_t align, int kind, uint64_t obj, uint64_t phys_base,
                         const PhysMap* map) {
    return ApplyFresh(Path::FreshPlain, mem, host, root, pre, pred, size, align, kind, obj, phys_base, map);
}

uint64_t ApplyFreshReserve(GuestMem& mem, FreshHost& host, uint64_t root, const Pools& pre, const Prediction& pred,
                           uint64_t size, uint32_t align, int kind, uint64_t obj, uint64_t phys_base,
                           const PhysMap* map) {
    return ApplyFresh(Path::FreshReserve, mem, host, root, pre, pred, size, align, kind, obj, phys_base, map);
}

uint64_t ApplyFreshSpill(GuestMem& mem, FreshHost& host, uint64_t root, const Pools& pre, const Prediction& pred,
                         uint64_t size, uint32_t align, int kind, uint64_t obj, uint64_t phys_base,
                         const PhysMap* map) {
    return ApplyFresh(Path::FreshSpill, mem, host, root, pre, pred, size, align, kind, obj, phys_base, map);
}

uint64_t ApplyFreshPoolFix(GuestMem& mem, FreshHost& host, uint64_t root, const Pools& pre, const Prediction& pred,
                           uint64_t size, uint32_t align, int kind, uint64_t obj, uint64_t phys_base,
                           const PhysMap* map) {
    return ApplyFresh(Path::FreshPoolFix, mem, host, root, pre, pred, size, align, kind, obj, phys_base, map);
}

} // namespace SotcPcPoolNativeFresh
