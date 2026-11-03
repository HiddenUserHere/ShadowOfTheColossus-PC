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

/// SOTC_DRAW_MERGE_STATS (read once). Every mode >= 1 keys the draws by content (Rasterizer::MergeContentKey):
/// the decoded T#/S#/V# of every stage (shader-addressed bases zeroed), the whole flat buffer with its user data
/// (shader-addressed base words zeroed), the render-state registers and the targets; per-draw copies of the same
/// descriptors at new addresses join a run. Indirect arguments need not be adjacent in this key.
///   1: reuse. A draw that joins the run of the last full bind (same content key, graphics bind epoch unchanged,
///      fast-bind generations unchanged, no storage writes) skips BindResources and the pipeline's descriptor and
///      push-constant record; it still records its own draw call. Draws are emitted one by one (no gather).
///   2: verify. Every draw is bound and emitted as before; a draw that joins a run is compared with the run's first
///      draw (buffer handles and ranges, image views, samplers, layouts, push data without ring offsets, bindless
///      slots) and the mismatches are counted.
///   3: measurement of the same key as 2, without the bound-content compare.
inline int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_DRAW_MERGE_STATS");
        return v && v[0] >= '1' && v[0] <= '3' ? v[0] - '0' : 0;
    }();
    return mode;
}
inline bool On() {
    return Mode() != 0;
}

