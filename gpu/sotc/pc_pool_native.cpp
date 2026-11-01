// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native exact-hit path of the game's pool allocator (pc_pool_native.h). The stores below are the ones
// of pool_alloc_region (0x19e00) and pool_take_free_run (0x1a800) on the exact-hit path, traced in the
// disassembly (out/eboot.elf). Store list with addresses (W1..W22), in the game's order:
//
//   Free-VA list unlink of node N (element E = N+0x20, prev link at E+8, next link at E+0x10):
//   W1  [prev+0x10] := next                 0x1a983   free head is set+0x38, tail set+0x30 (sentinel set+0x28)
//   W2  [next+0x8]  := prev                 0x1a98b
//   W3  [N+0x30]    := E                    0x1a98f   dead store: overwritten by W7
//   W4  [S+0x48]    := free count - 1       0x1a993   i32, dec dword
//   Region list link at the head (sentinel S itself, head S+0x10, tail S+0x8):
//   W5  [E+0]       := N                    0x1a997
//   W6  [E+0x8]     := S                    0x1a99b
//   W7  [E+0x10]    := old region head      0x1a9a3
//   W8  [S+0x10]    := E                    0x1a9a7
//   W9  [old head+8] := E                   0x1a9af   old head == S when the list was empty (this writes the tail)
//   W10 [S+0x20]    := region count + 1     0x1a9b3   i32, inc dword
//   W11 [N+0x38]    := S                    0x1a9b7   owner list
//   Accounting (0x1a9bb-0x1a9f6):
//   W12 [S+0x90]    := backed + size        0x1a9c5   u64
//   W13 [S+0x98]    := backed + size        0x1a9d5   only when the new value is above the peak
//   W14 [S+0x80]    := alloc + size         0x1a9e6   u64
//   W15 [S+0x88]    := alloc + size        0x1a9f6   only when the new value is above the peak
//   Node fields (0x1aaa0-0x1aaf3, no-split branch; N+0x50 is not written here):
//   W16 [N+0x68]    := pool id (byte)       0x1aae4
//   W17 [N+0x58]    := size                 0x1aae7   the tail of the run beyond size is dropped
//   W18 [N+0x69]    := 1                    0x1aaea   in use
//   W19 [N+0x40]    := obj                  0x1aaef   owner family
//   W20 [N+0x48]    := -1 (dword)           0x1aaf3
//   Owner tags (0x1ab04-0x1abc6), byte al = [obj+0x1c] (0 when obj is NULL), count bytes:
//   W21 pool id 0: root+0x20d918 + (base - flexbase) >> 16, one byte per 64 KiB unit of the range
//       pool id != 0: root+0x111908 + (base >> 20), one byte per 1 MiB unit (index = VA >> 20)
//   W22 [S+0x70]    := free pages - pages   0x1abd2   i32, sub dword (the last store of the path)
//
// Not stores of the path: the stack spills and the return value (rax = N) are not guest memory. The early
// reject and the S0 search (kind 1) read memory only.
//
// Guards (all read, nothing written before they pass):
//   - the set's free VA pages (+0x70, unsigned 32-bit) cover the request (0x1a820)
//   - the first element in list order with size >= request and an aligned start that fits is the exact hit:
//     pad 0 and backed (+0x50) <= request. Any other first fit (pad or split) is not this path (0x1a86e-0x1a9fd)
//   - primary sets, mode 0: (S1 + S2 free VA pages + cached pages) * 64 KiB >= request (0x19ea1-0x19f04);
//     kind 1 also requires that S0 has no hit, since S0 is searched first (0x19e5c)
//   - the header, region list and free-VA list read from guest match the snapshot pre
//
// Flexible node path (ApplyNodeHit: kind 1, S0 first fit with a pad, a split, or both). Node pops are
// pool_node_alloc (0x19660): it takes the first element E of the node pool (head [root+0x1f0], sentinel root+0x1e0,
// count [root+0x200]), node N = [E]. The stores of one pop, in the game's order:
//   P1 [N+0x38] := 0 (0x1968f)              P2 [N+0x20] := 0 (0x19697)
//   P3 [prev+0x10] := next (0x196a7)        prev = [N+0x28], next = [N+0x30] (E's links)
//   P4 [next+8] := prev (0x196af)           P5 [N+0x30] := E (0x196b3)     P6 [N+0x28] := E (0x196b7)
//   P7 [root+0x200] := count-1 (0x196c3); when that is 2 the game refills through pool_alloc_region 0x19e00 (64 KiB
//      exact hit, then the carve): SotcPcPoolNodes::PopNode does it on the overlay (pc_pool_native_nodes.cpp).
// Pad (pad = aligned start - base != 0): A is the pad node, popped first (0x1a8b1), N keeps the head [base, base+pad):
//   A1 [A+0x68] := N.pool (0x1a8cb)         A2 [A+0x58] := N.size-pad (0x1a8ce)
//   A3 [A+0x60] := N.base+pad (0x1a8d2)     A4 [A+0x69] := 0 (0x1a8d6)
//   A5 [A+0x40] := 0 (0x1a8da)              A6 [A+0x48] := -1 dword (0x1a8e2)
//   A7 [A+0x50] := N.size-pad (0x1a8f1)     N1 [N+0x58] := pad (0x1a8f5)     N2 [N+0x50] := pad (0x1a8fa)
//   A is inserted after N in the free list (0x1a902-0x1a961). The next element E' = [N+0x30], its node n' = [E']:
//   I1 [A+0x20] := A (0x1a917/0x1a93d)
//   next is the sentinel (n' == 0):   I2 [A+0x30] := F (0x1a945)  I3 [A+0x28] := S.tail (0x1a94d)
//                                     I4 [S+0x30] := A+0x20 (0x1a951)  I5 [A.prev+0x10] := A+0x20 (0x1a959)
//                                     I6 [S+0x48] += 1 (0x1a95d)
//   else:                             I2 [A+0x30] := n'+0x20 (0x1a91b)  I3 [A+0x28] := [n'+0x28] (0x1a923)
//                                     I4 [n'+0x28] := A+0x20 (0x1a927)  I5 [A.prev+0x10] := A+0x20 (0x1a92f)
//                                     I6 [S+0x48] += 1 (0x1a933)
//   I7 [A+0x38] := F (0x1a961)              (dead: W11 overwrites it)
// The block B is A (pad) or N (no pad). The common path is W1..W15 of the exact list with B for N (0x1a974-0x1a9f6):
// W4 decrements the free count as the insertion above left it (pad: +1 then -1, the net is 0).
// Split (B.backed > size, 0x1a9fd): a second pop R (P1..P7, after W15), then
//   S1 [R+0x68] := B.pool (0x1aa32)         S2 [R+0x58] := B.size-size (0x1aa35)
//   S3 [R+0x60] := B.base+size (0x1aa39)    S4 [R+0x69] := 0 (0x1aa3d)
//   S5 [R+0x40] := 0 (0x1aa41)              S6 [R+0x48] := -1 dword (0x1aa49)
//   S7 [R+0x50] := B.size-size (0x1aa61)    S8 [B+0x58] := size (0x1aa65)     S9 [B+0x50] := size (0x1aa69)
//   R is inserted before the next node n (n = [B.next]), or before the sentinel when n == 0:
//   next node:  R1 [R+0x20] := R (0x1aa74)  R2 [R+0x30] := n+0x20 (0x1aa78)  R3 [R+0x28] := [n+0x28] (0x1aa81)
//               R4 [n+0x28] := R+0x20 (0x1aa85)  R5 [R.prev+0x10] := R+0x20 (0x1aa8e)  R6 [S+0x48] += 1 (0x1aa92)
//               R7 [R+0x38] := F (0x1aa9a)
//   sentinel:   R1 [R+0x20] := R (0x1aaae)  R2 [R+0x30] := F (0x1aab6)  R3 [R+0x28] := S.tail (0x1aabe)
//               R4 [S+0x30] := R+0x20 (0x1aac2)  R5 [R.prev+0x10] := R+0x20 (0x1aaca)  R6 [S+0x48] += 1 (0x1aace)
//               R7 [R+0x38] := F (0x1aad2)
// Common tail on B (0x1aada-0x1aaf3): W16 [B+0x68] := pool id, W17 [B+0x58] := size, W18 [B+0x69] := 1,
// W19 [B+0x40] := obj, W20 [B+0x48] := -1 dword. Then the owner tags (W21, flexible span only: root+0x20d918 + the
// 64 KiB index of B.base, count = last-first+1, byte [obj+0x1c] or 0, one entry) and W22 [S+0x70] -= pages (0x1abd2).
// The node-path tag base is B.base (the aligned start for a pad), not the free run's base.
//
// Guards (all read before the first store of the commit; the reads inside the sequence see the overlay):
//   - the flexible set's header, free-VA list and region list match the snapshot; S0 free pages >= request
//   - the first fit (FindFirst) has a pad or backed > request (else ApplyExactHit's path or no fit); the
//     aligned start is pred.va
//   - pool pops (SotcPcPoolNodes::PopNode): the head is not the sentinel, E = [root+0x1f0] has [E] + 0x20 == E, the
//     count is not negative; a pop that leaves the count at 2 refills, and the refill is refused when its exact hit
//     fails or it would take the call's own run; the popped node is not the free run N nor the block
#include "pc_pool_native.h"
#include "pc_pool_native_nodes.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace SotcPcPoolNative {
namespace {

using SotcPcPool::Model::Outcome;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::Prediction;
using SotcPcPool::Model::Path;
using SotcPcPool::Model::Run;
using SotcPcPool::Model::Set;

constexpr uint64_t kPage = 0x10000;
constexpr uint64_t kSetOffset[3] = {0x000, 0x0a0, 0x140}; // S0 = root, S1 = root+0xa0, S2 = root+0x140

// Set fields, offsets from S.
constexpr uint64_t kRegionHead = 0x10;   // u64, region list head (the sentinel is S itself)
constexpr uint64_t kRegionCount = 0x20;  // i32
constexpr uint64_t kFreeSentinel = 0x28; // free-VA list sentinel F; F+8 = S+0x30 (tail), F+0x10 = S+0x38 (head)
constexpr uint64_t kFreeTail = 0x30;     // u64, free-VA list tail
constexpr uint64_t kFreeHead = 0x38;     // u64, free-VA list head
constexpr uint64_t kFreeCount = 0x48;    // i32
constexpr uint64_t kFreePages = 0x70;    // i32 (compared as u32 by the game)
constexpr uint64_t kCached = 0x74;       // i32
constexpr uint64_t kPoolId = 0x78;       // i32
constexpr uint64_t kAllocBytes = 0x80;   // u64
constexpr uint64_t kAllocPeak = 0x88;    // u64
constexpr uint64_t kBackedBytes = 0x90;  // u64
constexpr uint64_t kBackedPeak = 0x98;   // u64

// Root-relative tables.
constexpr uint64_t kFlexBase = 0x20d908; // u64, base of the flexible VA range
constexpr uint64_t kFlexTags = 0x20d918; // owner tags, one byte per 64 KiB unit from the flexible base
constexpr uint64_t kMibTags = 0x111908;  // owner tags, one byte per 1 MiB unit, index = VA >> 20

// Node fields. The element E = N+0x20 holds E+0 = N, E+8 = prev link, E+0x10 = next link, E+0x18 = owner list.
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

// One free-VA element in list order. e = N+0x20; prev and next are the element's link values.
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

// pool_take_free_run's first-fit walk (0x1a86e-0x1a8a1, 0x1a888): the first element with size >= bytes whose
// aligned start fits. *pad_out is the pad before the aligned start (0 for an exact start). Returns the index or -1.
// aarg is the alignment the caller passes in ecx; the game decrements it as a 32-bit value.
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

// Stores one write of the exact path: performs it on m and appends it to log (may be null: no log).
bool Store(GuestMem& m, std::vector<std::pair<uint64_t, size_t>>* log, uint64_t addr, const void* data, size_t n) {
    if (!m.Write(addr, data, n)) {
        return false;
    }
    if (log != nullptr) {
        log->push_back({addr, n});
    }
    return true;
}

} // namespace

