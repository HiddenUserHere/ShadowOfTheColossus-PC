// SPDX-FileCopyrightText: 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/icl/discrete_interval.hpp>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/split_interval_map.hpp>
#include <boost/icl/split_interval_set.hpp>
#include <boost/pool/pool.hpp>
#include <boost/pool/pool_alloc.hpp>
#include <boost/pool/poolfwd.hpp>
#include <algorithm>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/sotc_generation.h" // sotc: SOTC_FLAT_NOTES mode and its counters

namespace VideoCore {

template <class T>
using RangeSetsAllocator =
    boost::fast_pool_allocator<T, boost::default_user_allocator_new_delete,
                               boost::details::pool::default_mutex, 1024, 2048>;

struct RangeSet {
    using IntervalSet =
        boost::icl::interval_set<VAddr, std::less,
                                 ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, VAddr, std::less),
                                 RangeSetsAllocator>;
    using IntervalType = typename IntervalSet::interval_type;

    /// sotc: SOTC_FLAT_NOTES: how many intervals AddFast remembers as contained in the set.
    static constexpr size_t KnownSlots = 16;

    explicit RangeSet() = default;
    ~RangeSet() = default;

    /// sotc: returns whether the set grew (false: already contained). SOTC_FLAT_NOTES=2 also adds to
    /// shadow_set and counts a disagreement; the shadow's answer is returned.
    bool Add(VAddr base_address, size_t size) {
        if (SotcGen::FlatNotesMode() == 2) {
            const bool primary = AddIn(m_ranges_set, base_address, size);
            const bool shadow = AddIn(shadow_set, base_address, size);
            if (primary != shadow) {
                SotcGen::Count(SotcGen::Stat::FlatMismatch);
            }
            return shadow;
        }
        return AddIn(m_ranges_set, base_address, size);
    }

    /// sotc: SOTC_FLAT_NOTES: Add, but an interval inside a remembered one (contained in the set when
    /// it was remembered; every change of the set forgets them) returns false without the lookup.
    /// Same result and same set as Add in every mode. Mode 2: the remembered path runs on m_ranges_set
    /// and the plain Add on shadow_set; the results must agree (FlatMismatch), the shadow's is returned.
    bool AddFast(VAddr base_address, size_t size) {
        const int mode = SotcGen::FlatNotesMode();
        if (mode == 0 || size == 0) {
            return Add(base_address, size);
        }
        bool grew = false;
        if (KnownContains(base_address, size)) {
            SotcGen::Count(SotcGen::Stat::FlatAddHit);
        } else {
            SotcGen::Count(SotcGen::Stat::FlatAddMiss);
            grew = AddIn(m_ranges_set, base_address, size);
            Remember(IntervalType{base_address, base_address + size});
        }
        if (mode == 2) {
            const bool shadow = AddIn(shadow_set, base_address, size);
            if (shadow != grew) {
                SotcGen::Count(SotcGen::Stat::FlatMismatch);
            }
            return shadow;
        }
        return grew;
    }

    /// sotc: true when [base_address, base_address + size) lies inside one remembered interval, so the set
    /// contains it. Read-only: the caller holds the lock that guards the set.
    bool KnownContains(VAddr base_address, size_t size) const {
        const IntervalType interval{base_address, base_address + size};
        for (size_t i = 0; i < known_count; ++i) {
            if (boost::icl::contains(known[i], interval)) {
                return true;
            }
        }
        return false;
    }

    void Subtract(VAddr base_address, size_t size) {
        ForgetKnown();
        // sotc: BbToggle::WriteMarkMemo: a counter of removals; while it is unchanged, what Add
        // put into the set is still there.
        __atomic_fetch_add(&generation, 1, __ATOMIC_RELEASE);
        if (SotcGen::FlatNotesMode() == 2) {
            SubtractIn(shadow_set, base_address, size);
        }
        SubtractIn(m_ranges_set, base_address, size);
    }

    void Clear() {
        ForgetKnown();
        __atomic_fetch_add(&generation, 1, __ATOMIC_RELEASE);
        if (SotcGen::FlatNotesMode() == 2) {
            shadow_set.clear();
        }
        m_ranges_set.clear();
    }

    /// sotc: removals (Subtract, Clear, Swap) so far; read with __atomic_load_n.
    u64 generation{};

    /// sotc: SOTC_RB_TICK write ring.
    bool Empty() const {
        return Answer([](const IntervalSet& set) { return set.empty(); });
    }

    /// sotc: exchanges the contents without copying (the ring files a tick's writes). Both sets
    /// lose what they held, so both generations move (WriteMarkMemo).
    void Swap(RangeSet& other) {
        m_ranges_set.swap(other.m_ranges_set);
        shadow_set.swap(other.shadow_set);
        ForgetKnown();
        other.ForgetKnown();
        __atomic_fetch_add(&generation, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&other.generation, 1, __ATOMIC_RELEASE);
    }

