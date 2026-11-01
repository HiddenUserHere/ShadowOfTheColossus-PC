// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_VERIFY, the verify side of the PC-native model. See verify.h for the parts.
//
// Hot paths (the fault handler, OnSubmit's scan, Count) use atomics and fixed tables only. The per-window
// maps (unannounced rows, range log, caller split) are behind mutexes that only the submit thread, the
// upload/bind path of verify mode and the report thread take; no guest fault takes a mutex.
#include "verify.h"

#include "../arena_attrib.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "native.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SotcVerify {
namespace {

constexpr uint64_t kPageShift = 12; // the page tracker's granule
constexpr size_t kPendingBits = 12;
constexpr size_t kPendingSlots = size_t{1} << kPendingBits;
constexpr size_t kProbe = 16;         // linear probe length of the pending and last-write tables
constexpr size_t kLastWriteBits = 16;
constexpr size_t kLastWriteSlots = size_t{1} << kLastWriteBits;
constexpr uint64_t kRangeCap = 16384;      // range log entries per window
constexpr size_t kPublishCap = 65536;      // publish records
constexpr size_t kBoundCap = size_t{1} << 18; // addresses remembered for the first-bind share
constexpr size_t kRipCap = 4096;           // distinct RIPs per window
constexpr size_t kTopRips = 32;
constexpr size_t kTopResources = 10;
constexpr size_t kTopCallers = 4;
constexpr size_t kCallerCap = 256;
constexpr uint64_t kOldAge = 2;            // bind_frame - publish_frame >= this: "written long before"
constexpr size_t kSuspectBits = 14;
constexpr size_t kSuspectSlots = size_t{1} << kSuspectBits;
constexpr uint64_t kSuspectFrames = 4;     // an unannounced write the GPU has not taken by then expires

// Pending write faults: the page, the last faulting write of it, and the frame it happened in.
struct Pending {
    std::atomic<uint64_t> key{0}; // page + 1, 0 = free
    std::atomic<uint64_t> addr{0};
    std::atomic<uint64_t> size{0};
    std::atomic<uint64_t> rip{0};
};
Pending g_pending[kPendingSlots];
std::atomic<uint64_t> g_pending_dropped{0};
std::atomic<uint64_t> g_faults_win{0}; // write faults recorded, this window

// Unannounced writes waiting to be taken by the GPU (one per page, the latest; direct mapped).
struct Suspect {
    std::atomic<uint64_t> key{0}; // page + 1, 0 = free
    std::atomic<uint64_t> addr{0};
    std::atomic<uint64_t> size{0};
    std::atomic<uint64_t> rip{0};
    std::atomic<uint64_t> frame{0};
};
Suspect g_suspects[kSuspectSlots];
std::atomic<bool> g_any_suspect{false};
std::atomic<uint64_t> g_suspect_replaced{0}; // a newer suspect took a slot still live (lost, counted)

// Last write per page (direct mapped; a collision overwrites, which only costs the range log a "unknown").
struct LastWrite {
    std::atomic<uint64_t> key{0}; // page + 1
    std::atomic<uint64_t> frame{0};
    std::atomic<uint64_t> rip{0};
};
LastWrite g_last[kLastWriteSlots];

std::atomic<uint64_t> g_frame{0};        // frames submitted (OnSubmit count)
std::atomic<uint64_t> g_window_frame0{0}; // g_frame at the last report (window start)

// Frame data counters of the window (Counter order).
std::atomic<uint64_t> g_ctr[static_cast<size_t>(Counter::Count)];

std::mutex g_agg_mutex; // unannounced rows, read by the report
struct RipRow {
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint64_t last_addr = 0;
    uint64_t first_frame = 0;
    uint64_t last_frame = 0;
};
std::unordered_map<uint64_t, RipRow> g_rips;
uint64_t g_rips_dropped = 0;
uint64_t g_unannounced_win = 0;
uint64_t g_unannounced_total = 0;
// Consumed suspects (the GPU took the bytes): rows per RIP, split by path.
struct ConsumedRow {
    uint64_t count = 0;
    uint64_t by_path[static_cast<size_t>(ConsumePath::Count)] = {};
    uint64_t small_binds = 0; // taken by a bind of at most kSmallBind bytes
    uint64_t read_binds = 0;  // taken by a bind the shader does not write
    uint64_t read_small = 0;  // both: the strongest sign of a real GPU read
    uint64_t max_bind = 0;
    uint64_t last_addr = 0;
};
constexpr uint64_t kSmallBind = 64 * 1024;
std::unordered_map<uint64_t, ConsumedRow> g_consumed;
uint64_t g_consumed_win = 0;
uint64_t g_consumed_total = 0;

std::mutex g_range_mutex; // range log, publish table, first-bind set, caller split
struct RangeEntry {
    uint64_t addr = 0;
    uint64_t size = 0;
    uint64_t bind_frame = 0;
    uint64_t caller = 0;
    uint64_t publish_frame = 0;
    uint64_t last_write_frame = 0;
    uint64_t last_write_rip = 0;
    bool has_publish = false;
    bool has_write = false;
    bool first_bind = false;
    bool streamed = false;
    std::string resource;
};
std::vector<RangeEntry> g_range;
uint64_t g_range_dropped = 0;
std::unordered_map<uint64_t, uint64_t> g_publish; // exact base address -> publish frame
std::unordered_set<uint64_t> g_bound;             // every address bound so far (session)

struct CallerRow {
    uint64_t count = 0;
    uint64_t bytes = 0;
};
// Key: counter index in the high byte, caller address below.
std::unordered_map<uint64_t, CallerRow> g_callers;

// Counters that are maxima, not sums.
bool IsPeak(Counter c) {
    return c == Counter::DeleterPeak;
}

size_t Slot(uint64_t key, size_t bits) {
    return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}

// Pending slot of the page, claimed when `claim` (lock-free). Null when none is free in the probe.
Pending* FindPending(uint64_t page, bool claim) {
    const size_t h = Slot(page, kPendingBits);
    for (size_t i = 0; i < kProbe; ++i) {
        Pending& p = g_pending[(h + i) & (kPendingSlots - 1)];
        const uint64_t k = p.key.load(std::memory_order_relaxed);
        if (k == page + 1) {
            return &p;
        }
        if (k == 0 && claim) {
            uint64_t expect = 0;
            if (p.key.compare_exchange_strong(expect, page + 1, std::memory_order_relaxed)) {
                return &p;
            }
            if (expect == page + 1) {
                return &p;
            }
        }
    }
    return nullptr;
}

// "0x<eboot offset>" inside the image, "host:<module>+0x<offset>" for port code (report thread only).
void FormatRip(char* buf, size_t n, uint64_t addr) {
    uint64_t off = 0;
    if (SotcArenaAttrib::ImageOffset(addr, &off)) {
        std::snprintf(buf, n, "0x%llx", static_cast<unsigned long long>(off));
        return;
    }
#ifdef _WIN32
    HMODULE module = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &module) &&
        GetModuleFileNameA(module, path, sizeof path) != 0) {
        const char* name = std::strrchr(path, '\\');
        std::snprintf(buf, n, "host:%s+0x%llx", name ? name + 1 : path,
                      static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(module)));
        return;
    }
