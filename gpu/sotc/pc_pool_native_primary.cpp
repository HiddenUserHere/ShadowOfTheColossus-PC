// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native primary-set node hit (pc_pool_native_primary.h). The store order follows the disassembly of
// pool_alloc_region (0x19e00), pool_take_free_run (0x1a800) and pool_node_alloc (0x19660). Every value a store
// needs is read from the call's overlay at the point the game reads it, so a store that changes a later read (the
// pad insertion before the exact tail, the tail before the split, a refill's carve before the second pop) is seen
// the same way.
//
// Guards, all before the commit (a refusal writes nothing):
//   - the set headers, region lists and free-VA lists match the snapshot (as pc_pool_native.cpp)
//   - the first free-VA run in list order that fits (0x1a86e-0x1a8a9) is a node path: pad != 0 or a split, and its
//     aligned start is pred.va
//   - each node pop is SotcPcPoolNodes::PopNode (through PopNodeOn), which checks the list shape and the count and
//     refills when a pop takes the count from 3 to 2; a refill that would take the call's own run is refused

#include "pc_pool_native_primary.h"

#include <cstring>

#include "pc_pool_native_nodes.h"

namespace SotcPcPoolNative {
namespace {

using SotcPcPool::Model::Outcome;
using SotcPcPool::Model::Path;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::Prediction;
using SotcPcPool::Model::Run;
using SotcPcPool::Model::Set;

constexpr uint64_t kPage = 0x10000;
constexpr uint64_t kSetOffset[3] = {0x000, 0x0a0, 0x140}; // S0 = root, S1 = root+0xa0, S2 = root+0x140

// Set fields, offsets from S.
constexpr uint64_t kRegionHead = 0x10;   // u64, region list head (the sentinel is S itself)
constexpr uint64_t kRegionCount = 0x20;  // i32
constexpr uint64_t kFreeSentinel = 0x28; // free-VA sentinel F = S+0x28; F+8 = S+0x30 (tail), F+0x10 = S+0x38 (head)
constexpr uint64_t kFreeTail = 0x30;     // u64
constexpr uint64_t kFreeHead = 0x38;     // u64
constexpr uint64_t kFreeCount = 0x48;    // i32
constexpr uint64_t kFreePages = 0x70;    // i32
constexpr uint64_t kCached = 0x74;       // i32
constexpr uint64_t kPoolId = 0x78;       // i32
constexpr uint64_t kAllocBytes = 0x80;   // u64
constexpr uint64_t kAllocPeak = 0x88;    // u64
constexpr uint64_t kBackedBytes = 0x90;  // u64
constexpr uint64_t kBackedPeak = 0x98;   // u64

constexpr uint64_t kMibTags = 0x111908; // root-relative owner tags, one byte per 1 MiB unit (index = VA >> 20)

// Node fields. The element E = N+0x20 holds E+0 = N, E+8 = prev link, E+0x10 = next link.
constexpr uint64_t kElem = 0x20;
constexpr uint64_t kNodeOwnerFamily = 0x40;
constexpr uint64_t kNodeBacked = 0x50;
constexpr uint64_t kNodeSize = 0x58;
constexpr uint64_t kNodeBase = 0x60;
constexpr uint64_t kNodePool = 0x68;
constexpr uint64_t kNodeInUse = 0x69;
constexpr uint64_t kObjOwnerTag = 0x1c; // obj+0x1c

std::vector<std::pair<uint64_t, size_t>> g_writes;

struct Header {
    uint64_t region_head = 0;
    uint64_t free_head = 0;
    uint64_t free_tail = 0;
    uint64_t alloc = 0;
    uint64_t alloc_peak = 0;
    uint64_t backed = 0;
    uint64_t backed_peak = 0;
    int32_t region_count = 0;
    int32_t free_count = 0;
    int32_t free_pages = 0;
    int32_t cached = 0;
    int32_t pool_id = 0;
};

struct Elem {
    uint64_t e = 0;
    uint64_t node = 0;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t backed = 0;
    uint64_t prev = 0;
    uint64_t next = 0;
};

bool ReadU64(GuestMem& m, uint64_t addr, uint64_t* v) { return m.Read(addr, v, sizeof(*v)); }
bool ReadI32(GuestMem& m, uint64_t addr, int32_t* v) { return m.Read(addr, v, sizeof(*v)); }

bool ReadHeader(GuestMem& m, uint64_t S, Header* h) {
    return ReadU64(m, S + kRegionHead, &h->region_head) && ReadI32(m, S + kRegionCount, &h->region_count) &&
           ReadU64(m, S + kFreeTail, &h->free_tail) && ReadU64(m, S + kFreeHead, &h->free_head) &&
           ReadI32(m, S + kFreeCount, &h->free_count) && ReadI32(m, S + kFreePages, &h->free_pages) &&
           ReadI32(m, S + kCached, &h->cached) && ReadI32(m, S + kPoolId, &h->pool_id) &&
           ReadU64(m, S + kAllocBytes, &h->alloc) && ReadU64(m, S + kAllocPeak, &h->alloc_peak) &&
           ReadU64(m, S + kBackedBytes, &h->backed) && ReadU64(m, S + kBackedPeak, &h->backed_peak);
}

// The region list is circular through the sentinel S: head at S+0x10, each element's next at E+0x10.
bool CheckRegionList(GuestMem& m, uint64_t S, const Header& h) {
    if (h.region_count < 0) {
        return false;
    }
    uint64_t e = h.region_head;
    int64_t n = 0;
    while (e != S) {
        if (n >= h.region_count || e == 0) {
            return false;
        }
        if (!ReadU64(m, e + 0x10, &e)) {
            return false;
        }
        ++n;
    }
    return n == h.region_count;
}

// Walks the free-VA list of set S (sentinel F = S+0x28) in list order and checks the links in both directions.
bool WalkFree(GuestMem& m, uint64_t S, const Header& h, std::vector<Elem>* out) {
    out->clear();
    const uint64_t F = S + kFreeSentinel;
    uint64_t prev = F;
    uint64_t e = h.free_head;
    while (e != F) {
        if (h.free_count < 0 || out->size() >= static_cast<size_t>(h.free_count)) {
            return false;
        }
        Elem el;
        el.e = e;
        if (!ReadU64(m, e, &el.node) || el.node + kElem != e) {
            return false;
        }
        uint64_t link_prev = 0;
        if (!ReadU64(m, e + 0x8, &link_prev) || link_prev != prev) {
            return false;
        }
        el.prev = link_prev;
        if (!ReadU64(m, e + 0x10, &el.next) || !ReadU64(m, el.node + kNodeBase, &el.base) ||
            !ReadU64(m, el.node + kNodeSize, &el.size) || !ReadU64(m, el.node + kNodeBacked, &el.backed)) {
            return false;
        }
        out->push_back(el);
        prev = e;
        e = el.next;
    }
    return prev == h.free_tail && out->size() == static_cast<size_t>(h.free_count);
}

bool SameRuns(const std::vector<Elem>& list, const std::vector<Run>& runs) {
    if (list.size() != runs.size()) {
        return false;
    }
    for (size_t i = 0; i < runs.size(); ++i) {
        if (list[i].base != runs[i].base || list[i].size != runs[i].size || list[i].backed != runs[i].backed) {
            return false;
        }
    }
    return true;
}

// pool_take_free_run's first-fit walk (0x1a86e-0x1a8a9): the first element with size >= bytes whose aligned start
// fits (pad 0, or pad + bytes <= size). Returns the index or -1; *pad_out is the pad before the aligned start.
int FindFirst(const std::vector<Elem>& list, uint64_t bytes, uint32_t aarg, uint64_t* pad_out) {
    const uint64_t mask_low = static_cast<uint64_t>(aarg - 1u);
    for (size_t i = 0; i < list.size(); ++i) {
        const Elem& el = list[i];
        if (el.size < bytes) {
            continue;
        }
        const uint64_t aligned = (el.base + mask_low) & ~mask_low;
        const uint64_t pad = aligned - el.base;
        if (pad == 0 || pad + bytes <= el.size) {
            *pad_out = pad;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool StoreU64(Overlay& m, uint64_t addr, uint64_t v) { return m.Write(addr, &v, 8); }
bool StoreI32(Overlay& m, uint64_t addr, int32_t v) { return m.Write(addr, &v, 4); }
bool StoreU8(Overlay& m, uint64_t addr, uint8_t v) { return m.Write(addr, &v, 1); }

} // namespace

uint64_t ApplyPrimaryNodeHit(GuestMem& mem, uint64_t root, const Pools& pre, const Prediction& pred, uint64_t size,
                             uint32_t align, int kind, uint64_t obj, uint64_t overhead_addr) {
    g_writes.clear();
    ClearTouchedNodes();
    if (root == 0 || pred.outcome != Outcome::Predicted || pred.path != Path::PrimaryHitNode) {
        return 0;
    }
    const int idx = kind == 1 ? 1 : 2;
    if (pred.slot != idx || pred.size != size) {
        return 0;
    }
    if (size == 0 || size % kPage != 0 || (size / kPage) > 0x7fffffffu) {
        return 0;
    }
    const uint64_t pages = size / kPage;
    const uint32_t pages32 = static_cast<uint32_t>(pages);
    const uint32_t align64 = (align + 0xffffu) & 0xffff0000u;
    const uint32_t aarg = (align64 + 0xfffffu) & 0xfff00000u; // primary: rounded up to 1 MiB

    // Every read and write of the call goes through the overlay; nothing reaches mem before the commit.
    Overlay ov(mem);

    // Headers, region lists and the target free list against the snapshot (before any write).
    Header h[3];
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = root + kSetOffset[k];
        const Set& ps = pre.set[k];
        if (!ReadHeader(ov, S, &h[k]) || !CheckRegionList(ov, S, h[k])) {
            return 0;
        }
        if (ps.id != k || h[k].pool_id != k || h[k].region_count != ps.region_count ||
            h[k].free_pages != ps.free_va_pages || h[k].cached != ps.cached_phys ||
            h[k].free_count != ps.stored_free_count || h[k].alloc != ps.alloc_bytes ||
            h[k].backed != ps.backed_bytes) {
            return 0;
        }
    }
    const uint64_t S = root + kSetOffset[idx];
    std::vector<Elem> target;
    if (!WalkFree(ov, S, h[idx], &target) || !SameRuns(target, pre.set[idx].free_va)) {
        return 0;
    }
    for (const Elem& el : target) {
        NoteTouchedNode(el.node);
        if (InPoolRoot(root, el.node)) {
            return 0; // a corrupt free list: the node is in the root struct
        }
    }
    if (static_cast<uint32_t>(h[idx].free_pages) < pages32) {
        return 0;
    }
    // Early reject (0x19ea1-0x19f04): the free and cached pages of S1 and S2 cover the request.
    const uint32_t s1 = static_cast<uint32_t>(h[1].free_pages) + static_cast<uint32_t>(h[1].cached);
    const uint32_t s2 = static_cast<uint32_t>(h[2].free_pages) + static_cast<uint32_t>(h[2].cached);
    if (((uint64_t{s1} << 16) + (uint64_t{s2} << 16)) < size) {
        return 0;
    }
    if (kind == 1) {
        // 0x19e5c: S0 is searched first with the 64 KiB alignment; a hit there is not this path.
        std::vector<Elem> s0;
        if (!WalkFree(ov, root, h[0], &s0) || !SameRuns(s0, pre.set[0].free_va)) {
            return 0;
        }
        if (static_cast<uint32_t>(h[0].free_pages) >= pages32) {
            uint64_t pad0 = 0;
            if (FindFirst(s0, size, align64, &pad0) >= 0) {
                return 0;
            }
        }
    }

    // The first fit of the primary set (0x1a86e-0x1a8a9). It must be a node path with the predicted start.
    uint64_t pad = 0;
    const int found = FindFirst(target, size, aarg, &pad);
    if (found < 0) {
        return 0;
    }
    const Elem r = target[static_cast<size_t>(found)];
    if (r.base + pad != pred.va) {
        return 0;
    }
    // The split test sees the pad path's remainder (backed = size - pad, 0x1a8f1) or the run's own backed.
    const uint64_t test_backed = pad != 0 ? r.size - pad : r.backed;
    const bool split = test_backed > size;
    if (pad == 0 && !split) {
        return 0; // exact hit: ApplyExactHit's path
    }
    const uint64_t start = r.base + pad;

    // Owner tag (0x1aafb): [obj+0x1c] when obj is not NULL, else 0. Read before the first write.
    uint8_t tag = 0;
    if (obj != 0 && !ov.Read(obj + kObjOwnerTag, &tag, 1)) {
        return 0;
    }
    // Owner-tag range (0x1ab08-0x1ab32): MiB units of the block, index = VA >> 20 (primary sets only).
    const uint64_t first_unit = start >> 20;
    const uint64_t last_unit = (start + size - 1) >> 20;
    const uint64_t tag_count = last_unit - first_unit + 1;
    const uint64_t tag_addr = root + kMibTags + first_unit;
    if (tag_count == 0 || tag_count > 0x7fffffffu) {
        return 0;
    }

    // ---- writes, in the game's order -------------------------------------------------------------------
    uint64_t B = 0; // the block node
    uint64_t next_link = 0;
    if (pad != 0) {
        // Pop 1 supplies the pad's block M (0x1a8b1), then the pad setup (Q1-Q9) on M and the free-run node r.
        // The pop may refill (it is the call's first store group then; the refill keeps the call's run r).
        uint64_t M = 0;
        if (!PopNodeOn(ov, root, pre, overhead_addr, r.base, r.size, &M) || M == r.node) {
            return 0;
        }
        const uint64_t rN = r.node;
        uint8_t r_pool = 0;
        uint64_t r_base = 0;
        uint64_t r_size = 0;
        if (!ReadU64(ov, rN + kNodeBase, &r_base) || !ReadU64(ov, rN + kNodeSize, &r_size) ||
            !ov.Read(rN + kNodePool, &r_pool, 1)) {
            return 0;
        }
        const uint64_t rstart = r_base + pad;
        const uint64_t rrem = r_size - pad;
        bool ok = StoreU8(ov, M + kNodePool, r_pool);                  // Q1
        ok = ok && StoreU64(ov, M + kNodeSize, rrem);                  // Q2
        ok = ok && StoreU64(ov, M + kNodeBase, rstart);                // Q3
        ok = ok && StoreU8(ov, M + kNodeInUse, 0);                     // Q4
        ok = ok && StoreU64(ov, M + kNodeOwnerFamily, 0);              // Q5
        ok = ok && StoreI32(ov, M + 0x48, -1);                         // Q6
        ok = ok && StoreU64(ov, M + kNodeBacked, rrem);                // Q7
        ok = ok && StoreU64(ov, rN + kNodeSize, pad);                  // Q8
        ok = ok && StoreU64(ov, rN + kNodeBacked, pad);                // Q9
        if (!ok) {
            return 0;
        }
        // Insert M after r in the free-VA list (0x1a902-0x1a961). The next element is read after the pop.
        uint64_t e_next = 0;
        uint64_t n_next = 0;
        if (!ReadU64(ov, rN + 0x30, &e_next) || !ReadU64(ov, e_next, &n_next)) {
            return 0;
        }
        const uint64_t EM = M + kElem;
        if (n_next != 0) {
            uint64_t prev_of_next = 0;
            ok = StoreU64(ov, EM, M);                                  // I1
            ok = ok && StoreU64(ov, M + 0x30, n_next + kElem);         // I2
            ok = ok && ReadU64(ov, n_next + 0x28, &prev_of_next);
            ok = ok && StoreU64(ov, M + 0x28, prev_of_next);           // I3
            ok = ok && StoreU64(ov, n_next + 0x28, EM);                // I4
            uint64_t p = 0;
            ok = ok && ReadU64(ov, M + 0x28, &p) && StoreU64(ov, p + 0x10, EM); // I5
            int32_t fc = 0;
            ok = ok && ReadI32(ov, S + kFreeCount, &fc) && StoreI32(ov, S + kFreeCount, fc + 1); // I6
        } else {
            uint64_t old_tail = 0;
            ok = StoreU64(ov, EM, M);                                  // I1
            ok = ok && StoreU64(ov, M + 0x30, S + kFreeSentinel);      // I2
            ok = ok && ReadU64(ov, S + kFreeTail, &old_tail);
            ok = ok && StoreU64(ov, M + 0x28, old_tail);               // I3
            ok = ok && StoreU64(ov, S + kFreeTail, EM);                // I4
            uint64_t p = 0;
            ok = ok && ReadU64(ov, M + 0x28, &p) && StoreU64(ov, p + 0x10, EM); // I5
            int32_t fc = 0;
            ok = ok && ReadI32(ov, S + kFreeCount, &fc) && StoreI32(ov, S + kFreeCount, fc + 1); // I6
        }
        ok = ok && StoreU64(ov, M + 0x38, S + kFreeSentinel);          // I7
        if (!ok) {
            return 0;
        }
        B = M;
    } else {
        B = r.node;
    }

    // Exact tail on B (0x1a974-0x1a9fd): W1-W15. Every value is read from the overlay at its use.
    {
        const uint64_t E = B + kElem;
        uint64_t prev = 0;
        uint64_t next = 0;
        if (!ReadU64(ov, B + 0x28, &prev) || !ReadU64(ov, B + 0x30, &next)) {
            return 0;
        }
        next_link = next;
        uint64_t old_head = 0;
        int32_t rc = 0;
        int32_t fc = 0;
        if (!ReadU64(ov, S + kRegionHead, &old_head) || !ReadI32(ov, S + kRegionCount, &rc) ||
            !ReadI32(ov, S + kFreeCount, &fc)) {
            return 0;
        }
        bool ok = StoreU64(ov, prev + 0x10, next);          // W1
        ok = ok && StoreU64(ov, next + 0x8, prev);          // W2
        ok = ok && StoreU64(ov, B + 0x30, E);               // W3
        ok = ok && StoreI32(ov, S + kFreeCount, fc - 1);    // W4
        ok = ok && StoreU64(ov, E, B);                      // W5
        ok = ok && StoreU64(ov, E + 0x8, S);                // W6
        ok = ok && StoreU64(ov, E + 0x10, old_head);        // W7
        ok = ok && StoreU64(ov, S + kRegionHead, E);        // W8
        ok = ok && StoreU64(ov, old_head + 0x8, E);         // W9
        ok = ok && StoreI32(ov, S + kRegionCount, rc + 1);  // W10
        ok = ok && StoreU64(ov, B + 0x38, S);               // W11
        uint64_t backed = 0;
        uint64_t backed_peak = 0;
        ok = ok && ReadU64(ov, S + kBackedBytes, &backed) && ReadU64(ov, S + kBackedPeak, &backed_peak);
        const uint64_t backed_new = backed + size;
        ok = ok && StoreU64(ov, S + kBackedBytes, backed_new);  // W12
        if (ok && backed_new > backed_peak) {
            ok = StoreU64(ov, S + kBackedPeak, backed_new);     // W13
        }
        uint64_t alloc = 0;
        uint64_t alloc_peak = 0;
        ok = ok && ReadU64(ov, S + kAllocBytes, &alloc) && ReadU64(ov, S + kAllocPeak, &alloc_peak);
        const uint64_t alloc_new = alloc + size;
        ok = ok && StoreU64(ov, S + kAllocBytes, alloc_new);    // W14
        if (ok && alloc_new > alloc_peak) {
            ok = StoreU64(ov, S + kAllocPeak, alloc_new);       // W15
        }
        if (!ok) {
            return 0;
        }
    }

    // Split (0x1aa01-0x1aa9e): the remainder node N2 from a pop after the tail (0x1aa0f), inserted before the next
    // free-run node. r2 (the node of the element after the block) is read at 0x1a980, before that pop, as the game
    // does: a refill of the pop may take that element. That pop may refill: its exact hit is planned on the overlay's
    // state at that point (after W15).
    if (split) {
        uint64_t r2 = 0;
        if (!ReadU64(ov, next_link, &r2)) {
            return 0;
        }
        uint64_t N2 = 0;
        if (!PopNodeOn(ov, root, pre, overhead_addr, r.base, r.size, &N2) || N2 == B || N2 == r.node) {
            return 0;
        }
        uint64_t b_base = 0;
        uint64_t b_size = 0;
        uint8_t b_pool = 0;
        if (!ReadU64(ov, B + kNodeBase, &b_base) || !ReadU64(ov, B + kNodeSize, &b_size) ||
            !ov.Read(B + kNodePool, &b_pool, 1)) {
            return 0;
        }
        bool ok = StoreU8(ov, N2 + kNodePool, b_pool);                 // T1
        ok = ok && StoreU64(ov, N2 + kNodeSize, b_size - size);        // T2
        ok = ok && StoreU64(ov, N2 + kNodeBase, b_base + size);        // T3
        ok = ok && StoreU8(ov, N2 + kNodeInUse, 0);                    // T4
        ok = ok && StoreU64(ov, N2 + kNodeOwnerFamily, 0);             // T5
        ok = ok && StoreI32(ov, N2 + 0x48, -1);                        // T6
        ok = ok && StoreU64(ov, N2 + kNodeBacked, b_size - size);      // T7
        ok = ok && StoreU64(ov, B + kNodeSize, size);                  // T8
        ok = ok && StoreU64(ov, B + kNodeBacked, size);                // T9
        if (!ok) {
            return 0;
        }
        const uint64_t EN2 = N2 + kElem;
        if (r2 != 0) {
            uint64_t prev_r2 = 0;
            ok = ReadU64(ov, r2 + 0x28, &prev_r2);
            ok = ok && StoreU64(ov, N2 + kElem, N2);                   // J1
            ok = ok && StoreU64(ov, N2 + 0x30, r2 + kElem);            // J2
            ok = ok && StoreU64(ov, N2 + 0x28, prev_r2);               // J3
            ok = ok && StoreU64(ov, r2 + 0x28, EN2);                   // J4
            uint64_t p = 0;
            ok = ok && ReadU64(ov, N2 + 0x28, &p) && StoreU64(ov, p + 0x10, EN2); // J5
            int32_t fc = 0;
            ok = ok && ReadI32(ov, S + kFreeCount, &fc) && StoreI32(ov, S + kFreeCount, fc + 1); // J6
            ok = ok && StoreU64(ov, N2 + 0x38, S + kFreeSentinel);     // J7
        } else {
            uint64_t old_tail = 0;
            ok = StoreU64(ov, N2 + kElem, N2);                         // J1
            ok = ok && StoreU64(ov, N2 + 0x30, S + kFreeSentinel);     // J2
            ok = ok && ReadU64(ov, S + kFreeTail, &old_tail);
            ok = ok && StoreU64(ov, N2 + 0x28, old_tail);              // J3
            ok = ok && StoreU64(ov, S + kFreeTail, EN2);               // J4
            uint64_t p = 0;
            ok = ok && ReadU64(ov, N2 + 0x28, &p) && StoreU64(ov, p + 0x10, EN2); // J5
            int32_t fc = 0;
            ok = ok && ReadI32(ov, S + kFreeCount, &fc) && StoreI32(ov, S + kFreeCount, fc + 1); // J6
            ok = ok && StoreU64(ov, N2 + 0x38, S + kFreeSentinel);     // J7
        }
        if (!ok) {
            return 0;
        }
    }

    // Node fields of the block (0x1aada-0x1aaf3), the owner tags (0x1ab04-0x1abc6), then W22 (0x1abd2).
    {
        int32_t pool_id = 0;
        if (!ReadI32(ov, S + kPoolId, &pool_id)) {
            return 0;
        }
        bool ok = StoreU8(ov, B + kNodePool, static_cast<uint8_t>(pool_id)); // W16
        ok = ok && StoreU64(ov, B + kNodeSize, size);                         // W17
        ok = ok && StoreU8(ov, B + kNodeInUse, 1);                            // W18
        ok = ok && StoreU64(ov, B + kNodeOwnerFamily, obj);                   // W19
        ok = ok && StoreI32(ov, B + 0x48, -1);                                // W20
        if (ok) {
            const std::vector<uint8_t> tags(static_cast<size_t>(tag_count), tag);
            ok = ov.Write(tag_addr, tags.data(), tags.size());                // W21
        }
        int32_t fp = 0;
        ok = ok && ReadI32(ov, S + kFreePages, &fp) &&
             StoreI32(ov, S + kFreePages, static_cast<int32_t>(static_cast<uint32_t>(fp) - pages32)); // W22
        if (!ok) {
            return 0;
        }
    }

    // The commit: the staged stores reach mem in the order the call issued them. A failed base write leaves the
    // earlier ones in place (LastWriteSet is then non-empty, the caller treats the state as unknown).
    if (!ov.Commit(&g_writes)) {
        return 0;
    }
    return B;
}

std::vector<std::pair<uint64_t, size_t>> PrimaryLastWriteSet() { return g_writes; }

} // namespace SotcPcPoolNative
