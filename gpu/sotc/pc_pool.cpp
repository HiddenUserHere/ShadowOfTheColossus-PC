// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PcPool verification model (pc_pool.h, docs/re-stutters/PC-MEMORY-REWRITE.md section 2).
// Layout of the game's pool sets, from the disassembly of pool_alloc_region (0x19e00), pool_take_free_run
// (0x1a800), vmem_query_reserve_map (0x1abf0) and pool_free_block (0x1b0e0):
//   S+0x20 region count, S+0x38 head of the free-VA list (an element pointer; the sentinel is S+0x28,
//   and the list is empty when S+0x38 == S+0x28), S+0x48 free-VA node count, S+0x50 bitmap words
//   pointer, S+0x58 bitmap word count, S+0x60 page cap (an index bound), S+0x70 free VA pages,
//   S+0x74 cached phys pages, S+0x78 pool id, S+0x80 allocated bytes, S+0x90 backed bytes.
//   Element E = node+0x20: [E] = node, [node+0x30] = next element, node+0x58 size, node+0x60 base,
//   node+0x68 pool id, node+0x50 backed bytes.
//
// Allocation paths (pool_alloc_region, mode 0, no node, no VA hint):
//   1. kind 1 only: first fit in S0 (0x1a800 with 64 KiB alignment). A hit returns at once.
//   2. Early reject: (S1+S2 free VA pages + cached pages) * 64 KiB < size: return 0, change nothing.
//   3. Primary set P (S1 for kind 1, else S2): first fit in P's free-VA list with the alignment rounded up
//      to 1 MiB. A hit returns at once: the block is the free run's node, with its old backing, and no
//      bitmap, cached count or batch op changes (log cases: cached 128 unchanged, bitmap unchanged).
//   4. Otherwise a fresh VA range (sceKernelReserveVirtualRange, the VA is not predictable) and a new node
//      in P. The page loop then fills the node from the bitmap of P, or of the spill set F when P has
//      neither cached pages nor free VA (kind 1 spills to S2, kinds 0 and 2 to S1).
//      Page loop with cached == 0 runs vmem_query_reserve_map (flag 1) on a free-VA node of the set: the
//      mapped pages come back to the bitmap, the range is unmapped and consumed (the VA is not returned).
//
// The model part (namespace Model) is pure and compiled standalone by tests/pc_pool_test.cpp, which
// defines SOTC_PC_POOL_MODEL_ONLY before including this file. The glue below it reads the game through
// the backing view and is only built into the port.
#include "pc_pool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace SotcPcPool {
namespace Model {
namespace {

uint64_t AlignUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

bool IsPow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

bool TestBit(const Set& s, uint64_t bit) {
    return (s.words[bit / 64] >> (bit % 64)) & 1u;
}

void SetBit(Set* s, uint64_t bit) {
    s->words[bit / 64] |= uint64_t{1} << (bit % 64);
}

void ClearBit(Set* s, uint64_t bit) {
    s->words[bit / 64] &= ~(uint64_t{1} << (bit % 64));
}

uint64_t PopCount(const Set& s) {
    uint64_t n = 0;
    for (uint64_t w : s.words) {
        n += static_cast<uint64_t>(__builtin_popcountll(w));
    }
    return n;
}

// pool_take_free_run (0x1a800): the first run, in list order, whose size holds the request (0x1a800's
// walk stops at the first node with size >= request), and where the aligned start fits in it. A pad before
// the start is split off (the pad node keeps its own bytes; the remainder is assumed backed, see TakeRun).
// Returns the index or -1; *start is the aligned VA of the block. The set must have pages free in
// +0x70 (0x1a800 checks `pages <= S.70` first).
int FindRun(const Set& s, uint64_t bytes, uint64_t align, uint64_t* start) {
    const uint64_t pages = bytes / kPageBytes;
    if (static_cast<int64_t>(pages) > static_cast<int64_t>(s.free_va_pages)) {
        return -1;
    }
    for (size_t i = 0; i < s.free_va.size(); ++i) {
        const Run& r = s.free_va[i];
        if (r.size < bytes) {
            continue;
        }
        const uint64_t aligned = AlignUp(r.base, align);
        const uint64_t pad = aligned - r.base;
        if (pad + bytes <= r.size) {
            *start = aligned;
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Takes [start, start+bytes) out of the free run idx (0x1a800, the LAB_0001a974 path and the pad split).
//  - A pad before start stays a free run with backed = pad (the pad node is resized to pad, its backed
//    to pad).
//  - The remainder (aligned) takes backed = its size (0x1a800 sets the new node's backed to size - pad).
//  - Split when bytes < backed: the tail keeps the remainder (backed = size - bytes), and the block's
//    backed becomes bytes. Otherwise the tail is dropped: the node's size is set to bytes and nothing
//    else is kept (0x1a800 `*(node+0x58) = bytes` outside the split branch). This is the disassembly, not
//    a guess; the tail-drop case is verified only by the in-game comparison.
// Counters, as 0x1a800 updates them: region +1, alloc +bytes, backed +bytes, free VA pages -pages.
void TakeRun(Set* s, size_t idx, uint64_t start, uint64_t bytes, uint64_t pages) {
    const Run r = s->free_va[idx];
    std::vector<Run> repl;
    Run cur = r;
    if (start > r.base) {
        const uint64_t pad = start - r.base;
        repl.push_back(Run{r.base, pad, pad});
        cur = Run{start, r.size - pad, r.size - pad};
    }
    if (bytes < cur.backed && cur.size > bytes) {
        repl.push_back(Run{start + bytes, cur.size - bytes, cur.size - bytes});
    }
    s->free_va.erase(s->free_va.begin() + static_cast<std::ptrdiff_t>(idx));
    s->free_va.insert(s->free_va.begin() + static_cast<std::ptrdiff_t>(idx), repl.begin(), repl.end());
    s->region_count += 1;
    s->alloc_bytes += bytes;
    s->backed_bytes += bytes;
    s->free_va_pages -= static_cast<int32_t>(pages);
    s->stored_free_count = static_cast<int32_t>(s->free_va.size());
}

// The path of a free-run hit. A node comes from pool_node_alloc (0x19660) when the hit pads (start != base: the
// remainder gets a new node, 0x1a800 at 0x1a8xx) or splits (bytes < backed: the tail gets a new node, LAB_0001a974).
// Otherwise the run's own node is taken and no node is popped. Call before TakeRun (it reads the run).
Path HitPath(const Set& s, size_t idx, uint64_t start, uint64_t bytes, bool flex) {
    const Run& r = s.free_va[idx];
    const bool node = start != r.base || bytes < r.backed;
    if (flex) {
        return node ? Path::FlexHitNode : Path::FlexHitExact;
    }
    return node ? Path::PrimaryHitNode : Path::PrimaryHitExact;
}

// The node vmem_query_reserve_map (flag 1) is given, when the cached count is zero, by the search at
// 0x1a205-0x1a2b0 of pool_alloc_region. It runs at every page-loop iteration with cached == 0 (no carry
// between iterations). The key is the node's BACKED pages (node +0x50 >> 16), not its size, against the
// remaining pages r11d:
//   - the first node whose backed pages equal the remaining pages is taken at once;
//   - otherwise the candidate is the node with the fewest backed pages above the remaining pages
//     (`cmp esi, r9d; setb`), with the quirk that the first node stays a candidate whatever its value
//     (`cmp rax, r8; sete`: rax == r8 while the first node is the best so far), so a later node with
//     more backed pages than the first still replaces it;
//   - a list whose nodes all have fewer backed pages than the remaining ones gives the first node.
// The caller checks that the list is not empty (0x1a1e4 / 0x1a205 guards).
size_t SearchNode(const std::vector<Run>& list, uint64_t rem_pages) {
    size_t best = 0;
    uint64_t best_pages = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const uint64_t pages = list[i].backed / kPageBytes;
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

enum class RmStatus { Ok, NoMap, Crash, BadBit, Unknown };

// vmem_query_reserve_map flag 1 (0x1abf0) on the node chosen from set q's free-VA list. The game walks the
// consumed range with sceKernelVirtualQuery: each committed piece sets its phys bits (bit = (piece offset +
// VA - piece start - 0x100000) / 64 KiB) and adds its pages to +0x74, and its bytes come off the node's
// backed (+0x50). The model reads the same pages from the shadow map. The range is then unmapped (queued as
// op 1, reported in p->unmapped), and the set's free VA pages drop by the consumed pages. The consumed range
// is the front of the node: min(1 MiB-rounded remaining, node size). A node that is used up is removed
// (the VA is not returned to any list). Nothing changes when the status is not Ok.
RmStatus ReserveMap(Pools* post, int q, const PhysMap* map, uint64_t rem_pages, Prediction* p) {
    Set& s = post->set[q];
    if (s.free_va.empty()) {
        return RmStatus::Crash;
    }
    if (map == nullptr) {
        return RmStatus::NoMap;
    }
    const size_t idx = SearchNode(s.free_va, rem_pages);
    Run& n = s.free_va[idx];
    const uint64_t want = AlignUp(rem_pages * kPageBytes, kMiB);
    const uint64_t len = std::min(want, n.size);
    const uint64_t total_bits = static_cast<uint64_t>(s.words.size()) * 64;

    // Pass 1: the mapped pages and their bits. Nothing is changed until every page is known.
    std::vector<uint64_t> bits;
    for (uint64_t off = 0; off < len; off += kPageBytes) {
        uint64_t bit = 0;
        switch (map->Lookup(n.base + off, &bit)) {
        case PhysMap::State::Unknown:
            return RmStatus::Unknown;
        case PhysMap::State::Unmapped:
            break;
        case PhysMap::State::Mapped:
            if (bit >= total_bits) {
                return RmStatus::BadBit;
            }
            bits.push_back(bit);
            break;
        }
    }
    // Pass 2: the game's bitmap OR, cached +pages, backed -= committed bytes (no clamp: 0x1abf0 subtracts
    // the piece bytes as they are, so a node short of backing wraps, and the comparison reports it).
    for (uint64_t bit : bits) {
        SetBit(&s, bit);
    }
    s.cached_phys += static_cast<int32_t>(bits.size());
    n.backed -= static_cast<uint64_t>(bits.size()) * kPageBytes;
    s.free_va_pages -= static_cast<int32_t>(len / kPageBytes);
    p->unmapped.push_back(Run{n.base, len, 0});
    if (len == n.size) {
        s.free_va.erase(s.free_va.begin() + static_cast<std::ptrdiff_t>(idx));
    } else {
        n.base += len;
        n.size -= len;
    }
    s.stored_free_count = static_cast<int32_t>(s.free_va.size());
    return RmStatus::Ok;
}

// The first set bit of the bitmap, if it is below the page cap (0x1a2xx: the lowest bit of the first
// nonzero word, then `cap <= bit` -> none).
bool FirstSetBit(const Set& s, uint64_t* bit) {
    for (size_t w = 0; w < s.words.size(); ++w) {
        if (s.words[w] != 0) {
            const uint64_t b = static_cast<uint64_t>(w) * 64 + static_cast<uint64_t>(__builtin_ctzll(s.words[w]));
            if (b >= s.page_cap) {
                return false;
            }
            *bit = b;
            return true;
        }
    }
    return false;
}

uint32_t CompareSet(const Set& want, const Set& got, int index, std::string* details) {
    uint32_t mask = 0;
    char line[256];
    auto note = [&](const char* what, uint64_t pred, uint64_t actual, uint32_t bit) {
        mask |= bit;
        if (details) {
            std::snprintf(line, sizeof line, "S%d %s: pred %llu got %llu\n", index, what,
                          static_cast<unsigned long long>(pred), static_cast<unsigned long long>(actual));
            details->append(line);
        }
    };
    if (!(want.free_va == got.free_va)) {
        mask |= kFieldFreeVa;
        if (details) {
            size_t i = 0;
            while (i < want.free_va.size() && i < got.free_va.size() && want.free_va[i] == got.free_va[i]) {
                ++i;
            }
            std::snprintf(line, sizeof line, "S%d free-VA list: pred %zu runs got %zu, first difference at run %zu\n",
                          index, want.free_va.size(), got.free_va.size(), i);
            details->append(line);
        }
    }
    if (static_cast<int64_t>(want.free_va.size()) != got.stored_free_count) {
        note("free-VA count (+0x48)", want.free_va.size(), static_cast<uint64_t>(got.stored_free_count),
             kFieldFreeCount);
    }
    if (want.free_va_pages != got.free_va_pages) {
        note("free VA pages (+0x70)", static_cast<uint64_t>(want.free_va_pages),
             static_cast<uint64_t>(got.free_va_pages), kFieldFreeVaPages);
    }
    if (want.cached_phys != got.cached_phys) {
        note("cached phys (+0x74)", static_cast<uint64_t>(want.cached_phys),
             static_cast<uint64_t>(got.cached_phys), kFieldCached);
    }
    if (want.region_count != got.region_count) {
        note("region count (+0x20)", static_cast<uint64_t>(want.region_count),
             static_cast<uint64_t>(got.region_count), kFieldRegion);
    }
    if (want.alloc_bytes != got.alloc_bytes) {
        note("alloc bytes (+0x80)", want.alloc_bytes, got.alloc_bytes, kFieldAlloc);
    }
    if (want.backed_bytes != got.backed_bytes) {
        note("backed bytes (+0x90)", want.backed_bytes, got.backed_bytes, kFieldBacked);
    }
    if (want.has_bitmap) {
        if (want.words.size() != got.words.size()) {
            note("bitmap words (+0x58)", want.words.size(), got.words.size(), kFieldBitmap);
        } else {
            for (size_t w = 0; w < want.words.size(); ++w) {
                if (want.words[w] != got.words[w]) {
                    note("bitmap word", want.words[w], got.words[w], kFieldBitmap);
                    if (details) {
                        std::snprintf(line, sizeof line, "  (word %zu, S%d)\n", w, index);
                        details->append(line);
                    }
                    break;
                }
            }
        }
    }
    return mask;
}

} // namespace

PhysMap::State PhysMap::Lookup(uint64_t va, uint64_t* bit) const {
    const uint64_t page = va & ~(kPageBytes - 1);
    const auto u = unknown_.upper_bound(page);
    if (u != unknown_.begin() && std::prev(u)->second > page) {
        return State::Unknown;
    }
    const auto m = mapped_.upper_bound(page);
    if (m != mapped_.begin()) {
        const auto p = std::prev(m);
        if (p->first + p->second.bytes > page) {
            if (bit) {
                *bit = p->second.bit + (page - p->first) / kPageBytes;
            }
            return State::Mapped;
        }
    }
    return State::Unmapped;
}

void PhysMap::Subtract(uint64_t va, uint64_t bytes) {
    const uint64_t end = va + bytes;
    // Known runs: split the runs that straddle either end, drop the ones inside.
    auto it = mapped_.lower_bound(va);
    if (it != mapped_.begin()) {
        const auto p = std::prev(it);
        if (p->first + p->second.bytes > va) {
            it = p;
        }
    }
    while (it != mapped_.end() && it->first < end) {
        const uint64_t start = it->first;
        const uint64_t run_end = start + it->second.bytes;
        const uint64_t bit = it->second.bit;
        it = mapped_.erase(it);
        if (start < va) {
            mapped_.emplace(start, MapRun{va - start, bit});
        }
        if (run_end > end) {
            mapped_.emplace(end, MapRun{run_end - end, bit + (end - start) / kPageBytes});
        }
    }
    // Unknown ranges: the same split.
    auto u = unknown_.lower_bound(va);
    if (u != unknown_.begin()) {
        const auto p = std::prev(u);
        if (p->second > va) {
            u = p;
        }
    }
    while (u != unknown_.end() && u->first < end) {
        const uint64_t start = u->first;
        const uint64_t range_end = u->second;
        u = unknown_.erase(u);
        if (start < va) {
            unknown_.emplace(start, va);
        }
        if (range_end > end) {
            unknown_.emplace(end, range_end);
        }
    }
}

void PhysMap::Map(uint64_t va, uint64_t bytes, uint64_t bit) {
    if (bytes == 0) {
        return;
    }
    Subtract(va, bytes);
    mapped_.emplace(va, MapRun{bytes, bit});
}

void PhysMap::Unmap(uint64_t va, uint64_t bytes) {
    if (bytes != 0) {
        Subtract(va, bytes);
    }
}

void PhysMap::MarkUnknown(uint64_t va, uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    Subtract(va, bytes);
    unknown_.emplace(va, va + bytes);
}

void PhysMap::Clear() {
    mapped_.clear();
    unknown_.clear();
}

bool SeedMap(PhysMap* map, const Pools* pools) {
    map->Clear();
    if (pools == nullptr) {
        return false;
    }
    bool pristine = true;
    for (int s = 1; s < kSetCount; ++s) {
        const Set& set = pools->set[s];
        if (set.region_count != 0 || set.alloc_bytes != 0 || set.backed_bytes != 0) {
            pristine = false;
        }
    }
    if (pristine) {
        return false;
    }
    for (int s = 1; s < kSetCount; ++s) {
        for (const Run& run : pools->set[s].free_va) {
            map->MarkUnknown(run.base, run.size);
        }
    }
    return true;
}

void ApplyBatchOp(PhysMap* map, uint64_t va, uint64_t offset, uint64_t length, int32_t op, bool failed,
                  uint64_t phys_base) {
    switch (op) {
    case 0: { // map [va, va + length) to the phys offset; bit = (offset - phys_base) / 64 KiB
        const bool direct = offset >= phys_base && (offset - phys_base) % kPageBytes == 0 && length != 0 &&
                            length % kPageBytes == 0;
        if (failed || !direct) {
            map->MarkUnknown(va, length);
        } else {
            map->Map(va, length, (offset - phys_base) / kPageBytes);
        }
        return;
    }
    case 1: // unmap
        if (failed) {
            map->MarkUnknown(va, length);
        } else {
            map->Unmap(va, length);
        }
        return;
    case 2:
    case 4: // protection or type only: the mapping stays as it is
        return;
    default: // 3 (flexible mapping) and anything else: not tracked
        map->MarkUnknown(va, length);
        return;
    }
}

bool SameSet(const Set& a, const Set& b) {
    return a.region_count == b.region_count && a.free_va_pages == b.free_va_pages &&
           a.cached_phys == b.cached_phys && a.stored_free_count == b.stored_free_count &&
           a.alloc_bytes == b.alloc_bytes && a.backed_bytes == b.backed_bytes && a.free_va == b.free_va &&
           a.words == b.words;
}

const char* FieldName(int index) {
    static const char* const kNames[kFieldCount] = {
        "node", "va", "size", "slot", "free-VA list", "free-VA count", "free VA pages",
        "cached phys", "region count", "alloc bytes", "backed bytes", "bitmap",
    };
    return index >= 0 && index < kFieldCount ? kNames[index] : "?";
}

Prediction Predict(const Pools& pre, const Request& req, const PhysMap* map, Pools* post) {
    Prediction p;
    *post = pre;
    if (req.va_hint != 0 || req.mode != 0) {
        p.reason = "VA hint or mode != 0 (not the normal path)";
        return p;
    }
    if (req.node != 0) {
        p.reason = "node given (reuse path)";
        return p;
    }
    if (req.size == 0 || req.align == 0) {
        p.reason = "zero size or alignment";
        return p;
    }
    // pool_alloc_region rounds align up to 64 KiB (uVar17); a non power of two is not modelled.
    const uint64_t align64 = AlignUp(req.align, kPageBytes);
    if (!IsPow2(align64)) {
        p.reason = "alignment not a power of two";
        return p;
    }
    const uint64_t pages = AlignUp(req.size, kPageBytes) / kPageBytes;
    const uint64_t bytes = pages * kPageBytes;

    // Step 1: kind 1 tries the flexible set first, with the 64 KiB alignment.
    if (req.kind == 1) {
        uint64_t start = 0;
        const int idx = FindRun(pre.set[0], bytes, align64, &start);
        if (idx >= 0) {
            p.path = HitPath(pre.set[0], static_cast<size_t>(idx), start, bytes, true);
            TakeRun(&post->set[0], static_cast<size_t>(idx), start, bytes, pages);
            p.outcome = Outcome::Predicted;
            p.slot = 0;
            p.va = start;
            p.size = bytes;
            return p;
        }
    }

    // Step 2: early reject. The game compares (S1 + S2 free VA pages + cached pages) * 64 KiB with the
    // unrounded size (the sums of S0 cancel in the disassembly).
    const int64_t reachable = int64_t{pre.set[1].free_va_pages} + pre.set[2].free_va_pages +
                              int64_t{pre.set[1].cached_phys} + pre.set[2].cached_phys;
    if (reachable * static_cast<int64_t>(kPageBytes) < static_cast<int64_t>(req.size)) {
        p.outcome = Outcome::Null;
        p.path = Path::EarlyReject;
        p.reason = "early reject (free VA + cached phys below the request)";
        return p;
    }

    // Step 3: the primary set, the free-VA search with the alignment rounded up to 1 MiB.
    const int P = req.kind == 1 ? 1 : 2;
    const int F = 3 - P;
    const uint64_t align1 = AlignUp(align64, kMiB);
    uint64_t start = 0;
    const int idx = FindRun(pre.set[P], bytes, align1, &start);
    if (idx >= 0) {
        p.path = HitPath(pre.set[P], static_cast<size_t>(idx), start, bytes, false);
        TakeRun(&post->set[P], static_cast<size_t>(idx), start, bytes, pages);
        p.outcome = Outcome::Predicted;
        p.slot = P;
        p.va = start;
        p.size = bytes;
        return p;
    }

    // Step 4: a fresh VA range and a new node in P (the VA comes from sceKernelReserveVirtualRange).
    p.fresh = true;
    p.va_align = align1;
    p.slot = P;
    p.size = bytes;
    post->set[P].region_count += 1;
    post->set[P].alloc_bytes += bytes;

    // The page loop (spec 2.2 step 5, traced in 0x19e00 at 0x1a1f1-0x1a5e6).
    int cur = P;
    uint64_t rem = pages;
    std::vector<PhysRun> runs;
    bool switched = false;
    int protect_type = -1;
    bool used_reserve = false; // the path classification (p.path) of the fresh block
    bool poolfix = false;
    // The reserve-map call of the loop (0x1a2f0 -> 0x1abf0). Returns false with p already set when the
    // prediction stops here.
    auto reserve = [&](int q) {
        used_reserve = true;
        const RmStatus rs = ReserveMap(post, q, map, rem, &p);
        switch (rs) {
        case RmStatus::Ok:
            return true;
        case RmStatus::Crash:
            p.outcome = Outcome::WouldCrash;
            p.path = Path::WouldCrash;
            p.reason = "reserve-map with no free-VA node (the game dereferences null)";
            return false;
        case RmStatus::NoMap:
            p.outcome = Outcome::Unmodelled;
            p.reason = "reserve-map path needs the VA->phys map (not supplied)";
            return false;
        case RmStatus::BadBit:
            p.outcome = Outcome::Unmodelled;
            p.reason = "reserve-map maps a phys page outside the bitmap";
            return false;
        case RmStatus::Unknown:
            p.outcome = Outcome::Unmodelled;
            p.unknown_state = true;
            p.reason = "reserve-map reaches a VA range whose mapping the shadow map does not know";
            return false;
        }
        return false;
    };
    for (int guard = 0; rem > 0; ++guard) {
        if (guard > (1 << 20)) {
            p.outcome = Outcome::Unmodelled;
            p.reason = "page loop did not terminate (model bug)";
            return p;
        }
        if (post->set[cur].cached_phys == 0) {
            if (cur == P && post->set[P].free_va.empty()) {
                // The primary has no free VA: the page loop continues in the spill set F.
                cur = F;
                switched = true;
                protect_type = post->set[F].id != 2 ? 3 : 0; // (F.id != 2) * 3
            }
            if (post->set[cur].cached_phys == 0 && !reserve(cur)) {
                return p;
            }
        }
        uint64_t b = 0;
        if (!FirstSetBit(post->set[cur], &b)) {
            // POOL_FIX (spec 2.5): no free bit. The patch zeroes the cached count and takes the VA-node path
            // (reserve-map), which the empty-list guard turns into a crash when no node is left.
            if (post->set[cur].free_va.empty()) {
                p.outcome = Outcome::WouldCrash;
                p.path = Path::WouldCrash;
                p.reason = "bitmap has no free bit and the free-VA list is empty (POOL_FIX: null read)";
                return p;
            }
            poolfix = true;
            post->set[cur].cached_phys = 0;
            if (!reserve(cur)) {
                return p;
            }
            continue;
        }
        // The run: the lowest free bit and the set bits after it, bounded by the remaining pages and the cap.
        uint64_t len = 0;
        while (len < rem && b + len < post->set[cur].page_cap && b + len < post->set[cur].words.size() * 64 &&
               TestBit(post->set[cur], b + len)) {
            ClearBit(&post->set[cur], b + len);
            ++len;
        }
        post->set[cur].cached_phys -= static_cast<int32_t>(len);
        post->set[cur].backed_bytes += len * kPageBytes;
        runs.push_back(PhysRun{cur, b, len, 0, switched ? protect_type : -1});
        rem -= len;
    }

    p.outcome = Outcome::Predicted;
    p.runs = runs;
    // One path per call, the most specific mechanism first: POOL_FIX, then the spill, then the reserve-map.
    if (poolfix) {
        p.path = Path::FreshPoolFix;
    } else if (switched) {
        p.path = Path::FreshSpill;
    } else if (used_reserve) {
        p.path = Path::FreshReserve;
    } else {
        p.path = Path::FreshPlain;
    }
    return p;
}

bool IsFreshPath(Path path) {
    return path == Path::FreshPlain || path == Path::FreshReserve || path == Path::FreshSpill ||
           path == Path::FreshPoolFix;
}

bool FreshPopRefills(Path path, int32_t node_count_pre) {
    // pool_node_alloc pops the head (count - 1) and refills when the count after the pop is 2 (0x19660: dec, cmp 2).
    return IsFreshPath(path) && node_count_pre == 3;
}

bool Eligible(const Prediction& p, uint32_t mask) {
    if (p.outcome != Outcome::Predicted && p.outcome != Outcome::Null) {
        return false;
    }
    if (p.path == Path::Unmodelled || p.path == Path::WouldCrash) {
        return false;
    }
    return (mask & PathBit(p.path)) != 0;
}

const char* PathName(Path path) {
    static const char* const kNames[kPathCount] = {
        "flex-exact", "flex-node", "primary-exact", "primary-node", "fresh-plain", "fresh-reserve",
        "fresh-spill", "fresh-poolfix", "early-reject", "would-crash", "unmodelled",
    };
    const int i = static_cast<int>(path);
    return i >= 0 && i < kPathCount ? kNames[i] : "?";
}

uint32_t ParsePathMask(const char* spec, std::string* bad) {
    if (spec == nullptr) {
        return kDefaultPathMask;
    }
    uint32_t mask = 0;
    std::string token;
    auto flush = [&]() {
        size_t b = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        token = b == std::string::npos ? std::string() : token.substr(b, e - b + 1);
        if (token.empty()) {
            return;
        }
        if (token == "all") {
            mask = (uint32_t{1} << kPathCount) - 1;
        } else if (token == "none") {
            // no bits
        } else {
            bool found = false;
            for (int i = 0; i < kPathCount; ++i) {
                if (token == PathName(static_cast<Path>(i))) {
                    mask |= PathBit(static_cast<Path>(i));
                    found = true;
                    break;
                }
            }
            if (!found && bad) {
                if (!bad->empty()) {
                    *bad += ", ";
                }
                *bad += token;
            }
        }
        token.clear();
    };
    for (const char* c = spec; *c; ++c) {
        if (*c == ',') {
            flush();
        } else {
            token += *c;
        }
    }
    flush();
    return mask;
}

uint32_t Compare(const Prediction& p, const Pools& pre, const Pools& predicted, const Observed& got,
                 std::string* details, int* outside_sets) {
    uint32_t mask = 0;
    if (p.outcome == Outcome::Null) {
        if (got.node != 0) {
            mask |= kFieldNode;
            if (details) {
                details->append("node: pred 0 (early reject) got a block\n");
            }
        }
    } else if (got.node == 0) {
        mask |= kFieldNode;
        if (details) {
            details->append("node: pred a block got 0\n");
        }
    } else {
        char line[160];
        const bool va_ok = p.fresh ? (p.va_align == 0 || got.va % p.va_align == 0) : got.va == p.va;
        if (!va_ok) {
            mask |= kFieldVa;
            if (details) {
                std::snprintf(line, sizeof line, "va: pred 0x%llx (align 0x%llx) got 0x%llx\n",
                              static_cast<unsigned long long>(p.va), static_cast<unsigned long long>(p.va_align),
                              static_cast<unsigned long long>(got.va));
                details->append(line);
            }
        }
        if (got.size != p.size) {
            mask |= kFieldSize;
            if (details) {
                std::snprintf(line, sizeof line, "size: pred 0x%llx got 0x%llx\n",
                              static_cast<unsigned long long>(p.size), static_cast<unsigned long long>(got.size));
                details->append(line);
            }
        }
        if (got.slot != p.slot) {
            mask |= kFieldSlot;
            if (details) {
                std::snprintf(line, sizeof line, "slot: pred %d got %d\n", p.slot, got.slot);
                details->append(line);
            }
        }
    }
    // Delta check: a set the prediction leaves unchanged is not compared. A change there is an allocation
    // the hook does not see (a nested node-block call, or another caller), counted separately.
    for (int s = 0; s < kSetCount; ++s) {
        if (SameSet(pre.set[s], predicted.set[s])) {
            if (!SameSet(pre.set[s], got.post.set[s]) && outside_sets) {
                ++*outside_sets;
            }
            continue;
        }
        mask |= CompareSet(predicted.set[s], got.post.set[s], s, details);
    }
    return mask;
}

void AddFree(FreeAcc* acc, int slot, uint64_t size, uint64_t backed) {
    FreeAcc& a = acc[slot];
    const int64_t pages = static_cast<int64_t>(size / kPageBytes);
    const int64_t mapped = static_cast<int64_t>(backed / kPageBytes);
    a.frees += 1;
    a.region -= 1;                            // pool_free_block: P.20--
    a.alloc -= static_cast<int64_t>(size);    // P.80 -= size
    a.backed -= static_cast<int64_t>(backed); // P.90 -= node backed
    if (slot == 0) {
        // Flexible set: a fully backed node goes back to the address-ordered list (+pages). A partly backed
        // one takes the flag-1 path, which is net zero (see below).
        a.free_va_pages += backed == size ? pages : 0;
    } else {
        // pool_free_block with pool id != 0 puts the node on the free list (+pages), then vmem_query_reserve_map
        // flag 1 takes the pages back out (-pages) and unmaps the whole node. The node is recycled, so the
        // VA is consumed: net 0 in +0x70, and the mapped pages return to the bitmap and +0x74.
        a.bits += mapped;
        a.cached += mapped;
    }
    (void)pages;
}

uint32_t CheckFree(const Set& before, const Set& after, const FreeAcc& acc, int64_t* va_delta,
                   std::string* details) {
    uint32_t mask = 0;
    char line[160];
    if (static_cast<int64_t>(after.region_count) - before.region_count != acc.region) {
        mask |= kFieldRegion;
        if (details) {
            std::snprintf(line, sizeof line, "region count delta: pred %lld got %lld\n",
                          static_cast<long long>(acc.region),
                          static_cast<long long>(after.region_count) - before.region_count);
            details->append(line);
        }
    }
    if (static_cast<int64_t>(after.alloc_bytes) - static_cast<int64_t>(before.alloc_bytes) != acc.alloc) {
        mask |= kFieldAlloc;
    }
    if (static_cast<int64_t>(after.backed_bytes) - static_cast<int64_t>(before.backed_bytes) != acc.backed) {
        mask |= kFieldBacked;
    }
    if (after.has_bitmap && before.words.size() == after.words.size()) {
        const int64_t bits = static_cast<int64_t>(PopCount(after)) - static_cast<int64_t>(PopCount(before));
        if (bits != acc.bits) {
            mask |= kFieldBitmap;
            if (details) {
                std::snprintf(line, sizeof line, "bitmap popcount delta: pred %lld got %lld\n",
                              static_cast<long long>(acc.bits), static_cast<long long>(bits));
                details->append(line);
            }
        }
        const int64_t cached = static_cast<int64_t>(after.cached_phys) - before.cached_phys;
        if (cached != acc.cached) {
            mask |= kFieldCached;
            if (details) {
                std::snprintf(line, sizeof line, "cached phys delta: pred %lld got %lld\n",
                              static_cast<long long>(acc.cached), cached);
                details->append(line);
            }
        }
    }
    const int64_t fva = static_cast<int64_t>(after.free_va_pages) - before.free_va_pages;
    if (fva != acc.free_va_pages) {
        mask |= kFieldFreeVaPages;
        if (details) {
            std::snprintf(line, sizeof line, "free VA pages delta: pred %lld got %lld\n",
                          static_cast<long long>(acc.free_va_pages), static_cast<long long>(fva));
            details->append(line);
        }
    }
    *va_delta = fva;
    return mask;
}

// ---- native execution: the decision -----------------------------------------------------------------
// Per call (the game's pool lock is taken by the host, bounded, and released on every path):
//   1. killed: Original, nothing taken.
//   2. take the lock (busy: Original), snapshot the sets (failed: Original).
//   3. every kCheckInterval calls: when a verified native call is the last pool change, the snapshot must equal
//      the state that call left (else kill); otherwise the check is skipped and counted.
//   4. predict; not Predicted/Null in the path mask: Original.
//   5. ApplyExactHit: 0 (refused, nothing written): Original.
//   6. from here the game's state is changed and the call returns the node natively, whatever the checks say.
//      Write check (every written range inside the modelled memory of the snapshot), post read, Compare with the
//      prediction (fields, sets the prediction changes, sets it leaves untouched). Any failure kills. Then the
//      wrapper's tail (Host::Tail), unlock, return Native.
// A kill is final for the session: every later call runs the original.
namespace Native {

const char* FallbackName(Fallback fallback) {
    switch (fallback) {
    case Fallback::LockBusy:
        return "lock busy";
    case Fallback::SnapshotFailed:
        return "snapshot failed";
    case Fallback::Ineligible:
        return "ineligible path";
    case Fallback::Refused:
        return "refused";
    case Fallback::Killed:
        return "killed";
    case Fallback::kCount:
        break;
    }
    return "unknown";
}

void Controller::NoteUnseenChange(const char* source) {
    expect_valid_ = false;
    ++stats_.invalidated[source != nullptr && *source ? source : "unattributed"];
}

void Controller::Kill(Host& host, const std::string& why) {
    if (killed_) {
        return;
    }
    killed_ = true;
    kill_reason_ = why;
    ++stats_.kills;
    host.Log("SOTC PC POOL NATIVE KILLED: native execution is off for this session (no retry). " + why);
}

Route Controller::Run(Host& host, const Request& req, uint64_t* node) {
    *node = 0;
    ++stats_.calls;
    if (killed_) {
        ++stats_.fallback[static_cast<int>(Fallback::Killed)];
        return Route::Original;
    }
    if (!host.TakeLock()) {
        ++stats_.fallback[static_cast<int>(Fallback::LockBusy)];
        expect_valid_ = false;
        return Route::Original;
    }
    Pools pre;
    if (!host.Snapshot(&pre)) {
        host.ReleaseLock();
        ++stats_.fallback[static_cast<int>(Fallback::SnapshotFailed)];
        expect_valid_ = false;
        return Route::Original;
    }

    if (++since_check_ >= kCheckInterval) {
        since_check_ = 0;
        if (expect_valid_) {
            std::string details;
            uint32_t mask = 0;
            for (int s = 0; s < kSetCount; ++s) {
                mask |= CompareSet(expect_.set[s], pre.set[s], s, &details);
            }
            ++stats_.checks_full;
            if (mask != 0) {
                // sotc: not a kill. The game changes the sets outside the hooked wrapper (direct
                // 0x19e00 calls at 0x19c60/0x1a660, reserve-map, node refills), and every native
                // call re-reads the live state under the lock and verifies its own result, so a
                // stale expectation cannot lead to a wrong write. Counted and logged (first few).
                ++stats_.periodic_outside;
                if (stats_.periodic_outside <= 4) {
                    host.Log("SOTC PC POOL NATIVE: state changed outside native calls since the last one "
                             "(not a kill):\n" + details);
                }
                expect_valid_ = false;
            }
        } else {
            ++stats_.checks_skipped;
        }
    }

    Prediction pred;
    Pools predicted;
    host.Predict(pre, req, &pred, &predicted);
    if (!Eligible(pred, mask_)) {
        host.ReleaseLock();
        ++stats_.fallback[static_cast<int>(Fallback::Ineligible)];
        expect_valid_ = false;
        return Route::Original;
    }

    // Nothing is changed before this point. A refusal (0) has written nothing, so the original runs. The one
    // exception the native side documents: a write that fails in the middle leaves the earlier writes in place.
    // Then the state is unknown and the original would run on it, so the session is killed.
    const int path_index = static_cast<int>(pred.path);
    const uint64_t got_node = host.ApplyExactHit(pre, pred, req);
    if (got_node == 0) {
        if (!host.LastWriteSet().empty()) {
            Kill(host, "a native write failed in the middle of the sequence: the pool state is unknown\n");
        }
        host.ReleaseLock();
        ++stats_.fallback[static_cast<int>(Fallback::Refused)];
        ++stats_.refused_by_path[path_index];
        const char* reason = host.RefusalReason();
        ++stats_.refused_reason[std::string(PathName(pred.path)) + ": " + (reason != nullptr && *reason ? reason : "refused")];
        expect_valid_ = false;
        return Route::Original;
    }

    // The game's state has changed. From here the call returns natively and only the checks decide the session.
    ++stats_.native_calls;
    ++stats_.native_by_path[path_index];
    std::string details;
    bool ok = true;
    for (const auto& [addr, bytes] : host.LastWriteSet()) {
        if (!host.InModelledMemory(pre, addr, bytes)) {
            ok = false;
            char line[160];
            std::snprintf(line, sizeof line, "write outside the modelled memory: 0x%llx, %zu bytes\n",
                          static_cast<unsigned long long>(addr), bytes);
            details += line;
        }
    }
    if (!ok) {
        details += host.KillContext(); // sotc: which writer ran and the nodes it was allowed to touch
    }
    Observed got;
    got.node = got_node;
    if (!host.Observe(got_node, &got)) {
        ok = false;
        details += "post state unreadable\n";
    } else {
        int outside = 0;
        const uint32_t mask = Compare(pred, pre, predicted, got, &details, &outside);
        if (mask != 0) {
            ok = false;
        }
        if (outside != 0) {
            ok = false;
            details += "a set the prediction leaves untouched changed\n";
        }
    }
    host.Tail();
    host.ReleaseLock();

    if (ok) {
        ++stats_.verified;
        expect_ = got.post;
        expect_valid_ = true;
    } else {
        Kill(host, details);
        expect_valid_ = false;
    }
    *node = got_node;
    return Route::Native;
}

} // namespace Native

} // namespace Model
} // namespace SotcPcPool

#ifndef SOTC_PC_POOL_MODEL_ONLY
// ---------------------------------------------------------------------------------------------------
// Glue: reads the game's pool state through the backing view, runs the model in the PC_POOL_ALLOC
// wrap and in OnFreeNode, feeds the shadow VA map from the PC_POOL_MAP wrap (vmem_batch_map), and keeps
// the tallies. Built into the port only.
// The prediction is made in the pre phase (the map must be the one before the call: the call's own
// flush, inside the wrap, changes it). The fresh VA is only known in the post phase, where it is compared.
// Lock order: g_mutex (alloc hooks, report) before g_map_mutex. The batch hook takes only g_map_mutex and
// runs under the game's pool lock, so it never takes g_mutex and never takes the pool lock.
// ---------------------------------------------------------------------------------------------------
#include <xmmintrin.h> // _mm_pause
#include <cstdlib>
#include <mutex>

#include "hooks.h"
#include "pc_pool_native.h"         // SotcPcPoolNative::ApplyExactHit, ApplyNodeHit, LastWriteSet
#include "pc_pool_native_fresh.h"   // SotcPcPoolNativeFresh::ApplyFreshPlain / ApplyFreshSpill, FreshHost
#include "pc_pool_native_nodes.h"   // SotcPcPoolNodes::PredictNodeCall (AllocPre), the node-pool layout
#include "pc_pool_native_dispatch.h" // SotcPcPoolDispatch: writer dispatch, PredictCall, InModelled (native path)
#include "pc_pool_native_primary.h" // SotcPcPoolNative::ApplyPrimaryNodeHit, PrimaryLastWriteSet
#include "video_core/sotc_trace.h" // image bounds (SotcTrace::ImageBase/ImageSize)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
// runtime_memory.c: the reserve (sceKernelReserveVirtualRange) and batch map (sceKernelBatchMap) the game calls.
int32_t runtime_memory_reserve_range_export(void** inout, uint64_t size, int flags, uint64_t alignment);
int32_t runtime_memory_batch_map_export(void* entries, int count, int* processed);
}

namespace SotcPcPool {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;
using namespace Model;

// Image offsets (eboot VAs for the EU fingerprint; the expected addresses of scripts/hooks.d/pcpool.py).
constexpr uint64_t kRootGlobal = 0x2dfca78; // holds the pool root (root = S0)
using SotcPcPoolDispatch::Layout::kSetStride; // root, root+0xa0, root+0x140
constexpr uint64_t kPoolLockGlobal = 0x2dfc8a0; // the pool spin lock: 0 free, -1 held (spec 1.5)
constexpr uint64_t kLockSpins = 1u << 20;       // bounded: the handler gives up, it never deadlocks
constexpr uint64_t kMaxFreeRuns = 1u << 16;
constexpr uint64_t kMaxWords = 1u << 20;
constexpr int kLogLimit = 16;

// The batch queue (vmem_batch_map 0x1af80, spec 2.4): count i32 at root+0x2102f0, 64 entries of 32 bytes
// from root+0x2102f8. The phys base of bit 0 is the global at 0x2dfca70 (kPhysBaseGlobal, set by the bpe init).
using SotcPcPoolDispatch::Layout::kBatchCountOff;
using SotcPcPoolDispatch::Layout::kBatchEntriesOff;
using SotcPcPoolDispatch::Layout::kBatchMax;
using SotcPcPoolDispatch::Layout::kBatchEntryBytes;
constexpr uint64_t kPhysBaseGlobal = 0x2dfca70; ///< the phys base of bit 0 (eboot global, see kPhysBaseOffset)
constexpr uint64_t kVaSpace = uint64_t{1} << 48; ///< the whole address range, for "every VA is unknown"

// The node pool (pool_node_alloc 0x19660, pc_pool_native_nodes.cpp): sentinel element root+0x1e0, head link
// root+0x1f0, count i32 root+0x200. An element E holds the node N = [E] (E = N+0x20) and its next element at E+0x10.
using SotcPcPoolDispatch::Layout::kNodeCountOff;

// Node fields (block node, spec 1.3).
constexpr uint64_t kNodeBackedOff = 0x50;
constexpr uint64_t kNodeSizeOff = 0x58;
constexpr uint64_t kNodeBaseOff = 0x60;
constexpr uint64_t kNodeSlotOff = 0x68;

std::mutex g_mutex;

// The hook that announced a call or a change (one tag per hooked address, see Register). The block wrapper
// (0x19ae0) and the direct caller 0x19c60 take the wrapper's arguments; 0x1a660 takes the aligned form. The boot
// callers (0x18aa0 bpe init, 0x197d0 pool_init_flexible) change the sets without a block call: they only re-base.
enum class Source : uint8_t { Block, Simple, Aligned, BpeInit, FlexInit, kCount };
constexpr int kSourceCount = static_cast<int>(Source::kCount);
const char* SourceName(Source source) {
    static const char* const kNames[kSourceCount] = {"block 0x19ae0", "direct 0x19c60", "direct 0x1a660",
                                                     "boot bpe 0x18aa0", "boot flex 0x197d0"};
    return kNames[static_cast<int>(source)];
}
void* SourceTag(Source source) {
    static Source tags[kSourceCount] = {Source::Block, Source::Simple, Source::Aligned, Source::BpeInit,
                                        Source::FlexInit};
    return &tags[static_cast<int>(source)];
}

// Pending call between the pre and the post phase. The pre hook runs at wrapper entry, before the
// wrapper takes the game's pool lock, and the post hook after it releases it, so two threads can
// overlap. The state is per thread (pre and post of one call run on the same thread), and a call that
// another call entered or left during is not compared (its pre snapshot may be stale): counted apart.
struct CallState {
    bool active = false;
    Source src = Source::Block;
    uint64_t seq = 0;   ///< g_call_seq at this call's pre
    Request req;
    Pools pre;
    Prediction pred;    ///< predicted in the pre phase, with the map as it is before the call
    Pools predicted;
    uint64_t refill_block = 0; ///< the refill block PredictNodeCall named (0: none). The native path has its own copy.
};
thread_local CallState g_call;
uint64_t g_call_seq = 0;     ///< bumped (under g_mutex) at every pre and post
uint64_t g_overlapped = 0;   ///< calls skipped because another call overlapped them

// The shadow VA map (PhysMap) and the batch call between the pre and the post phase of vmem_batch_map.
// g_map_mutex guards both. Never held while calling the guest.
std::mutex g_map_mutex;
PhysMap g_map;
bool g_map_seeded = false;
struct BatchOp {
    uint64_t va = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    int32_t op = -1;
};
struct BatchCall {
    bool active = false;
    int count = 0;
    uint64_t phys_base = kPhysBaseOffset;
    BatchOp ops[kBatchMax];
};
BatchCall g_batch;

// The state after the last alloc (the baseline of the free checks) and the frees since then.
bool g_have_base = false;
Pools g_last_post;
FreeAcc g_acc[kSetCount];
uint64_t g_pending_frees = 0;

// Cumulative totals (never reset: the driver's report prints them every window).
uint64_t g_allocs = 0;
uint64_t g_frees = 0;
uint64_t g_free_unreadable = 0;
uint64_t g_matches = 0;
uint64_t g_mismatches = 0;
uint64_t g_would_crash = 0;
uint64_t g_unmodelled = 0;
uint64_t g_read_failed = 0;
uint64_t g_unseen_changes = 0; ///< pool state changed between two hooked allocs with no free in between
uint64_t g_outside_changes = 0; ///< sets the model leaves untouched that changed during an alloc (nested calls)
uint64_t g_lock_timeouts = 0;   ///< snapshots read without the pool lock (it was not free in time)
uint64_t g_field_hits[kFieldCount] = {};
uint64_t g_free_checks_ok = 0;
uint64_t g_free_checks_bad = 0;
uint64_t g_va_as_predicted[kSetCount] = {};
uint64_t g_va_unexpected[kSetCount] = {};
uint64_t g_unmodelled_unknown = 0; ///< unmodelled because the reserve-map path reached an unknown VA range
uint64_t g_batch_flushes = 0;      ///< vmem_batch_map calls with queued ops
uint64_t g_batch_ops = 0;          ///< ops applied to the map (map, unmap, protect type, ...)
uint64_t g_batch_failed = 0;       ///< flushes that returned an error (their map ops become unknown)
uint64_t g_batch_unreadable = 0;   ///< flushes whose queue could not be read (the whole map becomes unknown)
uint64_t g_seed_unknown = 0;       ///< first snapshots that were not pristine (S1/S2 free runs become unknown)
int g_logged_mismatch = 0;
int g_logged_crash = 0;
int g_logged_unmodelled = 0;
uint64_t g_path_allocs[kPathCount] = {};
uint64_t g_path_matches[kPathCount] = {};
uint64_t g_path_mismatches[kPathCount] = {};
uint64_t g_eligible_allocs = 0;     ///< predicted calls on a path in the mask (counted only: no native execution yet)
uint64_t g_eligible_mismatches = 0; ///< of those, the calls whose prediction did not match the game
uint64_t g_src_calls[kSourceCount] = {};      ///< calls (alloc sources) or announcements (boot sources) per source
uint64_t g_src_verified[kSourceCount] = {};   ///< verified calls per source (match)
uint64_t g_src_mismatch[kSourceCount] = {};   ///< mismatched calls per source
uint64_t g_src_unmodelled[kSourceCount] = {}; ///< unmodelled calls per source (the post state is re-based)
uint64_t g_src_rebase[kSourceCount] = {};     ///< boot announcements that re-based a clean base (a change seen)

// The two switches (read once). SOTC_PC_POOL=2 verifies (PC_POOL_ALLOC and PC_POOL_MAP wraps). SOTC_PC_POOL_NATIVE=1
// runs the eligible calls natively (PC_POOL_NATIVE, an entry hook at the same address as PC_POOL_ALLOC). Both at
// once are refused: the alloc hooks and the map hook are inert and one line says so. SOTC_PC_POOL=1 was reserved
// for native execution and is not a mode now.
bool WrapSwitch() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PC_POOL");
        if (v == nullptr || *v == 0) {
            return false;
        }
        const int value = std::atoi(v);
        if (value == 2) {
            return true;
        }
        if (value != 0) {
            std::fprintf(stderr,
                         "SOTC PC POOL: SOTC_PC_POOL=%s is not a mode (2 verifies; native is SOTC_PC_POOL_NATIVE=1): off\n",
                         v);
        }
        return false;
    }();
    return on;
}

bool NativeSwitch() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PC_POOL_NATIVE");
        if (v == nullptr || *v == 0) {
            return false;
        }
        const int value = std::atoi(v);
        if (value == 1) {
            return true;
        }
        if (value != 0) {
            std::fprintf(stderr, "SOTC PC POOL NATIVE: SOTC_PC_POOL_NATIVE=%s is not a mode (only 1): off\n", v);
        }
        return false;
    }();
    return on;
}