#endif
    std::snprintf(buf, n, "host:0x%llx", static_cast<unsigned long long>(addr));
}

} // namespace

bool VerifyAll() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_VERIFY");
        const bool value = v && *v && std::strcmp(v, "0") != 0;
        if (value) {
            std::fprintf(stderr, "SOTC VERIFY: on (native verify, range log, frame data counters)\n");
        }
        return value;
    }();
    return on;
}

void NoteWriteFault(uint64_t addr, uint64_t size, uint64_t rip) {
    const bool native = SotcNative::Verify();
    if (!native && !VerifyAll()) {
        return;
    }
    const uint64_t page = addr >> kPageShift;
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (VerifyAll()) {
        LastWrite& lw = g_last[Slot(page, kLastWriteBits)];
        // Direct mapped: the slot is found by hash only, so a colliding page takes it over.
        lw.key.store(page + 1, std::memory_order_relaxed);
        lw.frame.store(frame, std::memory_order_relaxed);
        lw.rip.store(rip, std::memory_order_relaxed);
    }
    if (!native) {
        return;
    }
    g_faults_win.fetch_add(1, std::memory_order_relaxed);
    Pending* p = FindPending(page, true);
    if (!p) {
        g_pending_dropped.fetch_add(1, std::memory_order_relaxed); // probe full: counted, not lost silently
        return;
    }
    p->addr.store(addr, std::memory_order_relaxed);
    p->size.store(size, std::memory_order_relaxed);
    p->rip.store(rip, std::memory_order_relaxed);
}

