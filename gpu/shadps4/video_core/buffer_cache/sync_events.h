// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_SYNC_EVENTS (default 0 = the per-op sync of today; 1 = on; 2 = verify).
//
// The dirty journal of the buffer and texture sync paths. Guest memory becomes newer than its GPU
// copy in two ways that the sync paths see: a CPU-modified bit rises on a buffer region page (guest
// write fault, ObtainBuffer's own marks, label and WriteData invalidations, SOTC_NATIVE exact
// copies), or a texture guest invalidation (TextureCache::InvalidateMemory). The first is recorded
// at RegionManager::UpdateStateAndProtection, the one choke point where a CPU bit goes from clear to
// set (the same place the SOTC_SYNC_FAST generations are bumped). The second is recorded at the top
// of TextureCache::InvalidateMemory.
//
// The journal is a WORK LIST, not the freshness proof. Its entries are hints:
//   - the draw-side drain (BufferCache::DrainSyncEvents, queued at the start of each outermost gfx
//     submit, before the submit's ops) runs SynchronizeMemory (read) over the hinted ranges, and
//     TextureCache::DrainImageEvents over the hinted image ranges. Both act on the CPU bits / image
//     flags themselves, exactly as a bind of that range would, so the upload is the same. A hint that
//     is lost, torn or points at memory that is not resident costs time only: the bits stay set and
//     the bind takes the full path.
//   - the per-bind skip (SOTC_SYNC_EVENTS=1, BufferCache::ObtainBuffer, read bindings only) is decided
//     by MemoryTracker::IsSyncClean over the bound range. That is an exact, lock-free read of the CPU
//     bits: IsSyncClean is true only when ForEachUploadRange(range, read) would find nothing, and it is
//     false for a range without a tracking region. A skipped SynchronizeMemory would have changed
//     nothing, so the skip is exact by construction; the drain only makes it true more often.
//
// Why no journal-only skip: a region is created with every CPU bit set (RegionManager's constructor,
// no journal entry), hot pages keep their CPU bit after an upload, and a hint can be lost. A skip
// that trusted "no hint since the drain" would miss those, so the bit read stays on the bind path.
//
// Overflow: the ring keeps the last kCapacity hints. Older ones are counted as lost. Losing a hint
// never makes a read stale, for the reason above.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/types.h"