uint64_t ApplyExactHitTo(GuestMem& mem, uint64_t root, const Pools& pre, const Prediction& pred, uint64_t size,
                         uint32_t align, int kind, uint64_t obj, std::vector<std::pair<uint64_t, size_t>>* log) {
    if (root == 0 || pred.outcome != Outcome::Predicted) {
        return 0;
        return 0;
    }
    const bool flex = pred.path == Path::FlexHitExact;
    const bool primary = pred.path == Path::PrimaryHitExact;
    if (!flex && !primary) {
        return 0;
    }
    // Set index as the game selects it: S0 for kind 1 on the flexible path, else P = S1 (kind 1) or S2.
    if (flex && kind != 1) {
        return 0;
    }
    const int idx = flex ? 0 : (kind == 1 ? 1 : 2);
    if (pred.slot != idx || pred.size != size) {
        return 0;
    }
    // The wrapper's size is a multiple of 64 KiB here (an unrounded size is not modelled).
    if (size == 0 || size % kPage != 0 || (size / kPage) > 0x7fffffffu) {
        return 0;
    }
    const uint64_t pages = size / kPage;
    const uint32_t pages32 = static_cast<uint32_t>(pages);

    // Rounding of pool_alloc_region: align to 64 KiB (ebx); the primary sets then round that to 1 MiB.
    const uint32_t align64 = (align + 0xffffu) & 0xffff0000u;
    const uint32_t aarg = flex ? align64 : ((align64 + 0xfffffu) & 0xfff00000u);

    // Read and check the three set headers and region lists against the snapshot before anything else.
    Header h[3];
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = root + kSetOffset[k];
        const Set& ps = pre.set[k];
        if (!ReadHeader(mem, S, &h[k]) || !CheckRegionList(mem, S, h[k])) {
            return 0;
        }
        if (ps.id != k || h[k].pool_id != k || h[k].region_count != ps.region_count ||
            h[k].free_pages != ps.free_va_pages || h[k].cached != ps.cached_phys ||
            h[k].free_count != ps.stored_free_count || h[k].alloc != ps.alloc_bytes ||
            h[k].backed != ps.backed_bytes) {
            return 0;
        }
    }

    // The target set: its list must match the snapshot, and its free VA pages must cover the request (0x1a820).
    const uint64_t S = root + kSetOffset[idx];
    std::vector<Elem> target;
    if (!WalkFree(mem, S, h[idx], &target) || !SameRuns(target, pre.set[idx].free_va)) {
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

    if (primary) {
        // 0x19ea1-0x19f04 (mode 0): the free and cached pages of S1 and S2 must cover the request.
        const uint32_t s1 = static_cast<uint32_t>(h[1].free_pages) + static_cast<uint32_t>(h[1].cached);
        const uint32_t s2 = static_cast<uint32_t>(h[2].free_pages) + static_cast<uint32_t>(h[2].cached);
        const uint64_t reach = (uint64_t{s1} << 16) + (uint64_t{s2} << 16);
        if (reach < size) {
            return 0;
        }
        if (kind == 1) {
            // 0x19e5c: S0 is searched first with the 64 KiB alignment; a hit there is not this path.
            std::vector<Elem> s0;
            if (!WalkFree(mem, root, h[0], &s0) || !SameRuns(s0, pre.set[0].free_va)) {
                return 0;
            }
            if (static_cast<uint32_t>(h[0].free_pages) >= pages32) {
                uint64_t pad0 = 0;
                if (FindFirst(s0, size, align64, &pad0) >= 0) {
                    return 0;
                }
            }
        }
    }

    uint64_t pad = 0;
    const int found = FindFirst(target, size, aarg, &pad);
    if (found < 0 || pad != 0) {
        return 0;
    }
    const Elem n = target[static_cast<size_t>(found)];
    if (n.base != pred.va || n.backed > size) {
        // A split (backed above the request) takes a node from pool_node_alloc: not this path.
        return 0;
    }

    // Owner tag: al = [obj+0x1c] when obj is not NULL, else 0 (0x1aafb).
    uint8_t tag = 0;
    if (obj != 0 && !mem.Read(obj + kObjOwnerTag, &tag, 1)) {
        return 0;
    }

    // Owner-tag range (0x1ab04-0x1ab3c and 0x1ab08-0x1ab32).
    uint64_t tag_addr = 0;
    uint64_t tag_count = 0;
    if (idx == 0) {
        uint64_t flex_base = 0;
        if (!ReadU64(mem, root + kFlexBase, &flex_base) || n.base < flex_base) {
            return 0;
        }
        const uint64_t first = (n.base - flex_base) >> 16;
        const uint64_t last = (n.base + size - 1 - flex_base) >> 16;
        tag_addr = root + kFlexTags + first;
        tag_count = last - first + 1;
    } else {
        const uint64_t first = n.base >> 20;
        const uint64_t last = (n.base + size - 1) >> 20;
        tag_addr = root + kMibTags + first;
        tag_count = last - first + 1;
    }
    if (tag_count == 0 || tag_count > 0x7fffffffu) {
        return 0;
    }

    // Values the stores need, all read before the first write.
    const uint64_t N = n.node;
    const uint64_t E = n.e;
    const uint64_t prev_link = n.prev;
    const uint64_t next_link = n.next;
    const uint64_t old_head = h[idx].region_head;
    if (old_head == 0) {
        return 0;
    }
    const uint64_t backed_new = h[idx].backed + size;
    const uint64_t alloc_new = h[idx].alloc + size;
    const uint8_t pool_byte = static_cast<uint8_t>(h[idx].pool_id);

    // W1..W22 in the game's order (see the store list at the top of the file).
    uint64_t v64 = 0;
    int32_t v32 = 0;
    uint8_t v8 = 0;
    bool ok = true;

    v64 = next_link;
    ok = ok && Store(mem, log, prev_link + 0x10, &v64, 8);                    // W1
    v64 = prev_link;
    ok = ok && Store(mem, log, next_link + 0x8, &v64, 8);                     // W2
    v64 = E;
    ok = ok && Store(mem, log, N + 0x30, &v64, 8);                            // W3
    v32 = h[idx].free_count - 1;
    ok = ok && Store(mem, log, S + kFreeCount, &v32, 4);                      // W4
    v64 = N;
    ok = ok && Store(mem, log, E, &v64, 8);                                   // W5
    v64 = S;
    ok = ok && Store(mem, log, E + 0x8, &v64, 8);                             // W6
    v64 = old_head;
    ok = ok && Store(mem, log, E + 0x10, &v64, 8);                            // W7
    v64 = E;
    ok = ok && Store(mem, log, S + kRegionHead, &v64, 8);                     // W8
    ok = ok && Store(mem, log, old_head + 0x8, &v64, 8);                      // W9
    v32 = h[idx].region_count + 1;
    ok = ok && Store(mem, log, S + kRegionCount, &v32, 4);                    // W10
    v64 = S;
    ok = ok && Store(mem, log, N + 0x38, &v64, 8);                            // W11
    ok = ok && Store(mem, log, S + kBackedBytes, &backed_new, 8);             // W12
    if (backed_new > h[idx].backed_peak) {
        ok = ok && Store(mem, log, S + kBackedPeak, &backed_new, 8);          // W13
    }
    ok = ok && Store(mem, log, S + kAllocBytes, &alloc_new, 8);               // W14
    if (alloc_new > h[idx].alloc_peak) {
        ok = ok && Store(mem, log, S + kAllocPeak, &alloc_new, 8);            // W15
    }
    v8 = pool_byte;
    ok = ok && Store(mem, log, N + kNodePool, &v8, 1);                        // W16
    v64 = size;
    ok = ok && Store(mem, log, N + kNodeSize, &v64, 8);                       // W17
    v8 = 1;
    ok = ok && Store(mem, log, N + kNodeInUse, &v8, 1);                       // W18
    v64 = obj;
    ok = ok && Store(mem, log, N + kNodeOwnerFamily, &v64, 8);                // W19
    v32 = -1;
    ok = ok && Store(mem, log, N + 0x48, &v32, 4);                            // W20
    if (ok) {
        std::vector<uint8_t> tags(static_cast<size_t>(tag_count), tag);
        ok = Store(mem, log, tag_addr, tags.data(), tags.size());             // W21
    }
    v32 = static_cast<int32_t>(static_cast<uint32_t>(h[idx].free_pages) - pages32);
    ok = ok && Store(mem, log, S + kFreePages, &v32, 4);                      // W22
    if (!ok) {
        return 0; // a failed Write: the stores before it stay (see the header)
    }
    return N;
}