void NoteAnnounce(uint64_t addr, uint64_t size) {
    if (size == 0 || !SotcNative::Verify()) {
        return;
    }
    const uint64_t first = addr >> kPageShift;
    const uint64_t last = (addr + size - 1) >> kPageShift;
    if (last - first < 256) {
        for (uint64_t page = first; page <= last; ++page) {
            if (Pending* p = FindPending(page, false)) {
                p->key.store(0, std::memory_order_relaxed);
            }
            // A late announce (after the submit) covers the suspect too.
            Suspect& s = g_suspects[Slot(page, kSuspectBits)];
            uint64_t expect = page + 1;
            s.key.compare_exchange_strong(expect, 0, std::memory_order_relaxed);
        }
        return;
    }
    // A large announce: one pass over the table is cheaper than probing every page.
    for (Pending& p : g_pending) {
        const uint64_t k = p.key.load(std::memory_order_relaxed);
        if (k != 0 && k - 1 >= first && k - 1 <= last) {
            p.key.store(0, std::memory_order_relaxed);
        }
    }
}

void OnSubmit() {
    if (SotcNative::Verify()) {
        for (Pending& p : g_pending) {
            if (p.key.exchange(0, std::memory_order_relaxed) == 0) {
                continue;
            }
            // Still pending at the submit: the GPU may read a range nobody announced.
            const uint64_t addr = p.addr.load(std::memory_order_relaxed);
            const uint64_t size = p.size.load(std::memory_order_relaxed);
            const uint64_t rip = p.rip.load(std::memory_order_relaxed);
            SotcNative::VerifyUnannouncedWrite(addr, size, rip);
            // A suspect until the GPU takes its bytes (NoteGpuConsume) or it expires.
            const uint64_t page = addr >> kPageShift;
            const uint64_t frame = g_frame.load(std::memory_order_relaxed);
            Suspect& s = g_suspects[Slot(page, kSuspectBits)];
            const uint64_t old = s.key.load(std::memory_order_relaxed);
            if (old != 0 && old != page + 1 && s.frame.load(std::memory_order_relaxed) + kSuspectFrames >= frame) {
                g_suspect_replaced.fetch_add(1, std::memory_order_relaxed);
            }
            s.addr.store(addr, std::memory_order_relaxed);
            s.size.store(size, std::memory_order_relaxed);
            s.rip.store(rip, std::memory_order_relaxed);
            s.frame.store(frame, std::memory_order_relaxed);
            s.key.store(page + 1, std::memory_order_release);
            g_any_suspect.store(true, std::memory_order_relaxed);
        }
    }
    g_frame.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Frame() {
    return g_frame.load(std::memory_order_relaxed);
}

void NoteGpuConsume(uint64_t addr, uint64_t size, ConsumePath path, uint64_t bind_size, bool bind_written) {
    if (size == 0 || !g_any_suspect.load(std::memory_order_relaxed) || !SotcNative::Verify()) {
        return;
    }
    const uint64_t now = g_frame.load(std::memory_order_relaxed);
    const auto take = [&](Suspect& s, uint64_t page) {
        if (s.key.load(std::memory_order_acquire) != page + 1) {
            return;
        }
        const uint64_t a = s.addr.load(std::memory_order_relaxed);
        const uint64_t n = s.size.load(std::memory_order_relaxed);
        if (a >= addr + size || a + n <= addr) {
            return; // the upload covers the page, not the written bytes
        }
        uint64_t expect = page + 1;
        if (!s.key.compare_exchange_strong(expect, 0, std::memory_order_relaxed)) {
            return;
        }
        if (s.frame.load(std::memory_order_relaxed) + kSuspectFrames < now) {
            return; // expired: never taken in time
        }
        const uint64_t rip = s.rip.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_agg_mutex);
        ++g_consumed_win;
        ++g_consumed_total;
        if (g_consumed.size() >= kRipCap && g_consumed.find(rip) == g_consumed.end()) {
            return;
        }
        ConsumedRow& row = g_consumed[rip];
        ++row.count;
        ++row.by_path[static_cast<size_t>(path)];
        if (bind_size <= kSmallBind) {
            ++row.small_binds;
        }
        if (!bind_written) {
            ++row.read_binds;
            if (bind_size <= kSmallBind) {
                ++row.read_small;
            }
        }
        row.max_bind = bind_size > row.max_bind ? bind_size : row.max_bind;
        row.last_addr = a;
    };
    const uint64_t first = addr >> kPageShift;
    const uint64_t last = (addr + size - 1) >> kPageShift;
    if (last - first < 256) {
        for (uint64_t page = first; page <= last; ++page) {
            take(g_suspects[Slot(page, kSuspectBits)], page);
        }
        return;
    }
    for (Suspect& s : g_suspects) { // a large upload: one pass over the table
        const uint64_t k = s.key.load(std::memory_order_relaxed);
        if (k != 0 && k - 1 >= first && k - 1 <= last) {
            take(s, k - 1);
        }
    }
}

