// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: N5 frame graph, observe and verify (vk_frame_graph.h). See the header for the model.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_frame_graph.h"
#include "video_core/sotc_stats.h"
#include "../../../sotc/native/native.h" // sotc: SOTC_NATIVE mode

namespace Vulkan::FrameGraph {

namespace {

/// A hazard group stops growing past this many ranges: the next op is then a barrier point.
constexpr std::size_t kMaxGroupRanges = 64;
/// Verify: how many missed ops are logged per process.
constexpr u32 kMaxMissLogs = 24;

struct Counters {
    std::atomic<u64> ops{};
    std::atomic<u64> passes{};
    std::atomic<u64> gfx_passes{};
    std::atomic<u64> comp_passes{};
    std::atomic<u64> restart_after_comp{};
    std::atomic<u64> restart_after_other{};
    std::atomic<u64> pred{};       ///< ops with a predicted barrier (hazard boundary)
    std::atomic<u64> op_events{};  ///< barrier events recorded inside ops
    std::atomic<u64> ev_total{};   ///< barrier events on the draw thread (inside and between ops)
    std::atomic<u64> missed{};     ///< ops with a barrier event and no predicted barrier
    std::atomic<u64> unknown{};    ///< draw / dispatch ops with an unknown footprint
    std::atomic<u64> func_ops{};   ///< command-processor ops (Class::Other)
    std::atomic<u64> owner_ns{};   ///< draw-thread time in footprint + Record
};
Counters cnt;

/// Draw-thread state. Touched by Record only.
struct DrawState {
    AmdGpu::OpFootprint group{}; ///< union of the footprints since the last predicted barrier
    bool poisoned{true};         ///< the next op is a barrier point (start, unknown, overflow)
    bool any{};
    Class prev_cls{Class::Other};
    u64 prev_key{};
    bool have_gfx{};
    u64 last_gfx_key{};
    u64 last_seen{};            ///< BarrierEvents() at the last Record
    u32 miss_logs{};
    u64 last_flips{};          ///< SotcStats::flip_count at the last Report
};
DrawState ds;

thread_local u64 tl_barrier_events{};

/// SOTC_N5_GRAPH: 0 off, 1 observe, 2 verify. Only with SOTC_NATIVE on.
int SwitchValue() {
    static const int value = [] {
        if (!SotcNative::On()) {
            return 0;
        }
        const char* v = std::getenv("SOTC_N5_GRAPH");
        if (!v) {
            return 0;
        }
        const long x = std::strtol(v, nullptr, 10);
        return x == 2 ? 2 : (x == 1 ? 1 : 0);
    }();
    return value;
}

u64 Take(std::atomic<u64>& a) {
    return a.exchange(0, std::memory_order_relaxed);
}

} // namespace

bool Enabled() {
    return SwitchValue() != 0;
}

bool Verify() {
    return SwitchValue() == 2;
}

void NoteBarrier(u32 count) noexcept {
    tl_barrier_events += count;
}

u64 BarrierEvents() noexcept {
    return tl_barrier_events;
}

void AddOwnerNs(u64 ns) noexcept {
    cnt.owner_ns.fetch_add(ns, std::memory_order_relaxed);
}

void Record(Sample& s) {
    const auto t0 = std::chrono::steady_clock::now();
    const u64 now = BarrierEvents();
    const u64 op_events = now - s.barrier0;
    cnt.ev_total.fetch_add(now - ds.last_seen, std::memory_order_relaxed);
    ds.last_seen = now;

    // Passes: a new one starts when the class changes or a graphics op changes its targets.
    const bool new_pass = !ds.any || s.cls != ds.prev_cls ||
                          (s.cls == Class::Graphics && s.target_key != ds.prev_key);
    if (new_pass) {
        cnt.passes.fetch_add(1, std::memory_order_relaxed);
        if (s.cls == Class::Graphics) {
            cnt.gfx_passes.fetch_add(1, std::memory_order_relaxed);
            // Same targets as the last graphics pass, interrupted in between: the render pass
            // was closed and reopened with nothing that needed it to close.
            if (ds.have_gfx && s.target_key == ds.last_gfx_key) {
                if (ds.prev_cls == Class::Compute) {
                    cnt.restart_after_comp.fetch_add(1, std::memory_order_relaxed);
                } else {
                    cnt.restart_after_other.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else if (s.cls == Class::Compute) {
            cnt.comp_passes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (s.cls == Class::Graphics) {
        ds.have_gfx = true;
        ds.last_gfx_key = s.target_key;
    }
    ds.any = true;
    ds.prev_cls = s.cls;
    ds.prev_key = s.target_key;

    // Hazard groups: predict a barrier before this op or not.
    bool pred;
    if (!s.known) {
        pred = true;
        ds.group = AmdGpu::OpFootprint{};
        ds.group.known = true;
        ds.poisoned = true;
        if (s.cls == Class::Other) {
            cnt.func_ops.fetch_add(1, std::memory_order_relaxed);
        } else {
            cnt.unknown.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (ds.poisoned || AmdGpu::Conflicts(ds.group, s.fp)) {
        pred = true;
        ds.group = s.fp;
        ds.group.known = true;
        ds.poisoned = false;
    } else {
        pred = false;
        ds.group.reads.insert(ds.group.reads.end(), s.fp.reads.begin(), s.fp.reads.end());
        ds.group.writes.insert(ds.group.writes.end(), s.fp.writes.begin(), s.fp.writes.end());
        if (ds.group.reads.size() + ds.group.writes.size() > kMaxGroupRanges) {
            ds.poisoned = true;
        }
    }

    cnt.ops.fetch_add(1, std::memory_order_relaxed);
    if (pred) {
        cnt.pred.fetch_add(1, std::memory_order_relaxed);
    }
    if (op_events) {
        cnt.op_events.fetch_add(op_events, std::memory_order_relaxed);
        if (!pred) {
            cnt.missed.fetch_add(1, std::memory_order_relaxed);
            if (Verify() && ds.miss_logs < kMaxMissLogs) {
                ++ds.miss_logs;
                LOG_INFO(Render,
                         "SOTC_N5_GRAPH verify: op with {} barrier event(s) not predicted "
                         "(class {}, targets {:#x}, footprint reads {} writes {})",
                         op_events, static_cast<int>(s.cls), s.target_key, s.fp.reads.size(),
                         s.fp.writes.size());
            }
        }
    }

    s.active = false;
    const auto dt = std::chrono::steady_clock::now() - t0;
    cnt.owner_ns.fetch_add(
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count()),
        std::memory_order_relaxed);
}

void Report(std::string& out, double seconds) {
    if (!Enabled()) {
        return;
    }
    const u64 ops = Take(cnt.ops);
    const u64 passes = Take(cnt.passes);
    const u64 gfx_passes = Take(cnt.gfx_passes);
    const u64 comp_passes = Take(cnt.comp_passes);
    const u64 restart_comp = Take(cnt.restart_after_comp);
    const u64 restart_other = Take(cnt.restart_after_other);
    const u64 pred = Take(cnt.pred);
    const u64 op_events = Take(cnt.op_events);
    const u64 ev_total = Take(cnt.ev_total);
    const u64 missed = Take(cnt.missed);
    const u64 unknown = Take(cnt.unknown);
    const u64 func_ops = Take(cnt.func_ops);
    const u64 owner_ns = Take(cnt.owner_ns);

    const u64 flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    u64 frames = flips - ds.last_flips;
    ds.last_flips = flips;
    if (frames == 0) {
        frames = 1;
    }
    if (ops == 0 || seconds <= 0.0) {
        return;
    }
    const double f = static_cast<double>(frames);
    const double ops_per_pass = passes ? static_cast<double>(ops) / static_cast<double>(passes) : 0.0;
    // Barriers outside ops (uploads, copies between ops) are not elided by the graph.
    const u64 non_op_events = ev_total > op_events ? ev_total - op_events : 0;

    char line[640];
    std::snprintf(
        line, sizeof(line),
        "N5 graph (SOTC_N5_GRAPH=%d): frames %llu, ops/f %.0f, passes/f %.1f (gfx %.1f, compute "
        "%.1f), ops/pass %.1f, same-target restarts/f %.2f (after compute %.2f, after other "
        "%.2f), barriers/f old %.1f (in ops %.1f) graph %.1f (predicted %.1f + outside ops %.1f), "
        "missed %llu, unknown fp %llu, command-processor ops/f %.0f, owner %.3f ms/f, workers 0 "
        "(pass recording not started)\n",
        Verify() ? 2 : 1, static_cast<unsigned long long>(frames), static_cast<double>(ops) / f,
        static_cast<double>(passes) / f, static_cast<double>(gfx_passes) / f,
        static_cast<double>(comp_passes) / f, ops_per_pass,
        static_cast<double>(restart_comp + restart_other) / f, static_cast<double>(restart_comp) / f,
        static_cast<double>(restart_other) / f, static_cast<double>(ev_total) / f,
        static_cast<double>(op_events) / f, (static_cast<double>(pred) + static_cast<double>(non_op_events)) / f,
        static_cast<double>(pred) / f, static_cast<double>(non_op_events) / f,
        static_cast<unsigned long long>(missed), static_cast<unsigned long long>(unknown),
        static_cast<double>(func_ops) / f, static_cast<double>(owner_ns) / 1e6 / f);
    out += line;
}

} // namespace Vulkan::FrameGraph