uint64_t ApplyExactHit(GuestMem& mem, uint64_t root, const Pools& pre, const Prediction& pred, uint64_t size,
                       uint32_t align, int kind, uint64_t obj) {
    g_writes.clear();
    ClearTouchedNodes();
    return ApplyExactHitTo(mem, root, pre, pred, size, align, kind, obj, &g_writes);
}


// ---- flexible node path ---------------------------------------------------------------------------------
// The writes of the node path go to an Overlay in the game's order (reads see the staged bytes), and reach guest
// memory only through Overlay::Commit after the whole sequence has passed. Each node pop is SotcPcPoolNodes::PopNode
// on the overlay, with the set state read back from the overlay at that point: the pop's own refill (its exact hit
// and its carve) is then planned on the state the game has there.
namespace {

uint64_t U64(Overlay& t, uint64_t a) {
    uint64_t v = 0;
    t.Read(a, &v, 8);
    return v;
}
int32_t I32(Overlay& t, uint64_t a) {
    int32_t v = 0;
    t.Read(a, &v, 4);
    return v;
}
uint8_t U8(Overlay& t, uint64_t a) {
    uint8_t v = 0;
    t.Read(a, &v, 1);
    return v;
}
void St64(Overlay& t, uint64_t a, uint64_t v) { t.Write(a, &v, 8); }
void St32(Overlay& t, uint64_t a, int32_t v) { t.Write(a, &v, 4); }
void St8(Overlay& t, uint64_t a, uint8_t v) { t.Write(a, &v, 1); }

// One pool_node_alloc pop (with its refill when the count goes 3 -> 2) on the overlay. keep_base/keep_size: the free
// run the call has chosen, which the refill must not take. False: refused (the overlay is then discarded).
bool PopOne(Overlay& t, uint64_t root, const Pools& pre, uint64_t overhead_addr, uint64_t keep_base,
            uint64_t keep_size, uint64_t* node) {
    Pools live;
    if (!RefreshSnapshot(t, root, pre, &live)) {
        return false;
    }
    SotcPcPoolNodes::Status st = SotcPcPoolNodes::Status::Refused;
    const bool ok = SotcPcPoolNodes::PopNode(t, root, live, node, nullptr, overhead_addr, &st, keep_base, keep_size);
    return ok && st == SotcPcPoolNodes::Status::Popped && !t.bad;
}

} // namespace