void NotePublish(uint64_t addr, uint64_t size) {
    (void)size;
    if (!VerifyAll()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_range_mutex);
    if (g_publish.size() >= kPublishCap && g_publish.find(addr) == g_publish.end()) {
        return;
    }
    g_publish[addr] = g_frame.load(std::memory_order_relaxed);
}

void NoteBind(uint64_t addr, uint64_t size, uint64_t bind_frame, uint64_t caller, const char* resource,
              bool streamed) {
    if (!VerifyAll()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_range_mutex);
    if (g_range.size() >= kRangeCap) {
        ++g_range_dropped;
        return;
    }
    RangeEntry e;
    e.addr = addr;
    e.size = size;
    e.bind_frame = bind_frame;
    e.caller = caller;
    e.streamed = streamed;
    e.resource = resource ? resource : "";
    if (auto it = g_publish.find(addr); it != g_publish.end()) {
        e.has_publish = true;
        e.publish_frame = it->second;
    }
    const uint64_t page = addr >> kPageShift;
    const LastWrite& lw = g_last[Slot(page, kLastWriteBits)];
    if (lw.key.load(std::memory_order_relaxed) == page + 1) {
        e.has_write = true;
        e.last_write_frame = lw.frame.load(std::memory_order_relaxed);
        e.last_write_rip = lw.rip.load(std::memory_order_relaxed);
    }
    if (g_bound.size() < kBoundCap) {
        e.first_bind = g_bound.insert(addr).second;
    }
    g_range.push_back(std::move(e));
}

void Count(Counter c, uint64_t n) {
    if (!VerifyAll()) {
        return;
    }
    auto& slot = g_ctr[static_cast<size_t>(c)];
    if (IsPeak(c)) {
        uint64_t cur = slot.load(std::memory_order_relaxed);
        while (n > cur && !slot.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {
        }
        return;
    }
    slot.fetch_add(n, std::memory_order_relaxed);
}

void CountCaller(Counter c, uint64_t n, uint64_t caller) {
    Count(c, n);
    if (!VerifyAll() || IsPeak(c)) {
        return;
    }
    const uint64_t key = (static_cast<uint64_t>(c) << 56) | (caller & ((uint64_t{1} << 56) - 1));
    std::lock_guard<std::mutex> lock(g_range_mutex);
    if (g_callers.size() >= kCallerCap && g_callers.find(key) == g_callers.end()) {
        return;
    }
    CallerRow& row = g_callers[key];
    ++row.count;
    row.bytes += n;
}

} // namespace SotcVerify

