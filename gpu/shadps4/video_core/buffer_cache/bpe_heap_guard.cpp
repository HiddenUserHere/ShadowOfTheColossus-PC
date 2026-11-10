// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

#include "core/memory.h"
#include "video_core/buffer_cache/bpe_heap_guard.h"
#include "video_core/sotc_stats.h"

extern "C" uint64_t runtime_memory_generation(void);
extern "C" uintptr_t runtime_sotc_heap(void);       // probe.c: the heap the readback guard uses
extern "C" uintptr_t runtime_sotc_heap_table(void); // probe.c: the engine's heap table

namespace VideoCore {

namespace {
// Block header: [0] previous physical block, [8] size | flags (bits 0-2). Free blocks carry tree links after
// the header: left, right, equal-size list (two links), parent | color
constexpr u64 HeaderSize = 0x10;
constexpr u64 FreeNodeSize = 0x38;
constexpr u64 SizeMask = ~u64{7};
// The heap hands out GPU-mapped direct memory above 4 GiB (seen up to ~0x4f6 GiB); anything outside user
// space is garbage. Reads through the backing reject unmapped addresses
constexpr VAddr HeapPointerLo = 0x100000000ULL;
constexpr VAddr HeapPointerHi = 0x10000000000ULL;
constexpr u64 MaxBlockSize = 0x100000000ULL;
constexpr size_t MaxTreeNodes = 1 << 16;
constexpr size_t MaxListNodes = 1 << 10;
constexpr size_t MaxChainSteps = 1 << 12;
constexpr u64 InUse = 1; // size | flags bit 0: set on allocated blocks, clear on free ones
constexpr size_t MaxUsedBlocks = 1 << 12;

bool IsPieceAligned(VAddr addr) {
    return addr >= HeapPointerLo && addr < HeapPointerHi && (addr & ((1ULL << 20) - 1)) == 0;
}

// sotc: the walk's set of nodes and ring members seen (open addressing on the draw thread, kept
// between builds: about 500 builds per 5 s at an area change, ~7000 nodes each)
class SeenSet {
public:
    void Reset(size_t expected) {
        size_t capacity = 1024;
        while (capacity < expected * 2) {
            capacity <<= 1;
        }
        if (table.size() != capacity) {
            table.assign(capacity, 0);
        } else {
            std::fill(table.begin(), table.end(), 0);
        }
        count = 0;
    }
    /// false when key (never 0) was already in
    bool Insert(u64 key) {
        if ((count + 1) * 2 > table.size()) {
            Grow();
        }
        const size_t mask = table.size() - 1;
        for (size_t i = (key * 0x9E3779B97F4A7C15ULL) >> 32 & mask;; i = (i + 1) & mask) {
            if (table[i] == key) {
                return false;
            }
            if (!table[i]) {
                table[i] = key;
                ++count;
                return true;
            }
        }
    }

private:
    void Grow() {
        std::vector<u64> old;
        old.swap(table);
        table.assign(old.size() * 2, 0);
        count = 0;
        for (const u64 key : old) {
            if (key) {
                Insert(key);
            }
        }
    }
    std::vector<u64> table;
    size_t count = 0;
};
thread_local SeenSet seen;
thread_local size_t last_seen = 0;
} // namespace

// sotc: SOTC_HEAP_GUARD_RUNS=1 (off by default). A guard is built again whenever its heap's free
// tree changed (~100 times a second while an area streams), and each build resolved the backing run
// of every node it read again (Read). The runs only change with the guest memory map: the last
// build's runs are handed to the next one while the runtime's table generation is the same (even:
// no writer), on the thread that builds the guards (the draw thread).
static bool GuardRunsCacheEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_HEAP_GUARD_RUNS");
        return v && v[0] == '1';
    }();
    return enabled;
}

BpeHeapGuard::BpeHeapGuard(Core::MemoryManager* memory_, VAddr heap_)
    : memory{memory_}, heap{heap_} {
    struct RunsCache {
        u64 generation = 1; // odd: empty
        std::vector<Run> runs;
    };
    thread_local RunsCache cache;
    const bool reuse = GuardRunsCacheEnabled();
    const u64 generation = reuse ? runtime_memory_generation() : 1;
    if (reuse && !(generation & 1) && cache.generation == generation) {
        runs = cache.runs;
    }
    u64 heap_size = 0;
    if (!Read(heap + 0x30, &heap_size, sizeof(heap_size)) || heap_size < 0x1000000 ||
        heap_size > MaxBlockSize) {
        return;
    }
    valid = true;
    const auto start = std::chrono::steady_clock::now();
    const size_t adds = CollectFreeTree();
    const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    SotcStats::heap_guard_ns.fetch_add(ns, std::memory_order_relaxed);
    if (ns > SotcStats::heap_guard_worst_ns.load(std::memory_order_relaxed)) {
        SotcStats::heap_guard_worst_ns.store(ns, std::memory_order_relaxed);
        SotcStats::heap_guard_worst_blocks.store(free_blocks.size(), std::memory_order_relaxed);
        SotcStats::heap_guard_worst_adds.store(adds, std::memory_order_relaxed);
    }
    if (reuse && !(generation & 1) && runtime_memory_generation() == generation) {
        cache.generation = generation;
        cache.runs = runs;
    }
}