// Both switches set: refused, printed once.
bool Refused() {
    static const bool refused = [] {
        const bool both = WrapSwitch() && NativeSwitch();
        if (both) {
            std::fprintf(stderr,
                         "SOTC PC POOL: SOTC_PC_POOL=2 and SOTC_PC_POOL_NATIVE=1 are both set: refused, the alloc "
                         "hooks and the map hook are inert (one mode per session)\n");
        }
        return both;
    }();
    return refused;
}

// Verification (wrap) hooks active.
bool Enabled() { return WrapSwitch() && !Refused(); }

// Native execution active.
bool NativeActive() { return NativeSwitch() && !Refused(); }

// The shadow VA map is kept under either switch (PC_POOL_MAP under SOTC_PC_POOL=2, PC_POOL_MAP_NATIVE under
// SOTC_PC_POOL_NATIVE=1). MapHookOn says whether that batch hook actually installed: without it the map misses
// the game's ops, so Predict must not read it (the reserve-map path then stays Unmodelled).
bool MapActive() { return (WrapSwitch() || NativeSwitch()) && !Refused(); }
bool MapHookOn() {
    static const bool on = SotcHooks::IsInstalled(WrapSwitch() ? "PC_POOL_MAP" : "PC_POOL_MAP_NATIVE");
    return on;
}