    bool Contains(VAddr base_address, size_t size) const {
        const IntervalType interval{base_address, base_address + size};
        return Answer([&](const IntervalSet& set) { return boost::icl::contains(set, interval); });
    }

    bool Intersects(VAddr base_address, size_t size) const {
        const IntervalType interval{base_address, base_address + size};
        return Answer([&](const IntervalSet& set) { return boost::icl::intersects(set, interval); });
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (SotcGen::FlatNotesMode() != 2) {
            ForEachIn(m_ranges_set, func);
            return;
        }
        std::vector<std::pair<VAddr, VAddr>> primary, shadow;
        ForEachIn(m_ranges_set, [&](VAddr lo, VAddr hi) { primary.emplace_back(lo, hi); });
        ForEachIn(shadow_set, [&](VAddr lo, VAddr hi) { shadow.emplace_back(lo, hi); });
        if (primary != shadow) {
            SotcGen::Count(SotcGen::Stat::FlatMismatch);
        }
        for (const auto& [lo, hi] : shadow) {
            func(lo, hi);
        }
    }

    template <typename Func>
    void ForEachInRange(VAddr base_addr, size_t size, Func&& func) const {
        if (SotcGen::FlatNotesMode() != 2) {
            ForEachInRangeIn(m_ranges_set, base_addr, size, func);
            return;
        }
        std::vector<std::pair<VAddr, VAddr>> primary, shadow;
        ForEachInRangeIn(m_ranges_set, base_addr, size,
                         [&](VAddr lo, VAddr hi) { primary.emplace_back(lo, hi); });
        ForEachInRangeIn(shadow_set, base_addr, size,
                         [&](VAddr lo, VAddr hi) { shadow.emplace_back(lo, hi); });
        if (primary != shadow) {
            SotcGen::Count(SotcGen::Stat::FlatMismatch);
        }
        for (const auto& [lo, hi] : shadow) {
            func(lo, hi);
        }
    }

    template <typename Func>
    void ForEachNotInRange(VAddr base_addr, size_t size, Func&& func) const {
        const VAddr end_addr = base_addr + size;
        ForEachInRange(base_addr, size, [&](VAddr range_addr, VAddr range_end) {
            if (size_t gap_size = range_addr - base_addr; gap_size != 0) {
                func(base_addr, gap_size);
            }
            base_addr = range_end;
        });
        if (base_addr != end_addr) {
            func(base_addr, end_addr - base_addr);
        }
    }

    IntervalSet m_ranges_set;

private:
    static bool AddIn(IntervalSet& set, VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        // sotc: draws mark the same written buffers again and again; a lookup costs far less
        // than an insertion that changes nothing.
        if (const auto it = set.find(interval); it != set.end() && boost::icl::contains(*it, interval)) {
            return false;
        }
        set.add(interval);
        return true;
    }

    static void SubtractIn(IntervalSet& set, VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        set.subtract(interval);
    }

    /// Mode 2 reader check: the query on both sets, a disagreement counted, the shadow's answer returned.
    template <typename Query>
    bool Answer(Query&& query) const {
        if (SotcGen::FlatNotesMode() != 2) {
            return query(m_ranges_set);
        }
        const bool primary = query(m_ranges_set);
        const bool shadow = query(shadow_set);
        if (primary != shadow) {
            SotcGen::Count(SotcGen::Stat::FlatMismatch);
        }
        return shadow;
    }

    template <typename Func>
    static void ForEachIn(const IntervalSet& set, Func&& func) {
        if (set.empty()) {
            return;
        }

        for (const auto& interval : set) {
            const VAddr inter_addr_end = interval.upper();
            const VAddr inter_addr = interval.lower();
            func(inter_addr, inter_addr_end);
        }
    }

    template <typename Func>
    static void ForEachInRangeIn(const IntervalSet& set, VAddr base_addr, size_t size, Func&& func) {
        if (set.empty()) {
            return;
        }
        const VAddr start_address = base_addr;
        const VAddr end_address = start_address + size;
        const IntervalType search_interval{start_address, end_address};
        auto it = set.lower_bound(search_interval);
        if (it == set.end()) {
            return;
        }
        auto end_it = set.upper_bound(search_interval);
        for (; it != end_it; it++) {
            VAddr inter_addr_end = it->upper();
            VAddr inter_addr = it->lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end);
        }
    }

    /// Remembers an interval that the set contains now (the oldest slot is overwritten when all are used).
    /// The valid slots are always [0, known_count): writes start at 0 after every ForgetKnown.
    void Remember(const IntervalType& interval) {
        known[known_next] = interval;
        known_next = (known_next + 1) % KnownSlots;
        known_count = std::min(known_count + 1, KnownSlots);
    }

    /// Every change that can remove content from the set forgets the remembered intervals.
    void ForgetKnown() {
        known_count = 0;
        known_next = 0;
    }

    IntervalSet shadow_set;                // sotc: SOTC_FLAT_NOTES=2 only: the plain set, kept in step
    IntervalType known[KnownSlots];         // sotc: SOTC_FLAT_NOTES=1,2: intervals the set contains
    size_t known_count = 0;
    size_t known_next = 0;
};