bool BpeHeapGuard::Read(VAddr addr, void* out, u64 size) const {
    // sotc: the run of the previous read first: the walk reads node after node in one run.
    if (last_run < runs.size()) {
        const Run& run = runs[last_run];
        if (addr - run.start < run.size && addr - run.start + size <= run.size) {
            std::memcpy(out, run.ptr + (addr - run.start), size);
            return true;
        }
    }
    auto it = std::upper_bound(runs.begin(), runs.end(), addr,
                               [](VAddr value, const Run& run) { return value < run.start; });
    if (it == runs.begin() || addr - std::prev(it)->start >= std::prev(it)->size) {
        Run run{};
        run.ptr = memory->GetBackingRun(addr, &run.start, &run.size);
        if (!run.ptr) {
            // sotc: the heap object itself is a global of the eboot, outside the guest memory
            // the runtime maps (no backing view): its fields are read in place.
            if (addr >= heap && addr + size <= heap + 0x1000) {
                std::memcpy(out, std::bit_cast<const void*>(addr), size);
                return true;
            }
            return false;
        }
        it = std::next(runs.insert(it, run));
    }
    const Run& run = *std::prev(it);
    last_run = static_cast<size_t>(std::prev(it) - runs.begin());
    if (addr - run.start + size > run.size) {
        // Crosses into the next physical area: take the general path
        return memory->TryReadBacking(addr, out, size);
    }
    std::memcpy(out, run.ptr + (addr - run.start), size);
    return true;
}

void BpeHeapGuard::Prefetch(VAddr addr) const {
    if (last_run < runs.size()) {
        const Run& run = runs[last_run];
        if (addr - run.start + 64 <= run.size) {
            __builtin_prefetch(run.ptr + (addr - run.start));
        }
    }
}

bool BpeHeapGuard::IsHeapPointer(VAddr addr) const {
    return addr >= HeapPointerLo && addr < HeapPointerHi && (addr & 7) == 0;
}

void BpeHeapGuard::AddFreeBlock(VAddr block) {
    free_blocks.push_back(block);
}

size_t BpeHeapGuard::CollectFreeTree() {
    // sotc: SOTC_GUARD_DEDUP=0 walks as before. The game changes the tree while this walk reads
    // it: at an area change a node reached twice (a link read mid-change) was walked again and
    // again up to MaxTreeNodes, each time with its ring of up to MaxListNodes members, and the
    // sort of those millions of repeats held the draw thread 1-3 s (the game showed its loading
    // screen). Nodes and ring members already seen are skipped: they only added repeats, which
    // the sort/unique below removed anyway.
    static const bool dedup = [] {
        const char* v = std::getenv("SOTC_GUARD_DEDUP");
        return !(v && v[0] == '0');
    }();
    if (dedup) {
        seen.Reset(last_seen);
    }
    size_t seen_keys = 0;
    u64 repeats = 0;
    const VAddr sentinel = heap + 0x80;
    VAddr root = 0;
    Read(sentinel, &root, sizeof(root));
    VAddr pending = 0;
    if (Read(heap + 0xc0, &pending, sizeof(pending)) && IsHeapPointer(pending)) {
        AddFreeBlock(pending);
    }
    std::vector<VAddr> stack{root};
    size_t visited = 0;
    while (!stack.empty() && visited < MaxTreeNodes) {
        const VAddr node = stack.back();
        stack.pop_back();
        if (node == sentinel || !IsHeapPointer(node)) {
            continue;
        }
        if (dedup && (++seen_keys, !seen.Insert(node))) {
            ++repeats;
            continue;
        }
        std::array<u64, 7> words{};
        if (!Read(node - HeaderSize, words.data(), sizeof(words))) {
            continue;
        }
        ++visited;
        const u64 key = words[1] & SizeMask;
        if (key == 0 || key >= MaxBlockSize) {
            // Already corrupted: still keep what is left of it, but do not follow its links
            AddFreeBlock(node - HeaderSize);
            continue;
        }
        AddFreeBlock(node - HeaderSize);
        // sotc: the walk is a chain of cache misses across the heap (~150 ns a node): the
        // children's headers start loading now, while this node's ring is followed.
        static const bool prefetch = [] {
            const char* v = std::getenv("SOTC_GUARD_PREFETCH");
            return !(v && v[0] == '0');
        }();
        if (prefetch) {
            if (IsHeapPointer(words[2])) Prefetch(words[2] - HeaderSize);
            if (IsHeapPointer(words[3])) Prefetch(words[3] - HeaderSize);
            if (IsHeapPointer(words[4])) Prefetch(words[4]);
        }
        // Blocks of equal size hang off the tree node in a ring through the list links
        VAddr member = words[4];
        for (size_t i = 0; i < MaxListNodes && IsHeapPointer(member) && member != node; ++i) {
            // members tagged in bit 0 (pointers are 8-aligned), apart from tree nodes
            if (dedup && (++seen_keys, !seen.Insert(member | 1))) {
                ++repeats;
                break;
            }
            AddFreeBlock(member - HeaderSize);
            VAddr next = 0;
            if (!Read(member + 0x10, &next, sizeof(next))) {
                break;
            }
            member = next;
        }
        stack.push_back(words[2]);
        stack.push_back(words[3]);
    }
    const size_t adds = free_blocks.size();
    last_seen = seen_keys;
    if (repeats) {
        SotcStats::heap_guard_repeats.fetch_add(repeats, std::memory_order_relaxed);
    }
    std::sort(free_blocks.begin(), free_blocks.end());
    free_blocks.erase(std::unique(free_blocks.begin(), free_blocks.end()), free_blocks.end());
    free_spans.reserve(free_blocks.size());
    for (const VAddr block : free_blocks) {
        free_spans.emplace_back(block, block + FreeNodeSize);
    }
    return adds;
}