// SOTC_PC_POOL_REFILL=1: a node pop that takes the pool count from 3 to 2 may refill natively (the 64 KiB refill
// through the nested exact hit, pc_pool_native_nodes.h). Off by default: such a pop is refused and the game runs the
// call, as before the switch existed. Read once.
bool RefillSwitch() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PC_POOL_REFILL");
        return v != nullptr && std::atoi(v) == 1;
    }();
    return on;
}

// SOTC_PC_POOL_PATHS: the paths eligible for native execution (default kDefaultPathMask). Read once.
uint32_t PathMask() {
    static const uint32_t mask = [] {
        std::string bad;
        const char* v = std::getenv("SOTC_PC_POOL_PATHS");
        const uint32_t m = v != nullptr ? ParsePathMask(v, &bad) : kDefaultPathMask;
        if (!bad.empty()) {
            std::fprintf(stderr, "SOTC PC POOL: unknown path names ignored: %s\n", bad.c_str());
        }
        return m;
    }();
    return mask;
}

// ---- guest memory -----------------------------------------------------------------------------

bool ReadGuest(uintptr_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || address + n > end) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

bool InImage(uintptr_t address, size_t n) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    return base != 0 && size != 0 && address >= base && n <= size && address - base <= size - n;
}

// Backing view first (pool bookkeeping lives in direct memory), then the mapped image.
bool ReadAny(uint64_t address, void* out, size_t n) {
    if (ReadGuest(static_cast<uintptr_t>(address), out, n)) {
        return true;
    }
    if (InImage(static_cast<uintptr_t>(address), n)) {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), n);
        return true;
    }
    return false;
}