bool PopNodeOn(Overlay& t, uint64_t root, const Pools& pre, uint64_t overhead_addr, uint64_t keep_base,
               uint64_t keep_size, uint64_t* node) {
    return PopOne(t, root, pre, overhead_addr, keep_base, keep_size, node);
}

bool RefreshSnapshot(GuestMem& mem, uint64_t root, const Pools& pre, Pools* out) {
    Pools p;
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = root + kSetOffset[k];
        Header h;
        if (!ReadHeader(mem, S, &h) || !CheckRegionList(mem, S, h)) {
            return false;
        }
        std::vector<Elem> list;
        if (!WalkFree(mem, S, h, &list)) {
            return false;
        }
        Set& s = p.set[k];
        s.id = k;
        s.has_bitmap = pre.set[k].has_bitmap;
        s.region_count = h.region_count;
        s.free_va_pages = h.free_pages;
        s.cached_phys = h.cached;
        s.stored_free_count = h.free_count;
        s.alloc_bytes = h.alloc;
        s.backed_bytes = h.backed;
        s.page_cap = pre.set[k].page_cap;
        s.words = pre.set[k].words;
        s.words_addr = pre.set[k].words_addr;
        for (const Elem& el : list) {
            s.free_va.push_back(Run{el.base, el.size, el.backed, el.node});
        }
    }
    *out = p;
    return true;
}

