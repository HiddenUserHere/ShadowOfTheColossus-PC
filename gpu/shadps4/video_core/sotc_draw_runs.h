// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DRAW_RUNS=1 (off by default; measurement only). The ceiling of coalescing indirect draws in the command
// processor (docs/re-backend/16-draw-path-gaps.md, section 7): runs of DRAW_INDEX_INDIRECT packets that follow each
// other in a command buffer with no other packet between them (NOP packets do not end a run, they carry no state)
// and the same args base (SET_BASE). Such a run has one state, so it could be one vkCmdDrawIndexedIndirect with a
// gathered record list. A run whose data offsets step by exactly one record (0x14) is contiguous and needs no gather.
// The parser calls NotePacket for every type-3 packet and NoteIndirect for each DRAW_INDEX_INDIRECT; one line every
// 5 s, "SOTC DRAW RUNS: ...".
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace SotcDrawRuns {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_DRAW_RUNS");
    return v && v[0] == '1';
}();

constexpr int kBuckets = 7; // run length 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64+

struct Counters {
    std::atomic<uint64_t> draws{0}, runs{0}, contiguous_steps{0}, max_run{0};
    std::atomic<uint64_t> bucket[kBuckets]{};
};
inline Counters g_counters;

/// sotc: SOTC_DRAW_COALESCE=1 (default off; liverpool.cpp ProcessGraphics / SubmitIndirectRun). The runs measured
/// above are submitted as one op per stretch of consecutive records instead of one op per draw.
inline const bool kCoalesce = [] {
    const char* v = std::getenv("SOTC_DRAW_COALESCE");
    return v && v[0] == '1';
}();
struct CoalesceCounters {
    std::atomic<uint64_t> runs{0}, draws{0}, ops{0}, dropped{0};
};
inline CoalesceCounters g_coalesce;

struct RunState {
    uint64_t base{};
    uint32_t last_offset{};
    uint64_t length{};
};
inline thread_local RunState t_run;

inline void CloseRun() {
    RunState& r = t_run;
    if (r.length == 0) {
        return;
    }
    int b = 0;
    while (b + 1 < kBuckets && r.length >= (uint64_t{2} << b)) {
        ++b;
    }
    g_counters.runs.fetch_add(1, std::memory_order_relaxed);
    g_counters.bucket[b].fetch_add(1, std::memory_order_relaxed);
    uint64_t cur = g_counters.max_run.load(std::memory_order_relaxed);
    while (r.length > cur && !g_counters.max_run.compare_exchange_weak(cur, r.length, std::memory_order_relaxed)) {
    }
    r.length = 0;
}

/// Every type-3 packet, before it is handled. is_indirect: DRAW_INDEX_INDIRECT (its draw is noted by NoteIndirect);
/// is_nop: a NOP, which does not end a run.
inline void NotePacket(bool is_indirect, bool is_nop) {
    if (!kOn || is_indirect || is_nop) {
        return;
    }
    CloseRun();
}

/// One DRAW_INDEX_INDIRECT: args base (SET_BASE) and data offset of its record.
inline void NoteIndirect(uint64_t base, uint32_t offset) {
    if (!kOn) {
        return;
    }
    RunState& r = t_run;
    g_counters.draws.fetch_add(1, std::memory_order_relaxed);
    if (r.length != 0 && r.base == base) {
        if (offset == r.last_offset + 0x14u) {
            g_counters.contiguous_steps.fetch_add(1, std::memory_order_relaxed);
        }
        ++r.length;
    } else {
        CloseRun();
        r.base = base;
        r.length = 1;
    }
    r.last_offset = offset;
}

/// End of a command buffer: a run never spans two.
inline void NoteEnd() {
    if (kOn) {
        CloseRun();
    }
}

inline void Report(std::string& out, double seconds) {
    if (kCoalesce) {
        const uint64_t runs = g_coalesce.runs.exchange(0), draws = g_coalesce.draws.exchange(0);
        const uint64_t ops = g_coalesce.ops.exchange(0), dropped = g_coalesce.dropped.exchange(0);
        char cline[256];
        std::snprintf(cline, sizeof cline,
                      "SOTC DRAW COALESCE (%.1f s): %.0f indirect draws/s in %.0f runs/s became %.0f ops/s (%.2f "
                      "draws per op; pre-pass drops %.0f/s)\n",
                      seconds, draws / seconds, runs / seconds, ops / seconds,
                      ops ? double(draws - dropped) / double(ops) : 0.0, dropped / seconds);
        out += cline;
    }
    if (!kOn) {
        return;
    }
    const auto take = [](std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
    const uint64_t draws = take(g_counters.draws), runs = take(g_counters.runs);
    const uint64_t steps = take(g_counters.contiguous_steps), max_run = take(g_counters.max_run);
    uint64_t b[kBuckets];
    for (int i = 0; i < kBuckets; ++i) {
        b[i] = take(g_counters.bucket[i]);
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "SOTC DRAW RUNS (%.1f s): DRAW_INDEX_INDIRECT %.0f/s in %.0f runs/s (avg %.2f, max %llu; len "
                  "1/2-3/4-7/8-15/16-31/32-63/64+: %llu/%llu/%llu/%llu/%llu/%llu/%llu); contiguous steps %.1f%% of "
                  "the joins; ops after coalescing %.0f/s (saved %.0f/s)\n",
                  seconds, draws / seconds, runs / seconds, runs ? double(draws) / double(runs) : 0.0,
                  (unsigned long long)max_run, (unsigned long long)b[0], (unsigned long long)b[1],
                  (unsigned long long)b[2], (unsigned long long)b[3], (unsigned long long)b[4],
                  (unsigned long long)b[5], (unsigned long long)b[6],
                  draws > runs ? 100.0 * double(steps) / double(draws - runs) : 0.0, runs / seconds,
                  (draws - runs) / seconds);
    out += line;
}

} // namespace SotcDrawRuns
