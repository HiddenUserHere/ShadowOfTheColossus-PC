// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: PC memory model, explicit readbacks v1 (docs/re-stutters/PC-MEMORY-REWRITE.md section 7,
// step 2). The engine tells the port which guest ranges the CPU reads back from the GPU:
//   - the kind-1 "Default TaggedArena" blocks, where BPXPR_BuildSetDraws (0x19b760) allocates the
//     XPR visibility masks from RenderSingleFrameCPU (allocator probe feed);
//   - the particle stats ring (ProcessStats 0x1018c0, read through 0x8c1b0; PARTICLE_COUNT_READ):
//     in the call that dispatches slot k%3, after the dispatch, that slot's array is asked for
//     (RequestCopy). It is first read two calls later, so the copy has that lead time.
//     Its arrays move every frame, so they are requests, not registered ranges (v1 registered
//     them and filled the table with 61 arrays in a short run).
// v3 (particle_prev.cpp): the request is made in the call that dispatches the slot, after the
// dispatch, for the array that the read two calls later uses (not the one the next call reads).
// Requests for the same array are merged while still pending. Counters: asked, queued, deduped,
// empty, dropped, taken, and the check of each stats read against its request.
// v2 timing (BufferCache::PrefetchIssue): a mask-arena window is copied only in a command
// processor frame after the one it was last written in. The consumer reads masks the GPU wrote two
// frames earlier (the RenderSingleFrameCPU ring alternates halves), so waiting for the frame's last
// cull costs nothing, and the copy no longer goes stale when a later cull writes the same window
// (v1: half the prefetches went stale).
//
// SOTC_RB_CLASS=1 (off by default). Needs SOTC_RB_OWNER=1 and SOTC_RB_MATERIALIZE=1 (the
// machinery it feeds), SOTC_HOOK_ALLOC_PROBE=1 (the arena blocks) and SOTC_PARTICLE_PREV=1 (the
// stats ring hook).
//
// sotc: SOTC_RB_CLASS=2 (buffer_cache.cpp TryServePrevious, c2_win) serves a guest read of a mask
// arena window from the last applied copy, and drops the GPU-modified state of the pages it
// serves. It needs everything above plus SOTC_HOOK_XPR_PROBE=1: the consumer hooks
// XPR_CONSUME_MASK (0x1a6d20) and XPR_CONSUME_MASK_INDIRECT (0x1a7290) record each visibility
// mask span (RecordMaskSpan, probe_xpr.cpp). Without the hook no span exists, so =2 serves
// nothing and counts every read as refused (reason "no mask hook"). A page is served only when
// every GPU-modified byte on it lies inside a mask span recorded in the last 4 command processor
// frames (MaskSpansSince); any other GPU-modified byte makes the read refuse ("not only masks"),
// so no other GPU data loses its GPU-modified state. See RecordMaskSpan for why the bound is
// 4 frames.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SotcRbClass {

bool On(); ///< SOTC_RB_CLASS=1, read once

/// sotc: SOTC_RB_CLASS2_PARTS=<mask> (bisect switch for SOTC_RB_CLASS=2; default 0 = no serve, decimal or 0x hex).
/// kClass2Serve: the class-2 serve (buffer_cache.cpp TryServePrevious) is on. 0 leaves class 2 as class 1
/// plus the XPR hooks and the applied-copy bookkeeping, which is the bisect baseline.
/// kClass2WindowDrop: also drop the GPU-modified state of the pages of the 64 KiB window that the read
/// did not ask for (the old scope). Off by default: those pages keep their GPU state, as in class 1.
constexpr uint32_t kClass2Serve = 1;
constexpr uint32_t kClass2WindowDrop = 2;
uint32_t Class2Parts();

struct Range {
    uint64_t addr;
    uint64_t size;
};

/// Allocator probe feed (any thread): keeps the kind-1 "Default TaggedArena" blocks.
void OnBlock(uint64_t base, uint64_t size, const char* name, int kind);
void OnFreeBlock(uint64_t base);

/// PARTICLE_COUNT_READ: copy this range back now (any thread). The draw thread takes the requests
/// at its next prefetch point (TakeRequests). A request for an address already pending is merged
/// into it (the larger size wins); a zero size is counted as empty and queues nothing.
void RequestCopy(uint64_t addr, uint64_t size);
void TakeRequests(std::vector<Range>& out);

/// Outcome of a stats-ring read checked against the copy requested for it two calls earlier.
/// kNoRequest: nothing was requested for that slot; kMismatch: the array moved since the request.
enum class StatsCheck { kNoRequest, kMatch, kMismatch };
/// One checked stats read. beyond = the read covers systems past the requested extent.
void NoteStatsReadCheck(StatsCheck check, bool beyond);

/// True when [addr, addr + size) overlaps a registered mask-arena block (draw thread; reads the
/// snapshot the caller passes).
bool InRanges(const std::vector<Range>& ranges, uint64_t addr, uint64_t size);

/// Counters the buffer cache reports through here: windows held for the next frame, requests.
void NoteHeld(uint64_t windows);
void NoteRequestIssued(uint64_t windows);

/// The registered ranges and a version that changes with them. Any thread.
std::vector<Range> Snapshot(uint64_t& version);
uint64_t Version();

/// sotc: SOTC_RB_CLASS=2 mask spans (probe_xpr.cpp, XPR consumer hooks; any guest thread).
/// The visibility mask of one draw set is 4 * ceil(n / 32) bytes, n = u16 [set + 0x64] (the
/// allocator size of sub_12820, reports 02 and 07): the consumer reads it at entry, before the
/// first read of the bytes, so the span is recorded before any fault on them.
///
/// Exactness argument (why a span is a set of mask bytes):
/// - the span is exactly the allocation the consumer is handed (its own size, not a window);
/// - the mask bytes are written by the cull (GPU) two command processor frames before the read,
///   and the consumer reads them at once, so a span recorded at frame C names bytes the mask writer
///   wrote at C-2 .. C;
/// - a span is kept for 4 command processor frames (kMaskSpanFrames) and then dropped, not
///   extended: the RenderSingleFrameCPU block may be reused by other data after that. Within
///   those 4 frames a reuse by another GPU writer is not excluded by this registry, only
///   bounded (residual risk, see the report).
/// - a read is served only if every GPU-modified byte of its pages is inside such a span (merged
///   snapshot from MaskSpansSince, CoveredBy), so bytes outside any span are never dropped.
/// The applied-copy freshness bound of TryServePrevious (rb_prev_frames, 150 ms) is NOT relaxed.
void RecordMaskSpan(uint64_t addr, uint64_t size, uint64_t frame);
/// Mask spans recorded in the last 4 frames as a sorted list of merged ranges (adjacent or
/// overlapping spans joined). Any thread.
std::vector<Range> MaskSpansSince(uint64_t frame);
/// True when [lo, hi) lies inside one merged span of `spans` (a MaskSpansSince result).
bool CoveredBy(const std::vector<Range>& spans, uint64_t lo, uint64_t hi);

/// SOTC FPS 5 s report line.
void Report(std::string& out, double seconds);

} // namespace SotcRbClass
