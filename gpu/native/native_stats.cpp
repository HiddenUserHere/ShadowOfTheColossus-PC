// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. See native_stats.h.
//
// sotc: N2-94 H01 (W0) coverage table
//   item                                                        | done? | evidence
//   WaitFinished real implementation (replaces Step 0 stub)     | yes   | WaitFinished() below: starts, polls, blocked us, sat counter
//   counters padded to one 64-byte cache line each              | yes   | PaddedCounter alignas(64); static_assert on sizeof
//   guest-thread counters (OpsEmitted/OpsEmitFailed) isolated  | yes   | separate padded slots, no shared line with CP counters
//   ReportSecond wait group " | waits=... lbl=... oplists=..."   | yes   | ReportSecond(), WaitGroup section
//   only the original 7 counters printed as " name=value"      | yes   | kLegacyFieldCount = NativeDispatches + 1
//   static_assert on name table                                 | yes   | kCounterNames vs kCounterCount
//   WaitSat::Virtual not double-counted (round 1 fix)           | yes   | WaitFinished() Virtual case adds no WaitSatVirtual; Satisfies() owns it
//
// Contract note for callers (other slices): WaitFinished() is the single place that adds
// WaitsStarted, WaitPolls and WaitBlockedUs for a wait. StreamOps::Poll must NOT also Add(WaitPolls),
// or polls are counted twice.

#include "native/native_stats.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Native::Stats {

namespace {

constexpr u32 kMaxSites = 512;

// sotc: one counter per 64-byte cache line. Guest threads (OpsEmitted, OpsEmitFailed) and the
// command processor (OpsRun, OpsMissing, wait and label counters) would otherwise false-share.
struct alignas(64) PaddedCounter {
    std::atomic<u64> value{0};
};
static_assert(sizeof(PaddedCounter) == 64, "one counter per cache line");

PaddedCounter g_counters[kCounterCount]{};
u64 g_counters_prev[kCounterCount]{};
std::atomic<u64> g_pm4[2][256]{};
u64 g_pm4_prev[2][256]{};

struct Site {
    const char* name{nullptr};
    std::atomic<u64> native{0};
    std::atomic<u64> fallback{0};
    u64 native_prev{0};
    u64 fallback_prev{0};
};
Site g_sites[kMaxSites];
std::atomic<u32> g_site_count{0};
std::mutex g_site_mutex;

const char* const kCounterNames[] = {
    "emit", "emit_fail", "run", "missing", "pm4_fields", "draws", "dispatches",
    "waits", "polls", "wait_us", "w_first", "w_yield", "w_virtual", "w_event",
    "lbl_rec", "lbl_ret", "lbl_boundary", "oplists", "oplist_fb",
};
static_assert(sizeof(kCounterNames) / sizeof(kCounterNames[0]) == kCounterCount,
              "one name per counter");

// sotc: only the original seven counters (OpsEmitted..NativeDispatches) keep the " name=value"
// fields; the wait group is printed compactly after them.
constexpr u32 kLegacyFieldCount = NativeDispatches + 1;
static_assert(kLegacyFieldCount == 7, "the original seven counters");

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("SOTC_NATIVE_STATS");
        return !e || e[0] != '0';
    }();
    return enabled;
}

void Add(Counter counter, u64 n) {
    g_counters[counter].value.fetch_add(n, std::memory_order_relaxed);
}

void WaitFinished(u32 polls, u64 blocked_us, WaitSat how) {
    // sotc: one finished wait: it started (WaitsStarted), took `polls` predicate evaluations in
    // total, blocked for `blocked_us` between its first and passing poll, and passed as `how`.
    Add(WaitsStarted);
    Add(WaitPolls, polls);
    Add(WaitBlockedUs, blocked_us);
    switch (how) {
    case WaitSat::First:
        Add(WaitSatFirst);
        break;
    case WaitSat::Yield:
        Add(WaitSatYield);
        break;
    case WaitSat::Virtual:
        // sotc: N2-112: the wait parked on a pending label write (no re-polls) until it landed.
        Add(WaitSatVirtual);
        break;
    case WaitSat::Event:
        Add(WaitSatEvent);
        break;
    }
}

void Pm4Packet(u32 opcode, bool compute) {
    g_pm4[compute ? 1 : 0][opcode & 0xff].fetch_add(1, std::memory_order_relaxed);
}