bool BpeHeapGuard::NextBlock(VAddr block, VAddr& next) const {
    u64 size_flags = 0;
    if (!Read(block + 8, &size_flags, sizeof(size_flags))) {
        return false;
    }
    const u64 size = size_flags & SizeMask;
    if (size == 0 || size >= MaxBlockSize) {
        return false;
    }
    next = block + size + HeaderSize;
    // The next block must point back: this rejects anything that only looks like a header
    VAddr prev = 0;
    return IsHeapPointer(next) && Read(next, &prev, sizeof(prev)) && prev == block;
}

void BpeHeapGuard::CollectHeaders(VAddr lo, VAddr hi, std::vector<Span>& spans) const {
    // Start from the closest free block at or below the range, whose header is known to be real, and follow
    // the physical chain through the range
    auto it = std::upper_bound(free_blocks.begin(), free_blocks.end(), lo);
    std::vector<VAddr> starts;
    // Readbacks usually start at a binding base, i.e. the user pointer of an allocation: start from its
    // header too (accepted only if the next block points back to it)
    if (VAddr next = 0; lo > HeaderSize && NextBlock(lo - HeaderSize, next)) {
        starts.push_back(lo - HeaderSize);
    }
    if (it != free_blocks.begin()) {
        starts.push_back(*std::prev(it));
    }
    for (; it != free_blocks.end() && *it < hi; ++it) {
        starts.push_back(*it);
    }
    std::sort(starts.begin(), starts.end());
    VAddr covered_until = 0;
    for (VAddr block : starts) {
        if (block < covered_until) {
            continue;
        }
        for (size_t step = 0; step < MaxChainSteps && block < hi; ++step) {
            if (block + HeaderSize > lo) {
                spans.emplace_back(block, block + HeaderSize);
            }
            VAddr next = 0;
            if (!NextBlock(block, next)) {
                break;
            }
            block = next;
        }
        covered_until = block;
    }
}

std::vector<BpeHeapGuard::Span> BpeHeapGuard::CollectSpans(VAddr addr, VAddr end) {
    std::vector<Span> spans;
    for (auto it = std::lower_bound(free_spans.begin(), free_spans.end(),
                                    Span{addr > FreeNodeSize ? addr - FreeNodeSize : 0, 0});
         it != free_spans.end() && it->first < end; ++it) {
        if (it->second > addr) {
            spans.push_back(*it);
        }
    }
    CollectHeaders(addr, end, spans);
    std::sort(spans.begin(), spans.end());
    return spans;
}

void BpeHeapGuard::Hits(VAddr addr, VAddr end, std::vector<MetaHit>& hits) {
    for (const auto& [lo, hi] : CollectSpans(addr, end)) {
        // Every span starts at its block's header (the free-list spans at the block too)
        u64 size_flags = 0;
        const bool known = Read(lo + 8, &size_flags, sizeof(size_flags));
        const bool in_use = known && (size_flags & InUse);
        // The tree links of a block in use are user data: only its header is metadata
        const VAddr span_hi = in_use && hi - lo > HeaderSize ? lo + HeaderSize : hi;
        if (span_hi <= addr || lo >= end) {
            continue;
        }
        hits.push_back(MetaHit{lo, span_hi, lo, known && !in_use});
    }
}

