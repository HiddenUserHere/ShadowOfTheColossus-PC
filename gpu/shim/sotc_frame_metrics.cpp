// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_FRAME_METRICS=<csv path> (see video_core/sotc_frame_metrics.h). One row per guest frame.
//
// Frame f is the work between flip f-1 and flip f: the flip is the command processor's PrepareFrame
// (the flip is queued in the command stream), and "flip" below means that point.
//
// Columns (times in ms, 3 decimals):
//   frame            f, from 0 (the first flip)
//   wall_ms          flip f-1 to flip f, measured when the command processor notes the flips
//                    (submission, not presentation). Empty for frame 0.
//   draw_busy_ms     draw thread time inside the ops of frame f, minus the draw thread's own waits
//                    inside those ops (DrawWait: GPU, recorder, upload, texture cache lock).
//                    Exact per op. Zero when the draw thread is off (the ops then run on the
//                    command processor, counted in cp_busy_ms).
//   draw_ops         ops the draw thread executed for frame f (draws, indirect draws, dispatches,
//                    func ops; express-replayed ops count too). Exact.
//   cp_busy_ms       command processor parse time between flip f-1 and flip f (cp_parse_all_ns).
//                    Exact by the command stream order (the command processor runs the packets of
//                    frame f before flip f). Includes the time the command processor was blocked
//                    (cp_blocked_ms), so cp_blocked_ms is a part of it.
//   cp_blocked_ms    command processor time blocked on the draw thread's op ring in the same window
//                    (cp_blocked_ns, part of cp_busy_ms). Same window rule.
//   rec_busy_ms      recording thread busy time (rec_busy_ns) sampled at flip f. APPROXIMATE: the
//                    recorder runs behind the command processor, so this is the time spent while
//                    flip f-1 .. f was being submitted, not exactly the recording of frame f.
//   gpu_ms           EMPTY: real GPU time of the frame's command buffers is not implemented here
//                    (it needs the SOTC_TRACE_GPU timestamp pairs, which need SOTC_TRACE).
//   game_wait_ms     guest event flag wait time (evf_wait, src/runtime_sotc.c), summed over all
//                    guest threads, attributed to the open frame when the wait ends. APPROXIMATE:
//                    not limited to the "Game Render" thread, and a wait can end in a later frame.
//   upload_bytes     guest memory bytes copied to the GPU (BufferCache, SynchronizeMemory), per
//                    upload, attributed to the draw op that made it, else to the open frame.
//   shader_compiles  shader modules created (SotcStats::shaders_compiled), same attribution.
//   pipeline_compiles graphics and compute pipelines created (SotcStats::pipelines_new), same
//                    attribution. Compiles on async worker threads go to the open frame.
//
// Attribution: each op gets the sequence number the Liverpool ring gave it (back_head at queue
// time). At flip f the command processor records how many ops it had queued (boundary f). An op
// with sequence s belongs to the smallest f with boundary(f) > s, so the draw thread's work follows
// the frame the game submitted, however far the draw thread lags. Work with no op (recorder, game
// thread, async workers) goes to the open frame when it is measured.
//
// Output: rows are formatted into the file kLag frames after their flip, so the file grows during
// the run; the rest (and the open frame's closed predecessors) is written at exit. Off: nothing here
// runs, and the hooks are one test each.
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "video_core/sotc_frame_metrics.h"

