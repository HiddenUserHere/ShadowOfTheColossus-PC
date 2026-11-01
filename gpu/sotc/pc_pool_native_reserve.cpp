// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native vmem_query_reserve_map flag 1 (eboot 0x1abf0) and the POOL_FIX case (eboot 0x1a321). Companion of
// pc_pool_native_reserve.h. The reference is the model's ReserveMap and Predict (pc_pool.cpp, namespace Model).
//
// Store list, eboot addresses from the disassembly of out/eboot.elf (llvm-objdump, 0x1abf0-0x1af09), in the
// game's order. S = the set (its header at root + 0 / 0xa0 / 0x140), R = root, E = N+0x20 (the set-list element
// of node N: E+0 = N, E+8 = prev link, E+0x10 = next link), prev/next = the links of E.
//
//   Per committed piece (the game walks the pieces with sceKernelVirtualQuery; here one pass over the PhysMap):
//   R1  [S+0x50 + 8*w] |= mask        0x1ad75   bitmap OR (the native path writes each changed word once)
//   R2  [S+0x74] += piece pages       0x1ad90   i32 (the native path writes the total once)
//   R3  [N+0x50] -= piece bytes       0x1ad9b   u64 backed (the native path writes the total once)
//   After the pieces, when the unmap flag is set (r8d = 1 on this path):
//   Q1  [entry+0x00] := VA            0x1ae0e   u64  (entry = R+0x2102f8 + 32 * count; flush at count 0x40, see QueueFull)
//   Q2  [entry+0x08] := 0             0x1ae16   u64  phys offset
//   Q3  [entry+0x10] := consumed      0x1ae22   u64  length
//   Q4  [entry+0x18] := 0             0x1ae2a   u8   prot
//   Q5  [entry+0x19] := 0             0x1ae3d   u8   type
//   Q6  [entry+0x1c] := 1             0x1ae53   i32  op 1 (unmap)
//   Q7  [R+0x2102f0] := count + 1    0x1ae5e   i32
//   R4  [S+0x70] -= consumed >> 16    0x1ae6b   i32  free VA pages
//   Used up (consumed == N+0x58):
//   U1  [prev+0x10] := next           0x1ae93   u64  unlink, head case: prev = S+0x28, so this writes S+0x38
//   U2  [next+0x08] := prev           0x1ae9b   u64  unlink, tail case: next = S+0x28, so this writes S+0x30
//   U3  [N+0x30] := E                 0x1ae9f   u64  (dead: U12 overwrites it)
//   U4  [S+0x48] -= 1                 0x1aea3   i32  free-VA count
//   U5  [N+0x69] := 0                 0x1aea7   u8   in-use flag
//   U6  [N+0x40] := 0                 0x1aeab   u64  owner family
//   U7  [N+0x50..N+0x5f] := 0         0x1aeba   16 B backed and size (vmovups)
//   U8  [N+0x60] := 0                 0x1aebe   u64  base
//   U9  [N+0x48] := -1                0x1aec6   i32
//   U10 [E+0x00] := N                 0x1aed4   u64  element back pointer
//   U11 [E+0x08] := R+0x1e0           0x1aedf   u64  node pool sentinel
//   U12 [E+0x10] := old pool head     0x1aeea   u64  old head = [R+0x1f0]
//   U13 [R+0x1f0] := E                0x1aeee   u64  node pool head
//   U14 [old head+0x08] := E          0x1aef9   u64
//   U15 [R+0x200] += 1                0x1aefd   i32  node pool count
//   U16 [N+0x38] := R+0x1e0           0x1af03   u64
//   Not used up (consumed < N+0x58):
//   P1  [N+0x68] := pool id           0x1af33   u8   (low byte of [S+0x78])
//   P2  [N+0x58] := size - consumed   0x1af36   u64
//   P3  [N+0x60] := base + consumed   0x1af3a   u64
//   P4  [N+0x69] := 0                 0x1af3e   u8
//   P5  [N+0x40] := 0                 0x1af42   u64
//   P6  [N+0x48] := -1                0x1af4a   i32
//
// The cached-zero store of POOL_FIX (the patch at 0x1a321, mov dword [r14+0x74], 0) comes first in ApplyPoolFix.
//
// Documented difference from the patched game (the latent crash): the POOL_FIX case with no free bit AND an
// empty free-VA list. The patched game zeroes the cached count and jumps to 0x1a1f1, which skips the empty-list guard
// of 0x1a1e4; with rax = 0 it reads [0 + 0x58] (a null read). This file writes nothing in that case and returns
// Status::NextSet, the continuation the unpatched guard takes (the next pool set, 0x1a275). Counted in
// ReserveCounters::poolfix_next_set. The continuation itself (0x1a275) is not traced here.
//
// Not reproduced from the disassembly: the caller's computation of the consumed length (min of the 1 MiB-rounded
// remainder and the node size; the model's value, 0x1a2xx not traced here) and the flag's origin.
#include "pc_pool_native_reserve.h"