u64 BpeHeapGuard::Write(VAddr addr, const u8* data, u64 size) {
    const VAddr end = addr + size;
    const std::vector<Span> spans = CollectSpans(addr, end);

    u64 differing = 0;
    VAddr cursor = addr;
    const auto write = [&](VAddr lo, VAddr hi) {
        if (hi > lo) {
            memory->TryWriteBacking(std::bit_cast<void*>(lo), data + (lo - addr), hi - lo);
        }
    };
    for (const auto& [span_lo, span_hi] : spans) {
        const VAddr lo = std::max(span_lo, addr);
        const VAddr hi = std::min(span_hi, end);
        if (hi <= lo || hi <= cursor) {
            continue;
        }
        const VAddr keep_lo = std::max(lo, cursor);
        write(cursor, keep_lo);
        std::array<u8, FreeNodeSize> current{};
        const u64 len = hi - keep_lo;
        if (Read(keep_lo, current.data(), len) &&
            std::memcmp(current.data(), data + (keep_lo - addr), len) != 0) {
            differing += len;
        }
        cursor = hi;
    }
    write(cursor, end);
    return differing;
}

bool BpeHeapGuard::FindUsedBlock(VAddr addr, VAddr& block, VAddr& next, int* why) const {
    int no_why = 0;
    why = why ? why : &no_why;
    const auto it = std::upper_bound(free_blocks.begin(), free_blocks.end(), addr);
    // why: 1 no free block below, 2 broken chain, 3 too many steps, 4 in a header, 5 free block,
    // 6 past the fence that ends the chain (`block` is the fence)
    if (it == free_blocks.begin()) {
        *why = 1;
        return false;
    }
    block = *std::prev(it);
    for (size_t step = 0; step < MaxChainSteps; ++step) {
        if (!NextBlock(block, next)) {
            // An in-use block of size 0 ends the chain: the heap's last 1 MiB ([heap+0xd0]) after it is
            // not split into blocks, and the culling readbacks land there
            u64 size_flags = 0;
            if (addr >= block + HeaderSize && Read(block + 8, &size_flags, sizeof(size_flags)) &&
                size_flags == InUse) {
                *why = 6;
                next = block + HeaderSize;
                return false;
            }
            *why = 2;
            return false;
        }
        if (addr < next) {
            u64 size_flags = 0;
            if (addr < block + HeaderSize) {
                *why = 4;
                return false;
            }
            if (!Read(block + 8, &size_flags, sizeof(size_flags)) || !(size_flags & InUse) ||
                std::binary_search(free_blocks.begin(), free_blocks.end(), block)) {
                *why = 5;
                return false;
            }
            return true;
        }
        block = next;
    }
    *why = 3;
    return false;
}

bool BpeUsedBlocks::StillUsed(VAddr heap, VAddr block, VAddr next) const {
    // The block being freed is kept at [heap+0xc0] before it joins the tree
    const VAddr pending = *std::bit_cast<const VAddr*>(heap + 0xc0);
    u64 size_flags = 0;
    VAddr prev = 0;
    return pending != block && pending != block + HeaderSize &&
           memory->TryReadBacking(block + 8, &size_flags, sizeof(size_flags)) &&
           (size_flags & InUse) && block + (size_flags & SizeMask) + HeaderSize == next &&
           memory->TryReadBacking(next, &prev, sizeof(prev)) && prev == block;
}

bool BpeUsedBlocks::Covers(VAddr heap, VAddr addr, VAddr end, int* why,
                           const BpeHeapPieces* pieces) {
    int no_why = 0;
    why = why ? why : &no_why;
    // Only the unsplit tail right after the fence ([heap+0xd0], 1 MiB) is free of metadata: the
    // world heaps span many 1 MiB pieces, and the free blocks of the later ones lie above it too
    u64 tail = *std::bit_cast<const u64*>(heap + 0xd0);
    if (tail < (64ULL << 10) || tail > (16ULL << 20)) {
        tail = 1ULL << 20;
    }
    if (fence && addr >= fence + HeaderSize && end <= fence + HeaderSize + tail &&
        pieces && pieces->Overlaps(addr, end)) {
        SotcStats::hg_piece[3].fetch_add(1, std::memory_order_relaxed);
    } else if (fence && addr >= fence + HeaderSize && end <= fence + HeaderSize + tail) {
        u64 size_flags = 0;
        if (memory->TryReadBacking(fence + 8, &size_flags, sizeof(size_flags)) && size_flags == InUse) {
            return true;
        }
        fence = 0;
    }
    auto it = std::upper_bound(blocks.begin(), blocks.end(), std::pair{addr, ~VAddr{0}});
    // why: 1 no block remembered there, 2 the write leaves the block, 3 block no longer in use
    if (it == blocks.begin()) {
        *why = 1;
        return false;
    }
    --it;
    const auto [block, next] = *it;
    if (addr >= next) {
        *why = 1;
        return false;
    }
    if (addr < block + HeaderSize || end > next) {
        *why = 2;
        return false;
    }
    if (!StillUsed(heap, block, next)) {
        *why = 3;
        blocks.erase(it);
        return false;
    }
    return true;
}