bool Overlay::Read(uint64_t addr, void* out, size_t n) {
    if (!base_.Read(addr, out, n)) {
        bad = true;
        return false;
    }
    auto* o = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) {
        const auto it = overlay_.find(addr + i);
        if (it != overlay_.end()) {
            o[i] = it->second;
        }
    }
    return true;
}

bool Overlay::Write(uint64_t addr, const void* in, size_t n) {
    if (n == 0) {
        bad = true;
        return false;
    }
    std::vector<uint8_t> probe(n);
    if (!base_.Read(addr, probe.data(), n)) { // the range must be mapped (the commit writes it)
        bad = true;
        return false;
    }
    const auto* p = static_cast<const uint8_t*>(in);
    for (size_t i = 0; i < n; ++i) {
        overlay_[addr + i] = p[i];
    }
    stores_.push_back(Store{addr, std::vector<uint8_t>(p, p + n)});
    return true;
}

bool Overlay::Commit(std::vector<std::pair<uint64_t, size_t>>* log) {
    for (const Store& s : stores_) {
        if (!base_.Write(s.addr, s.data.data(), s.data.size())) {
            return false;
        }
        if (log != nullptr) {
            log->push_back({s.addr, s.data.size()});
        }
    }
    return true;
}

uint64_t ApplyNodeHit(GuestMem& mem, uint64_t root, const Pools& pre, const Prediction& pred, uint64_t size,
                      uint32_t align, int kind, uint64_t obj, uint64_t overhead_addr) {
    g_writes.clear();
    ClearTouchedNodes();
    if (root == 0 || pred.outcome != Outcome::Predicted || pred.path != Path::FlexHitNode) {
        return 0;
    }
    // Flexible set only: kind 1 searches S0 first, and the block's pool id is 0.
    if (kind != 1 || pred.slot != 0 || pred.size != size) {
        return 0;
    }
    if (size == 0 || size % kPage != 0 || (size / kPage) > 0x7fffffffu) {
        return 0;
    }
    const uint32_t pages32 = static_cast<uint32_t>(size / kPage);
    // pool_alloc_region rounds the alignment to 64 KiB (ebx); S0 takes that value as it is (0x19e5c).
    const uint32_t align64 = (align + 0xffffu) & 0xffff0000u;

    Overlay t(mem);
    const uint64_t S = root; // S0
    const uint64_t F = S + kFreeSentinel;

    // Read and check the three set headers and region lists, as ApplyExactHit does.
    Header h[3];
    for (int k = 0; k < 3; ++k) {
        const uint64_t Sk = root + kSetOffset[k];
        const Set& ps = pre.set[k];
        if (!ReadHeader(t, Sk, &h[k]) || !CheckRegionList(t, Sk, h[k])) {
            return 0;
        }
        if (ps.id != k || h[k].pool_id != k || h[k].region_count != ps.region_count ||
            h[k].free_pages != ps.free_va_pages || h[k].cached != ps.cached_phys ||
            h[k].free_count != ps.stored_free_count || h[k].alloc != ps.alloc_bytes ||
            h[k].backed != ps.backed_bytes) {
            return 0;
        }
    }
    std::vector<Elem> target;
    if (!WalkFree(t, S, h[0], &target) || !SameRuns(target, pre.set[0].free_va)) {
        return 0;
    }
    for (const Elem& el : target) {
        NoteTouchedNode(el.node);
        if (InPoolRoot(root, el.node)) {
            return 0; // a corrupt free list: the node is in the root struct
        }
    }
    if (static_cast<uint32_t>(h[0].free_pages) < pages32) {
        return 0;
    }

    // The first fit. A pad or a backed size above the request is this path; an exact start is ApplyExactHit's.
    uint64_t pad = 0;
    const int found = FindFirst(target, size, align64, &pad);
    if (found < 0) {
        return 0;
    }
    const Elem n0 = target[static_cast<size_t>(found)];
    if (pad == 0 && n0.backed <= size) {
        return 0;
    }
    if (n0.base + pad != pred.va) {
        return 0;
    }
    const uint64_t N0 = n0.node;

    // The block: A (a pad node popped first) or N itself. The run N0 is the call's own run: a refill must not take it.
    uint64_t block = N0;
    if (pad != 0) {
        // 0x1a8a9-0x1a8fa: pop A; A takes [aligned, end), N keeps [base, base+pad).
        uint64_t A = 0;
        if (!PopOne(t, root, pre, overhead_addr, n0.base, n0.size, &A) || A == N0) {
            return 0;
        }
        const uint8_t pool_byte = U8(t, N0 + kNodePool);                 // 0x1a8c0
        const uint64_t rem = U64(t, N0 + kNodeSize) - pad;               // 0x1a8c8
        St8(t, A + kNodePool, pool_byte);                                // A1
        St64(t, A + kNodeSize, rem);                                     // A2
        St64(t, A + kNodeBase, n0.base + pad);                           // A3
        St8(t, A + kNodeInUse, 0);                                       // A4
        St64(t, A + kNodeOwnerFamily, 0);                                // A5
        St32(t, A + 0x48, -1);                                           // A6
        St64(t, A + kNodeBacked, U64(t, N0 + kNodeSize) - pad);          // A7
        St64(t, N0 + kNodeSize, pad);                                    // N1
        St64(t, N0 + kNodeBacked, pad);                                  // N2

        // 0x1a902-0x1a961: insert A after N.
        const uint64_t nx = U64(t, N0 + 0x30);
        const uint64_t nxn = U64(t, nx);
        St64(t, A + kElem, A);                                           // I1
        if (nxn == 0) {
            St64(t, A + 0x30, F);                                        // I2
            St64(t, A + 0x28, U64(t, S + kFreeTail));                    // I3
            St64(t, S + kFreeTail, A + kElem);                           // I4
            St64(t, U64(t, A + 0x28) + 0x10, A + kElem);                 // I5
            St32(t, S + kFreeCount, I32(t, S + kFreeCount) + 1);         // I6
        } else {
            St64(t, A + 0x30, nxn + kElem);                              // I2
            St64(t, A + 0x28, U64(t, nxn + 0x28));                       // I3
            St64(t, nxn + 0x28, A + kElem);                              // I4
            St64(t, U64(t, A + 0x28) + 0x10, A + kElem);                 // I5
            St32(t, S + kFreeCount, I32(t, S + kFreeCount) + 1);         // I6
        }
        St64(t, A + 0x38, F);                                            // I7
        block = A;
    }
    if (t.bad) {
        return 0;
    }

    // 0x1a974-0x1a9f6: unlink the block, link it at the head of the region list, count it.
    const uint64_t bE = block + kElem;
    const uint64_t b_next = U64(t, block + 0x30);
    const uint64_t b_prev = U64(t, block + 0x28);
    const uint64_t next_node = U64(t, b_next); // r12: 0 when the next element is the sentinel
    St64(t, b_prev + 0x10, b_next);                                      // W1
    St64(t, U64(t, block + 0x30) + 8, b_prev);                           // W2
    St64(t, block + 0x30, bE);                                           // W3
    St32(t, S + kFreeCount, I32(t, S + kFreeCount) - 1);                 // W4
    St64(t, bE, block);                                                  // W5
    St64(t, bE + 8, S);                                                  // W6
    St64(t, bE + 0x10, U64(t, S + kRegionHead));                         // W7
    St64(t, S + kRegionHead, bE);                                        // W8
    St64(t, U64(t, bE + 0x10) + 8, bE);                                  // W9
    St32(t, S + kRegionCount, I32(t, S + kRegionCount) + 1);             // W10
    St64(t, block + 0x38, S);                                            // W11
    const uint64_t backed_new = U64(t, S + kBackedBytes) + size;
    St64(t, S + kBackedBytes, backed_new);                               // W12
    if (backed_new > U64(t, S + kBackedPeak)) {
        St64(t, S + kBackedPeak, backed_new);                            // W13
    }
    const uint64_t alloc_new = U64(t, S + kAllocBytes) + size;
    St64(t, S + kAllocBytes, alloc_new);                                 // W14
    if (alloc_new > U64(t, S + kAllocPeak)) {
        St64(t, S + kAllocPeak, alloc_new);                              // W15
    }
    if (t.bad) {
        return 0;
    }

    // 0x1a9fd: the block keeps more backed bytes than the request: the tail becomes a node R (0x1aa0f).
    if (U64(t, block + kNodeBacked) > size) {
        uint64_t R = 0;
        if (!PopOne(t, root, pre, overhead_addr, n0.base, n0.size, &R) || R == block || R == N0) {
            return 0;
        }
        const uint64_t bbase = U64(t, block + kNodeBase);
        const uint64_t bsize = U64(t, block + kNodeSize);
        const uint8_t bpool = U8(t, block + kNodePool);
        St8(t, R + kNodePool, bpool);                                    // S1
        St64(t, R + kNodeSize, bsize - size);                            // S2
        St64(t, R + kNodeBase, bbase + size);                            // S3
        St8(t, R + kNodeInUse, 0);                                       // S4
        St64(t, R + kNodeOwnerFamily, 0);                                // S5
        St32(t, R + 0x48, -1);                                           // S6
        St64(t, R + kNodeBacked, bsize - size);                          // S7
        St64(t, block + kNodeSize, size);                                // S8
        St64(t, block + kNodeBacked, size);                              // S9
        if (next_node != 0) {
            St64(t, R + kElem, R);                                       // R1
            St64(t, R + 0x30, next_node + kElem);                        // R2
            St64(t, R + 0x28, U64(t, next_node + 0x28));                 // R3
            St64(t, next_node + 0x28, R + kElem);                        // R4
            St64(t, U64(t, R + 0x28) + 0x10, R + kElem);                 // R5
            St32(t, S + kFreeCount, I32(t, S + kFreeCount) + 1);         // R6
        } else {
            St64(t, R + kElem, R);                                       // R1
            St64(t, R + 0x30, F);                                        // R2
            St64(t, R + 0x28, U64(t, S + kFreeTail));                    // R3
            St64(t, S + kFreeTail, R + kElem);                           // R4
            St64(t, U64(t, R + 0x28) + 0x10, R + kElem);                 // R5
            St32(t, S + kFreeCount, I32(t, S + kFreeCount) + 1);         // R6
        }
        St64(t, R + 0x38, F);                                            // R7
    }
    if (t.bad) {
        return 0;
    }

    // 0x1aada-0x1ab02: the block's fields (W16..W20) and the owner tag byte.
    const int32_t pool_id = I32(t, S + kPoolId);
    if (pool_id != 0) {
        return 0; // the flexible set only
    }
    St8(t, block + kNodePool, static_cast<uint8_t>(pool_id));            // W16
    St64(t, block + kNodeSize, size);                                    // W17
    St8(t, block + kNodeInUse, 1);                                       // W18
    St64(t, block + kNodeOwnerFamily, obj);                              // W19
    St32(t, block + 0x48, -1);                                           // W20
    uint8_t tag = 0;
    if (obj != 0) {
        tag = U8(t, obj + kObjOwnerTag); // 0x1aafd
    }
    if (t.bad) {
        return 0;
    }

    // W21: the flexible span's owner tags, one byte per 64 KiB unit of the block (0x1ab3c-0x1ab63).
    const uint64_t tb = U64(t, block + kNodeBase);
    const uint64_t fb = U64(t, root + kFlexBase);
    if (t.bad || tb < fb) {
        return 0;
    }
    const uint64_t first = (tb - fb) >> 16;
    const uint64_t last = (tb + size - 1 - fb) >> 16;
    const uint64_t count = last - first + 1;
    if (count == 0 || count > 0x7fffffffu) {
        return 0;
    }
    const std::vector<uint8_t> tags(static_cast<size_t>(count), tag);
    if (!t.Write(root + kFlexTags + first, tags.data(), tags.size())) {
        return 0;
    }
    // W22 (0x1abd2): the last store.
    St32(t, S + kFreePages, static_cast<int32_t>(static_cast<uint32_t>(I32(t, S + kFreePages)) - pages32));
    if (t.bad) {
        return 0;
    }

    // A failed write during the commit returns 0 with the earlier writes in place: LastWriteSet is then non-empty
    // and the caller treats the state as unknown (see the header).
    if (!t.Commit(&g_writes)) {
        return 0;
    }
    return block;
}

std::vector<std::pair<uint64_t, size_t>> LastWriteSet() { return g_writes; }

namespace {
std::vector<uint64_t> g_touched;
} // namespace

void ClearTouchedNodes() { g_touched.clear(); }
void NoteTouchedNode(uint64_t node) { g_touched.push_back(node); }
std::vector<uint64_t> TouchedNodes() { return g_touched; }

} // namespace SotcPcPoolNative