template <typename T>
class RangeMap {
public:
    using IntervalMap =
        boost::icl::interval_map<VAddr, T, boost::icl::total_absorber, std::less,
                                 boost::icl::inplace_identity, boost::icl::inter_section,
                                 ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, VAddr, std::less),
                                 RangeSetsAllocator>;
    using IntervalType = typename IntervalMap::interval_type;

public:
    RangeMap() = default;
    ~RangeMap() = default;

    RangeMap(RangeMap const&) = delete;
    RangeMap& operator=(RangeMap const&) = delete;

    RangeMap(RangeMap&& other);
    RangeMap& operator=(RangeMap&& other);

    void Add(VAddr base_address, size_t size, const T& value) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map.add({interval, value});
    }

    void Subtract(VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map -= interval;
    }

    void Clear() {
        m_ranges_map.clear();
    }

    bool Contains(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::contains(m_ranges_map, interval);
    }

    bool Intersects(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::intersects(m_ranges_map, interval);
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }

        for (const auto& [interval, value] : m_ranges_map) {
            const VAddr inter_addr_end = interval.upper();
            const VAddr inter_addr = interval.lower();
            func(inter_addr, inter_addr_end, value);
        }
    }

    template <typename Func>
    void ForEachInRange(VAddr base_addr, size_t size, Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }
        const VAddr start_address = base_addr;
        const VAddr end_address = start_address + size;
        const IntervalType search_interval{start_address, end_address};
        auto it = m_ranges_map.lower_bound(search_interval);
        if (it == m_ranges_map.end()) {
            return;
        }
        auto end_it = m_ranges_map.upper_bound(search_interval);
        for (; it != end_it; it++) {
            VAddr inter_addr_end = it->first.upper();
            VAddr inter_addr = it->first.lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    template <typename Func>
    void ForEachNotInRange(VAddr base_addr, size_t size, Func&& func) const {
        const VAddr end_addr = base_addr + size;
        ForEachInRange(base_addr, size, [&](VAddr range_addr, VAddr range_end, const T&) {
            if (size_t gap_size = range_addr - base_addr; gap_size != 0) {
                func(base_addr, gap_size);
            }
            base_addr = range_end;
        });
        if (base_addr != end_addr) {
            func(base_addr, end_addr - base_addr);
        }
    }

private:
    IntervalMap m_ranges_map;
};

template <typename T>
class SplitRangeMap {
public:
    using IntervalMap = boost::icl::split_interval_map<
        VAddr, T, boost::icl::total_absorber, std::less, boost::icl::inplace_identity,
        boost::icl::inter_section, ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, VAddr, std::less),
        RangeSetsAllocator>;
    using IntervalType = typename IntervalMap::interval_type;

public:
    SplitRangeMap() = default;
    ~SplitRangeMap() = default;

    SplitRangeMap(SplitRangeMap const&) = delete;
    SplitRangeMap& operator=(SplitRangeMap const&) = delete;

    SplitRangeMap(SplitRangeMap&& other);
    SplitRangeMap& operator=(SplitRangeMap&& other);

    void Add(VAddr base_address, size_t size, const T& value) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map.add({interval, value});
    }

    void Subtract(VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map -= interval;
    }

    void Clear() {
        m_ranges_map.clear();
    }

    bool Contains(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::contains(m_ranges_map, interval);
    }

    bool Intersects(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::intersects(m_ranges_map, interval);
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }

        for (const auto& [interval, value] : m_ranges_map) {
            const VAddr inter_addr_end = interval.upper();
            const VAddr inter_addr = interval.lower();
            func(inter_addr, inter_addr_end, value);
        }
    }

    template <typename Func>
    void ForEachInRange(VAddr base_addr, size_t size, Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }
        const VAddr start_address = base_addr;
        const VAddr end_address = start_address + size;
        const IntervalType search_interval{start_address, end_address};
        auto it = m_ranges_map.lower_bound(search_interval);
        if (it == m_ranges_map.end()) {
            return;
        }
        auto end_it = m_ranges_map.upper_bound(search_interval);
        for (; it != end_it; it++) {
            VAddr inter_addr_end = it->first.upper();
            VAddr inter_addr = it->first.lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    template <typename Func>
    void ForEachNotInRange(VAddr base_addr, size_t size, Func&& func) const {
        const VAddr end_addr = base_addr + size;
        ForEachInRange(base_addr, size, [&](VAddr range_addr, VAddr range_end, const T&) {
            if (size_t gap_size = range_addr - base_addr; gap_size != 0) {
                func(base_addr, gap_size);
            }
            base_addr = range_end;
        });
        if (base_addr != end_addr) {
            func(base_addr, end_addr - base_addr);
        }
    }

private:
    IntervalMap m_ranges_map;
};

} // namespace VideoCore
