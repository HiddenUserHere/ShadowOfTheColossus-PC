// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_FRAME_METRICS=<csv path> (off by default): one CSV row per guest frame with the costs
// a flag can change. Column definitions and the attribution rule are in sotc_frame_metrics.cpp.
// Setting it also turns the SOTC_FPS accounting on (sotc_stats.h: kFpsLog, and the clock reads of
// kFpsTimers; kWriteMarks stays SOTC_FPS only), because the columns are read from those counters.
// Off: every hook is one test of kOn.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "common/types.h"
#include "video_core/sotc_stats.h"

namespace SotcFrameMetrics {

/// SOTC_FRAME_METRICS names a file (read once).
inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_FRAME_METRICS");
    return v != nullptr && v[0] != '\0';
}();

/// The per-frame sums (one cell each).
enum Col : u32 {
    DrawBusy,        ///< draw thread ns executing ops, waits inside them excluded
    DrawOps,         ///< ops the draw thread executed
    CpBusy,          ///< command processor parse ns (cp_parse_all_ns), attributed at the flip
    CpBlocked,       ///< command processor ns blocked on the draw thread (cp_blocked_ns)
    RecBusy,         ///< recording thread busy ns (rec_busy_ns), sampled at the flip
    GameWait,        ///< guest event flag wait ns (any guest thread), see bbgpu_frame_metrics_game_wait
    UploadBytes,     ///< bytes of guest memory copied to the GPU (BufferCache, per upload)
    ShaderCompiles,  ///< shader modules created
    PipelineCompiles,///< graphics and compute pipelines created
    Cols
};

/// Frame f keeps its sums in slot f % kRing. Rows are written kLag frames behind the flip, so a
/// slot is reused only after its row went out.
inline constexpr u64 kRing = 4096;
inline constexpr u64 kLag = 12; // sotc: small, so a run that ends with _exit (no atexit) loses only its last frames
/// Frame of an op that has no frame (the calling thread is not running a draw op).
inline constexpr u64 kNoFrame = ~u64{0};
/// Writes the rows of every frame flipped so far (call before the process exits).
void FlushAll();

inline std::atomic<u64> g_cells[kRing * Cols]{};
/// Flips noted by the command processor (NoteFlip, vk_presenter.cpp). Frame g is closed by flip g;
/// the open frame is g_flips itself.
inline std::atomic<u64> g_flips{0};
/// For flip f: the number of ops the command processor had queued when it was noted, and the time
/// (steady clock ns). Ops with sequence < g_boundary_ops[f] were submitted before flip f.
inline std::atomic<u64> g_boundary_ops[kRing]{};
inline std::atomic<u64> g_boundary_ns[kRing]{};
/// Draw thread: the frame of its last op (ops come in sequence order, so the cursor only moves on).
inline thread_local u64 tl_cursor = 0;
/// Frame of the draw op this thread is executing (DrawOpScope), kNoFrame otherwise.
inline thread_local u64 tl_frame = kNoFrame;

/// Adds n to column col of frame `frame`.
inline void Add(u64 frame, Col col, u64 n) {
    g_cells[(frame % kRing) * Cols + col].fetch_add(n, std::memory_order_relaxed);
}

/// Adds to the frame of the draw op on this thread, else to the open frame (game thread, recorder,
/// async workers: their work is attributed to the frame open when it happened).
inline void AddCurrent(Col col, u64 n) {
    if (!kOn) {
        return;
    }
    Add(tl_frame != kNoFrame ? tl_frame : g_flips.load(std::memory_order_relaxed), col, n);
}

/// Draw thread: the frame that holds op `seq`, i.e. the smallest f with boundary(f) > seq, or the
/// open frame when no flip has been noted after the op was queued.
inline u64 FrameOfOp(u64 seq) {
    const u64 flips = g_flips.load(std::memory_order_acquire);
    u64 c = tl_cursor;
    while (c < flips && g_boundary_ops[c % kRing].load(std::memory_order_relaxed) <= seq) {
        ++c;
    }
    tl_cursor = c;
    return c;
}

/// Sum of the draw thread's exclusive waits (SotcStats::DrawWait), read by DrawOpScope.
inline u64 DrawWaitSum() {
    u64 sum = 0;
    for (u32 k = 0; k < SotcStats::WaitKinds; ++k) {
        sum += SotcStats::draw_wait_ns[k].load(std::memory_order_relaxed);
    }
    return sum;
}

/// Wraps one op of the draw loop (liverpool.cpp). Busy = wall time of the op minus the draw thread's
/// waits inside it. Does nothing when kOn is false.
struct DrawOpScope {
    bool on = false;
    u64 frame = 0;
    u64 t0 = 0;
    u64 waits0 = 0;
    explicit DrawOpScope(u64 seq) {
        if (!kOn) {
            return;
        }
        on = true;
        frame = FrameOfOp(seq);
        tl_frame = frame;
        waits0 = DrawWaitSum();
        t0 = SotcStats::NowNs();
    }
    ~DrawOpScope() {
        if (!on) {
            return;
        }
        const u64 elapsed = SotcStats::NowNs() - t0;
        const u64 waits = DrawWaitSum() - waits0;
        Add(frame, DrawBusy, elapsed > waits ? elapsed - waits : 0);
        Add(frame, DrawOps, 1);
        tl_frame = kNoFrame;
    }
    DrawOpScope(const DrawOpScope&) = delete;
    DrawOpScope& operator=(const DrawOpScope&) = delete;
};

/// Command processor, at each flip (vk_presenter.cpp, PrepareFrame). queued_ops: the number of ops
/// queued so far (Liverpool::QueuedOps). Closes the open frame, opens the next and writes the rows
/// that are final (sotc_frame_metrics.cpp).
void NoteFlip(u64 queued_ops);

} // namespace SotcFrameMetrics