namespace SotcFrameMetrics {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
bool g_failed = false;
u64 g_next_row = 0; ///< first frame not written yet
// Command processor thread (under g_mutex): the counter values at the previous flip.
u64 g_last_parse = 0, g_last_blocked = 0, g_last_rec = 0;

// Writes the rows of frames [g_next_row, upto). Caller holds g_mutex. Every frame below upto was
// noted by a flip.
void WriteRows(u64 upto) {
    if (!g_file) {
        g_next_row = upto;
        return;
    }
    const auto ms = [](u64 ns) { return static_cast<double>(ns) / 1e6; };
    while (g_next_row < upto) {
        const u64 f = g_next_row++;
        const u64 slot = (f % kRing) * Cols;
        const auto cell = [&](Col c) { return g_cells[slot + c].load(std::memory_order_relaxed); };
        char wall[32] = "";
        if (f >= 1) {
            const u64 now = g_boundary_ns[f % kRing].load(std::memory_order_relaxed);
            const u64 prev = g_boundary_ns[(f - 1) % kRing].load(std::memory_order_relaxed);
            std::snprintf(wall, sizeof(wall), "%.3f", ms(now > prev ? now - prev : 0));
        }
        std::fprintf(g_file, "%llu,%s,%.3f,%llu,%.3f,%.3f,%.3f,,%.3f,%llu,%llu,%llu\n",
                     static_cast<unsigned long long>(f), wall, ms(cell(DrawBusy)),
                     static_cast<unsigned long long>(cell(DrawOps)), ms(cell(CpBusy)),
                     ms(cell(CpBlocked)), ms(cell(RecBusy)), ms(cell(GameWait)),
                     static_cast<unsigned long long>(cell(UploadBytes)),
                     static_cast<unsigned long long>(cell(ShaderCompiles)),
                     static_cast<unsigned long long>(cell(PipelineCompiles)));
    }
    std::fflush(g_file);
}

void OpenFile() {
    g_failed = true; // only one attempt: a failed open turns the metrics off for the run
    const char* path = std::getenv("SOTC_FRAME_METRICS");
    g_file = path ? std::fopen(path, "w") : nullptr;
    if (!g_file) {
        std::fprintf(stderr, "SOTC_FRAME_METRICS: cannot open %s\n", path ? path : "(null)");
        return;
    }
    g_failed = false;
    std::fprintf(g_file, "frame,wall_ms,draw_busy_ms,draw_ops,cp_busy_ms,cp_blocked_ms,rec_busy_ms,"
                         "gpu_ms,game_wait_ms,upload_bytes,shader_compiles,pipeline_compiles\n");
    std::atexit([] {
        std::scoped_lock lock{g_mutex};
        WriteRows(g_flips.load(std::memory_order_acquire));
        if (g_file) {
            std::fclose(g_file);
            g_file = nullptr;
        }
    });
}

} // namespace

void NoteFlip(u64 queued_ops) {
    if (!kOn) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_file && !g_failed) {
        OpenFile();
    }
    // Frame f closes at this flip: the CP work, and the recorder's busy time, since the last flip.
    const auto take = [](const std::atomic<u64>& counter, u64& last) {
        const u64 now = counter.load(std::memory_order_relaxed);
        const u64 delta = now >= last ? now - last : 0;
        last = now;
        return delta;
    };
    const u64 f = g_flips.load(std::memory_order_relaxed);
    Add(f, CpBusy, take(SotcStats::cp_parse_all_ns, g_last_parse));
    Add(f, CpBlocked, take(SotcStats::cp_blocked_ns, g_last_blocked));
    Add(f, RecBusy, take(SotcStats::rec_busy_ns, g_last_rec));

    g_boundary_ops[f % kRing].store(queued_ops, std::memory_order_relaxed);
    g_boundary_ns[f % kRing].store(SotcStats::NowNs(), std::memory_order_relaxed);
    // Open frame f+1: its slot held frame f+1-kRing, written long ago (kLag << kRing).
    const u64 open = f + 1;
    for (u32 c = 0; c < Cols; ++c) {
        g_cells[(open % kRing) * Cols + c].store(0, std::memory_order_relaxed);
    }
    // The release store publishes the boundary to the draw thread (FrameOfOp reads g_flips first).
    g_flips.store(open, std::memory_order_release);
    if (open > kLag) {
        WriteRows(open - kLag);
    }
}

// sotc: every row up to the last flip, before the process ends with _Exit (no atexit handlers run then).
void FlushAll() {
    if (!kOn) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    WriteRows(g_flips.load(std::memory_order_acquire));
    if (g_file) {
        std::fflush(g_file);
    }
}

} // namespace SotcFrameMetrics

// C interface for the loader (src/runtime_sotc.c binds these weakly).
extern "C" int bbgpu_frame_metrics_enabled(void) {
    return SotcFrameMetrics::kOn ? 1 : 0;
}

// A guest event flag wait of ns nanoseconds, from any guest thread (see the GameWait column).
extern "C" void bbgpu_frame_metrics_game_wait(uint64_t ns) {
    SotcFrameMetrics::AddCurrent(SotcFrameMetrics::GameWait, ns);
}