#include <algorithm>
#include <cstring>

namespace SotcPcPoolNativeReserve {
namespace {

using SotcPcPool::Model::kMiB;
using SotcPcPool::Model::kPageBytes;
using SotcPcPool::Model::PhysMap;
using SotcPcPool::Model::Pools;
using SotcPcPool::Model::Run;
using SotcPcPool::Model::Set;

constexpr uint64_t kPage = kPageBytes;
constexpr uint64_t kSetOffset[3] = {0x000, 0x0a0, 0x140}; // S0 = root, S1 = root+0xa0, S2 = root+0x140

// Set fields, offsets from S (the same layout as pc_pool_native.cpp).
constexpr uint64_t kRegionHead = 0x10;   // u64 (the sentinel is S itself)
constexpr uint64_t kRegionCount = 0x20;  // i32
constexpr uint64_t kFreeSentinel = 0x28; // F = S+0x28; F+8 = S+0x30 (tail), F+0x10 = S+0x38 (head)
constexpr uint64_t kFreeTail = 0x30;     // u64
constexpr uint64_t kFreeHead = 0x38;     // u64
constexpr uint64_t kFreeCount = 0x48;    // i32
constexpr uint64_t kBitmapPtr = 0x50;    // u64 bitmap words (S0 has none)
constexpr uint64_t kBitmapWords = 0x58;  // i32 word count
constexpr uint64_t kPageCap = 0x60;      // u32 index bound
constexpr uint64_t kFreePages = 0x70;    // i32
constexpr uint64_t kCached = 0x74;       // i32
constexpr uint64_t kPoolId = 0x78;       // i32
constexpr uint64_t kAllocBytes = 0x80;   // u64
constexpr uint64_t kAllocPeak = 0x88;    // u64
constexpr uint64_t kBackedBytes = 0x90;  // u64
constexpr uint64_t kBackedPeak = 0x98;   // u64

// Node fields.
constexpr uint64_t kElem = 0x20;         // E = N+0x20
constexpr uint64_t kNodeOwner = 0x40;    // u64
constexpr uint64_t kNodeFreeMark = 0x48; // i32 (-1)
constexpr uint64_t kNodeBacked = 0x50;   // u64
constexpr uint64_t kNodeSize = 0x58;     // u64
constexpr uint64_t kNodeBase = 0x60;     // u64
constexpr uint64_t kNodePool = 0x68;     // u8 pool id
constexpr uint64_t kNodeInUse = 0x69;    // u8

// Root-relative: node pool and batch queue.
constexpr uint64_t kNodePoolSentinel = 0x1e0; // R+0x1e0
constexpr uint64_t kNodePoolHead = 0x1f0;     // R+0x1f0
constexpr uint64_t kNodePoolCount = 0x200;    // R+0x200 (i32)
constexpr uint64_t kQueueCount = 0x2102f0;    // i32
constexpr uint64_t kQueueEntries = 0x2102f8;  // 64 entries of 32 bytes
constexpr int32_t kQueueSize = 64;
constexpr uint64_t kQueueStride = 32;

std::vector<std::pair<uint64_t, size_t>> g_writes;
ReserveCounters g_counters;

uint64_t AlignUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

struct Header {
    uint64_t region_head = 0;
    uint64_t free_tail = 0;
    uint64_t free_head = 0;
    uint64_t alloc = 0;
    uint64_t alloc_peak = 0;
    uint64_t backed = 0;
    uint64_t backed_peak = 0;
    uint64_t bitmap_ptr = 0;
    int32_t region_count = 0;
    int32_t free_count = 0;
    int32_t free_pages = 0;
    int32_t cached = 0;
    int32_t pool_id = 0;
    int32_t bitmap_words = 0;
    uint32_t page_cap = 0;
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
bool ReadU32(GuestMem& m, uint64_t addr, uint32_t* v) { return m.Read(addr, v, sizeof(*v)); }

bool ReadHeader(GuestMem& m, uint64_t S, Header* h) {
    return ReadU64(m, S + kRegionHead, &h->region_head) && ReadI32(m, S + kRegionCount, &h->region_count) &&
           ReadU64(m, S + kFreeTail, &h->free_tail) && ReadU64(m, S + kFreeHead, &h->free_head) &&
           ReadI32(m, S + kFreeCount, &h->free_count) && ReadI32(m, S + kFreePages, &h->free_pages) &&
           ReadI32(m, S + kCached, &h->cached) && ReadI32(m, S + kPoolId, &h->pool_id) &&
           ReadU64(m, S + kAllocBytes, &h->alloc) && ReadU64(m, S + kAllocPeak, &h->alloc_peak) &&
           ReadU64(m, S + kBackedBytes, &h->backed) && ReadU64(m, S + kBackedPeak, &h->backed_peak) &&
           ReadU64(m, S + kBitmapPtr, &h->bitmap_ptr) && ReadI32(m, S + kBitmapWords, &h->bitmap_words) &&
           ReadU32(m, S + kPageCap, &h->page_cap);
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

// Walks the free-VA list of set S (sentinel F = S+0x28) in list order, checking the links in both directions.
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

// The model's SearchNode (pc_pool.cpp, 0x1a205-0x1a2b0), copied exactly, sticky first candidate included:
// the first node whose backed pages equal the remaining pages is taken at once; otherwise the candidate is the
// node with the fewest backed pages above the remaining ones, where the first node stays a candidate whatever its
// value (best == 0 means "the first node is the best so far"). A list with no node above the remaining pages gives
// the first node. The list must not be empty.
size_t SearchNode(const std::vector<Elem>& list, uint64_t rem_pages) {
    size_t best = 0;
    uint64_t best_pages = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const uint64_t pages = list[i].backed / kPage;
        if (pages == rem_pages) {
            return i;
        }
        if (pages > rem_pages && (best == 0 || pages < best_pages)) {
            best = i;
            best_pages = pages;
        }
    }
    return best;
}

// The model's FirstSetBit (pc_pool.cpp): the lowest set bit of the first nonzero word, below the page cap.
// false is the "no free bit" case that the POOL_FIX patch replaces.
bool HasFreeBit(const Set& s) {
    for (size_t w = 0; w < s.words.size(); ++w) {
        if (s.words[w] != 0) {
            const uint64_t b = static_cast<uint64_t>(w) * 64 + static_cast<uint64_t>(__builtin_ctzll(s.words[w]));
            return b < s.page_cap;
        }
    }
    return false;
}

bool Store(GuestMem& m, uint64_t addr, const void* data, size_t n) {
    if (!m.Write(addr, data, n)) {
        return false;
    }
    g_writes.push_back({addr, n});
    return true;
}

// The header, region lists, free-VA list and bitmap of q must match pre (nothing is written before this passes).
// Shared by both entry points. zero_cached: the POOL_FIX variant (the cached count is zeroed by the patch first).
Status Core(GuestMem& mem, uint64_t root, const Pools& pre, int q, uint64_t rem, const PhysMap& map, bool zero_cached,
            uint64_t node_hint,
            ReserveResult* out) {
    if (q < 1 || q > 2 || rem == 0) {
        return Status::NotApplicable;
    }
    if (root == 0) {
        return Status::Refused;
    }
    // Any unknown range refuses the call (the native path has no sceKernelVirtualQuery walk to fall back on).
    if (map.UnknownRuns() != 0) {
        return Status::UnknownMap;
    }

    Header h[3];
    for (int k = 0; k < 3; ++k) {
        const uint64_t S = root + kSetOffset[k];
        const Set& ps = pre.set[k];
        if (!ReadHeader(mem, S, &h[k]) || !CheckRegionList(mem, S, h[k])) {
            return Status::Refused;
        }
        if (ps.id != k || h[k].pool_id != k || h[k].region_count != ps.region_count ||
            h[k].free_pages != ps.free_va_pages || h[k].cached != ps.cached_phys ||
            h[k].free_count != ps.stored_free_count || h[k].alloc != ps.alloc_bytes ||
            h[k].backed != ps.backed_bytes) {
            return Status::Refused;
        }
        if (k > 0 && (h[k].bitmap_ptr != ps.words_addr || h[k].bitmap_words != static_cast<int32_t>(ps.words.size()) ||
                      h[k].page_cap != ps.page_cap)) {
            return Status::Refused;
        }
    }

    const Set& ps = pre.set[q];
    const uint64_t S = root + kSetOffset[q];
    std::vector<Elem> list;
    if (!WalkFree(mem, S, h[q], &list)) {
        return Status::Refused;
    }
    if (!SameRuns(list, ps.free_va)) {
        return Status::Refused;
    }
    const size_t nwords = ps.words.size();
    if (nwords == 0 || ps.words_addr == 0) {
        return Status::Refused;
    }
    std::vector<uint64_t> words(nwords);
    if (!mem.Read(ps.words_addr, words.data(), nwords * sizeof(uint64_t)) || words != ps.words) {
        return Status::Refused;
    }
    if (list.empty()) {
        if (node_hint != 0) {
            return Status::Refused; // the hinted node is not on the list
        }
        // The model's Crash: the game reads a null node. ApplyPoolFix turns the same state into NextSet.
        return zero_cached ? Status::NextSet : Status::NoNode;
    }

    // The node the step takes: the hinted node (the game's rax, no search), else the model's SearchNode. The consumed
    // length: min(1 MiB-rounded remainder, node size).
    size_t idx = list.size();
    if (node_hint != 0) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].node == node_hint) {
                idx = i;
                break;
            }
        }
        if (idx == list.size()) {
            return Status::Refused;
        }
    } else {
        idx = SearchNode(list, rem);
    }
    const Elem n = list[idx];
    if (n.size == 0) {
        return Status::Refused;
    }
    const uint64_t want = AlignUp(rem * kPage, kMiB);
    const uint64_t len = std::min(want, n.size);
    const uint64_t total_bits = static_cast<uint64_t>(nwords) * 64;

    // The committed (mapped) pages of the consumed range, from the shadow map (no sceKernelVirtualQuery here).
    std::vector<uint64_t> bits;
    for (uint64_t off = 0; off < len; off += kPage) {
        uint64_t bit = 0;
        switch (map.Lookup(n.base + off, &bit)) {
        case PhysMap::State::Unknown:
            return Status::UnknownMap;
        case PhysMap::State::Unmapped:
            break;
        case PhysMap::State::Mapped:
            if (bit >= total_bits) {
                return Status::Refused;
            }
            bits.push_back(bit);
            break;
        }
    }

    // Values the stores need, read before the first write.
    uint64_t pool_head = 0;
    int32_t pool_count = 0;
    int32_t queue_count = 0;
    if (!ReadU64(mem, root + kNodePoolHead, &pool_head) || !ReadI32(mem, root + kNodePoolCount, &pool_count) ||
        !ReadI32(mem, root + kQueueCount, &queue_count)) {
        return Status::Refused;
    }
    if (pool_head == 0) {
        return Status::Refused; // the node pool head is never null; a null head would write to address 8
    }
    if (queue_count == kQueueSize) {
        return Status::QueueFull;
    }
    if (queue_count < 0 || queue_count > kQueueSize) {
        return Status::Refused;
    }

    const bool node_removed = len == n.size;
    const uint64_t pages_taken = bits.size();
    const uint64_t backed_new = n.backed - pages_taken * kPage; // no clamp: the game subtracts as it goes
    const int32_t free_pages_new = static_cast<int32_t>(static_cast<int64_t>(ps.free_va_pages) -
                                                       static_cast<int64_t>(len / kPage));
    std::vector<uint64_t> nw = words;
    for (uint64_t bit : bits) {
        nw[bit / 64] |= uint64_t{1} << (bit % 64);
    }

    // The model's post state for set q (Model::ReserveMap on a copy, plus the bitmap and cached count).
    *out = ReserveResult();
    out->node_base = n.base;
    out->node = n.node;
    out->returned = node_removed ? 0 : n.node;
    out->consumed = len;
    out->node_removed = node_removed;
    out->bits = bits;
    out->unmapped.push_back({n.base, len});
    Set& post = out->post;
    post = ps;
    post.words = nw;
    post.cached_phys = static_cast<int32_t>(pages_taken);
    post.free_va_pages = free_pages_new;
    post.free_va.clear();
    for (size_t i = 0; i < list.size(); ++i) {
        if (i != idx) {
            post.free_va.push_back(Run{list[i].base, list[i].size, list[i].backed, list[i].node});
        } else if (!node_removed) {
            post.free_va.push_back(Run{n.base + len, n.size - len, backed_new, n.node});
        }
    }
    post.stored_free_count = static_cast<int32_t>(post.free_va.size());

    // ---- writes, in the game's order (see the store list at the top of this file) ----
    bool ok = true;
    if (zero_cached) {
        const int32_t zero = 0;
        ok = Store(mem, S + kCached, &zero, 4); // the POOL_FIX patch: mov dword [r14+0x74], 0
    }
    for (size_t w = 0; ok && w < nwords; ++w) {
        if (nw[w] != words[w]) {
            ok = Store(mem, ps.words_addr + 8 * w, &nw[w], 8); // R1
        }
    }
    if (ok && pages_taken != 0) {
        const int32_t cached = static_cast<int32_t>(pages_taken);
        ok = Store(mem, S + kCached, &cached, 4); // R2 (the game adds per committed piece: none, no store)
    }
    if (ok && pages_taken != 0) {
        ok = Store(mem, n.node + kNodeBacked, &backed_new, 8); // R3
    }
    // Q1..Q7: the unmap queue entry (op 1, prot 0, type 0, phys offset 0), then the count.
    const uint64_t entry = root + kQueueEntries + kQueueStride * static_cast<uint64_t>(queue_count);
    if (ok) {
        const uint64_t va = n.base;
        const uint64_t zero64 = 0;
        const uint64_t length = len;
        const uint8_t zero8 = 0;
        const int32_t op = 1;
        ok = Store(mem, entry + 0x00, &va, 8) && Store(mem, entry + 0x08, &zero64, 8) &&
             Store(mem, entry + 0x10, &length, 8) && Store(mem, entry + 0x18, &zero8, 1) &&
             Store(mem, entry + 0x19, &zero8, 1) && Store(mem, entry + 0x1c, &op, 4);
    }
    if (ok) {
        const int32_t count = queue_count + 1;
        ok = Store(mem, root + kQueueCount, &count, 4); // Q7
    }
    if (ok) {
        ok = Store(mem, S + kFreePages, &free_pages_new, 4); // R4
    }
    if (ok && node_removed) {
        // U1..U16: unlink the used-up node, zero it, push it onto the node pool.
        const uint64_t N = n.node;
        const uint64_t E = n.e;
        const int32_t free_count = h[q].free_count - 1;
        const uint8_t zero8 = 0;
        const uint64_t zero64 = 0;
        const int32_t minus_one = -1;
        const uint64_t sentinel = root + kNodePoolSentinel;
        const int32_t pool_count1 = pool_count + 1;
        const uint8_t zero16[16] = {};
        ok = Store(mem, n.prev + 0x10, &n.next, 8);       // U1
        if (ok) {
            ok = Store(mem, n.next + 0x8, &n.prev, 8);    // U2
        }
        if (ok) {
            ok = Store(mem, N + 0x30, &E, 8);             // U3
        }
        if (ok) {
            ok = Store(mem, S + kFreeCount, &free_count, 4); // U4
        }
        if (ok) {
            ok = Store(mem, N + kNodeInUse, &zero8, 1);   // U5
        }
        if (ok) {
            ok = Store(mem, N + kNodeOwner, &zero64, 8);  // U6
        }
        if (ok) {
            ok = Store(mem, N + kNodeBacked, zero16, 16); // U7
        }
        if (ok) {
            ok = Store(mem, N + kNodeBase, &zero64, 8);   // U8
        }
        if (ok) {
            ok = Store(mem, N + kNodeFreeMark, &minus_one, 4); // U9
        }
        if (ok) {
            ok = Store(mem, E, &N, 8);                    // U10
        }
        if (ok) {
            ok = Store(mem, E + 0x8, &sentinel, 8);       // U11
        }
        if (ok) {
            ok = Store(mem, E + 0x10, &pool_head, 8);     // U12
        }
        if (ok) {
            ok = Store(mem, root + kNodePoolHead, &E, 8); // U13
        }
        if (ok) {
            ok = Store(mem, pool_head + 0x8, &E, 8);      // U14
        }
        if (ok) {
            ok = Store(mem, root + kNodePoolCount, &pool_count1, 4); // U15
        }
        if (ok) {
            ok = Store(mem, N + 0x38, &sentinel, 8);      // U16
        }
    } else if (ok) {
        // P1..P6: the node stays in the set; its base and size move past the consumed range.
        const uint8_t pool_byte = static_cast<uint8_t>(h[q].pool_id);
        const uint64_t new_size = n.size - len;
        const uint64_t new_base = n.base + len;
        const uint8_t zero8 = 0;
        const uint64_t zero64 = 0;
        const int32_t minus_one = -1;
        ok = Store(mem, n.node + kNodePool, &pool_byte, 1) &&      // P1
             Store(mem, n.node + kNodeSize, &new_size, 8) &&       // P2
             Store(mem, n.node + kNodeBase, &new_base, 8) &&       // P3
             Store(mem, n.node + kNodeInUse, &zero8, 1) &&         // P4
             Store(mem, n.node + kNodeOwner, &zero64, 8) &&        // P5
             Store(mem, n.node + kNodeFreeMark, &minus_one, 4);    // P6
    }
    if (!ok) {
        return Status::WriteFailed;
    }
    return Status::Ok;
}