bool BpeUsedBlocks::TryUserPointer(VAddr heap, VAddr addr, VAddr end) {
    // sotc: SOTC_GUARD_USER_POINTER=1 trusts these without building the guard. Readbacks mostly
    // start at a binding base, the user pointer of an allocation; at an area change new blocks
    // came every frame and each one rebuilt the guard (a walk of the whole free tree, up to 670
    // per 5 s). The header before addr is trusted as CollectHeaders and StillUsed trust one: in
    // use, a sane size, and the next block points back to it. Off by default since 29 Oct: the
    // check and the full-range write are not one step, and a block the game frees in between gets
    // its free tree links (kept inside free blocks) overwritten. This sent more writes down that
    // path exactly while loads free blocks; suspected in the 0x49800 crash 6 s after a load, 24 min
    // into the user's session. It saved almost nothing.
    static const bool on = [] {
        const char* v = std::getenv("SOTC_GUARD_USER_POINTER");
        return v && v[0] == '1';
    }();
    if (!on || addr < HeapPointerLo + HeaderSize || (addr & 7)) {
        return false;
    }
    const VAddr block = addr - HeaderSize;
    u64 size_flags = 0;
    if (!memory->TryReadBacking(block + 8, &size_flags, sizeof(size_flags)) || !(size_flags & InUse)) {
        return false;
    }
    const u64 size = size_flags & SizeMask;
    if (size == 0 || size >= MaxBlockSize) {
        return false;
    }
    const VAddr next = block + size + HeaderSize;
    if (end > next || !StillUsed(heap, block, next)) {
        return false;
    }
    Add(block, next);
    return true;
}

void BpeUsedBlocks::Add(VAddr block, VAddr next) {
    if (blocks.size() >= MaxUsedBlocks) {
        // sotc: keep every other one (was: forget all): at an area change the list filled up
        // within seconds, and each forgotten block cost a guard rebuild on its next readback
        size_t kept = 0;
        for (size_t i = 0; i < blocks.size(); i += 2) {
            blocks[kept++] = blocks[i];
        }
        blocks.resize(kept);
    }
    const auto it = std::lower_bound(blocks.begin(), blocks.end(), std::pair{block, VAddr{0}});
    if (it != blocks.end() && it->first == block) {
        it->second = next;
    } else {
        blocks.insert(it, {block, next});
    }
}

namespace BpeNative {
namespace {

constexpr size_t MaxHeaps = 8;
constexpr size_t RingSize = 64;
constexpr size_t KindCount = static_cast<size_t>(Kind::Count);
constexpr u64 NearBytes = 1ULL << 20;
constexpr const char* KindNames[KindCount] = {"bind",  "gpu-fill", "gpu-copy", "cpu-fill", "cpu-copy",
                                              "hle-copy", "hle-table", "image", "cp"};
const auto RefreshEvery = std::chrono::milliseconds(250); // guard rebuild age; the free list is read live
const auto ReportEvery = std::chrono::seconds(5);

struct Overlap {
    VAddr addr = 0;
    u64 size = 0;
    VAddr block = 0; // the first metadata block the write reached
    bool freed = false;
    Kind kind = Kind::Bind;
    u64 detail = 0;
    u64 seq = 0;
};

struct HeapSlot {
    VAddr heap = 0;
    bool active = false; // listed by the last refresh
    std::chrono::steady_clock::time_point built{};
    std::optional<BpeHeapGuard> guard;
};

struct State {
    std::mutex mutex;
    std::array<HeapSlot, MaxHeaps> heaps{};
    std::chrono::steady_clock::time_point listed{};
    std::array<Overlap, RingSize> ring{}; // the last overlaps, for the fault report
    u64 seq = 0;                          // overlaps since start
    // Counters of the current 5 s window
    std::chrono::steady_clock::time_point window{};
    u64 checks = 0;
    u64 check_bytes = 0;
    u64 overlaps = 0;
    u64 freed_overlaps = 0;
    u64 overlap_bytes = 0;
    u64 clamps = 0;
    u64 clamp_bytes = 0;
    u64 builds = 0;
    u64 build_ns = 0;
    std::array<u64, KindCount> kind_checks{};
    std::array<u64, KindCount> kind_overlaps{};
    bool announced = false;
};

State& GetState() {
    static State state;
    return state;
}

// Heaps to check: the heap the readback guard uses, and the heaps of its class from the engine's table
// (ApplyDownloads lists the same ones). A guard is rebuilt at most every RefreshEvery.
void RefreshHeaps(State& s) {
    const auto now = std::chrono::steady_clock::now();
    if (now - s.listed < RefreshEvery) {
        return;
    }
    s.listed = now;
    std::array<VAddr, MaxHeaps> wanted{};
    size_t count = 0;
    if (const VAddr chosen = runtime_sotc_heap(); chosen != 0) {
        wanted[count++] = chosen;
        const u64 heap_class = *std::bit_cast<const u64*>(chosen);
        if (const VAddr table = runtime_sotc_heap_table(); table != 0) {
            for (u32 id = 1; id < 48 && count < wanted.size(); ++id) {
                const VAddr other = std::bit_cast<const VAddr*>(table)[id];
                if (other == 0 || other == chosen || !(other >= 0x10000 && other < 0x10000000000ULL)) {
                    continue;
                }
                const bool in_image = other - table < 0x20000000;
                if (!in_image && !Core::Memory::Instance()->GetBackingPointer(other)) {
                    continue;
                }
                if (*std::bit_cast<const u64*>(other) == heap_class) {
                    wanted[count++] = other;
                }
            }
        }
    }
    for (auto& slot : s.heaps) {
        slot.active = false;
    }
    for (size_t w = 0; w < count; ++w) {
        // The heap keeps its slot (and guard) while listed; otherwise a free slot, or one of a heap not listed
        auto found = std::find_if(s.heaps.begin(), s.heaps.end(),
                                  [&](const HeapSlot& c) { return c.heap == wanted[w]; });
        if (found == s.heaps.end()) {
            found = std::find_if(s.heaps.begin(), s.heaps.end(),
                                 [](const HeapSlot& c) { return c.heap == 0 || !c.active; });
        }
        if (found == s.heaps.end()) {
            continue;
        }
        HeapSlot* slot = &*found;
        slot->active = true;
        if (slot->heap != wanted[w]) {
            slot->heap = wanted[w];
            slot->guard.reset();
        }
        if (!slot->guard || now - slot->built >= RefreshEvery) {
            const auto start = std::chrono::steady_clock::now();
            slot->guard.emplace(Core::Memory::Instance(), slot->heap);
            slot->built = start;
            s.builds++;
            s.build_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - start)
                                               .count());
        }
    }
}