namespace SotcSyncEvents {

/// SOTC_SYNC_EVENTS: 0 off (every path as before), 1 on, 2 verify (the full sync runs too and the
/// result is counted). Read once at namespace scope; the initializer reads only the variable.
inline const int kMode = [] {
    const char* v = std::getenv("SOTC_SYNC_EVENTS");
    if (!v || !v[0]) {
        return 0;
    }
    if (v[0] == '2') {
        return 2;
    }
    return v[0] == '1' ? 1 : 0;
}();

inline bool On() {
    return kMode != 0;
}

/// Hint kinds. HintWords: data is a page mask over the 256 KiB word at addr (bit i = page i of the
/// word, RegionManager's CPU bits as they rose). HintImage: data is the byte size of a range that
/// the texture cache invalidated.
enum : u32 { HintWords = 0, HintImage = 1 };

struct Hint {
    VAddr addr{};
    u64 data{};
    u32 kind{};
};

/// One ring slot. stamp holds idx + 1 once the slot is published (release); a reader accepts a slot
/// only when stamp matches its index (acquire). Field loads are relaxed: a slot overwritten while it
/// is read can give a mixed hint, which is still a hint (see the header).
struct Slot {
    std::atomic<u64> stamp{0};
    std::atomic<u64> addr{0};
    std::atomic<u64> data{0};
    std::atomic<u32> kind{0};
};

constexpr u64 kCapacity = 1ULL << 16;
inline Slot ring[kCapacity];
inline std::atomic<u64> write_pos{0};
inline std::atomic<u64> read_pos{0};

// Window counters (Report takes and resets them). Relaxed: statistics only.
inline std::atomic<u64> hints_lost{0};       ///< overwritten or unpublished before a drain read them
inline std::atomic<u64> drained_hints{0};    ///< hints a drain read
inline std::atomic<u64> drained_ranges{0};   ///< merged buffer ranges synchronized by a drain
inline std::atomic<u64> drained_bytes{0};    ///< bytes a drain uploaded
inline std::atomic<u64> drained_images{0};   ///< images a drain updated (CPU-dirty or maybe-dirty)
inline std::atomic<u64> drain_nonresident{0}; ///< drained ranges skipped: not fully resident
inline std::atomic<u64> bind_checks{0};      ///< read bindings on the draw thread that checked the bits
inline std::atomic<u64> bind_clean{0};       ///< ... of which the bits were clean (IsSyncClean)
inline std::atomic<u64> bind_skips{0};       ///< SOTC_SYNC_EVENTS=1: SynchronizeMemory not called
inline std::atomic<u64> verify_stale{0};     ///< =2: a skipped bind whose full sync uploaded bytes

/// Records a CPU rise: the page mask of one 256 KiB word (RegionManager::UpdateStateAndProtection).
/// Any thread, under the region mutex; lock-free.
inline void PushWords(VAddr word_addr, u64 page_mask) {
    const u64 idx = write_pos.fetch_add(1, std::memory_order_relaxed);
    Slot& s = ring[idx & (kCapacity - 1)];
    s.addr.store(word_addr, std::memory_order_relaxed);
    s.data.store(page_mask, std::memory_order_relaxed);
    s.kind.store(HintWords, std::memory_order_relaxed);
    s.stamp.store(idx + 1, std::memory_order_release);
}

/// Records a texture guest invalidation of [addr, addr + size). Any thread; lock-free.
inline void PushImage(VAddr addr, u64 size) {
    const u64 idx = write_pos.fetch_add(1, std::memory_order_relaxed);
    Slot& s = ring[idx & (kCapacity - 1)];
    s.addr.store(addr, std::memory_order_relaxed);
    s.data.store(size, std::memory_order_relaxed);
    s.kind.store(HintImage, std::memory_order_relaxed);
    s.stamp.store(idx + 1, std::memory_order_release);
}

/// Takes the hints published since the last call into out (appended). Returns the number lost.
/// A drain may run on any thread; two drains racing can read the same hint twice, which only costs
/// a second, no-op synchronization.
inline u64 TakeBatch(std::vector<Hint>& out) {
    const u64 wp = write_pos.load(std::memory_order_acquire);
    u64 start = read_pos.exchange(wp, std::memory_order_relaxed);
    u64 lost = 0;
    if (wp - start > kCapacity) {
        lost = wp - kCapacity - start;
        start = wp - kCapacity;
    }
    for (u64 idx = start; idx < wp; ++idx) {
        const Slot& s = ring[idx & (kCapacity - 1)];
        if (s.stamp.load(std::memory_order_acquire) != idx + 1) {
            ++lost; // not published yet, or already overwritten
            continue;
        }
        out.push_back(Hint{s.addr.load(std::memory_order_relaxed),
                           s.data.load(std::memory_order_relaxed),
                           s.kind.load(std::memory_order_relaxed)});
    }
    if (lost) {
        hints_lost.fetch_add(lost, std::memory_order_relaxed);
    }
    drained_hints.fetch_add(out.size(), std::memory_order_relaxed);
    return lost;
}

/// SOTC_SYNC_EVENTS report line (videoout/driver.cpp, every 5 s window). Resets the window counters.
inline void Report(std::string& out, double seconds) {
    const u64 ranges = drained_ranges.exchange(0, std::memory_order_relaxed);
    const u64 images = drained_images.exchange(0, std::memory_order_relaxed);
    const u64 bytes = drained_bytes.exchange(0, std::memory_order_relaxed);
    const u64 stale = verify_stale.exchange(0, std::memory_order_relaxed);
    const u64 hints = drained_hints.exchange(0, std::memory_order_relaxed);
    const u64 lost = hints_lost.exchange(0, std::memory_order_relaxed);
    const u64 nonres = drain_nonresident.exchange(0, std::memory_order_relaxed);
    const u64 checks = bind_checks.exchange(0, std::memory_order_relaxed);
    const u64 clean = bind_clean.exchange(0, std::memory_order_relaxed);
    const u64 skips = bind_skips.exchange(0, std::memory_order_relaxed);
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "SOTC SYNC_EVENTS (mode %d): drained ranges %llu, images %llu, uploads %llu KiB, "
                  "verify stale %llu | hints %llu (lost %llu, nonresident %llu) binds %llu clean %llu "
                  "skipped %llu (%.1f s window)\n",
                  kMode, static_cast<unsigned long long>(ranges),
                  static_cast<unsigned long long>(images),
                  static_cast<unsigned long long>(bytes >> 10),
                  static_cast<unsigned long long>(stale), static_cast<unsigned long long>(hints),
                  static_cast<unsigned long long>(lost), static_cast<unsigned long long>(nonres),
                  static_cast<unsigned long long>(checks), static_cast<unsigned long long>(clean),
                  static_cast<unsigned long long>(skips), seconds);
    out += buf;
}

} // namespace SotcSyncEvents
