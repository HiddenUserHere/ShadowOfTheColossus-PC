// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_WRITE_JOURNAL (docs/re-stutters/native/ARCH-BINDSET.md, stage 4). Per-op write bookkeeping of the draw
// thread without the std::map / std::multimap walks and without the lock when no entry can be affected.
//
// Mode 0 (default): nothing changes, every consumer runs its old code.
// Mode 1: a write that no live entry's page covers returns before any lock and any tree walk. The entries are
//         native copy registrations (native_copies.cpp) and hashed uploads (buffer_cache.cpp, EraseHashedUploads and
//         the dedupe lookup of SynchronizeMemoryFull). Each written range is also appended to a per-thread journal
//         that is coalesced at the epoch end (SotcNativeCopies::EpochEnd, the start of the next submit).
// Mode 2: the old path runs in full; the page filter's answer is compared with what the old path did, and every
//         difference is counted (verify diff, must read 0), and the coalesced journal is checked against its ranges.
//
// Why a page filter is exact: an entry can be affected by a write only when they share a byte, and then they share a
// page. The filter keeps one count per hashed page slot for every page of every live entry (added when the entry
// appears, removed when it leaves). A slot with a count of zero proves that no entry covers any page of the write.
// Collisions only make the answer "may touch", which runs the old path. Counts are atomic: the writers hold the
// owner's lock; the draw thread reads them without it (acquire), which orders it after the registration it needs.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "common/types.h"

namespace SotcWriteJournal {

/// SOTC_WRITE_JOURNAL (read once): 0 off, 1 filter and journal, 2 verify.
inline int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_WRITE_JOURNAL");
        return (v && v[0] == '1') ? 1 : ((v && v[0] == '2') ? 2 : 0);
    }();
    return mode;
}

/// True for SOTC_WRITE_JOURNAL=1 or 2.
inline bool On() {
    return Mode() != 0;
}

/// True for SOTC_WRITE_JOURNAL=2.
inline bool Verify() {
    return Mode() == 2;
}

/// Window counters (Report prints and resets them). Lock-free; updated by the draw thread and the owners' locks.
struct Counters {
    std::atomic<u64> ops{0};             // NoteWrittenBind calls (written binds of ops that watch registrations)
    std::atomic<u64> ranges{0};          // written ranges appended to the journal (size > 0)
    std::atomic<u64> coalesced{0};       // intervals left by coalescing (EpochEnd and the journal's cap)
    std::atomic<u64> coalesce_ns{0};     // time of the coalescing (the sort of the epoch's ranges)
    std::atomic<u64> native_filtered{0}; // native copy writes that took neither the lock nor the walk
    std::atomic<u64> native_walks{0};    // native copy writes that took the locked walk
    std::atomic<u64> erase_filtered{0};  // EraseHashedUploads calls the page filter rejected
    std::atomic<u64> lookup_filtered{0}; // hashed-upload dedupe lookups the page filter rejected
    std::atomic<u64> may_false{0};       // mode 2: the filter said "may touch" and the old path found nothing
    std::atomic<u64> verify_diff{0};     // mode 2: decisions of the filter or the journal that differ from the old path
};
inline Counters g_counters;

/// Adds n to a counter (the callers test On() first).
inline void Count(std::atomic<u64>& c, u64 n = 1) {
    c.fetch_add(n, std::memory_order_relaxed);
}

/// Hashed page counts over the entries of one owner (see the top of this file).
class PageCounts {
public:
    static constexpr u32 kSlots = 1u << 16;
    static constexpr u64 kPageShift = 12;
    /// A query over more pages than this takes the exact path (its answer is "may touch").
    static constexpr u64 kMaxPages = 256;

    /// Allocates the table only in modes 1 and 2 (mode 0 never queries it).
    PageCounts() {
        if (On()) {
            counts_.reset(new std::atomic<u32>[kSlots]()); // value-initialized: all zero
        }
    }

    /// Adds delta (+1 or -1) to the count of every page [addr, addr + size) touches. A size of 0 counts one page
    /// (the page of addr): EraseHashedUploads also erases an entry that starts inside a zero-size write.
    /// The owner's lock is held by the caller (adds and removes are serialized).
    void Add(u64 addr, u64 size, int delta) {
        if (!counts_) {
            return;
        }
        const u64 first = addr >> kPageShift;
        const u64 last = (addr + (size != 0 ? size - 1 : 0)) >> kPageShift;
        for (u64 page = first; page <= last; ++page) {
            counts_[Slot(page)].fetch_add(static_cast<u32>(delta), std::memory_order_release);
        }
    }

    /// False only when no entry covers any page of [addr, addr + size) (same size rule as Add). Lock-free.
    bool MayTouch(u64 addr, u64 size) const {
        if (!counts_) {
            return true;
        }
        const u64 first = addr >> kPageShift;
        const u64 last = (addr + (size != 0 ? size - 1 : 0)) >> kPageShift;
        if (last - first >= kMaxPages) {
            return true;
        }
        for (u64 page = first; page <= last; ++page) {
            if (counts_[Slot(page)].load(std::memory_order_acquire) != 0) {
                return true;
            }
        }
        return false;
    }

private:
    static u32 Slot(u64 page) {
        return static_cast<u32>((page * 0x9E3779B97F4A7C15ull) >> (64 - 16)); // kSlots == 2^16
    }

    std::unique_ptr<std::atomic<u32>[]> counts_;
};

/// The report line of the window (appended to out; nothing when the switch is off). seconds = window length.
inline void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const u64 ops = take(g_counters.ops);
    const u64 ranges = take(g_counters.ranges);
    const u64 coalesced = take(g_counters.coalesced);
    const u64 coalesce_ns = take(g_counters.coalesce_ns);
    const u64 diff = take(g_counters.verify_diff);
    const u64 native_filtered = take(g_counters.native_filtered);
    const u64 native_walks = take(g_counters.native_walks);
    const u64 erase_filtered = take(g_counters.erase_filtered);
    const u64 lookup_filtered = take(g_counters.lookup_filtered);
    const u64 may_false = take(g_counters.may_false);
    (void)seconds; // the counts are per window; the window length is the report's cadence
    char line[320];
    std::snprintf(line, sizeof line,
                  "SOTC WRITE_JOURNAL: ops %llu, ranges %llu, coalesced %llu, verify diff %llu, coalesce %.1f us\n",
                  static_cast<unsigned long long>(ops), static_cast<unsigned long long>(ranges),
                  static_cast<unsigned long long>(coalesced), static_cast<unsigned long long>(diff),
                  static_cast<double>(coalesce_ns) / 1000.0);
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC WRITE_JOURNAL filter: mode %d, native copies filtered %llu / walked %llu, hashed erase "
                  "filtered %llu, hashed lookup filtered %llu, may-touch without a hit %llu\n",
                  Mode(), static_cast<unsigned long long>(native_filtered),
                  static_cast<unsigned long long>(native_walks), static_cast<unsigned long long>(erase_filtered),
                  static_cast<unsigned long long>(lookup_filtered), static_cast<unsigned long long>(may_false));
    out += line;
}

} // namespace SotcWriteJournal