Status CountReserve(Status s) {
    if (s == Status::Ok) {
        ++g_counters.reserve_ok;
    } else if (s == Status::NotApplicable) {
        ++g_counters.reserve_not_applicable;
    } else {
        ++g_counters.reserve_refused;
    }
    return s;
}

} // namespace

const char* StatusName(Status status) {
    switch (status) {
    case Status::Ok:
        return "ok";
    case Status::NotApplicable:
        return "not-applicable";
    case Status::UnknownMap:
        return "unknown-map";
    case Status::Refused:
        return "refused";
    case Status::NoNode:
        return "no-node";
    case Status::NextSet:
        return "next-set";
    case Status::QueueFull:
        return "queue-full";
    case Status::WriteFailed:
        return "write-failed";
    }
    return "?";
}

Status ApplyReserveMap(GuestMem& mem, uint64_t root, const Pools& pre, int q, uint64_t rem_pages,
                       const PhysMap& map, ReserveResult* out, uint64_t node_hint) {
    g_writes.clear();
    ReserveResult scratch;
    if (out == nullptr) {
        out = &scratch;
    }
    if (q < 1 || q > 2 || pre.set[q].cached_phys != 0) {
        return CountReserve(Status::NotApplicable); // the fresh path needs cached == 0
    }
    return CountReserve(Core(mem, root, pre, q, rem_pages, map, false, node_hint, out));
}