bool ReadU64(uint64_t address, uint64_t* v) { return ReadAny(address, v, 8); }

bool ReadI32(uint64_t address, int32_t* v) { return ReadAny(address, v, 4); }

// Walks the free-VA list (spec 1.1; pool_take_free_run 0x1a800): E = [S+0x38] until E == S+0x28;
// node = [E]; base/size/backed at node+0x60/+0x58/+0x50; next E = [node+0x30].
bool ReadSet(uint64_t s, int expect_id, Set* out) {
    Set r;
    r.id = expect_id;
    r.has_bitmap = expect_id != 0;
    int32_t id = -1;
    if (!ReadI32(s + 0x78, &id) || id != expect_id) {
        return false;
    }
    int32_t v32 = 0;
    if (!ReadI32(s + 0x20, &r.region_count) || !ReadI32(s + 0x70, &r.free_va_pages) ||
        !ReadI32(s + 0x74, &r.cached_phys) || !ReadI32(s + 0x48, &r.stored_free_count)) {
        return false;
    }
    if (!ReadU64(s + 0x80, &r.alloc_bytes) || !ReadU64(s + 0x90, &r.backed_bytes)) {
        return false;
    }
    if (!ReadI32(s + 0x60, &v32)) {
        return false;
    }
    r.page_cap = static_cast<uint32_t>(v32);

    const uint64_t sentinel = s + 0x28;
    uint64_t element = 0;
    if (!ReadU64(s + 0x38, &element)) {
        return false;
    }
    while (element != sentinel) {
        if (r.free_va.size() >= kMaxFreeRuns) {
            return false;
        }
        uint64_t node = 0;
        if (!ReadU64(element, &node) || node == 0) {
            return false;
        }
        Run run;
        if (!ReadU64(node + kNodeBaseOff, &run.base) || !ReadU64(node + kNodeSizeOff, &run.size) ||
            !ReadU64(node + kNodeBackedOff, &run.backed)) {
            return false;
        }
        run.node = node;
        r.free_va.push_back(run);
        if (!ReadU64(node + 0x30, &element)) {
            return false;
        }
    }

    if (r.has_bitmap) {
        uint64_t words_ptr = 0;
        int32_t count = 0;
        if (!ReadU64(s + 0x50, &words_ptr) || !ReadI32(s + 0x58, &count)) {
            return false;
        }
        if (words_ptr == 0 || count <= 0 || static_cast<uint64_t>(count) > kMaxWords) {
            return false;
        }
        r.words.resize(static_cast<size_t>(count));
        if (!ReadAny(words_ptr, r.words.data(), r.words.size() * sizeof(uint64_t))) {
            return false;
        }
        r.words_addr = words_ptr;
    }
    *out = std::move(r);
    return true;
}