namespace SotcVerify {

// sotc: the per-RIP aggregation of an unannounced write; SotcNative::VerifyUnannouncedWrite (native.cpp)
// counts it and calls this.
void AggregateUnannounced(uint64_t addr, uint64_t size, uint64_t rip) {
    std::lock_guard<std::mutex> lock(SotcVerify::g_agg_mutex);
    ++SotcVerify::g_unannounced_win;
    ++SotcVerify::g_unannounced_total;
    const uint64_t frame = SotcVerify::g_frame.load(std::memory_order_relaxed);
    auto it = SotcVerify::g_rips.find(rip);
    if (it == SotcVerify::g_rips.end()) {
        if (SotcVerify::g_rips.size() >= SotcVerify::kRipCap) {
            ++SotcVerify::g_rips_dropped;
            return;
        }
        it = SotcVerify::g_rips.emplace(rip, SotcVerify::RipRow{}).first;
        it->second.first_frame = frame;
    }
    SotcVerify::RipRow& row = it->second;
    ++row.count;
    row.bytes += size;
    row.last_addr = addr;
    row.last_frame = frame;
}

} // namespace SotcNative

namespace SotcVerify {

void Report(std::string& out, double seconds) {
    if (!VerifyAll() && !SotcNative::Verify()) {
        return;
    }
    char buf[512];

    // Unannounced writes: top RIPs of the window, then the window totals.
    {
        std::lock_guard<std::mutex> lock(g_agg_mutex);
        std::vector<std::pair<uint64_t, RipRow>> rows(g_rips.begin(), g_rips.end());
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
        std::snprintf(buf, sizeof buf,
                      "SOTC VERIFY UNANNOUNCED window=%.1fs writes=%llu faults=%llu rips=%zu dropped_rips=%llu "
                      "pending_dropped=%llu total=%llu\n",
                      seconds, static_cast<unsigned long long>(g_unannounced_win),
                      static_cast<unsigned long long>(g_faults_win.load(std::memory_order_relaxed)),
                      g_rips.size(), static_cast<unsigned long long>(g_rips_dropped),
                      static_cast<unsigned long long>(g_pending_dropped.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_unannounced_total));
        out += buf;
        for (size_t i = 0; i < rows.size() && i < kTopRips; ++i) {
            const RipRow& r = rows[i].second;
            char where[64];
            FormatRip(where, sizeof where, rows[i].first);
            std::snprintf(buf, sizeof buf,
                          "SOTC VERIFY UNANNOUNCED %zu rip=%s n=%llu bytes=%llu last_addr=0x%llx frames=%llu-%llu\n",
                          i + 1, where, static_cast<unsigned long long>(r.count),
                          static_cast<unsigned long long>(r.bytes), static_cast<unsigned long long>(r.last_addr),
                          static_cast<unsigned long long>(r.first_frame),
                          static_cast<unsigned long long>(r.last_frame));
            out += buf;
        }
        g_rips.clear();
        g_unannounced_win = 0;
        g_rips_dropped = 0;

        // The suspects the GPU took (real misses), top RIPs by count.
        std::vector<std::pair<uint64_t, ConsumedRow>> taken(g_consumed.begin(), g_consumed.end());
        std::sort(taken.begin(), taken.end(),
                  [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
        std::snprintf(buf, sizeof buf,
                      "SOTC VERIFY CONSUMED window=%.1fs writes=%llu rips=%zu total=%llu suspects_replaced=%llu "
                      "(unannounced writes whose bytes the GPU uploaded within %llu frames; must be 0)\n",
                      seconds, static_cast<unsigned long long>(g_consumed_win), g_consumed.size(),
                      static_cast<unsigned long long>(g_consumed_total),
                      static_cast<unsigned long long>(g_suspect_replaced.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(kSuspectFrames));
        out += buf;
        for (size_t i = 0; i < taken.size() && i < kTopRips; ++i) {
            const ConsumedRow& r = taken[i].second;
            char where[64];
            FormatRip(where, sizeof where, taken[i].first);
            std::snprintf(buf, sizeof buf,
                          "SOTC VERIFY CONSUMED %zu rip=%s n=%llu buffer=%llu image=%llu small_binds=%llu "
                          "read_binds=%llu read_small=%llu max_bind=%llu last_addr=0x%llx\n",
                          i + 1, where, static_cast<unsigned long long>(r.count),
                          static_cast<unsigned long long>(r.by_path[0]),
                          static_cast<unsigned long long>(r.by_path[1]),
                          static_cast<unsigned long long>(r.small_binds),
                          static_cast<unsigned long long>(r.read_binds),
                          static_cast<unsigned long long>(r.read_small),
                          static_cast<unsigned long long>(r.max_bind),
                          static_cast<unsigned long long>(r.last_addr));
            out += buf;
        }
        g_consumed.clear();
        g_consumed_win = 0;
    }
    g_faults_win.store(0, std::memory_order_relaxed);
    g_pending_dropped.store(0, std::memory_order_relaxed);

    if (!VerifyAll()) {
        return;
    }

    // Range log: the window's sync-copy ranges, split by old / first-bind / class, top resources by bytes.
    {
        std::lock_guard<std::mutex> lock(g_range_mutex);
        uint64_t bytes = 0, old_bytes = 0, known_bytes = 0, first_bytes = 0;
        uint64_t cls_static = 0, cls_streamed = 0, cls_dynamic = 0, cls_unknown = 0;
        std::unordered_map<std::string, CallerRow> by_res;
        for (const RangeEntry& e : g_range) {
            bytes += e.size;
            if (e.has_publish) {
                known_bytes += e.size;
                if (e.bind_frame >= e.publish_frame + kOldAge) {
                    old_bytes += e.size;
                }
            }
            if (e.first_bind) {
                first_bytes += e.size;
            }
            if (!e.has_publish) {
                cls_unknown += e.size;
            } else if (e.has_write && e.last_write_frame > e.publish_frame) {
                cls_dynamic += e.size;
            } else if (e.streamed) {
                cls_streamed += e.size;
            } else {
                cls_static += e.size;
            }
            CallerRow& r = by_res[e.resource.empty() ? std::string("-") : e.resource];
            ++r.count;
            r.bytes += e.size;
        }
        auto pct = [](uint64_t part, uint64_t whole) {
            return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
        };
        std::snprintf(buf, sizeof buf,
                      "SOTC VERIFY RANGE window entries=%zu dropped=%llu bytes=%.1fMiB written_long_before=%.1f%% "
                      "(of %.1fMiB with publish) first_bind=%.1f%% class static=%.1f%% streamed=%.1f%% "
                      "dynamic=%.1f%% unknown=%.1f%%\n",
                      g_range.size(), static_cast<unsigned long long>(g_range_dropped),
                      static_cast<double>(bytes) / (1024.0 * 1024.0), pct(old_bytes, known_bytes),
                      static_cast<double>(known_bytes) / (1024.0 * 1024.0), pct(first_bytes, bytes),
                      pct(cls_static, bytes), pct(cls_streamed, bytes), pct(cls_dynamic, bytes),
                      pct(cls_unknown, bytes));
        out += buf;
        std::vector<std::pair<std::string, CallerRow>> res(by_res.begin(), by_res.end());
        std::sort(res.begin(), res.end(),
                  [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        for (size_t i = 0; i < res.size() && i < kTopResources; ++i) {
            std::snprintf(buf, sizeof buf, "SOTC VERIFY RANGE RES %zu name=%s n=%llu bytes=%.1fMiB\n", i + 1,
                          res[i].first.c_str(), static_cast<unsigned long long>(res[i].second.count),
                          static_cast<double>(res[i].second.bytes) / (1024.0 * 1024.0));
            out += buf;
        }
        g_range.clear();
    }

    // Frame data: per-frame averages of the window, then the top callers of the split counters.
    const uint64_t frames_now = g_frame.load(std::memory_order_relaxed);
    const uint64_t frames0 = g_window_frame0.exchange(frames_now, std::memory_order_relaxed);
    const uint64_t frames = frames_now > frames0 ? frames_now - frames0 : 0;
    uint64_t v[static_cast<size_t>(Counter::Count)];
    for (size_t i = 0; i < static_cast<size_t>(Counter::Count); ++i) {
        v[i] = g_ctr[i].exchange(0, std::memory_order_relaxed);
    }
    const double f = frames ? static_cast<double>(frames) : 1.0;
    auto per = [f](uint64_t x) { return static_cast<double>(x) / f; };
    const auto at = [&v](Counter c) { return v[static_cast<size_t>(c)]; };
    const uint64_t class_total = at(Counter::DcbBytes) + at(Counter::CcbBytes) + at(Counter::HeapBytes) +
                                 at(Counter::ResourceBytes);
    std::snprintf(
        buf, sizeof buf,
        "SOTC FRAMEDATA window=%.1fs frames=%llu a_dcb=%.0fB b_ccb=%.0fB c_heap=%.0fB/%.1f calls d_heapfail=%.1f "
        "d_tier=%.1f e_deleter=%.1f/%.0fB peak=%llu f_ctx=%.1f/%.0fB g_res=%.1f/%.0fB h_const=%.1f/%.1f slots "
        "i_disp=%.1f/%.0fB j_classes=%.0fB per frame (%.1f MiB/s)\n",
        seconds, static_cast<unsigned long long>(frames), per(at(Counter::DcbBytes)), per(at(Counter::CcbBytes)),
        per(at(Counter::HeapBytes)), per(at(Counter::HeapCalls)), per(at(Counter::HeapFail)),
        per(at(Counter::HeapTierHits)), per(at(Counter::DeleterItems)), per(at(Counter::DeleterBytes)),
        static_cast<unsigned long long>(at(Counter::DeleterPeak)), per(at(Counter::ContextCalls)),
        per(at(Counter::ContextBytes)), per(at(Counter::ResourceCalls)), per(at(Counter::ResourceBytes)),
        per(at(Counter::ConstWrites)), per(at(Counter::SlotWrites)), per(at(Counter::DispatchCalls)),
        per(at(Counter::DispatchBytes)), per(class_total),
        seconds > 0.0 ? static_cast<double>(class_total) / (1024.0 * 1024.0) / seconds : 0.0);
    out += buf;

    std::lock_guard<std::mutex> lock(g_range_mutex);
    for (size_t c = 0; c < 3; ++c) { // a, b, c: the caller-split counters
        std::vector<std::pair<uint64_t, CallerRow>> rows;
        for (const auto& [key, row] : g_callers) {
            if ((key >> 56) == c) {
                rows.emplace_back(key & ((uint64_t{1} << 56) - 1), row);
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        static const char* const names[3] = {"a_dcb", "b_ccb", "c_heap"};
        for (size_t i = 0; i < rows.size() && i < kTopCallers; ++i) {
            char where[64];
            FormatRip(where, sizeof where, rows[i].first);
            std::snprintf(buf, sizeof buf, "SOTC FRAMEDATA CALLER %s %zu caller=%s n=%llu bytes=%llu\n", names[c],
                          i + 1, where, static_cast<unsigned long long>(rows[i].second.count),
                          static_cast<unsigned long long>(rows[i].second.bytes));
            out += buf;
        }
    }
    g_callers.clear();
}

} // namespace SotcVerify
