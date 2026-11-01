// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_GEN_BIND / SOTC_GEN_DMA generation table (sotc_generation.h).
//
// Storage: one u64 counter per 64 KiB granule of the 40-bit guest space, in lazily allocated
// chunks of 4096 counters (256 MiB of guest space each, 32 KiB per chunk). The top level is a
// fixed array of 4096 atomic chunk pointers, so an untouched part of the space costs nothing and
// a chunk is created by the first bump in it (compare-exchange; the loser frees its copy). A
// counter is never reset, so a Sum over a range that never had a bump is 0 whether or not its
// chunk exists.
//
// Ordering: Bump is a release increment, Sum an acquire load. Callers bump after the store that
// invalidates (region_manager.h, memory_tracker.h, buffer_cache.cpp) and load Sum before the work
// that depends on it, so a change either shows in the Sum they load, or is ordered after it.
//
// Counters are u64 and only grow, so the Sum changes on every bump of any granule of the range.

#include "video_core/sotc_generation.h"

#include <atomic>
#include <cstdio>
#include <memory>

namespace SotcGen {
namespace {

constexpr u64 kChunkBits = 12;                           // granules per chunk: 4096
constexpr u64 kChunkCount = u64{1} << (kAddrBits - kShift - kChunkBits); // 4096 chunks
constexpr u64 kGranulesPerWord = 4;                      // 256 KiB word / 64 KiB granule
constexpr u64 kPagesPerGranule = 16;                     // 4 KiB pages / 64 KiB granule

// sotc: a 2 MiB block level (32 granules): Sum reads one block counter for every whole block of
// the range instead of 32 granule counters (a 32 MiB ring8 bind: 16 loads instead of 512; with
// granule loads only, SOTC_GEN_BIND cost ~11 ms of draw work per frame, 1 Nov). A block counter
// is bumped with each of its granules, so it always equals the sum of its granules: Sum returns
// the same value either way.
constexpr u64 kBlockBits = 5; // granules per block: 32 (2 MiB)

struct Chunk {
    std::atomic<u64> count[u64{1} << kChunkBits];
    std::atomic<u64> block[u64{1} << (kChunkBits - kBlockBits)];
};

// Zero-initialized static storage: every chunk pointer starts null.
std::atomic<Chunk*> g_chunks[kChunkCount];

// Window counters (Report takes and resets them). Relaxed: they are statistics only.
std::atomic<u64> g_stats[static_cast<size_t>(Stat::Count)];

// The chunk of granule g, created when create is set. Null when absent and !create.
Chunk* ChunkOf(u64 granule, bool create) {
    auto& slot = g_chunks[granule >> kChunkBits];
    Chunk* chunk = slot.load(std::memory_order_acquire);
    if (chunk || !create) {
        return chunk;
    }
    auto fresh = std::make_unique<Chunk>(); // value-initialized: every counter starts at 0
    Chunk* expected = nullptr;
    if (slot.compare_exchange_strong(expected, fresh.get(), std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
        return fresh.release();
    }
    return expected; // another thread created it first
}

void BumpGranule(u64 granule, u64 n) {
    Chunk* chunk = ChunkOf(granule, true);
    const u64 local = granule & ((u64{1} << kChunkBits) - 1);
    chunk->count[local].fetch_add(n, std::memory_order_release);
    chunk->block[local >> kBlockBits].fetch_add(n, std::memory_order_release);
}

} // namespace

void Bump(u64 addr, u64 size) {
    // sotc: no counter work at all while no switch reads the generations (call sites on hot paths).
    if (!Bumps() || !Covered(addr, size)) {
        return;
    }
    const u64 first = addr >> kShift;
    const u64 last = (addr + size - 1) >> kShift;
    for (u64 g = first; g <= last; ++g) {
        BumpGranule(g, 1);
    }
}

void BumpWordPages(u64 word_base, u64 page_mask) {
    if (!Bumps() || page_mask == 0 || !Covered(word_base, u64{1} << 18)) {
        return;
    }
    for (u64 j = 0; j < kGranulesPerWord; ++j) {
        const u64 pages = (page_mask >> (j * kPagesPerGranule)) & 0xFFFF;
        if (pages != 0) {
            BumpGranule((word_base >> kShift) + j, 1);
        }
    }
}

u64 Sum(u64 addr, u64 size) {
    if (!Covered(addr, size)) {
        return 0;
    }
    const u64 first = addr >> kShift;
    const u64 last = (addr + size - 1) >> kShift;
    u64 sum = 0;
    Chunk* chunk = nullptr;
    u64 chunk_index = ~0ULL;
    constexpr u64 kBlockGranules = u64{1} << kBlockBits;
    for (u64 g = first; g <= last;) {
        const u64 index = g >> kChunkBits;
        if (index != chunk_index) {
            chunk_index = index;
            chunk = ChunkOf(g, false); // a missing chunk has no bumps: contributes 0
        }
        const u64 local = g & ((u64{1} << kChunkBits) - 1);
        // A whole block inside the range (blocks never cross a chunk): its block counter.
        if ((local & (kBlockGranules - 1)) == 0 && last - g + 1 >= kBlockGranules) {
            if (chunk) {
                sum += chunk->block[local >> kBlockBits].load(std::memory_order_acquire);
            }
            g += kBlockGranules;
            continue;
        }
        if (chunk) {
            sum += chunk->count[local].load(std::memory_order_acquire);
        }
        ++g;
    }
    return sum;
}

void Count(Stat s, u64 n) {
    g_stats[static_cast<size_t>(s)].fetch_add(n, std::memory_order_relaxed);
}

void Report(std::string& out, double seconds) {
    if (!Bumps()) {
        return;
    }
    const auto take = [](Stat s) {
        return g_stats[static_cast<size_t>(s)].exchange(0, std::memory_order_relaxed);
    };
    const u64 hit = take(Stat::BindHit);
    const u64 miss = take(Stat::BindMiss);
    const u64 bind_bad = take(Stat::BindVerifyMismatch);
    const u64 skip = take(Stat::DmaRunSkip);
    const u64 sync = take(Stat::DmaRunSync);
    const u64 dma_bad = take(Stat::DmaVerifyMismatch);
    char line[512];
    std::snprintf(line, sizeof line,
                  "SOTC GEN BIND: bind=%d dma=%d window=%.1fs memo hits=%llu misses=%llu "
                  "verify mismatches=%llu; DMA runs skipped=%llu synced=%llu verify "
                  "mismatches=%llu\n",
                  kBindMode, kDmaMode, seconds, static_cast<unsigned long long>(hit),
                  static_cast<unsigned long long>(miss), static_cast<unsigned long long>(bind_bad),
                  static_cast<unsigned long long>(skip), static_cast<unsigned long long>(sync),
                  static_cast<unsigned long long>(dma_bad));
    out += line;
}

} // namespace SotcGen