void ReportWindow(State& s, std::chrono::steady_clock::time_point now) {
    const double secs = std::chrono::duration<double>(now - s.window).count();
    std::fprintf(stderr,
                 "SOTC BPE NATIVE (%.1f s): mode %d, in-place writes checked %llu (%.2f MiB), metadata "
                 "overlaps %llu (free blocks %llu, %.1f KiB), clamped %llu (%.1f KiB dropped), heaps %zu, "
                 "guard builds %llu (%.2f ms)\n",
                 secs, Mode(), static_cast<unsigned long long>(s.checks),
                 static_cast<double>(s.check_bytes) / (1 << 20),
                 static_cast<unsigned long long>(s.overlaps),
                 static_cast<unsigned long long>(s.freed_overlaps),
                 static_cast<double>(s.overlap_bytes) / 1024.0,
                 static_cast<unsigned long long>(s.clamps),
                 static_cast<double>(s.clamp_bytes) / 1024.0,
                 static_cast<size_t>(std::count_if(s.heaps.begin(), s.heaps.end(),
                                                   [](const HeapSlot& h) { return h.active; })),
                 static_cast<unsigned long long>(s.builds), static_cast<double>(s.build_ns) / 1e6);
    std::fprintf(stderr, "SOTC BPE NATIVE by kind (overlapping / checked writes):");
    for (size_t k = 0; k < KindCount; ++k) {
        std::fprintf(stderr, " %s %llu/%llu", KindNames[k],
                     static_cast<unsigned long long>(s.kind_overlaps[k]),
                     static_cast<unsigned long long>(s.kind_checks[k]));
    }
    std::fprintf(stderr, "\n");
    s.checks = s.check_bytes = s.overlaps = s.freed_overlaps = s.overlap_bytes = 0;
    s.clamps = s.clamp_bytes = s.builds = s.build_ns = 0;
    s.kind_checks.fill(0);
    s.kind_overlaps.fill(0);
    s.window = now;
}

} // namespace

int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_BPE_FIX");
        return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
    }();
    return mode;
}