// The three sets: root (S0), root+0xa0 (S1), root+0x140 (S2). root is the image global 0x2dfca78.
bool ReadPools(Pools* out) {
    uint64_t root = 0;
    if (!ReadAny(SotcTrace::ImageBase() + kRootGlobal, &root, 8) || root == 0) {
        return false;
    }
    for (int i = 0; i < kSetCount; ++i) {
        if (!ReadSet(root + static_cast<uint64_t>(i) * kSetStride, i, &out->set[i])) {
            return false;
        }
    }
    return true;
}

// Takes the game's pool lock the way its own code does (lock cmpxchg 0 -> -1, spec 1.5). Returns the
// lock word, or nullptr when it was not free within kLockSpins tries (for example when this thread
// already holds it). The caller releases it with ReleaseGameLock.
uint64_t* TakeGameLock() {
    const uintptr_t address = static_cast<uintptr_t>(SotcTrace::ImageBase() + kPoolLockGlobal);
    if (!InImage(address, 8)) {
        return nullptr;
    }
    auto* word = reinterpret_cast<uint64_t*>(address);
    for (uint64_t spin = 0; spin < kLockSpins; ++spin) {
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, ~uint64_t{0}, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return word;
        }
        _mm_pause();
    }
    return nullptr;
}

void ReleaseGameLock(uint64_t* word) { __atomic_store_n(word, uint64_t{0}, __ATOMIC_RELEASE); }

// ReadPools with the game's pool lock held, so the snapshot is not torn by another thread's alloc or
// free. The lock is released before returning: the wrapper takes it itself. node_count (optional) gets the
// node pool count (i32 at root+0x200) read under the same lock.
bool ReadPoolsLocked(Pools* out, int32_t* node_count = nullptr) {
    uint64_t* word = TakeGameLock();
    if (word == nullptr) {
        ++g_lock_timeouts;
    }
    bool ok = ReadPools(out);
    if (ok && node_count != nullptr) {
        uint64_t root = 0;
        ok = ReadAny(SotcTrace::ImageBase() + kRootGlobal, &root, 8) && root != 0 &&
             ReadAny(root + kNodeCountOff, node_count, sizeof *node_count);
    }
    if (word != nullptr) {
        ReleaseGameLock(word);
    }
    return ok;
}

bool SamePools(const Pools& a, const Pools& b) {
    for (int i = 0; i < kSetCount; ++i) {
        if (!SameSet(a.set[i], b.set[i])) {
            return false;
        }
    }
    return true;
}

// ---- shadow VA map ----------------------------------------------------------------------------

// Seeds the map once, at the first observation (g_map_mutex held). Every mapping of pool memory goes through
// the batch queue, and the hook is installed before the game runs, so the map starts empty. A first snapshot
// with S1 or S2 already in use means pool activity the hook did not see: the free-VA runs of S1 and S2 (the
// only VA that vmem_query_reserve_map ever reads, since Onion and Garlic VA is consumed, never returned) become
// unknown. pools == nullptr: the first observation is a batch flush (no snapshot): the map is taken as empty.
void SeedMapLocked(const Pools* pools) {
    if (g_map_seeded) {
        return;
    }
    g_map_seeded = true;
    if (SeedMap(&g_map, pools)) {
        ++g_seed_unknown;
    }
}

// vmem_batch_map (0x1af80) pre phase, under the game's pool lock. Reads the queued ops (they are complete:
// the callers flush when the queue is full or at the end of their call) and keeps them for the post phase.
// Only guest memory is read; the pool lock is held by the caller and is not taken here.
HookAction BatchPre(GuestRegs&, void*) {
    if (!MapActive()) {
        return HookAction::Continue;
    }
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_batch = BatchCall{};
    SeedMapLocked(nullptr);

    uint64_t root = 0;
    int32_t count = 0;
    if (!ReadAny(SotcTrace::ImageBase() + kRootGlobal, &root, 8) || root == 0 ||
        !ReadI32(root + kBatchCountOff, &count) || count < 0 || count > kBatchMax) {
        // The ops cannot be seen: the whole address space is unknown until the game maps or unmaps a range.
        ++g_batch_unreadable;
        g_map.MarkUnknown(0, kVaSpace);
        return HookAction::Continue;
    }
    for (int i = 0; i < count; ++i) {
        unsigned char raw[kBatchEntryBytes];
        if (!ReadAny(root + kBatchEntriesOff + static_cast<uint64_t>(i) * kBatchEntryBytes, raw, sizeof raw)) {
            ++g_batch_unreadable;
            g_map.MarkUnknown(0, kVaSpace);
            return HookAction::Continue;
        }
        // BatchEntry (src/runtime_memory.c): u64 start, u64 offset, u64 length, u8 prot, u8 type, u16 reserved,
        // i32 operation.
        BatchOp& o = g_batch.ops[i];
        std::memcpy(&o.va, raw + 0, 8);
        std::memcpy(&o.offset, raw + 8, 8);
        std::memcpy(&o.length, raw + 16, 8);
        std::memcpy(&o.op, raw + 28, 4);
    }
    // The phys base of bit 0 (global 0x2dfca70, the same value the page loop and the reserve-map read).
    if (!ReadAny(SotcTrace::ImageBase() + kPhysBaseGlobal, &g_batch.phys_base, 8)) {
        ++g_batch_unreadable;
        g_map.MarkUnknown(0, kVaSpace);
        return HookAction::Continue;
    }
    g_batch.count = count;
    g_batch.active = count > 0;
    return HookAction::Continue;
}

