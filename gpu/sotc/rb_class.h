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
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SotcRbClass {

bool On(); ///< SOTC_RB_CLASS=1, read once

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

/// SOTC FPS 5 s report line.
void Report(std::string& out, double seconds);

} // namespace SotcRbClass