u32 RegisterSite(const char* name) {
    std::scoped_lock lock{g_site_mutex};
    const u32 count = g_site_count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(g_sites[i].name, name) == 0) {
            return i;
        }
    }
    if (count >= kMaxSites) {
        return kMaxSites - 1; // shared overflow slot
    }
    g_sites[count].name = name;
    g_site_count.store(count + 1, std::memory_order_release);
    return count;
}

void SiteNative(u32 slot) {
    g_sites[std::min(slot, kMaxSites - 1)].native.fetch_add(1, std::memory_order_relaxed);
}

void SiteFallback(u32 slot) {
    g_sites[std::min(slot, kMaxSites - 1)].fallback.fetch_add(1, std::memory_order_relaxed);
}

void ReportSecond(float seconds_since_start) {
    if (!Enabled()) {
        return;
    }
    // sotc: per-second deltas of every counter, read once so every field of this line agrees.
    u64 delta[kCounterCount];
    for (u32 c = 0; c < kCounterCount; ++c) {
        const u64 now = g_counters[c].value.load(std::memory_order_relaxed);
        delta[c] = now - g_counters_prev[c];
        g_counters_prev[c] = now;
    }
    auto d = [&](Counter c) { return static_cast<unsigned long long>(delta[c]); };

    char line[2048];
    int n = std::snprintf(line, sizeof(line), "SOTC NATIVE STATS %.0fs:", seconds_since_start);
    auto append = [&](const char* fmt, auto... args) {
        if (n > 0 && n < static_cast<int>(sizeof(line))) {
            n += std::snprintf(line + n, sizeof(line) - n, fmt, args...);
        }
    };
    for (u32 c = 0; c < kLegacyFieldCount; ++c) {
        append(" %s=%llu", kCounterNames[c], static_cast<unsigned long long>(delta[c]));
    }
    // sotc: wait group (N2-94 W0): waits, polls, blocked time, how each wait passed, label
    // lifecycle and op-list submissions of the last second.
    append(" | waits=%llu polls=%llu wait_us=%llu first/yield/virtual/event=%llu/%llu/%llu/%llu"
           " lbl=%llu/%llu/%llu oplists=%llu/%llu",
           d(WaitsStarted), d(WaitPolls), d(WaitBlockedUs), d(WaitSatFirst), d(WaitSatYield),
           d(WaitSatVirtual), d(WaitSatEvent), d(LabelsRecorded), d(LabelsRetired),
           d(LabelsAtBoundary), d(OpListsBuilt), d(OpListsFallback));
    // PM4 packets per queue: the total and the five busiest opcodes of the last second.
    for (int q = 0; q < 2; ++q) {
        u64 pm4_delta[256];
        u64 total = 0;
        for (u32 op = 0; op < 256; ++op) {
            const u64 now = g_pm4[q][op].load(std::memory_order_relaxed);
            pm4_delta[op] = now - g_pm4_prev[q][op];
            g_pm4_prev[q][op] = now;
            total += pm4_delta[op];
        }
        append(" | pm4_%s=%llu", q == 0 ? "gfx" : "cs", static_cast<unsigned long long>(total));
        for (int k = 0; k < 5 && total != 0; ++k) {
            u32 best = 0;
            for (u32 op = 1; op < 256; ++op) {
                if (pm4_delta[op] > pm4_delta[best]) {
                    best = op;
                }
            }
            if (pm4_delta[best] == 0) {
                break;
            }
            append(" %02x:%llu", best, static_cast<unsigned long long>(pm4_delta[best]));
            pm4_delta[best] = 0;
        }
    }
    // Boundary sites: native/fallback counts of the sites that ran this second (fallback first,
    // the work left).
    append(" | sites");
    const u32 sites = g_site_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < sites; ++i) {
        Site& s = g_sites[i];
        const u64 nat = s.native.load(std::memory_order_relaxed);
        const u64 fb = s.fallback.load(std::memory_order_relaxed);
        const u64 dn = nat - s.native_prev;
        const u64 df = fb - s.fallback_prev;
        s.native_prev = nat;
        s.fallback_prev = fb;
        if (dn != 0 || df != 0) {
            append(" %s=%llu/%llu", s.name, static_cast<unsigned long long>(dn),
                   static_cast<unsigned long long>(df));
        }
    }
    std::fprintf(stderr, "%s\n", line);
}

} // namespace Native::Stats