// vmem_batch_map post phase. rax is the last error of the flush (0 when no entry failed). The game skips a
// failing entry and goes on, and the port's batch stops at it, so with an error the entries that change the
// mapping are unknown: the model cannot tell which of them took effect.
void BatchPost(GuestRegs& regs, void*) {
    std::lock_guard<std::mutex> lock(g_map_mutex);
    if (!g_batch.active) {
        return;
    }
    g_batch.active = false;
    ++g_batch_flushes;
    const bool failed = static_cast<uint32_t>(regs.rax) != 0;
    if (failed) {
        ++g_batch_failed;
    }
    for (int i = 0; i < g_batch.count; ++i) {
        const BatchOp& o = g_batch.ops[i];
        ApplyBatchOp(&g_map, o.va, o.offset, o.length, o.op, failed, g_batch.phys_base);
        ++g_batch_ops;
    }
}

// ---- hooks ------------------------------------------------------------------------------------

void ReportMismatch(const char* what, const CallState& c, const Prediction& p, uint32_t mask,
                    const std::string& details) {
    std::string fields;
    for (int i = 0; i < kFieldCount; ++i) {
        if (mask & (1u << i)) {
            if (!fields.empty()) {
                fields += ", ";
            }
            fields += FieldName(i);
        }
    }
    std::fprintf(stderr,
                 "SOTC PC POOL %s: kind %d size 0x%llx align 0x%llx pred %s va 0x%llx%s slot %d; fields: %s\n%s",
                 what, c.req.kind, static_cast<unsigned long long>(c.req.size),
                 static_cast<unsigned long long>(c.req.align), p.reason[0] ? p.reason : "(ok)",
                 static_cast<unsigned long long>(p.va), p.fresh ? " (fresh)" : "", p.slot, fields.c_str(),
                 details.c_str());
}

// Free checks: applied at the next alloc, when the state before it is known.
void CheckPendingFrees(const Pools& now) {
    if (g_pending_frees == 0) {
        return;
    }
    if (g_have_base) {
        for (int s = 0; s < kSetCount; ++s) {
            if (g_acc[s].frees == 0) {
                continue;
            }
            int64_t va_delta = 0;
            std::string details;
            const uint32_t mask = CheckFree(g_last_post.set[s], now.set[s], g_acc[s], &va_delta, &details);
            if (mask) {
                ++g_free_checks_bad;
                if (g_logged_mismatch < kLogLimit) {
                    ++g_logged_mismatch;
                    std::fprintf(stderr, "SOTC PC POOL FREE S%d: %llu frees; %s", s,
                                 static_cast<unsigned long long>(g_acc[s].frees), details.c_str());
                }
            } else {
                ++g_free_checks_ok;
            }
            if (mask & kFieldFreeVaPages) {
                ++g_va_unexpected[s];
            } else {
                ++g_va_as_predicted[s];
            }
        }
    }
    std::memset(g_acc, 0, sizeof g_acc);
    g_pending_frees = 0;
}

HookAction AllocPre(GuestRegs& regs, void* user) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_call = CallState{};
    if (!Enabled()) {
        return HookAction::Continue;
    }
    g_call.active = true;
    g_call.seq = ++g_call_seq;
    g_call.src = user != nullptr ? *static_cast<const Source*>(user) : Source::Block;
    if (g_call.src == Source::Aligned) {
        // pool_alloc_region_aligned (0x1a660, a wrap): rdi = obj, rsi = va_hint, rdx = node, rcx = size, r8 = mode,
        // r9 = align, the kind is the seventh argument at [rsp+8] (rsp points at the return address at entry).
        g_call.req.va_hint = regs.rsi;
        g_call.req.node = regs.rdx;
        g_call.req.size = regs.rcx;
        g_call.req.mode = static_cast<uint32_t>(regs.r8);
        g_call.req.align = static_cast<uint32_t>(regs.r9);
        int32_t kind = 0;
        if (!ReadAny(regs.rsp + 8, &kind, sizeof kind)) {
            ++g_read_failed;
            g_have_base = false;
            g_call.active = false;
            return HookAction::Continue;
        }
        g_call.req.kind = kind;
        g_call.req.obj = regs.rdi;
    } else {
        // Entry of pool_alloc_block_wrapper (0x19ae0), and 0x19c60 (the same arguments: rsi = size, edx = align,
        // ecx = kind). The wrapper calls pool_alloc_region with va_hint = 0, node = 0, mode = 0, passes align and
        // kind as 32-bit values (r9d, and the stack argument), and size as the full register.
        g_call.req.va_hint = 0;
        g_call.req.node = 0;
        g_call.req.mode = 0;
        g_call.req.size = regs.rsi;
        g_call.req.align = static_cast<uint32_t>(regs.rdx);
        g_call.req.kind = static_cast<int32_t>(regs.rcx);
    }

    int32_t node_count_pre = 0;
    if (!ReadPoolsLocked(&g_call.pre, &node_count_pre)) {
        ++g_read_failed;
        g_have_base = false;
        g_call.active = false;
        return HookAction::Continue;
    }
    // Changed since the last hooked alloc with no free seen in between: an allocation this hook does not
    // see (0x19c60 and 0x1a660 call 0x19e00 directly), or frees the free hook missed.
    if (g_have_base && g_pending_frees == 0 && !SamePools(g_last_post, g_call.pre)) {
        ++g_unseen_changes;
    }
    CheckPendingFrees(g_call.pre);
    // The prediction uses the map as it is before this call: the call's own flush (PC_POOL_MAP, inside the
    // wrap) changes the map after this point.
    // PredictNodeCall (not Predict): a FlexHitNode or PrimaryHitNode call pops a node, and when the pool count is 3
    // that pop refills through a nested 0x19e00. The prediction takes the refill from the count read above, so the
    // refill is predicted, not an unseen change. Other paths get Predict's answer unchanged.
    {
        std::lock_guard<std::mutex> map_lock(g_map_mutex);
        SeedMapLocked(&g_call.pre);
        SotcPcPoolNodes::PopPrediction pop;
        g_call.pred = SotcPcPoolNodes::PredictNodeCall(g_call.pre, g_call.req, &g_map, &g_call.predicted,
                                                       node_count_pre, &pop);
        g_call.refill_block = (pop.ok && pop.refill) ? pop.block_va : 0;
    }
    // A fresh block pops its node inside the call too (pool_alloc_region 0x19fbf -> pool_node_alloc 0x19660). The pop
    // changes no set field unless it refills (count 3 -> 2): the refill is a nested 64 KiB pool_alloc_region that
    // PredictNodeCall does not model on fresh paths. Such a call is unmodelled (re-based), not a mismatch.
    if (g_call.pred.outcome == Outcome::Predicted && FreshPopRefills(g_call.pred.path, node_count_pre)) {
        g_call.pred.outcome = Outcome::Unmodelled;
        g_call.pred.reason = "fresh block: its node pop refills the node pool (count 3 -> 2, nested 0x19e00)";
    }
    return HookAction::Continue;
}

void AllocPost(GuestRegs& regs, void*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_call.active) {
        return;
    }
    g_call.active = false;
    // Another call's pre or post ran between this call's pre and now: its snapshot may be stale.
    const bool overlapped = g_call_seq != g_call.seq;
    ++g_call_seq;
    if (overlapped) {
        ++g_overlapped;
        g_have_base = false; // the next call's unseen-change check has no clean base either
        return;
    }
    ++g_allocs;
    const int src = static_cast<int>(g_call.src);
    ++g_src_calls[src];
    const int path_index = static_cast<int>(g_call.pred.path);
    ++g_path_allocs[path_index];

    Observed got;
    got.node = regs.rax;
    if (got.node != 0) {
        uint8_t slot8 = 0xff;
        if (!ReadU64(got.node + kNodeBaseOff, &got.va) || !ReadU64(got.node + kNodeSizeOff, &got.size) ||
            !ReadAny(got.node + kNodeSlotOff, &slot8, 1)) {
            got.va = 0;
            got.size = 0;
        }
        got.slot = slot8;
    }
    // Wrap post: the wrapper has returned and released the pool lock, so the state is read under it again.
    const bool post_ok = ReadPoolsLocked(&got.post);
    if (!post_ok) {
        ++g_read_failed;
    }

    // The prediction was made in the pre phase (the map before the call). The fresh VA (if any) is known from
    // the node now. A null-node read or a failed post read leaves nothing to compare.
    const Prediction& p = g_call.pred;
    const Pools& predicted = g_call.predicted;
    switch (p.outcome) {
    case Outcome::Unmodelled:
        ++g_unmodelled;
        ++g_src_unmodelled[src];
        if (p.unknown_state) {
            ++g_unmodelled_unknown;
        }
        if (g_logged_unmodelled < kLogLimit) {
            ++g_logged_unmodelled;
            std::fprintf(stderr, "SOTC PC POOL unmodelled: kind %d size 0x%llx: %s\n", g_call.req.kind,
                         static_cast<unsigned long long>(g_call.req.size), p.reason);
        }
        break;
    case Outcome::WouldCrash:
        ++g_would_crash;
        if (g_logged_crash < kLogLimit) {
            ++g_logged_crash;
            std::fprintf(stderr, "SOTC PC POOL WOULD CRASH: kind %d size 0x%llx: %s\n", g_call.req.kind,
                         static_cast<unsigned long long>(g_call.req.size), p.reason);
        }
        break;
    case Outcome::Predicted:
    case Outcome::Null: {
        if (!post_ok) {
            break;
        }
        std::string details;
        int outside = 0;
        const uint32_t mask = Compare(p, g_call.pre, predicted, got, &details, &outside);
        g_outside_changes += static_cast<uint64_t>(outside);
        const bool eligible = Eligible(p, PathMask());
        if (eligible) {
            ++g_eligible_allocs;
        }
        if (mask == 0) {
            ++g_matches;
            ++g_path_matches[path_index];
            ++g_src_verified[src];
        } else {
            ++g_mismatches;
            ++g_src_mismatch[src];
            ++g_path_mismatches[path_index];
            if (eligible) {
                ++g_eligible_mismatches;
            }
            for (int i = 0; i < kFieldCount; ++i) {
                if (mask & (1u << i)) {
                    ++g_field_hits[i];
                }
            }
            if (g_logged_mismatch < kLogLimit) {
                ++g_logged_mismatch;
                ReportMismatch("MISMATCH", g_call, p, mask, details);
            }
        }
        break;
    }
    }

    if (post_ok && p.outcome != Outcome::WouldCrash) {
        g_last_post = got.post;
        g_have_base = true;
    } else {
        g_have_base = false;
    }
}

} // namespace

