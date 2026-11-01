// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: N5 frame graph, observe and verify (vk_frame_graph.h). See the header for the model.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_frame_graph.h"
#include "video_core/renderer_vulkan/vk_queues.h"
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

// sotc: N5 async compute classifier (SOTC_N5_ASYNC_COMPUTE=2). Observe only; see
// impl-n5-asynccomp.md for the rule. A compute pass is a maximal run of Class::Compute ops of one
// frame. It is INDEPENDENT (would move to the async queue) when its footprint is known and no
// earlier op of the frame that stayed on graphics conflicts with it (AmdGpu::Conflicts, the
// pass's writes against any earlier access, its reads against earlier writes). Moved passes
// before it are ordered on the same in-order queue, so they need no wait. Other passes stay on
// graphics and would need one submit split first (counted, not moved). An independent pass joins
// at the first later op that conflicts with it, or that is Other or unknown; an unmoved compute
// pass that conflicts with it is a join too. Passes without a join run to the frame end.

/// Sampled frames: 1 in SOTC_N5_ASYNC_EVERY (default 8). Classification is O(ops x pending).
u32 AsyncEvery() {
    static const u32 every = [] {
        const char* v = std::getenv("SOTC_N5_ASYNC_EVERY");
        const long x = v ? std::strtol(v, nullptr, 10) : 0;
        return x >= 1 ? static_cast<u32>(x) : 8u;
    }();
    return every;
}

struct AsyncAccess {
    AmdGpu::OpRange range;
    bool write;
};

struct AsyncPass {
    AmdGpu::OpFootprint fp{};
    u64 window_ops{};  ///< ops after the pass ended (compute ops included), up to its join
    u64 window_gfx{};  ///< of those, graphics ops that do not conflict with the pass
};

enum class AsyncJoin : u8 { Graphics, UnmovedCompute, OtherOrUnknown, FrameEnd };

struct AsyncState {
    u64 frame{~u64{0}};
    bool sampled{};
    bool in_pass{};
    bool barrier_seen{};             ///< an unknown op of this frame stayed on graphics
    AsyncPass cur{};                 ///< the open compute pass
    std::vector<AsyncAccess> accesses; ///< accesses of this frame that stayed on graphics
    std::vector<AsyncPass> pending;  ///< independent passes not joined yet
};
AsyncState as;

struct AsyncCounters {
    std::atomic<u64> frames{};            ///< sampled frames
    std::atomic<u64> passes{};            ///< compute passes
    std::atomic<u64> independent{};       ///< would move (known footprint, no earlier conflict)
    std::atomic<u64> needs_split{};       ///< would move after one submit split (earlier conflict)
    std::atomic<u64> unknown{};           ///< unknown footprint: stays on graphics, blocks later
    std::atomic<u64> join[4]{};           ///< joins of independent passes, by AsyncJoin
    std::atomic<u64> joined{};            ///< independent passes joined before the frame end or not
    std::atomic<u64> win_ops{};           ///< window ops summed over joined passes
    std::atomic<u64> win_gfx{};           ///< window graphics ops that do not conflict, summed
    std::atomic<u64> zero_window{};       ///< joined passes whose join was the next op
};
AsyncCounters acnt;

bool Overlap(const AmdGpu::OpRange& a, const AmdGpu::OpRange& b) {
    return a.size != 0 && b.size != 0 && a.addr < b.addr + b.size && b.addr < a.addr + a.size;
}