u64 Check(VAddr addr, u64 size, Kind kind, u64 detail, bool in_place, bool clamp) {
    const int mode = Mode();
    if (mode == 0 || size == 0 || !in_place) {
        return size;
    }
    State& s = GetState();
    std::scoped_lock lock{s.mutex};
    const auto now = std::chrono::steady_clock::now();
    if (!s.announced) {
        s.announced = true;
        s.window = now;
        std::fprintf(stderr, "SOTC BPE NATIVE: SOTC_BPE_FIX=%d: in-place GPU writes are checked against the BPE heap "
                             "metadata%s\n", mode, mode == 1 ? " and written bindings are clamped at it" : " (count only)");
    }
    RefreshHeaps(s);
    thread_local std::vector<BpeHeapGuard::MetaHit> hits;
    VAddr first_hit = 0;   // the first metadata block the write reaches
    VAddr first_freed = 0; // ... and the first one that is free now
    bool any = false;
    bool freed_any = false;
    u64 keep = size; // the clean prefix of the write
    u64 bytes = 0;   // metadata bytes the write covers
    const VAddr end = addr + size;
    for (auto& slot : s.heaps) {
        if (!slot.active || !slot.guard || !slot.guard->IsValid()) {
            continue;
        }
        hits.clear();
        slot.guard->Hits(addr, end, hits);
        for (const auto& hit : hits) {
            if (!any) {
                first_hit = hit.block;
            }
            if (hit.freed && !freed_any) {
                first_freed = hit.block;
            }
            any = true;
            freed_any |= hit.freed;
            keep = std::min<u64>(keep, hit.lo > addr ? hit.lo - addr : 0);
            bytes += std::min<VAddr>(hit.hi, end) - std::max<VAddr>(hit.lo, addr);
        }
    }
    s.checks++;
    s.check_bytes += size;
    s.kind_checks[static_cast<size_t>(kind)]++;
    if (any) {
        s.overlaps++;
        s.freed_overlaps += freed_any ? 1 : 0;
        s.overlap_bytes += bytes;
        s.kind_overlaps[static_cast<size_t>(kind)]++;
        Overlap& o = s.ring[s.seq % RingSize];
        o = Overlap{addr, size, freed_any ? first_freed : first_hit, freed_any, kind, detail, s.seq + 1};
        s.seq++;
    }
    const bool clamping = clamp && mode == 1;
    if (clamping && keep < size) {
        s.clamps++;
        s.clamp_bytes += size - keep;
    }
    if (now - s.window >= ReportEvery) {
        ReportWindow(s, now);
    }
    return clamping ? keep : size;
}

void ReportNearFault(VAddr fault_addr) {
    if (Mode() == 0) {
        return;
    }
    State& s = GetState();
    std::unique_lock lock{s.mutex, std::try_to_lock}; // the fault may come from a thread inside Check
    if (!lock.owns_lock()) {
        std::fprintf(stderr, "SOTC BPE NATIVE near fault: the overlap state is busy\n");
        return;
    }
    int printed = 0;
    for (u64 i = 0; i < std::min<u64>(s.seq, RingSize) && printed < 3; ++i) {
        const Overlap& o = s.ring[(s.seq - 1 - i) % RingSize];
        const bool near_write = fault_addr + NearBytes >= o.addr && fault_addr < o.addr + o.size + NearBytes;
        const bool near_block = fault_addr + NearBytes >= o.block && fault_addr < o.block + NearBytes;
        if (!near_write && !near_block) {
            continue;
        }
        const long long from_block = static_cast<long long>(fault_addr) - static_cast<long long>(o.block);
        std::fprintf(stderr,
                     "SOTC BPE NATIVE overlap near the fault: #%llu %s detail 0x%llx wrote [0x%llx, +0x%llx) "
                     "over the metadata of block 0x%llx (%s); the fault is %+lld bytes from that block\n",
                     static_cast<unsigned long long>(o.seq), KindNames[static_cast<size_t>(o.kind)],
                     static_cast<unsigned long long>(o.detail), static_cast<unsigned long long>(o.addr),
                     static_cast<unsigned long long>(o.size), static_cast<unsigned long long>(o.block),
                     o.freed ? "freed" : "in use", from_block);
        printed++;
    }
    if (printed == 0) {
        std::fprintf(stderr,
                     "SOTC BPE NATIVE no metadata overlap within 1 MiB of the fault (%llu overlaps since start)\n",
                     static_cast<unsigned long long>(s.seq));
    }
}

} // namespace BpeNative