/// Mode 1 graphics bind epoch. Every command buffer access outside a draw (blits, passes, dispatches, deferred
/// recordings, render pass begin and end) bumps it: those may bind graphics descriptors, push constants or the
/// pipeline. A run of draws may reuse the last full bind only while the epoch is unchanged.
inline std::atomic<u64> g_bind_epoch{};
inline thread_local bool t_in_draw = false;
inline u64 BindEpoch() {
    return g_bind_epoch.load(std::memory_order_relaxed);
}
/// SOTC_DRAW_MDI (needs mode 1). 1: the joined draws of a run are deferred and recorded at its flush (multi-draw when
/// VK_EXT_multi_draw is on). 2: verify: the draws are recorded one by one; a joined draw's index inputs are compared
/// with the run head's and the mismatches are counted.
inline int MdiMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_DRAW_MDI");
        return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
    }();
    return mode;
}
/// Why an open run is flushed (the run's own recordings are made before the point that asked).
enum class Flush : u32 { Recording, NonJoin, Invalid, Submit, Count };
/// Set by the rasterizer (mode 1): records the open run. Called before every recording outside a draw (NoteBind),
/// before every submission (NoteSubmit) and at the rasterizer's own flush points.
inline void (*g_flush_hook)(Flush) = nullptr;
inline void NoteBind() {
    if (Mode() == 1 && !t_in_draw) {
        if (g_flush_hook) {
            g_flush_hook(Flush::Recording);
        }
        g_bind_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}
/// A submission ends the command buffer: an open run is recorded into it first (also from inside a draw).
inline void NoteSubmit() {
    if (Mode() == 1 && g_flush_hook) {
        g_flush_hook(Flush::Submit);
    }
}
/// Marks the draw path on this thread: its own recordings do not bump the epoch (see MergeReuse in vk_rasterizer.cpp).
struct DrawScope {
    bool prev;
    DrawScope() : prev(t_in_draw) {
        t_in_draw = true;
    }
    ~DrawScope() {
        t_in_draw = prev;
    }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;
};

/// Why a run ends: the draw that ends it starts the next run. None joins the draw to the run.
enum class Break : u32 { None, Pipeline, Resources, Targets, Dynamic, Barrier, Epoch, Gens, Submit, Args, Count };

/// Mode 1: why a draw did not reuse the last full bind (its own refusal, not a run break). Seen counters (prepared,
/// packet, heap, barrier static, guest barrier) count draws that take the path; they do not refuse.
enum class Refuse : u32 { Feedback, Writes, Shape, NoBind, Dlss, Count };

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
    std::atomic<u64> verify_checks{};   // mode 2: joining draws compared with their run's first draw
    std::atomic<u64> verify_mismatch{}; // mode 2: of those, bound content that differed
    std::atomic<u64> skipped{};         // mode 1: draws that reused the last full bind (no BindResources)
    std::array<std::atomic<u64>, static_cast<u32>(Refuse::Count)> refused{}; // mode 1: by Refuse
    std::atomic<u64> seen_prepared{};   // mode 1: draws with a prepared bind (reused like any other)
    std::atomic<u64> seen_packet{};     // mode 1: draws while the packet path is on
    std::atomic<u64> seen_heap{};       // mode 1: draws while the heap path is on
    std::atomic<u64> seen_barrier{};    // mode 1: draws while BarrierStatic is on (touch replayed)
    std::atomic<u64> seen_guest{};      // mode 1: draws while GuestBarrier is active
    std::array<std::atomic<u64>, static_cast<u32>(Flush::Count)> mdi_flush{}; // SOTC_DRAW_MDI: runs closed, by Flush
    std::atomic<u64> mdi_runs{};        // runs flushed with at least one deferred draw
    std::atomic<u64> mdi_deferred{};    // draws deferred to a run's flush (not counting the run's head)
    std::atomic<u64> mdi_calls{};       // draw calls recorded for the deferred draws (one per multi-draw call)
    std::atomic<u64> mdi_ext_calls{};   // of those, VK_EXT_multi_draw calls
    std::atomic<u64> mdi_checks{};      // mode 2: joined draws whose index inputs were compared with the head's
    std::atomic<u64> mdi_mismatch{};    // mode 2: of those, different
    std::atomic<u64> mdi_ext_avail{};   // 1 when VK_EXT_multi_draw is enabled on the device
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
    const u64 checks = take(c.verify_checks);
    const u64 mismatch = take(c.verify_mismatch);
    const u64 skipped = take(c.skipped);
    std::array<u64, static_cast<u32>(Refuse::Count)> refused{};
    for (u32 i = 0; i < refused.size(); ++i) {
        refused[i] = take(c.refused[i]);
    }
    const u64 seen_prepared = take(c.seen_prepared);
    const u64 seen_packet = take(c.seen_packet);
    const u64 seen_heap = take(c.seen_heap);
    const u64 seen_barrier = take(c.seen_barrier);
    const u64 seen_guest = take(c.seen_guest);
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
                  "resources %llu, targets %llu, dynamic %llu, barrier %llu, epoch %llu, submit %llu, args %llu\n",
                  static_cast<unsigned long long>(direct), static_cast<unsigned long long>(direct_in),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Pipeline)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Resources)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Targets)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Dynamic)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Barrier)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Epoch)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Submit)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Args)]));
    out += line;
    const u64 runs = r2 + r34 + r58 + r9;
    const double avg = runs ? double(saved + runs) / double(runs) : 0.0;
    std::snprintf(line, sizeof line,
                  "SOTC DRAW MERGE: draws %llu, merged %llu (runs %llu, avg len %.2f), flushes by reason "
                  "pipeline %llu, resources %llu, targets %llu, dynamic %llu, barrier %llu, submit %llu, "
                  "args %llu, verify checks %llu, mismatches %llu%s\n",
                  static_cast<unsigned long long>(indirect + direct),
                  static_cast<unsigned long long>(saved), static_cast<unsigned long long>(runs), avg,
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Pipeline)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Resources)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Targets)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Dynamic)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Barrier)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Submit)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Args)]),
                  static_cast<unsigned long long>(checks), static_cast<unsigned long long>(mismatch),
                  Mode() == 1 ? "" : " (not mode 1: no binds skipped)");
    out += line;
    std::snprintf(line, sizeof line,
                  "SOTC DRAW MERGE: mode %d skipped binds %llu (of draws %llu; run ends by epoch %llu, gens %llu)\n",
                  Mode(), static_cast<unsigned long long>(skipped),
                  static_cast<unsigned long long>(indirect + direct),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Epoch)]),
                  static_cast<unsigned long long>(brk[static_cast<u32>(Break::Gens)]));
    out += line;
    if (MdiMode() != 0) {
        const u64 runs_mdi = take(c.mdi_runs);
        const u64 deferred = take(c.mdi_deferred);
        const u64 calls = take(c.mdi_calls);
        const u64 ext_calls = take(c.mdi_ext_calls);
        const u64 checks = take(c.mdi_checks);
        const u64 mdi_mism = take(c.mdi_mismatch);
        std::array<u64, static_cast<u32>(Flush::Count)> fl{};
        for (u32 i = 0; i < fl.size(); ++i) {
            fl[i] = take(c.mdi_flush[i]);
        }
        const u64 saved = deferred > calls ? deferred - calls : 0;
        std::snprintf(line, sizeof line,
                      "SOTC DRAW MDI: mode %d runs %llu, deferred draws %llu, calls saved %llu (ext calls %llu), "
                      "flush by reason recording %llu, nonjoin %llu, invalid %llu, submit %llu, "
                      "multi_draw ext %s, verify checks %llu, mismatches %llu\n",
                      MdiMode(), static_cast<unsigned long long>(runs_mdi),
                      static_cast<unsigned long long>(deferred), static_cast<unsigned long long>(saved),
                      static_cast<unsigned long long>(ext_calls),
                      static_cast<unsigned long long>(fl[static_cast<u32>(Flush::Recording)]),
                      static_cast<unsigned long long>(fl[static_cast<u32>(Flush::NonJoin)]),
                      static_cast<unsigned long long>(fl[static_cast<u32>(Flush::Invalid)]),
                      static_cast<unsigned long long>(fl[static_cast<u32>(Flush::Submit)]),
                      c.mdi_ext_avail.load(std::memory_order_relaxed) ? "on" : "off",
                      static_cast<unsigned long long>(checks), static_cast<unsigned long long>(mdi_mism));
        out += line;
    }
    std::snprintf(line, sizeof line,
                  "SOTC DRAW MERGE: refused feedback %llu, writes %llu, indirect shape %llu, no bind %llu, "
                  "dlss capture %llu; seen prepared %llu, packet %llu, heap %llu, barrier static %llu, "
                  "guest barrier %llu\n",
                  static_cast<unsigned long long>(refused[static_cast<u32>(Refuse::Feedback)]),
                  static_cast<unsigned long long>(refused[static_cast<u32>(Refuse::Writes)]),
                  static_cast<unsigned long long>(refused[static_cast<u32>(Refuse::Shape)]),
                  static_cast<unsigned long long>(refused[static_cast<u32>(Refuse::NoBind)]),
                  static_cast<unsigned long long>(refused[static_cast<u32>(Refuse::Dlss)]),
                  static_cast<unsigned long long>(seen_prepared), static_cast<unsigned long long>(seen_packet),
                  static_cast<unsigned long long>(seen_heap), static_cast<unsigned long long>(seen_barrier),
                  static_cast<unsigned long long>(seen_guest));
    out += line;
}

} // namespace SotcDrawMerge
