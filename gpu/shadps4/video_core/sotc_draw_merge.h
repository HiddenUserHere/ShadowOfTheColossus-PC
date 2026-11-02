// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DRAW_MERGE_STATS=1 (off by default; a measurement, no behaviour change). Every draw the rasterizer records
// (direct, and indirect with its arguments in memory) is keyed by what one multi-draw call would have to share: the
// pipeline, the push constants and the descriptors, the render targets, the dynamic state, the submit (command buffer)
// and the count of recorded barriers. A run is a sequence of consecutive draws with one key and no barrier between
// them; two indirect draws next to each other also need arguments that follow each other with one constant stride
// in guest memory. Rasterizer::DrawMergeNote builds the keys and the runs (vk_rasterizer.cpp); this header keeps the
// counters the runs close into, and the report line (driver.cpp, every 5 s).
#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/types.h"

namespace SotcDrawMerge {

/// SOTC_DRAW_MERGE_STATS=1 (read once).
inline bool On() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DRAW_MERGE_STATS");
        return v && v[0] == '1';
    }();
    return on;
}

/// Why a run ends: the draw that ends it starts the next run. None joins the draw to the run.
enum class Break : u32 { None, Pipeline, Resources, Targets, Dynamic, Barrier, Submit, Args, Count };

struct Counters {
    std::atomic<u64> indirect{};        // draw-indirect ops recorded
    std::atomic<u64> direct{};          // direct draws recorded
    std::atomic<u64> runs2{};           // mergeable runs (two or more draws), by length
    std::atomic<u64> runs3_4{};
    std::atomic<u64> runs5_8{};
    std::atomic<u64> runs9{};
    std::atomic<u64> saved{};           // draws saved: sum of (length - 1) over the mergeable runs
    std::atomic<u64> direct_in_runs{};  // direct draws that are members of a mergeable run
    std::array<std::atomic<u64>, static_cast<u32>(Break::Count)> breaks{}; // by Break, None unused
};
inline Counters g_counters;

/// One report line pair into out, and the window's counters reset. Called from the report hub (driver.cpp).
inline void Report(std::string& out, double seconds) {
    if (!On()) {
        return;
    }
    (void)seconds;
    const auto take = [](std::atomic<u64>& c) { return c.exchange(0, std::memory_order_relaxed); };
    auto& c = g_counters;
    const u64 indirect = take(c.indirect);
    const u64 direct = take(c.direct);
    const u64 r2 = take(c.runs2);
    const u64 r34 = take(c.runs3_4);
    const u64 r58 = take(c.runs5_8);
    const u64 r9 = take(c.runs9);
    const u64 saved = take(c.saved);
    const u64 direct_in = take(c.direct_in_runs);
    std::array<u64, static_cast<u32>(Break::Count)> brk{};
    for (u32 i = 0; i < brk.size(); ++i) {
        brk[i] = take(c.breaks[i]);
    }
    if (indirect == 0 && direct == 0) {
        return;
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "SOTC DRAW_MERGE: indirect ops %llu, mergeable runs: len2 %llu, len3-4 %llu, len5-8 %llu, "
                  "len9+ %llu, ops saved %llu\n",
                  static_cast<unsigned long long>(indirect), static_cast<unsigned long long>(r2),
                  static_cast<unsigned long long>(r34), static_cast<unsigned long long>(r58),
                  static_cast<unsigned long long>(r9), static_cast<unsigned long long>(saved));
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC DRAW_MERGE: direct draws %llu (%llu in mergeable runs); run ends: pipeline %llu, "
                  "resources %llu, targets %llu, dynamic %llu, barrier %llu, submit %llu, args %llu\n",
                  static_cast<unsigned long long>(direct), static_cast<unsigned long long>(direct_in),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Pipeline)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Resources)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Targets)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Dynamic)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Barrier)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Submit)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Args)]));
    out += line;
}

} // namespace SotcDrawMerge
