// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_WRITE_SCOPE_FLAT. The GPU writes of the command buffer being built (BufferCache::current_writes and the
// write ring of RollCurrentWrites) without a boost::icl interval set on the per-op path.
//
// Mode 0 (default): WriteSet keeps the RangeSet only, exactly as before.
// Mode 1: WriteSet keeps a FlatWriteSet only. An add appends the byte range to a vector and sets one bit per page
//         it touches in a 2^16-slot hashed page bitmap (O(pages) per op, no tree). A query first tests the bits of
//         its pages: no bit set means no range can share a byte, so the answer is false. Otherwise the vector is
//         sorted and merged once (lazily, at the first query that needs it) and binary-searched, which is exact.
// Mode 2: both are kept and every query is answered by both; the RangeSet answer is returned (the old behaviour),
//         and every difference is counted (verify mismatches, must read 0).
//
// Why the page bitmap is a safe prefilter: two ranges that share a byte share a page, so the page of every byte
// of the set is marked. A hashed slot can only add false "may touch" answers, which take the exact path.
// Clearing unsets only the words that were non-zero (dirty word list), so a tick's reset costs O(pages written).
// A write covering more pages than there are slots marks every slot: the answers are then all "may touch".
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/range_set.h"

namespace VideoCore {

namespace WriteSetFlat {

/// SOTC_WRITE_SCOPE_FLAT (read once): 0 off (RangeSet), 1 the flat set, 2 verify (both, compared at each query).
inline int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_WRITE_SCOPE_FLAT");
        return (v && v[0] == '1') ? 1 : ((v && v[0] == '2') ? 2 : 0);
    }();
    return mode;
}

/// Window counters (Report prints them every 5 seconds). Atomic: writers and readers may be on different threads.
struct Counters {
    std::atomic<u64> ops{0};            // written ranges added (size > 0)
    std::atomic<u64> ranges{0};         // intervals left by the merge of the vector (counted at each merge)
    std::atomic<u64> pages{0};          // page bits set (writes covering more slots than exist count all slots)
    std::atomic<u64> verify_queries{0}; // mode 2: queries answered by both sets
    std::atomic<u64> mismatches{0};     // mode 2: queries whose two answers differ (must read 0)
};
inline Counters g_counters;

/// Flat set of byte ranges with a hashed page bitmap as a prefilter. Not thread-safe (the owner's rules apply).
class FlatWriteSet {
public:
    static constexpr u64 kPageShift = 12;
    static constexpr u32 kSlotBits = 16;
    static constexpr u64 kSlots = u64{1} << kSlotBits;
    static constexpr u32 kWords = static_cast<u32>(kSlots / 64);
    /// A query over more pages than this skips the bitmap and takes the exact path.
    static constexpr u64 kMaxProbePages = 256;

    FlatWriteSet() {
        if (Mode() != 0) { // mode 0 never touches the set: no bitmap allocated
            words_.assign(kWords, 0);
        }
    }

    /// Adds [addr, addr + size). A size of 0 adds nothing (as RangeSet::Add does).
    void Add(VAddr addr, u64 size) {
        if (size == 0) {
            return;
        }
        g_counters.ops.fetch_add(1, std::memory_order_relaxed);
        ranges_.emplace_back(addr, addr + size);
        normal_ = false;
        const u64 first = addr >> kPageShift;
        const u64 last = (addr + size - 1) >> kPageShift;
        if (last - first + 1 >= kSlots) { // covers every slot: the bitmap says "may touch" for everything
            for (u32 w = 0; w < kWords; ++w) {
                SetWord(w, ~u64{0});
            }
            g_counters.pages.fetch_add(kSlots, std::memory_order_relaxed);
            return;
        }
        for (u64 page = first; page <= last; ++page) {
            const u32 slot = Slot(page);
            SetWord(slot >> 6, u64{1} << (slot & 63));
        }
        g_counters.pages.fetch_add(last - first + 1, std::memory_order_relaxed);
    }

    /// True when some added range shares a byte with [addr, addr + size). Same answer as RangeSet::Intersects.
    bool Intersects(VAddr addr, u64 size) const {
        if (size == 0 || ranges_.empty()) {
            return false;
        }
        const u64 end = addr + size;
        const u64 first = addr >> kPageShift;
        const u64 last = (end - 1) >> kPageShift;
        if (last - first < kMaxProbePages) {
            bool may_touch = false;
            for (u64 page = first; page <= last && !may_touch; ++page) {
                const u32 slot = Slot(page);
                may_touch = (words_[slot >> 6] >> (slot & 63)) & 1;
            }
            if (!may_touch) {
                return false;
            }
        }
        Normalize();
        // Disjoint and sorted: the only interval that can reach addr is the last one that starts before end.
        const auto it = std::lower_bound(ranges_.begin(), ranges_.end(), end,
                                         [](const std::pair<VAddr, VAddr>& r, VAddr v) { return r.first < v; });
        return it != ranges_.begin() && std::prev(it)->second > addr;
    }