Status ApplyPoolFix(GuestMem& mem, uint64_t root, const Pools& pre, int q, uint64_t rem_pages, const PhysMap& map,
                    ReserveResult* out) {
    g_writes.clear();
    ReserveResult scratch;
    if (out == nullptr) {
        out = &scratch;
    }
    if (q < 1 || q > 2 || HasFreeBit(pre.set[q])) {
        ++g_counters.poolfix_not_applicable;
        return Status::NotApplicable; // a free bit exists: the scan finds it, the patch is not taken
    }
    const Status s = Core(mem, root, pre, q, rem_pages, map, true, 0, out); // the patch zeroes rax (xor eax, eax)
    switch (s) {
    case Status::Ok:
        ++g_counters.poolfix_ok;
        break;
    case Status::NextSet:
        ++g_counters.poolfix_next_set;
        break;
    case Status::NotApplicable:
        ++g_counters.poolfix_not_applicable;
        break;
    default:
        ++g_counters.poolfix_refused;
        break;
    }
    return s;
}

std::vector<std::pair<uint64_t, size_t>> LastWriteSet() { return g_writes; }

const ReserveCounters& Counters() { return g_counters; }

void ResetCounters() { g_counters = ReserveCounters(); }

ReserveCounters SnapshotCounters() { return g_counters; }

void RestoreCounters(const ReserveCounters& saved) { g_counters = saved; }

} // namespace SotcPcPoolNativeReserve