namespace {

// ---- native execution (SOTC_PC_POOL_NATIVE=1, PC_POOL_NATIVE) -----------------------------------------
// The entry hook at pool_alloc_block_wrapper (0x19ae0). It takes the game's pool lock the way the wrapper does
// (lock order: g_mutex, then the pool lock, then g_map_mutex), and hands the call to Native::Controller, which
// decides, runs and verifies it (Model::Native). Returns HookAction::Return with the node for a native call;
// otherwise HookAction::Continue and the wrapper runs as it would without the hook.
constexpr uint64_t kLowPeakGlobal = 0x2dfca58;   ///< min of the 0x110/0x70/0x1b0 sum (wrapper 0x19bd2..0x19bdb)
constexpr uint64_t kHighPeakGlobal = 0x2dfca50;  ///< max of the 0x130/0x90/0x1d0 sum (wrapper 0x19bf7..0x19c00)

// The game's uint64 field sum for the peak counters (wrapper 0x19b8a..0x19bcf): each 64-bit value is folded to
// (hi32 + lo32) mod 2^32 shifted left 16 bits, masked to bits 16..47, and the three folds are added.
uint64_t PeakFold(uint64_t value) {
    const uint32_t folded = static_cast<uint32_t>(value >> 32) + static_cast<uint32_t>(value);
    return (static_cast<uint64_t>(folded) << 16) & 0xffffffff0000ull;
}

bool ReadRoot(uint64_t* root) {
    *root = 0;
    return ReadAny(SotcTrace::ImageBase() + kRootGlobal, root, 8) && *root != 0;
}

// Guest memory for SotcPcPoolNative: reads through the backing view (as ReadGuest), writes the same way; image
// globals are written in place.
class PoolGuestMem final : public SotcPcPoolNative::GuestMem {
public:
    bool Read(uint64_t address, void* out, size_t n) override { return ReadAny(address, out, n); }
    // sotc: same order as ReadAny (mapped guest memory through its backing view first, then the
    // image), so a write lands where the writer's checking read came from.
    bool Write(uint64_t address, const void* data, size_t n) override {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (runtime_memory_region(static_cast<uintptr_t>(address), &start, &end, &mapped) && mapped &&
            address >= start && address + n <= end) {
            unsigned char* p =
                const_cast<unsigned char*>(runtime_memory_backing_pointer(static_cast<uintptr_t>(address)));
            if (p != nullptr) {
                std::memcpy(p, data, n);
                return true;
            }
        }
        if (InImage(static_cast<uintptr_t>(address), n)) {
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), data, n);
            return true;
        }
        return false;
    }
};

// The FreshHost of the native fresh path: the VA reservation and the batch flush the game makes, through the
// runtime exports. The flush also feeds the shadow VA map (g_map) with the same ops the PC_POOL_MAP wrap applies
// to the game's flush. g_map is kept under SOTC_PC_POOL_NATIVE=1 too (PC_POOL_MAP_NATIVE), and Predict reads it.
class PoolFreshHost final : public SotcPcPoolNativeFresh::FreshHost {
public:
    uint64_t ReserveVirtual(uint64_t size, uint64_t align) override {
        void* va = nullptr;
        if (runtime_memory_reserve_range_export(&va, size, 0, align) != 0) {
            return 0;
        }
        return reinterpret_cast<uint64_t>(va);
    }

    int32_t BatchMap(const SotcPcPoolNativeFresh::BatchEntry* entries, size_t count) override {
        int processed = 0;
        const int32_t error = runtime_memory_batch_map_export(
            const_cast<void*>(static_cast<const void*>(entries)), static_cast<int>(count), &processed);
        uint64_t phys_base = kPhysBaseOffset;
        const bool have_base = ReadAny(SotcTrace::ImageBase() + kPhysBaseGlobal, &phys_base, 8);
        std::lock_guard<std::mutex> map_lock(g_map_mutex);
        if (!have_base) {
            g_map.MarkUnknown(0, kVaSpace);
            return error;
        }
        for (size_t i = 0; i < count; ++i) {
            const SotcPcPoolNativeFresh::BatchEntry& e = entries[i];
            ApplyBatchOp(&g_map, e.va, e.offset, e.length, e.op, error != 0, phys_base);
        }
        return error;
    }
};

// The glue side of Native::Host. One instance per call; the controller holds the game's pool lock while it runs.
class NativeHost final : public Native::Host {
public:
    bool TakeLock() override {
        lock_word_ = TakeGameLock();
        if (lock_word_ == nullptr) {
            ++g_lock_timeouts;
        }
        return lock_word_ != nullptr;
    }

    void ReleaseLock() override {
        if (lock_word_ != nullptr) {
            ReleaseGameLock(lock_word_);
        }
        lock_word_ = nullptr;
    }

    bool Snapshot(Pools* out) override { return ReadRoot(&root_) && ReadPools(out); }

    // The shadow VA map is passed when its batch hook is installed (MapHookOn), seeded from pre at the first use. The
    // map is read under g_map_mutex, which the game's flush takes under the pool lock too, so the order is the same
    // (pool lock, then map). Without the hook the reserve-map path is Unmodelled, as before.
    // With SOTC_PC_POOL_REFILL=1 the node paths are predicted with their node pop's refill (PredictNodeCall); the
    // refill block is kept for the writer's modelled-memory check. Without it this is Model::Predict, as before.
    void Predict(const Pools& pre, const Request& req, Prediction* p, Pools* predicted) override {
        const bool refill = RefillSwitch();
        int32_t count_pre = 0;
        if (refill) {
            ReadAny(root_ + kNodeCountOff, &count_pre, sizeof count_pre);
        }
        uint64_t refill_block = 0;
        if (!MapHookOn()) {
            *p = SotcPcPoolDispatch::PredictCall(pre, req, nullptr, predicted, count_pre, refill, &refill_block);
        } else {
            std::lock_guard<std::mutex> map_lock(g_map_mutex);
            SeedMapLocked(&pre);
            *p = SotcPcPoolDispatch::PredictCall(pre, req, &g_map, predicted, count_pre, refill, &refill_block);
        }
        st_.refill_block = refill_block;
    }

    // sotc: diagnostic text for a kill (the writer that ran, its touched nodes).
    std::string KillContext() override {
        std::string out = "  writer " + std::string(SotcPcPoolDispatch::WriterName(st_.writer)) + ", touched nodes:";
        char buf[32];
        for (uint64_t node : SotcPcPoolNative::TouchedNodes()) {
            std::snprintf(buf, sizeof buf, " 0x%llx", static_cast<unsigned long long>(node));
            out += buf;
        }
        out += ", pool nodes collected " + std::to_string(st_.pool_nodes.size()) + "\n";
        return out;
    }

    const char* RefusalReason() override { return SotcPcPoolDispatch::RefusalReason(st_); }

    // Dispatch by the predicted path (the opt-in paths of SOTC_PC_POOL_PATHS; the default mask is the two exact
    // hits). The writers are SotcPcPoolDispatch::Apply: each refuses with 0 and nothing written what is not its path,
    // so a path with no writer there (EarlyReject, WouldCrash, Unmodelled) is refused. The fresh writers get the shadow
    // PhysMap when its batch hook is installed (a reserve-map step needs it; without it they refuse UnknownMap). They
    // read g_map under the game pool lock without g_map_mutex: BatchMap takes g_map_mutex itself, so it must not be
    // held across the call.
    uint64_t ApplyExactHit(const Pools& pre, const Prediction& p, const Request& req) override {
        PoolGuestMem mem;
        st_.root = root_;
        st_.refill = RefillSwitch();
        st_.overhead_addr = SotcPcPoolNodes::OverheadAddr(SotcTrace::ImageBase());
        st_.map = FreshMap();
        return SotcPcPoolDispatch::Apply(mem, fresh_host_, &st_, pre, p, req);
    }

    std::vector<std::pair<uint64_t, size_t>> LastWriteSet() override { return SotcPcPoolDispatch::LastWriteSet(st_); }

    // Owned by the model in pre (the list is in SotcPcPoolDispatch::InModelled).
    bool InModelledMemory(const Pools& pre, uint64_t addr, size_t n) override {
        return SotcPcPoolDispatch::InModelled(st_, pre, addr, n);
    }

    bool Observe(uint64_t node, Observed* got) override {
        uint8_t slot8 = 0xff;
        if (!ReadU64(node + kNodeBaseOff, &got->va) || !ReadU64(node + kNodeSizeOff, &got->size) ||
            !ReadAny(node + kNodeSlotOff, &slot8, 1)) {
            return false;
        }
        got->slot = slot8;
        return ReadPools(&got->post);
    }

    // vmem_batch_map is not called by an exact hit; the peak counters are the wrapper's tail (0x19bae..0x19c00).
    // Both are written only when the game's own compare would write them (min for the low sum, max for the high).
    void Tail() override {
        uint64_t root = 0;
        if (!ReadRoot(&root)) {
            return;
        }
        uint64_t v110 = 0, v70 = 0, v1b0 = 0, v130 = 0, v90 = 0, v1d0 = 0;
        if (!ReadU64(root + 0x110, &v110) || !ReadU64(root + 0x70, &v70) || !ReadU64(root + 0x1b0, &v1b0) ||
            !ReadU64(root + 0x130, &v130) || !ReadU64(root + 0x90, &v90) || !ReadU64(root + 0x1d0, &v1d0)) {
            return;
        }
        const uint64_t low_sum = PeakFold(v110) + PeakFold(v70) + PeakFold(v1b0);
        const uint64_t high_sum = v130 + v90 + v1d0;
        PoolGuestMem mem;
        const uint64_t low_addr = SotcTrace::ImageBase() + kLowPeakGlobal;
        const uint64_t high_addr = SotcTrace::ImageBase() + kHighPeakGlobal;
        uint64_t current = 0;
        if (ReadU64(low_addr, &current) && low_sum < current) {
            mem.Write(low_addr, &low_sum, 8);
        }
        if (ReadU64(high_addr, &current) && high_sum > current) {
            mem.Write(high_addr, &high_sum, 8);
        }
    }

    void Log(const std::string& text) override { std::fprintf(stderr, "%s\n", text.c_str()); }

private:
    // sotc: the shadow PhysMap for the fresh writers (null when its batch hook is not installed).
    static const PhysMap* FreshMap() { return MapHookOn() ? &g_map : nullptr; }

    uint64_t root_ = 0;
    uint64_t* lock_word_ = nullptr;
    SotcPcPoolDispatch::State st_; ///< the call's dispatch state (pc_pool_native_dispatch.h)
    PoolFreshHost fresh_host_;
};

Native::Controller& NativeController() {
    static Native::Controller controller(PathMask());
    return controller;
}

// The direct callers (0x19c60, 0x1a660) and the boot callers (0x18aa0, 0x197d0). Verify (SOTC_PC_POOL=2): a block
// call takes the same pre/post pair as the wrapper. Native (SOTC_PC_POOL_NATIVE=1): the call is not verified; it is
// announced to the controller, so its change is attributed to its source and the next checkpoint re-bases.
HookAction DirectPre(GuestRegs& regs, void* user) {
    if (Refused()) {
        return HookAction::Continue;
    }
    if (WrapSwitch()) {
        return AllocPre(regs, user);
    }
    if (NativeSwitch()) {
        const Source src = *static_cast<const Source*>(user);
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_src_calls[static_cast<int>(src)];
        if (NativeActive()) {
            NativeController().NoteUnseenChange(SourceName(src));
        }
    }
    return HookAction::Continue;
}

void DirectPost(GuestRegs& regs, void* user) {
    if (Enabled()) {
        AllocPost(regs, user);
    }
}

// The normal exit of pool_alloc_region_aligned (0x1a7db, after the pool lock is released): the post phase of the entry
// pair (DirectPre). The node is r14 at this point (the displaced instruction loads rax from it). The handler works on a
// copy of the registers, so the guest's rax is untouched.
HookAction AlignedExit(GuestRegs& regs, void* user) {
    if (Enabled()) {
        GuestRegs node = regs;
        node.rax = regs.r14;
        AllocPost(node, user);
    }
    return HookAction::Continue;
}

// The boot callers: bpe init (0x18aa0, with its two pool_node_alloc sites) and pool_init_flexible (0x197d0). They
// change the sets once, at boot. An entry hook re-bases: the next verified call has no clean base to count an unseen
// change against, and the native expectation is voided with the source named.
HookAction BootPre(GuestRegs&, void* user) {
    if (Refused()) {
        return HookAction::Continue;
    }
    const Source src = *static_cast<const Source*>(user);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_src_calls[static_cast<int>(src)];
    if (g_have_base) {
        g_have_base = false;
        ++g_src_rebase[static_cast<int>(src)];
    }
    if (NativeActive()) {
        NativeController().NoteUnseenChange(SourceName(src));
    }
    return HookAction::Continue;
}