void AsyncJoined(const AsyncPass& p, AsyncJoin kind) {
    acnt.join[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    acnt.joined.fetch_add(1, std::memory_order_relaxed);
    acnt.win_ops.fetch_add(p.window_ops, std::memory_order_relaxed);
    acnt.win_gfx.fetch_add(p.window_gfx, std::memory_order_relaxed);
    if (p.window_ops == 0) {
        acnt.zero_window.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Removes pending[i] by moving the last entry into its place (no self move-assign).
void RemovePending(std::size_t i) {
    if (i + 1 != as.pending.size()) {
        as.pending[i] = std::move(as.pending.back());
    }
    as.pending.pop_back();
}

/// Joins every pending pass that conflicts with fp (an unknown fp conflicts with all).
void JoinConflicting(const AmdGpu::OpFootprint& fp, AsyncJoin kind) {
    for (std::size_t i = 0; i < as.pending.size();) {
        if (AmdGpu::Conflicts(as.pending[i].fp, fp)) {
            AsyncJoined(as.pending[i], kind);
            RemovePending(i);
        } else {
            ++i;
        }
    }
}

/// True when fp must wait for an earlier access of this frame that stayed on graphics.
bool NeedsSplit(const AmdGpu::OpFootprint& fp) {
    if (as.barrier_seen) {
        return true;
    }
    for (const AsyncAccess& a : as.accesses) {
        if (a.write) {
            for (const auto& r : fp.reads) {
                if (Overlap(r, a.range)) {
                    return true; // RAW
                }
            }
        }
        for (const auto& w : fp.writes) {
            if (Overlap(w, a.range)) {
                return true; // WAW (earlier write) or WAR (earlier read)
            }
        }
    }
    return false;
}

void AddAccesses(const AmdGpu::OpFootprint& fp) {
    for (const auto& r : fp.reads) {
        as.accesses.push_back({r, false});
    }
    for (const auto& w : fp.writes) {
        as.accesses.push_back({w, true});
    }
}

void AsyncClosePass() {
    if (!as.in_pass) {
        return;
    }
    as.in_pass = false;
    acnt.passes.fetch_add(1, std::memory_order_relaxed);
    if (!as.cur.fp.known) {
        acnt.unknown.fetch_add(1, std::memory_order_relaxed);
        JoinConflicting(as.cur.fp, AsyncJoin::UnmovedCompute);
        as.barrier_seen = true;
        return;
    }
    if (NeedsSplit(as.cur.fp)) {
        acnt.needs_split.fetch_add(1, std::memory_order_relaxed);
        JoinConflicting(as.cur.fp, AsyncJoin::UnmovedCompute);
        AddAccesses(as.cur.fp);
        return;
    }
    acnt.independent.fetch_add(1, std::memory_order_relaxed);
    as.pending.push_back(std::move(as.cur));
}

/// The frame ends: an open pass closes, every pending pass joins at the frame end.
void AsyncEndFrame() {
    AsyncClosePass();
    for (const AsyncPass& p : as.pending) {
        AsyncJoined(p, AsyncJoin::FrameEnd);
    }
    as.pending.clear();
    as.accesses.clear();
    as.barrier_seen = false;
}

void AsyncObserveOp(const Sample& s) {
    if (s.cls == Class::Compute) {
        if (!as.in_pass) {
            as.in_pass = true;
            as.cur = AsyncPass{};
            as.cur.fp.known = true;
        }
        if (!s.known) {
            as.cur.fp.known = false;
        } else if (as.cur.fp.known) {
            as.cur.fp.reads.insert(as.cur.fp.reads.end(), s.fp.reads.begin(), s.fp.reads.end());
            as.cur.fp.writes.insert(as.cur.fp.writes.end(), s.fp.writes.begin(), s.fp.writes.end());
        }
        for (AsyncPass& p : as.pending) {
            p.window_ops++; // a compute op after the pass: not a join (in-order queue)
        }
        return;
    }
    AsyncClosePass();
    const bool blocking = s.cls == Class::Other || !s.known;
    for (std::size_t i = 0; i < as.pending.size();) {
        AsyncPass& p = as.pending[i];
        if (blocking || AmdGpu::Conflicts(p.fp, s.fp)) {
            AsyncJoined(p, blocking ? AsyncJoin::OtherOrUnknown : AsyncJoin::Graphics);
            RemovePending(i);
            continue;
        }
        p.window_ops++;
        if (s.cls == Class::Graphics) {
            p.window_gfx++;
        }
        ++i;
    }
    if (!s.known) {
        as.barrier_seen = true;
    } else {
        AddAccesses(s.fp);
    }
}

/// Record hook (draw thread). Frame changes come from SotcStats::flip_count.
void AsyncObserve(const Sample& s) {
    const u64 flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (flips != as.frame) {
        if (as.sampled) {
            AsyncEndFrame();
        }
        as.frame = flips;
        as.sampled = flips % AsyncEvery() == 0;
        if (as.sampled) {
            acnt.frames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (as.sampled) {
        AsyncObserveOp(s);
    }
}

} // namespace

// sotc: N5 async compute move state (draw thread only). A pass here is one dispatch.
namespace {

struct MovedPass {
    AmdGpu::OpFootprint fp;
    u64 value; ///< async tick of the dispatch's submit
};

/// Beyond this many graphics accesses in a frame, no dispatch moves until the frame ends (and the
/// list stops growing: every dispatch is then a conflict, see MoveNeedsSplit).
constexpr std::size_t kMoveMaxAccesses = 1024;

/// Window counters of the move (ReportMove). Draw thread writes, the report thread takes.
struct MoveCounters {
    std::atomic<u64> frames{};     ///< frame changes seen
    std::atomic<u64> candidates{}; ///< dispatches AsyncBefore allowed to move
    std::atomic<u64> moved{};      ///< dispatches recorded on the async queue
    std::atomic<u64> unmoved{};    ///< dispatches recorded on graphics with a known footprint
    std::atomic<u64> joins{};      ///< joins that made graphics work wait for moved dispatches
    std::atomic<u64> joined{};     ///< moved dispatches those joins completed
};
MoveCounters mvc;

struct MoveState {
    u64 frame{~u64{0}};
    bool barrier_seen{};               ///< an unknown op stayed on graphics this frame
    std::vector<AsyncAccess> accesses; ///< accesses of the graphics ops and dispatches of this frame
    std::vector<MovedPass> moved;      ///< moved dispatches not joined yet, in submit order
};
MoveState mv;

void MoveAddAccesses(const AmdGpu::OpFootprint& fp) {
    if (mv.accesses.size() > kMoveMaxAccesses) {
        return;
    }
    for (const auto& r : fp.reads) {
        mv.accesses.push_back({r, false});
    }
    for (const auto& w : fp.writes) {
        mv.accesses.push_back({w, true});
    }
}

/// True when fp conflicts with a graphics access of this frame: such a dispatch cannot move.
bool MoveNeedsSplit(const AmdGpu::OpFootprint& fp) {
    if (mv.barrier_seen || mv.accesses.size() > kMoveMaxAccesses) {
        return true;
    }
    for (const AsyncAccess& a : mv.accesses) {
        if (a.write) {
            for (const auto& r : fp.reads) {
                if (Overlap(r, a.range)) {
                    return true; // RAW
                }
            }
        }
        for (const auto& w : fp.writes) {
            if (Overlap(w, a.range)) {
                return true; // WAW or WAR
            }
        }
    }
    return false;
}

/// Joins the moved dispatches fp conflicts with (all of them when blocking). The value of the
/// join completes every earlier submit of the async queue, so those leave the list too.
u64 MoveJoin(const AmdGpu::OpFootprint& fp, bool blocking) {
    u64 join = 0;
    for (std::size_t i = 0; i < mv.moved.size();) {
        if (blocking || AmdGpu::Conflicts(mv.moved[i].fp, fp)) {
            join = std::max(join, mv.moved[i].value);
        }
        ++i;
    }
    if (join == 0) {
        return 0;
    }
    mvc.joins.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < mv.moved.size();) {
        if (mv.moved[i].value <= join) {
            if (i + 1 != mv.moved.size()) {
                mv.moved[i] = std::move(mv.moved.back());
            }
            mv.moved.pop_back();
            mvc.joined.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++i;
        }
    }
    return join;
}

u64 MoveAll() {
    u64 join = 0;
    for (const MovedPass& m : mv.moved) {
        join = std::max(join, m.value);
    }
    mv.moved.clear();
    return join;
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

    if (AsyncVerify()) { // sotc: SOTC_N5_ASYNC_COMPUTE=2 (classifier, sampled frames)
        AsyncObserve(s);
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

bool AsyncVerify() {
    return SotcQueues::AsyncMode() == 2;
}

void ReportAsync(std::string& out, double seconds) {
    if (!AsyncVerify()) {
        return;
    }
    const u64 frames = Take(acnt.frames);
    const u64 passes = Take(acnt.passes);
    const u64 independent = Take(acnt.independent);
    const u64 needs_split = Take(acnt.needs_split);
    const u64 unknown = Take(acnt.unknown);
    const u64 join_gfx = Take(acnt.join[static_cast<std::size_t>(AsyncJoin::Graphics)]);
    const u64 join_comp = Take(acnt.join[static_cast<std::size_t>(AsyncJoin::UnmovedCompute)]);
    const u64 join_other = Take(acnt.join[static_cast<std::size_t>(AsyncJoin::OtherOrUnknown)]);
    const u64 join_end = Take(acnt.join[static_cast<std::size_t>(AsyncJoin::FrameEnd)]);
    const u64 joined = Take(acnt.joined);
    const u64 win_ops = Take(acnt.win_ops);
    const u64 win_gfx = Take(acnt.win_gfx);
    const u64 zero = Take(acnt.zero_window);
    if (frames == 0 || seconds <= 0.0) {
        return;
    }
    const double f = static_cast<double>(frames);
    const double jd = joined ? static_cast<double>(joined) : 1.0;
    const u64 waits = join_gfx + join_comp + join_other; // graphics submits that wait on async
    char line[900];
    std::snprintf(
        line, sizeof(line),
        "N5 async (SOTC_N5_ASYNC_COMPUTE=2, 1 in %u frames sampled, %llu frames): compute "
        "passes/f %.1f: independent %.1f, would move after one submit split %.1f, unknown fp "
        "%.1f; moved 0 (move not implemented); joins/f: graphics %.1f, unmoved compute %.1f, "
        "other/unknown %.1f, frame end %.1f; window %.1f ops, zero-window %.0f%%, graphics ops in "
        "windows/f %.1f (overlap candidates, not GPU time); timeline waits/f %.1f, submit "
        "splits/f %.1f\n",
        AsyncEvery(), static_cast<unsigned long long>(frames), static_cast<double>(passes) / f,
        static_cast<double>(independent) / f, static_cast<double>(needs_split) / f,
        static_cast<double>(unknown) / f, static_cast<double>(join_gfx) / f,
        static_cast<double>(join_comp) / f, static_cast<double>(join_other) / f,
        static_cast<double>(join_end) / f, static_cast<double>(win_ops) / jd,
        100.0 * static_cast<double>(zero) / jd, static_cast<double>(win_gfx) / f,
        static_cast<double>(waits) / f, static_cast<double>(waits) / f);
    out += line;
}

bool AsyncMove() {
    return SotcQueues::AsyncMoveOn();
}

AsyncPre AsyncBefore(const Sample& s) {
    AsyncPre out;
    if (!s.active) {
        return out;
    }
    const u64 flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (flips != mv.frame) {
        // The frame ended: every moved dispatch of it is joined by the tick being recorded, and the
        // graphics accesses of the frame are not needed by the next one (Scheduler::AsyncBegin
        // flushes every graphics op before a dispatch moves).
        mv.frame = flips;
        mvc.frames.fetch_add(1, std::memory_order_relaxed);
        out.join = MoveAll();
        out.frame_change = true;
        mv.accesses.clear();
        mv.barrier_seen = false;
    }
    if (s.cls == Class::Compute) {
        out.candidate = s.known && !MoveNeedsSplit(s.fp);
        if (out.candidate) {
            mvc.candidates.fetch_add(1, std::memory_order_relaxed);
        }
        return out;
    }
    const bool blocking = s.cls == Class::Other || !s.known;
    out.join = std::max(out.join, MoveJoin(s.fp, blocking));
    if (blocking) {
        mv.barrier_seen = true;
    } else {
        MoveAddAccesses(s.fp);
    }
    return out;
}

u64 AsyncConflicts(const AmdGpu::OpFootprint& fp) {
    return MoveJoin(fp, !fp.known);
}

void AsyncAfter(const AmdGpu::OpFootprint& fp, bool moved, u64 value) {
    if (moved) {
        mvc.moved.fetch_add(1, std::memory_order_relaxed);
        mv.moved.push_back(MovedPass{fp, value});
        return;
    }
    if (!fp.known) {
        mv.barrier_seen = true;
        return;
    }
    mvc.unmoved.fetch_add(1, std::memory_order_relaxed);
    MoveAddAccesses(fp);
}

void ReportMove(std::string& out, double seconds) {
    if (!AsyncMove()) {
        return;
    }
    const u64 frames = mvc.frames.exchange(0, std::memory_order_relaxed);
    const u64 candidates = mvc.candidates.exchange(0, std::memory_order_relaxed);
    const u64 moved = mvc.moved.exchange(0, std::memory_order_relaxed);
    const u64 unmoved = mvc.unmoved.exchange(0, std::memory_order_relaxed);
    const u64 joins = mvc.joins.exchange(0, std::memory_order_relaxed);
    const u64 joined = mvc.joined.exchange(0, std::memory_order_relaxed);
    if (seconds <= 0.0) {
        return;
    }
    const double f = frames ? static_cast<double>(frames) : 1.0;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "N5 move (SOTC_N5_ASYNC_COMPUTE=1): frames %llu, candidates/f %.1f, moved/f %.1f, "
                  "graphics dispatches/f %.1f, joins/f %.2f (passes joined/join %.2f)\n",
                  static_cast<unsigned long long>(frames), static_cast<double>(candidates) / f,
                  static_cast<double>(moved) / f, static_cast<double>(unmoved) / f,
                  static_cast<double>(joins) / f,
                  joins ? static_cast<double>(joined) / static_cast<double>(joins) : 0.0);
    out += line;
}

} // namespace Vulkan::FrameGraph