bool BpeHeapPieces::Refresh(VAddr heap) {
    const auto field = [&](u64 offset) { return *std::bit_cast<const volatile u64*>(heap + offset); };
    const VAddr new_head = field(0xb8);
    const u64 new_count = field(0x58);
    const u64 new_unit = field(0xd0);
    const u64 total = field(0x30);
    if (new_head == head && new_count == count && new_unit == unit && total == heap_size) {
        return valid;
    }
    head = new_head;
    count = new_count;
    unit = new_unit;
    heap_size = total;
    valid = false;
    pieces.clear();
    initial_lo = initial_hi = 0;
    constexpr u64 MiB = 1ULL << 20;
    if (new_unit != MiB || new_count > 4096 || total < new_count * MiB ||
        (new_count != 0 && !IsPieceAligned(new_head))) {
        return false;
    }
    if (const u64 initial = total - new_count * MiB; initial != 0) {
        const VAddr base = field(0x28);
        if (base < HeapPointerLo || initial > MaxBlockSize || base + initial > HeapPointerHi) {
            return false;
        }
        // its list node (the sentinel) sits just below heap+0x28
        initial_lo = base & ~(MiB - 1);
        initial_hi = (base + initial + MiB - 1) & ~(MiB - 1);
    }
    std::vector<VAddr> starts;
    VAddr piece = new_head;
    for (u64 i = 0; piece != 0; ++i) {
        u64 sentinel[2] = {};
        if (i > new_count || !IsPieceAligned(piece) ||
            !memory->TryReadBacking(piece, sentinel, sizeof(sentinel)) || sentinel[1] != 3) {
            return false;
        }
        if (piece < initial_lo || piece >= initial_hi) {
            starts.push_back(piece);
        }
        piece = sentinel[0];
    }
    // Each piece is at least 1 MiB; the heap must not have changed during the walk
    if (starts.size() > new_count || field(0xb8) != new_head || field(0x58) != new_count ||
        field(0x30) != total) {
        head = 0; // read again next time
        return false;
    }
    const u64 longest = (1 + new_count - starts.size()) * MiB;
    std::sort(starts.begin(), starts.end());
    for (size_t i = 0; i < starts.size(); ++i) {
        VAddr end = starts[i] + longest;
        if (i + 1 < starts.size()) {
            end = std::min(end, starts[i + 1]);
        }
        if (starts[i] < initial_lo) {
            end = std::min(end, initial_lo);
        }
        // sotc: the piece's own end when its last block is found there (a heap that has MiB to
        // spare otherwise took each piece as the longest one possible, and guarded the writes to
        // the buffers after its 1 MiB pieces: ~126k guard builds' worth per ride)
        for (VAddr at = starts[i] + MiB; at < end; at += MiB) {
            if (IsPieceEnd(starts[i], at)) {
                end = at;
                break;
            }
        }
        pieces.emplace_back(starts[i], end);
    }
    valid = true;
    return true;
}

bool BpeHeapPieces::IsPieceEnd(VAddr piece, VAddr end) const {
    // The piece's last block: in use, size 0, right before end; the block before it ends there
    const VAddr fence = end - HeaderSize;
    u64 words[2] = {};
    if (!memory->TryReadBacking(fence, words, sizeof(words)) || words[1] != InUse) {
        return false;
    }
    const VAddr prev = words[0];
    u64 prev_size = 0;
    return prev >= piece + HeaderSize && prev < fence && (prev & 7) == 0 &&
           memory->TryReadBacking(prev + 8, &prev_size, sizeof(prev_size)) &&
           prev + (prev_size & SizeMask) + HeaderSize == fence;
}

bool BpeHeapPieces::Overlaps(VAddr addr, VAddr end) const {
    if (addr < initial_hi && initial_lo < end) {
        return true;
    }
    // the last piece starting before end
    auto it = std::lower_bound(pieces.begin(), pieces.end(), std::pair{end, VAddr{0}});
    return it != pieces.begin() && std::prev(it)->second > addr;
}

std::pair<VAddr, VAddr> BpeHeapPieces::PieceOf(VAddr addr) const {
    auto it = std::upper_bound(pieces.begin(), pieces.end(), std::pair{addr, ~VAddr{0}});
    if (it == pieces.begin() || addr >= std::prev(it)->second) {
        return {0, 0};
    }
    return *std::prev(it);
}

bool BpeUsedBlocks::FindInPiece(VAddr heap, VAddr piece, VAddr piece_end, VAddr addr, VAddr end) {
    if (end > piece_end) {
        return false;
    }
    // The piece's first block follows its sentinel and points back to it
    VAddr block = piece + HeaderSize;
    VAddr prev = 0;
    if (!memory->TryReadBacking(block, &prev, sizeof(prev)) || prev != piece) {
        return false;
    }
    // A remembered block of this piece below addr, still in use, is a closer start
    auto it = std::upper_bound(blocks.begin(), blocks.end(), std::pair{addr, ~VAddr{0}});
    if (it != blocks.begin() && std::prev(it)->first >= block &&
        StillUsed(heap, std::prev(it)->first, std::prev(it)->second)) {
        block = std::prev(it)->first;
    }
    for (size_t step = 0; step < MaxChainSteps; ++step) {
        u64 size_flags = 0;
        if (!memory->TryReadBacking(block + 8, &size_flags, sizeof(size_flags))) {
            return false;
        }
        const u64 size = size_flags & SizeMask;
        if (size == 0 || size >= piece_end - piece) {
            return false; // the piece's last block, or not a block
        }
        const VAddr next = block + size + HeaderSize;
        if (next > piece_end || !memory->TryReadBacking(next, &prev, sizeof(prev)) ||
            prev != block) {
            return false;
        }
        if (addr < next) {
            if (addr < block + HeaderSize || end > next || !(size_flags & InUse) ||
                !StillUsed(heap, block, next)) {
                return false;
            }
            Add(block, next);
            return true;
        }
        block = next;
    }
    return false;
}

} // namespace VideoCore