// The PC_POOL_NATIVE handler (entry at 0x19ae0; rsi = size, edx = align, ecx = kind; rdi = owner, not modelled).
HookAction NativeEntry(GuestRegs& regs, void*) {
    if (!NativeActive()) {
        return HookAction::Continue;
    }
    // The controller needs the frees: without the FREE_BLOCK hook a free would be invisible and every checkpoint
    // would read as a kill. Off instead, one line.
    static const bool free_hooked = SotcHooks::IsInstalled("FREE_BLOCK");
    if (!free_hooked) {
        static const bool logged = [] {
            std::fprintf(stderr, "SOTC PC POOL NATIVE: FREE_BLOCK hook is off (SOTC_HOOK_ALLOC_PROBE): native execution off\n");
            return true;
        }();
        (void)logged;
        return HookAction::Continue;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Request req;
    req.va_hint = 0;
    req.node = 0;
    req.mode = 0;
    req.size = regs.rsi;
    req.align = static_cast<uint32_t>(regs.rdx);
    req.kind = static_cast<int32_t>(regs.rcx);
    req.obj = regs.rdi;
    NativeHost host;
    uint64_t node = 0;
    if (NativeController().Run(host, req, &node) == Native::Route::Native) {
        regs.rax = node;
        return HookAction::Return;
    }
    return HookAction::Continue;
}

void ReportNative(std::string& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Native::Controller& c = NativeController();
    const Native::Stats& s = c.stats();
    char line[640];
    std::snprintf(line, sizeof line,
                  "SOTC PC POOL NATIVE: %s, calls %llu, native %llu, verified %llu, kills %llu, periodic checks full %llu "
                  "skipped %llu\n",
                  c.killed() ? "KILLED (off for the session)" : "on",
                  static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.native_calls),
                  static_cast<unsigned long long>(s.verified), static_cast<unsigned long long>(s.kills),
                  static_cast<unsigned long long>(s.checks_full), static_cast<unsigned long long>(s.checks_skipped));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC PC POOL NATIVE: fallbacks to the original: lock busy %llu, snapshot failed %llu, ineligible %llu, "
                  "refused %llu, after kill %llu\n",
                  static_cast<unsigned long long>(s.fallback[static_cast<int>(Native::Fallback::LockBusy)]),
                  static_cast<unsigned long long>(s.fallback[static_cast<int>(Native::Fallback::SnapshotFailed)]),
                  static_cast<unsigned long long>(s.fallback[static_cast<int>(Native::Fallback::Ineligible)]),
                  static_cast<unsigned long long>(s.fallback[static_cast<int>(Native::Fallback::Refused)]),
                  static_cast<unsigned long long>(s.fallback[static_cast<int>(Native::Fallback::Killed)]));
    out += line;
    // Per path: native calls / writer refusals (the calls that ran the original). Every path is listed, so a path
    // outside SOTC_PC_POOL_PATHS shows as 0/0 and a refusal shows as n/m.
    std::string per_path = "SOTC PC POOL NATIVE paths (native/refused):";
    for (int i = 0; i < kPathCount; ++i) {
        char item[96];
        std::snprintf(item, sizeof item, " %s %llu/%llu", PathName(static_cast<Path>(i)),
                      static_cast<unsigned long long>(s.native_by_path[i]),
                      static_cast<unsigned long long>(s.refused_by_path[i]));
        per_path += item;
    }
    out += per_path + "\n";
    std::snprintf(line, sizeof line, "SOTC PC POOL NATIVE: changed outside native calls (periodic checkpoints) %llu\n",
                  static_cast<unsigned long long>(s.periodic_outside));
    out += line;
    std::string invalidated = "SOTC PC POOL NATIVE: expectation voided by:";
    for (const auto& [source, count] : s.invalidated) {
        char item[128];
        std::snprintf(item, sizeof item, " %s %llu;", source.c_str(), static_cast<unsigned long long>(count));
        invalidated += item;
    }
    out += invalidated + "\n";
    for (const auto& [reason, count] : s.refused_reason) {
        std::snprintf(line, sizeof line, "SOTC PC POOL NATIVE refused %s: %llu\n", reason.c_str(),
                      static_cast<unsigned long long>(count));
        out += line;
    }
}

} // namespace

void Register() {
    // Refusal of both switches is printed here, once, at install time.
    (void)Refused();
    if (NativeSwitch() && RefillSwitch()) {
        std::fprintf(stderr, "SOTC PC POOL NATIVE: node refills on (SOTC_PC_POOL_REFILL=1)\n");
    }

    SotcHooks::Handler h;
    h.pre = &AllocPre;
    h.post = &AllocPost;
    SotcHooks::RegisterHandler("PC_POOL_ALLOC", h);

    SotcHooks::Handler m;
    m.pre = &BatchPre;
    m.post = &BatchPost;
    SotcHooks::RegisterHandler("PC_POOL_MAP", m);

    // Native mode's copy of the map hook (same handlers; scripts/hooks.d/pcpool.py PC_POOL_MAP_NATIVE).
    SotcHooks::RegisterHandler("PC_POOL_MAP_NATIVE", m);

    // Direct and boot callers (scripts/hooks.d/pcpool.py). Each name carries its source tag as the handler's user.
    // Role: Wrap (pre and post, the block pair), Entry (pre only: the aligned call's arguments and snapshot), Exit
    // (the post of the Entry pair, at the normal exit of 0x1a660), Boot (entry, re-base only).
    enum class Role : uint8_t { Wrap, Entry, Exit, Boot };
    struct SourcedHook {
        const char* name;
        Source source;
        Role role;
    };
    static const SourcedHook kSourced[] = {
        {"PC_POOL_SIMPLE", Source::Simple, Role::Wrap},
        {"PC_POOL_SIMPLE_NATIVE", Source::Simple, Role::Wrap},
        {"PC_POOL_ALIGNED", Source::Aligned, Role::Entry},
        {"PC_POOL_ALIGNED_NATIVE", Source::Aligned, Role::Entry},
        {"PC_POOL_ALIGNED_EXIT", Source::Aligned, Role::Exit},
        {"PC_POOL_ALIGNED_EXIT_NATIVE", Source::Aligned, Role::Exit},
        {"PC_POOL_BPE_INIT", Source::BpeInit, Role::Boot},
        {"PC_POOL_BPE_INIT_NATIVE", Source::BpeInit, Role::Boot},
        {"PC_POOL_FLEX_INIT", Source::FlexInit, Role::Boot},
        {"PC_POOL_FLEX_INIT_NATIVE", Source::FlexInit, Role::Boot},
    };
    for (const SourcedHook& hook : kSourced) {
        SotcHooks::Handler sourced;
        sourced.user = SourceTag(hook.source);
        switch (hook.role) {
        case Role::Wrap:
            sourced.pre = &DirectPre;
            sourced.post = &DirectPost;
            break;
        case Role::Entry:
            sourced.pre = &DirectPre;
            break;
        case Role::Exit:
            sourced.pre = &AlignedExit;
            break;
        case Role::Boot:
            sourced.pre = &BootPre;
            break;
        }
        SotcHooks::RegisterHandler(hook.name, sourced);
    }

    SotcHooks::Handler n;
    n.pre = &NativeEntry;
    SotcHooks::RegisterHandler("PC_POOL_NATIVE", n);
}

void OnFreeNode(uint64_t node) {
    if (node == 0) {
        return;
    }
    if (NativeActive()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        NativeController().NoteUnseenChange("free");
    }
    if (!Enabled()) {
        return;
    }
    uint8_t slot8 = 0xff;
    uint64_t size = 0, backed = 0;
    const bool ok = ReadAny(node + kNodeSlotOff, &slot8, 1) && ReadU64(node + kNodeSizeOff, &size) &&
                    ReadU64(node + kNodeBackedOff, &backed);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_frees;
    if (!ok || slot8 >= kSetCount) {
        ++g_free_unreadable;
        return;
    }
    AddFree(g_acc, slot8, size, backed);
    ++g_pending_frees;
}

void Report(std::string& out, double seconds) {
    (void)seconds;
    if (NativeActive()) {
        ReportNative(out);
    }
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[640];
    std::snprintf(line, sizeof line,
                  "SOTC PC POOL: allocs %llu, frees %llu, matches %llu, mismatches %llu, would-crash %llu\n",
                  static_cast<unsigned long long>(g_allocs), static_cast<unsigned long long>(g_frees),
                  static_cast<unsigned long long>(g_matches), static_cast<unsigned long long>(g_mismatches),
                  static_cast<unsigned long long>(g_would_crash));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC PC POOL: overlapped calls (not compared) %llu, unmodelled %llu, read failed %llu, "
                  "unseen state changes %llu, outside-set changes %llu, lock timeouts %llu, free checks ok %llu bad %llu (unreadable frees %llu); "
                  "free VA as predicted S0 %llu S1 %llu S2 %llu, unexpected S0 %llu S1 %llu S2 %llu\n",
                  static_cast<unsigned long long>(g_overlapped),
                  static_cast<unsigned long long>(g_unmodelled), static_cast<unsigned long long>(g_read_failed),
                  static_cast<unsigned long long>(g_unseen_changes), static_cast<unsigned long long>(g_outside_changes),
                  static_cast<unsigned long long>(g_lock_timeouts), static_cast<unsigned long long>(g_free_checks_ok),
                  static_cast<unsigned long long>(g_free_checks_bad), static_cast<unsigned long long>(g_free_unreadable),
                  static_cast<unsigned long long>(g_va_as_predicted[0]),
                  static_cast<unsigned long long>(g_va_as_predicted[1]),
                  static_cast<unsigned long long>(g_va_as_predicted[2]),
                  static_cast<unsigned long long>(g_va_unexpected[0]),
                  static_cast<unsigned long long>(g_va_unexpected[1]),
                  static_cast<unsigned long long>(g_va_unexpected[2]));
    out += line;
    {
        std::lock_guard<std::mutex> map_lock(g_map_mutex);
        std::snprintf(line, sizeof line,
                      "SOTC PC POOL: map flushes %llu (failed %llu, unreadable %llu), ops %llu, known runs %zu, "
                      "unknown ranges %zu, first snapshot not pristine %llu; unmodelled with map unknown %llu\n",
                      static_cast<unsigned long long>(g_batch_flushes), static_cast<unsigned long long>(g_batch_failed),
                      static_cast<unsigned long long>(g_batch_unreadable), static_cast<unsigned long long>(g_batch_ops),
                      g_map.MappedRuns(), g_map.UnknownRuns(), static_cast<unsigned long long>(g_seed_unknown),
                      static_cast<unsigned long long>(g_unmodelled_unknown));
        out += line;
    }
    for (int i = 0; i < kPathCount; ++i) {
        if (g_path_allocs[i] == 0) {
            continue;
        }
        const Path path = static_cast<Path>(i);
        std::snprintf(line, sizeof line,
                      "SOTC PC POOL path %s: allocs %llu, matches %llu, mismatches %llu, eligible %s\n",
                      PathName(path), static_cast<unsigned long long>(g_path_allocs[i]),
                      static_cast<unsigned long long>(g_path_matches[i]),
                      static_cast<unsigned long long>(g_path_mismatches[i]),
                      (PathMask() & PathBit(path)) != 0 ? "yes" : "no");
        out += line;
    }
    std::snprintf(line, sizeof line,
                  "SOTC PC POOL: eligible calls %llu, of them mismatched %llu (counted only: SOTC_PC_POOL=2 verifies, no native execution)\n",
                  static_cast<unsigned long long>(g_eligible_allocs),
                  static_cast<unsigned long long>(g_eligible_mismatches));
    out += line;
    // Per source: calls (or boot announcements), verified, mismatched, unmodelled (re-based), and the boot re-bases.
    for (int i = 0; i < kSourceCount; ++i) {
        std::snprintf(line, sizeof line,
                      "SOTC PC POOL source %s: calls %llu, verified %llu, mismatched %llu, unmodelled %llu, re-based %llu\n",
                      SourceName(static_cast<Source>(i)), static_cast<unsigned long long>(g_src_calls[i]),
                      static_cast<unsigned long long>(g_src_verified[i]),
                      static_cast<unsigned long long>(g_src_mismatch[i]),
                      static_cast<unsigned long long>(g_src_unmodelled[i]),
                      static_cast<unsigned long long>(g_src_rebase[i]));
        out += line;
    }
    if (g_mismatches) {
        std::string fields;
        for (int i = 0; i < kFieldCount; ++i) {
            if (g_field_hits[i]) {
                std::snprintf(line, sizeof line, "%s%s %llu", fields.empty() ? "" : ", ", FieldName(i),
                              static_cast<unsigned long long>(g_field_hits[i]));
                fields += line;
            }
        }
        out += "SOTC PC POOL: mismatch fields: " + fields + "\n";
    }
}

} // namespace SotcPcPool
#endif // SOTC_PC_POOL_MODEL_ONLY