    bool Empty() const {
        return ranges_.empty();
    }

    void Clear() {
        ranges_.clear();
        normal_ = true;
        for (const u32 w : dirty_words_) {
            words_[w] = 0;
        }
        dirty_words_.clear();
    }

    /// Exchanges the contents (the write ring files a tick's writes this way).
    void Swap(FlatWriteSet& other) {
        ranges_.swap(other.ranges_);
        std::swap(normal_, other.normal_);
        words_.swap(other.words_);
        dirty_words_.swap(other.dirty_words_);
    }

private:
    static u32 Slot(u64 page) {
        return static_cast<u32>((page * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits));
    }

    /// ORs mask into word w and remembers the word once it becomes non-zero.
    void SetWord(u32 w, u64 mask) {
        if (words_[w] == 0 && mask != 0) {
            dirty_words_.push_back(w);
        }
        words_[w] |= mask;
    }

    /// Sorts the vector and merges overlapping or touching ranges into disjoint ones.
    void Normalize() const {
        if (normal_) {
            return;
        }
        std::sort(ranges_.begin(), ranges_.end());
        size_t out = 0;
        for (size_t i = 0; i < ranges_.size(); ++i) {
            if (out != 0 && ranges_[i].first <= ranges_[out - 1].second) {
                ranges_[out - 1].second = std::max(ranges_[out - 1].second, ranges_[i].second);
            } else {
                ranges_[out++] = ranges_[i];
            }
        }
        ranges_.resize(out);
        normal_ = true;
        g_counters.ranges.fetch_add(out, std::memory_order_relaxed);
    }

    mutable std::vector<std::pair<VAddr, VAddr>> ranges_; // [lo, hi); sorted and merged when normal_
    mutable bool normal_{true};
    std::vector<u64> words_;       // one bit per page slot
    std::vector<u32> dirty_words_; // indices of the words that are non-zero
};

/// The set BufferCache uses for current_writes and the write ring. Mode 0 uses RangeSet, mode 1 FlatWriteSet,
/// mode 2 both (see the top of this file). Queries return the RangeSet's answer in every mode.
class WriteSet {
public:
    void Add(VAddr addr, u64 size) {
        const int mode = Mode();
        if (mode != 1) {
            old_.Add(addr, size);
        }
        if (mode != 0) {
            flat_.Add(addr, size);
        }
    }

    bool Intersects(VAddr addr, u64 size) const {
        const int mode = Mode();
        if (mode == 0) {
            return old_.Intersects(addr, size);
        }
        if (mode == 1) {
            return flat_.Intersects(addr, size);
        }
        const bool want = old_.Intersects(addr, size);
        const bool got = flat_.Intersects(addr, size);
        g_counters.verify_queries.fetch_add(1, std::memory_order_relaxed);
        if (want != got) {
            g_counters.mismatches.fetch_add(1, std::memory_order_relaxed);
        }
        return want;
    }

    bool Empty() const {
        const int mode = Mode();
        if (mode == 0) {
            return old_.Empty();
        }
        if (mode == 1) {
            return flat_.Empty();
        }
        const bool want = old_.Empty();
        const bool got = flat_.Empty();
        g_counters.verify_queries.fetch_add(1, std::memory_order_relaxed);
        if (want != got) {
            g_counters.mismatches.fetch_add(1, std::memory_order_relaxed);
        }
        return want;
    }

    void Clear() {
        const int mode = Mode();
        if (mode != 1) {
            old_.Clear();
        }
        if (mode != 0) {
            flat_.Clear();
        }
    }

    void Swap(WriteSet& other) {
        old_.Swap(other.old_);
        flat_.Swap(other.flat_);
    }

private:
    RangeSet old_;
    FlatWriteSet flat_;
};

/// The report line (appended to out) every 5 seconds of report windows; nothing when the switch is off.
/// seconds = length of the window that just ended. The counters are taken (reset) when a line is printed.
inline void Report(std::string& out, double seconds) {
    if (Mode() == 0) {
        return;
    }
    static double window_seconds = 0.0; // only the report hub calls this
    window_seconds += seconds;
    if (window_seconds < 5.0) {
        return;
    }
    window_seconds = 0.0;
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC WRITE SCOPE FLAT: ops %llu, ranges %llu, pages %llu, verify queries %llu, mismatches %llu\n",
                  static_cast<unsigned long long>(take(g_counters.ops)),
                  static_cast<unsigned long long>(take(g_counters.ranges)),
                  static_cast<unsigned long long>(take(g_counters.pages)),
                  static_cast<unsigned long long>(take(g_counters.verify_queries)),
                  static_cast<unsigned long long>(take(g_counters.mismatches)));
    out += line;
}

} // namespace WriteSetFlat

using WriteSet = WriteSetFlat::WriteSet;

} // namespace VideoCore
